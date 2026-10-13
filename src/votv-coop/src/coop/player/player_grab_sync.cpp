// coop/player/player_grab_sync.cpp -- see coop/player/player_grab_sync.h.
//
// How the lane works:
//   SEND (this peer is the grabber): the local physics-handle grab (mainPlayer
//   grabbing_actor) is watched each tick. A remote peer's ragdoll is visible on this
//   machine as the RagdollDisplay's spawned playerRagdoll_C mirror body, a simulating
//   physics body the handle can grab natively. When the held actor IS such a mirror body,
//   the grab edge (action=1), drag updates (action=2, ~12 Hz or on a significant move,
//   carrying the mirror body's position and the local yaw) and the release edge go out as
//   reliable PlayerGrabState, grabber-authored, host-relayed.
//   RECEIVE (this peer is the target): the target's client applies the drag to its OWN
//   pawn -- it is the authority for its own position, so moving itself (rather than the
//   grabber moving it) keeps the local pose stream authoritative and prevents the
//   snap-back to the pre-ragdoll position.
//
// IMPLEMENTED, UNVERIFIED -- no live two-client test has run. Every HYPOTHESIS below
// needs the live test to confirm or kill it.

#include "coop/player/player_grab_sync.h"

#include "coop/element/element.h"
#include "coop/element/player.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_physics.h"

#include <chrono>
#include <cmath>
#include <cstdint>

namespace coop::playergrab_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Wire actions (must match protocol.h's PlayerGrabPayload comment).
constexpr uint8_t kRelease = 0;
constexpr uint8_t kGrab    = 1;
constexpr uint8_t kDrag    = 2;
constexpr uint8_t kThrow   = 3;

// DESIGN DECISION: the drag send rate. Reliable, so ~12 Hz plus an immediate resend on a
// significant move; every frame is too much for the reliable channel.
constexpr uint64_t kDragIntervalMs   = 80;
constexpr float    kDragResendDistCm = 25.f;
constexpr float    kDragResendYawDeg = 10.f;
// DESIGN DECISION: a release with the body moving faster than this is a throw (action=3,
// pos+vel); slower is a plain release (action=0). A fast drag-release flings the body
// physically, so the velocity classification is honest, but the threshold is a guess.
constexpr float kThrowSpeedCmS = 150.f;
// DESIGN DECISION: the drag latch goes stale this long after the last drag packet. A lost
// release or a departed grabber must not pin the local player down forever.
constexpr uint64_t kDragLatchStaleMs = 2000;
// The stay-down re-flop is not hammered: at most one re-entry per cooldown, so a native
// wake-up in progress is not fought at tick rate.
constexpr uint64_t kReflopCooldownMs = 500;
// World-coordinate sanity (the map is a few km across; NaN/1e30 must never teleport us).
constexpr float kMaxPosCm = 2.0e6f;
constexpr float kMaxVelCmS = 2.0e5f;
// HYPOTHESIS: the wire drag pos is the mirror body's world position (roughly ground
// level); the capsule teleports to pos + halfH so the player stands at the dragged spot
// on recover instead of at ground level. Needs the live test.
constexpr float kHalfHeightFallbackCm = 90.f;
// HYPOTHESIS: the body-follows servo gain/clamp, pulling the native flop toward the
// dragged point on the target's own screen. Needs the live test.
constexpr float kServoGainPerSec = 6.f;
constexpr float kServoMaxCmS     = 1200.f;
constexpr float kServoDeadCm     = 5.f;

// --- send-side edge state (this peer as the grabber) ---
uint8_t g_grabSlot = coop::players::kPeerIdUnknown;  // whose ragdoll body we hold
coop::element::ElementId g_grabTargetEid = coop::element::kInvalidId;
uint64_t g_lastDragSendMs = 0;
ue_wrap::FVector g_lastDragPos{};
float g_lastDragYaw = 0.f;
ue_wrap::FVector g_lastBodyVel{};  // release-edge fallback if the body is already gone

// --- receive-side drag latch (this peer as the dragged target) ---
bool g_dragLatched = false;
uint32_t g_draggedByEid = 0;  // grabber's element id while latched
uint64_t g_lastDragRxMs = 0;
uint64_t g_lastReflopMs = 0;

bool Finite3(const float v[3]) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

float Dist3(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The registry slot whose puppet's LIVE ragdoll mirror body is `body`, or kPeerIdUnknown.
// The local player's own native ragdoll is never a puppet body, so a self-match is
// impossible; a stale body (puppet destroyed, address recycled) fails the slot check.
uint8_t SlotForRagdollBody(void* body) {
    if (!body) return coop::players::kPeerIdUnknown;
    auto& reg = coop::players::Registry::Get();
    const uint8_t selfSlot = reg.LocalPeerId();
    for (uint8_t slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        if (slot == selfSlot) continue;
        coop::RemotePlayer* rp = reg.Puppet(slot);
        if (!rp || !rp->valid()) continue;
        void* rb = rp->RagdollBody();
        if (rb && rb == body && R::IsLiveByIndex(rb, rp->RagdollBodyIdx())) return slot;
    }
    return coop::players::kPeerIdUnknown;
}

coop::element::ElementId ElementIdForSlot(uint8_t slot) {
    coop::element::Player* el = coop::players::Registry::Get().GetPlayerElement(slot);
    return el ? el->GetId() : coop::element::kInvalidId;
}

bool SendGrab(coop::net::Session& session, uint8_t action,
              coop::element::ElementId targetEid,
              const ue_wrap::FVector& pos, const ue_wrap::FVector& vel, float yaw) {
    const coop::element::ElementId selfEid =
        coop::players::Registry::Get().LocalPlayerElementId();
    if (selfEid == coop::element::kInvalidId) {
        UE_LOGW("playergrab: not sending action=%u -- no local player element id yet",
                static_cast<unsigned>(action));
        return false;
    }
    coop::net::PlayerGrabPayload p{};
    p.grabberElementId = selfEid;
    p.targetElementId  = static_cast<uint32_t>(targetEid);
    p.action           = action;
    p.pos[0] = pos.X; p.pos[1] = pos.Y; p.pos[2] = pos.Z;
    p.vel[0] = vel.X; p.vel[1] = vel.Y; p.vel[2] = vel.Z;
    p.yaw = ue_wrap::NormalizeAxis(yaw);
    const bool ok = session.SendReliable(
        coop::net::ReliableKind::PlayerGrabState, &p, sizeof(p));
    if (!ok) {
        UE_LOGW("playergrab: SendReliable action=%u failed (channel busy?)",
                static_cast<unsigned>(action));
    } else {
        UE_LOGI("playergrab: sent action=%u target=0x%08x pos=(%.0f,%.0f,%.0f) yaw=%.0f",
                static_cast<unsigned>(action),
                static_cast<unsigned>(p.targetElementId),
                p.pos[0], p.pos[1], p.pos[2], p.yaw);
    }
    return ok;
}

// HYPOTHESIS (stay-down): the native ragdoll's own recovery stands the player up while
// the grabber is still holding them -- the observed "stands up very fast" -- which lets
// the player walk out of the drag. While a drag latch is live we re-enter the plain
// ragdoll (C-key style: ragdoll=true, passOut=false, death=false) if the native state
// cleared. Conservative: only while the latch is fresh (Tick expires it), never for a
// dead player, never past the grabber's release, and at most one re-entry per cooldown
// so a wake-up in progress is not hammered at tick rate. The live test decides whether
// the native recovery even fires mid-drag; if it does not, this is a harmless no-op.
void StayDownWhileDragged(void* local) {
    if (!local || !R::IsLive(local)) return;
    bool isRagdoll = false, dead = false;
    if (!E::ReadMainPlayerRagdollState(local, isRagdoll, dead)) return;
    if (dead || isRagdoll) return;
    const uint64_t now = NowMs();
    if (now - g_lastReflopMs < kReflopCooldownMs) return;
    if (E::SetMainPlayerRagdollMode(local, /*ragdoll=*/true, /*passOut=*/false,
                                   /*death=*/false)) {
        g_lastReflopMs = now;
        UE_LOGI("playergrab: stay-down re-entered ragdoll while dragged "
                "(grabber=0x%08x)", static_cast<unsigned>(g_draggedByEid));
    }
}

// The authority move: the target moves ITSELF. The capsule is the pose authority, so the
// local pose stream republishes the dragged position and no peer -- grabber included --
// sees a snap-back. HYPOTHESIS on the anchor: the wire pos is the mirror body's position
// (roughly ground level); the capsule goes to pos + halfH so a later stand-up lands at
// the dragged spot rather than at ground level.
void ApplyDragTransform(void* local, const ue_wrap::FVector& pos, float yaw) {
    bool isRagdoll = false, dead = false;
    const bool stateOk = E::ReadMainPlayerRagdollState(local, isRagdoll, dead);
    if (stateOk && dead) return;  // a dead player's drag is death_revive's business
    if (stateOk && !isRagdoll) StayDownWhileDragged(local);

    float halfH = E::GetActorCharacterHalfHeight(local);
    if (!(halfH > 1.f) || halfH > 500.f) halfH = kHalfHeightFallbackCm;
    const ue_wrap::FVector dest{pos.X, pos.Y, pos.Z + halfH};
    E::SetActorLocation(local, dest);  // bTeleport snap; ~12 Hz steps are small
    E::SetActorRotation(local, ue_wrap::FRotator{0.f, ue_wrap::NormalizeAxis(yaw), 0.f});

    // HYPOTHESIS (body follows): the visible flop on our own screen is the native ragdoll
    // body, a separate simulated actor the capsule teleport does not move. Nudge its
    // linear velocity toward the dragged point so the flop chases the drag instead of
    // being left behind. If the live test shows the capsule alone is the right anchor,
    // this servo is the first thing to cut.
    ue_wrap::FVector rdLoc{}, lin{}, ang{};
    ue_wrap::FRotator rdRot{};
    if (E::ReadLocalRagdollPelvisPhysics(local, rdLoc, rdRot, lin, ang)) {
        if (void* mesh = E::GetLocalRagdollBodyMesh(local)) {
            ue_wrap::FVector to{pos.X - rdLoc.X, pos.Y - rdLoc.Y, pos.Z - rdLoc.Z};
            const float d = std::sqrt(to.X * to.X + to.Y * to.Y + to.Z * to.Z);
            if (d > kServoDeadCm) {
                ue_wrap::FVector v{to.X * kServoGainPerSec, to.Y * kServoGainPerSec,
                                   to.Z * kServoGainPerSec};
                const float vm = std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
                if (vm > kServoMaxCmS) {
                    const float s = kServoMaxCmS / vm;
                    v.X *= s; v.Y *= s; v.Z *= s;
                }
                E::SetComponentLinearVelocity(mesh, v.X, v.Y, v.Z);
            }
        }
    }
}

void ApplyThrowTransform(void* local, const ue_wrap::FVector& pos, const ue_wrap::FVector& vel,
                         float yaw) {
    bool isRagdoll = false, dead = false;
    const bool stateOk = E::ReadMainPlayerRagdollState(local, isRagdoll, dead);
    if (stateOk && dead) return;
    float halfH = E::GetActorCharacterHalfHeight(local);
    if (!(halfH > 1.f) || halfH > 500.f) halfH = kHalfHeightFallbackCm;
    const ue_wrap::FVector dest{pos.X, pos.Y, pos.Z + halfH};
    // TeleportTo: the engine's own long-range teleport; a throw can fling far enough that
    // K2_SetActorLocation would be silently reverted by Character constraints.
    E::TeleportTo(local, dest, ue_wrap::FRotator{0.f, ue_wrap::NormalizeAxis(yaw), 0.f});
    // The visible flop keeps the throw: the native body's mesh takes the release velocity.
    // HYPOTHESIS like the drag servo -- the live test confirms the body actually flies.
    if (void* mesh = E::GetLocalRagdollBodyMesh(local)) {
        E::SetComponentLinearVelocity(mesh, vel.X, vel.Y, vel.Z);
        UE_LOGI("playergrab: throw applied pos=(%.0f,%.0f,%.0f) vel=(%.0f,%.0f,%.0f)",
                dest.X, dest.Y, dest.Z, vel.X, vel.Y, vel.Z);
    } else {
        UE_LOGI("playergrab: throw applied pos=(%.0f,%.0f,%.0f) (no native ragdoll mesh "
                "right now; body velocity not set)", dest.X, dest.Y, dest.Z);
    }
}

// The game-thread apply of a for-us packet. The payload was validated at the trust
// boundary in OnGrabState; this re-resolves the local pawn (it can die between the post
// and the dispatch) and applies the action.
void ApplyGrab(const coop::net::PlayerGrabPayload& p) {
    void* local = coop::players::Registry::Get().Local();
    if (!local || !R::IsLive(local)) return;
    const uint64_t now = NowMs();
    const ue_wrap::FVector pos{p.pos[0], p.pos[1], p.pos[2]};
    const ue_wrap::FVector vel{p.vel[0], p.vel[1], p.vel[2]};
    switch (p.action) {
    case kGrab:
        g_dragLatched = true;
        g_draggedByEid = p.grabberElementId;
        g_lastDragRxMs = now;
        UE_LOGI("playergrab: grabbed by 0x%08x -- drag latch set",
                static_cast<unsigned>(p.grabberElementId));
        break;
    case kDrag: {
        if (!g_dragLatched || g_draggedByEid != p.grabberElementId) {
            // A drag with no grab latch (the grab edge was lost): latch now rather than
            // dropping the drag; the release still clears it.
            g_dragLatched = true;
            g_draggedByEid = p.grabberElementId;
            UE_LOGI("playergrab: drag without a grab latch from 0x%08x -- latching",
                    static_cast<unsigned>(p.grabberElementId));
        }
        g_lastDragRxMs = now;
        ApplyDragTransform(local, pos, p.yaw);
        break;
    }
    case kThrow:
        ApplyThrowTransform(local, pos, vel, p.yaw);
        g_dragLatched = false;
        g_draggedByEid = 0;
        break;
    case kRelease:
        g_dragLatched = false;
        g_draggedByEid = 0;
        UE_LOGI("playergrab: released by 0x%08x -- drag latch cleared",
                static_cast<unsigned>(p.grabberElementId));
        break;
    default:
        break;  // validated at the boundary; unreachable
    }
}

void TickSend(coop::net::Session& session, void* local) {
    // The held actor: the physics-handle grab only. A ragdoll mirror body is a simulating
    // physics body, so it arrives through grabbing_actor, never the holding_actor carry
    // slot. HYPOTHESIS for the live test: the E-grab must actually latch onto the spawned
    // mirror body -- if the handle refuses it, this whole send side never fires.
    ue_wrap::engine::MainPlayerGrabState gs{};
    void* held = nullptr;
    if (local && E::ReadMainPlayerGrabState(local, gs) &&
        gs.grabbingActor && R::IsLive(gs.grabbingActor)) {
        held = gs.grabbingActor;
    }

    const uint8_t slot = SlotForRagdollBody(held);
    const uint64_t now = NowMs();

    if (slot == coop::players::kPeerIdUnknown) {
        // Not holding a peer body. A latched grab ending here is the release edge.
        if (g_grabSlot != coop::players::kPeerIdUnknown) {
            // Throw vs drop: the mirror body's speed right after the release, which
            // includes any throw impulse the game just applied; the last held-tick
            // sample is the fallback if the body is already gone.
            ue_wrap::FVector relVel = g_lastBodyVel;
            if (coop::RemotePlayer* rp =
                    coop::players::Registry::Get().Puppet(g_grabSlot)) {
                if (void* rb = rp->RagdollBody()) {
                    if (R::IsLiveByIndex(rb, rp->RagdollBodyIdx()))
                        relVel = E::GetActorVelocity(rb);
                }
            }
            const float speed = std::sqrt(relVel.X * relVel.X + relVel.Y * relVel.Y +
                                          relVel.Z * relVel.Z);
            const bool isThrow = speed >= kThrowSpeedCmS;
            const ue_wrap::FVector zero{};
            SendGrab(session, isThrow ? kThrow : kRelease, g_grabTargetEid, g_lastDragPos,
                     isThrow ? relVel : zero, g_lastDragYaw);
            g_grabSlot = coop::players::kPeerIdUnknown;
            g_grabTargetEid = coop::element::kInvalidId;
        }
        return;
    }

    const coop::element::ElementId targetEid = ElementIdForSlot(slot);
    if (targetEid == coop::element::kInvalidId) return;  // no wire identity yet; cannot author
    ue_wrap::FVector bodyPos{};
    if (!E::TryGetActorLocation(held, bodyPos)) return;
    ue_wrap::FRotator lr{};
    const float yaw =
        E::TryGetActorRotation(local, lr) ? ue_wrap::NormalizeAxis(lr.Yaw) : 0.f;
    g_lastBodyVel = E::GetActorVelocity(held);

    if (g_grabSlot != slot) {
        // New grab edge -- or a straight switch from one peer's body to another's, which
        // closes the old grab as a plain release first (its body already left the handle,
        // so no velocity is meaningful).
        if (g_grabSlot != coop::players::kPeerIdUnknown) {
            const ue_wrap::FVector zero{};
            SendGrab(session, kRelease, g_grabTargetEid, g_lastDragPos, zero, g_lastDragYaw);
        }
        const ue_wrap::FVector zero{};
        SendGrab(session, kGrab, targetEid, bodyPos, zero, yaw);
        g_grabSlot = slot;
        g_grabTargetEid = targetEid;
        g_lastDragPos = bodyPos;
        g_lastDragYaw = yaw;
        g_lastDragSendMs = now;
        return;
    }

    // Continuing drag: the rate limit plus an immediate resend on a significant move, so a
    // fast yank does not wait for the next interval.
    const float moved = Dist3(bodyPos, g_lastDragPos);
    float dyaw = std::fabs(yaw - g_lastDragYaw);
    if (dyaw > 180.f) dyaw = 360.f - dyaw;
    if (now - g_lastDragSendMs >= kDragIntervalMs || moved >= kDragResendDistCm ||
        dyaw >= kDragResendYawDeg) {
        const ue_wrap::FVector zero{};
        SendGrab(session, kDrag, targetEid, bodyPos, zero, yaw);
        g_lastDragPos = bodyPos;
        g_lastDragYaw = yaw;
        g_lastDragSendMs = now;
    }
}

void TickReceiveUpkeep() {
    if (!g_dragLatched) return;
    void* local = coop::players::Registry::Get().Local();
    const uint64_t now = NowMs();
    if (now - g_lastDragRxMs > kDragLatchStaleMs) {
        g_dragLatched = false;
        g_draggedByEid = 0;
        UE_LOGI("playergrab: drag latch expired (no drag packet for %llu ms) -- cleared",
                static_cast<unsigned long long>(kDragLatchStaleMs));
        return;
    }
    // The stay-down enforcement between packets (HYPOTHESIS, see StayDownWhileDragged):
    // the native recovery can stand the player up in the gap between drag packets.
    if (local) StayDownWhileDragged(local);
}

}  // namespace

void OnSessionStart() {
    g_grabSlot = coop::players::kPeerIdUnknown;
    g_grabTargetEid = coop::element::kInvalidId;
    g_lastDragSendMs = 0;
    g_lastDragPos = {};
    g_lastDragYaw = 0.f;
    g_lastBodyVel = {};
    g_dragLatched = false;
    g_draggedByEid = 0;
    g_lastDragRxMs = 0;
    g_lastReflopMs = 0;
}

void Tick(coop::net::Session& session, void* local) {
    if (!session.connected()) return;
    TickSend(session, local);
    TickReceiveUpkeep();
}

void OnGrabState(const coop::net::PlayerGrabPayload& p, int senderSlot) {
    // Trust-boundary validation (the coordinator checked the payload size).
    if (p.action > kThrow) {
        UE_LOGW("playergrab: action=%u out of range -- dropping",
                static_cast<unsigned>(p.action));
        return;
    }
    if (!Finite3(p.pos) || !Finite3(p.vel) || !std::isfinite(p.yaw)) {
        UE_LOGW("playergrab: action=%u non-finite floats -- dropping",
                static_cast<unsigned>(p.action));
        return;
    }
    if (p.grabberElementId == p.targetElementId) {
        UE_LOGW("playergrab: grabber == target (0x%08x) -- dropping",
                static_cast<unsigned>(p.grabberElementId));
        return;
    }
    if (std::fabs(p.pos[0]) > kMaxPosCm || std::fabs(p.pos[1]) > kMaxPosCm ||
        std::fabs(p.pos[2]) > kMaxPosCm ||
        std::fabs(p.vel[0]) > kMaxVelCmS || std::fabs(p.vel[1]) > kMaxVelCmS ||
        std::fabs(p.vel[2]) > kMaxVelCmS || std::fabs(p.yaw) > 360.f) {
        UE_LOGW("playergrab: action=%u out of world bounds -- dropping",
                static_cast<unsigned>(p.action));
        return;
    }

    const coop::element::ElementId selfEid =
        coop::players::Registry::Get().LocalPlayerElementId();
    // Self-echo: our own send relayed back by the host.
    if (selfEid != coop::element::kInvalidId && p.grabberElementId == selfEid) {
        UE_LOGI("playergrab: self-echo action=%u -- dropping",
                static_cast<unsigned>(p.action));
        return;
    }
    const bool forMe = (selfEid != coop::element::kInvalidId &&
                        p.targetElementId == selfEid);
    if (!forMe) {
        // Third party: observed only. The pose + ragdoll streams already render it; no
        // engine work here.
        UE_LOGI("playergrab: observed action=%u grabber=0x%08x target=0x%08x (slot %d)",
                static_cast<unsigned>(p.action),
                static_cast<unsigned>(p.grabberElementId),
                static_cast<unsigned>(p.targetElementId), senderSlot);
        return;
    }
    // For us: apply on the game thread. The coordinator's dispatch already posts to the
    // game thread (the item_activate pattern), but posting again is harmless and keeps
    // this correct if it ever arrives on a net thread; the pawn is re-resolved inside.
    const coop::net::PlayerGrabPayload copy = p;
    GT::Post([copy] { ApplyGrab(copy); });
}

void OnDisconnect() {
    OnSessionStart();  // the same clear: no grab state survives into the next session
}

}  // namespace coop::playergrab_sync
