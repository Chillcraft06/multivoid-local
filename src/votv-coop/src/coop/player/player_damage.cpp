// coop/player/player_damage.cpp -- see coop/player/player_damage.h.
//
// The damage relay's wire half: the host SENDS PlayerDamage to the hit peer, and the owner APPLIES
// it to its own possessed player. The vitals tail -- the pose stream's health fraction, and the
// puppet hurt flash its drop arms -- is already wired, so this module's job ends at making the
// owner's local saveSlot.health drop.

#include "coop/player/player_damage.h"

#include "coop/element/element.h"
#include "coop/element/player.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/actors/vitals.h"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace coop::player_damage {
namespace {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace E = ue_wrap::engine;

std::atomic<coop::net::Session*> g_session{nullptr};  // read in the impact interceptor (any thread)

// Defensive per-hit bound: VOTV max health is ~100, so 1000 is generous headroom for
// any legitimate big hit while rejecting an absurd/exploit value before it reaches the
// damage BP. NaN/Inf/<=0 are rejected too.
constexpr float kMaxDamagePerHit = 1000.f;

bool ValidDamage(float d) { return std::isfinite(d) && d > 0.f && d <= kMaxDamagePerHit; }

// Mass tier boundary for prop-vs-puppet impacts (kg). From Jordan's 2026-10-14 tutorial-level
// mass dump: hand items (drives, tools, crowbar, reelbox) top out at 20.5 kg; the next heaviest
// is a 37.4 kg cardboard box, then 84+ kg furniture. 30 kg sits in the gap. Below: the prop just
// blocks (current behavior, no relay). At/above: knock over + damage via the owner relay.
constexpr float kHeavyPropMassKg = 30.0f;

// Knock-over tuning: shove velocity = mass * kKnockVelPerKg, clamped. A 100 kg trash can gives
// 800 cm/s (a solid shove; walk speed is ~500). Direction comes from the game's impact vector.
constexpr float kKnockVelPerKg = 8.0f;
constexpr float kKnockVelMin = 200.0f;
constexpr float kKnockVelMax = 1200.0f;

// impactDamageCPP param frame offsets, from the 2026-10-14 shimloader object dump
// (Jordan's 70MB dump, /Game/main/mainPlayer.mainPlayer_C:impactDamageCPP):
//   Damage : FloatProperty  [o: 0]
//   Hit    : StructProperty [o: 4]  (FHitResult, unread)
//   Actor  : ObjectProperty [o: 90] (the hitting actor)
//   impact : StructProperty [o: 98] (FVector, the impact vector)
constexpr int kImpactDamageCpp_ActorOffset = 90;
constexpr int kImpactDamageCpp_ImpactOffset = 98;

// The impact-entry cancel. mainPlayer_C's three native->BP impact events, one shared PRE callback
// registered once for each. The class is loaded at the main menu and lives for the process
// (coop/dev/class_lifetime_probe measures it), so the registrations hold across a world change.
const wchar_t* const kImpactEntryNames[3] = {
    L"impactDamage", L"impactDamageCPP", L"impactSquishCPP",
};
void*    g_impactFns[3] = {};
bool     g_impactInterceptorsDone = false;
void*    g_atvImpactFn = nullptr;
bool     g_atvInterceptorDone = false;  // ATV registration retries independently of the
                                        // mainPlayer latch: ATV_C (world content) can load
                                        // after mainPlayer_C first resolves.
bool     g_atvWaitLogged = false;
uint32_t g_canceled = 0;   // rate-latched log counter

// Puppet actor snapshot, published by Tick (game thread) for the ATV interceptor's
// any-thread membership check -- the same pattern as g_localPawn below. kMaxPeers is 4,
// so a plain array + atomic count is enough; Tick rewrites it every call before any latch
// early-out, and the interceptor only reads it with an acquire load of the count.
void*             g_puppetSnapshot[coop::players::kMaxPeers] = {};
std::atomic<int>  g_puppetSnapshotCount{0};

bool IsPuppetSnapshot(void* actor) {
    if (!actor) return false;
    const int n = g_puppetSnapshotCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_puppetSnapshot[i] == actor) return true;
    }
    return false;
}

// The LOCAL possessed player, published by Tick (game thread) for the interceptor's any-thread
// compare. The interceptor may not reach the registry: Registry::Local() mutates game-thread-only
// cache state, and a ProcessEvent interceptor can fire on a parallel-anim worker (game_thread.h's
// own contract). A published atomic keeps the CANCEL effective on every dispatching thread with one
// acquire load and no registry access.
std::atomic<void*> g_localPawn{nullptr};

// PRE cancel: a physical impact dispatched on a mainPlayer BODY that is NOT the local possessed
// player (a peer's puppet, or the F1 skin-preview mannequin) must not run the BP body -- it writes
// the PER-MACHINE saveSlot.health singleton, so lethal contact resolved against a puppet in the
// host's world drained the HOST, and killing a player took the server down with them. The victim's
// own machine computes the same contact on its own possessed player (self==local -> pass-through),
// so nothing is lost: MTA's victim-authoritative shape, no relay. Coop-session gated, because solo
// play has only the local body anyway and an unset registry must never eat solo damage.
bool OnImpactEntryPre(void* self, void* /*params*/) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return false;
    if (!self) return false;
    void* localPawn = g_localPawn.load(std::memory_order_acquire);
    if (!localPawn) return false;           // pre-resolve boot window: native behavior
    if (self == localPawn) return false;    // own body: native path
    const uint32_t n = ++g_canceled;
    if (n <= 5 || (n % 50) == 0) {
        UE_LOGI("player_damage: canceled impact entry on NON-LOCAL body %p (#%u) -- "
                "the hit body's owner computes its own damage", self, n);
    }
    return true;  // cancel: the singleton saveSlot write must not run for a foreign body
}

// Find the peer slot whose puppet actor is `puppetActor`. Returns -1 if none.
int SlotForPuppetActor(void* puppetActor) {
    auto& reg = coop::players::Registry::Get();
    for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        coop::RemotePlayer* pup = reg.Puppet(slot);
        if (pup && pup->GetActor() == puppetActor) return static_cast<int>(slot);
    }
    return -1;
}

// impactDamageCPP-only PRE: the mass-tier dispatcher. Reads the hitting Actor and impact vector
// from the raw params (offsets from Jordan's dump, see above). The interceptor can run on any
// thread, so it only reads the params and cancels; the mass check and relay are posted to the
// game thread (GetActorRootMass uses ProcessEvent; the registry is game-thread-only).
//   - mass < kHeavyPropMassKg: light prop (thrown drive etc.) -- cancel, block, no relay.
//   - mass >= kHeavyPropMassKg: heavy prop (dragged furniture etc.) -- cancel the local body AND
//     relay damage + knock impulse to the puppet's owner, who applies both to their real player.
// Always cancels for non-local bodies (the singleton-saveSlot guard); the tier only decides
// whether the owner gets a relay.
bool OnImpactDamageCppPre(void* self, void* params) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return false;
    if (!self || !params) return false;
    void* localPawn = g_localPawn.load(std::memory_order_acquire);
    if (!localPawn) return false;
    if (self == localPawn) return false;    // own body: native path

    const uint8_t* bytes = static_cast<const uint8_t*>(params);
    void* hitActor = *reinterpret_cast<void* const*>(bytes + kImpactDamageCpp_ActorOffset);
    const float* impactVec = reinterpret_cast<const float*>(bytes + kImpactDamageCpp_ImpactOffset);
    const float damage = *reinterpret_cast<const float*>(bytes);  // offset 0
    const float ivx = impactVec[0], ivy = impactVec[1], ivz = impactVec[2];

    const uint32_t n = ++g_canceled;
    if (!hitActor) {
        if (n <= 5 || (n % 50) == 0) {
            UE_LOGI("player_damage: canceled impactDamageCPP on non-local body %p (#%u) -- no hitting actor",
                    self, n);
        }
        return true;
    }

    // Game thread: mass check, tier decision, and the heavy-tier relay.
    GT::Post([self, hitActor, damage, ivx, ivy, ivz, n] {
        if (!R::IsLive(hitActor) || !R::IsLive(self)) return;
        const float mass = E::GetActorRootMass(hitActor);
        if (!(mass >= kHeavyPropMassKg)) {
            // Light tier: block only. Thrown drives, floppies, food -- they bounce off.
            if (n <= 5 || (n % 50) == 0) {
                UE_LOGI("player_damage: light prop hit puppet (mass=%.1f kg < %.0f) -- blocked, no relay (#%u)",
                        mass, kHeavyPropMassKg, n);
            }
            return;
        }
        // Heavy tier: knock over + damage. Relay to the owner.
        const int ownerSlot = SlotForPuppetActor(self);
        if (ownerSlot < 0) {
            UE_LOGW("player_damage: heavy prop hit (mass=%.1f kg) but no owner slot for puppet %p -- blocked, no relay",
                    mass, self);
            return;
        }
        // Knock direction: the game's impact vector, normalized; fall back to away-from-hitter.
        float ix = ivx, iy = ivy, iz = ivz;
        float len = std::sqrt(ix*ix + iy*iy + iz*iz);
        if (!(len > 1e-3f)) {
            ue_wrap::FVector hitterPos{}, puppetPos{};
            if (E::TryGetActorLocation(hitActor, hitterPos) && E::TryGetActorLocation(self, puppetPos)) {
                ix = puppetPos.X - hitterPos.X;
                iy = puppetPos.Y - hitterPos.Y;
                iz = 0.f;
                len = std::sqrt(ix*ix + iy*iy);
            }
        }
        float kx = 0.f, ky = 0.f, kz = 0.f;
        if (len > 1e-3f) {
            float speed = mass * kKnockVelPerKg;
            if (speed < kKnockVelMin) speed = kKnockVelMin;
            if (speed > kKnockVelMax) speed = kKnockVelMax;
            kx = ix / len * speed;
            ky = iy / len * speed;
            kz = iz / len * speed;
        }
        UE_LOGI("player_damage: HEAVY prop hit puppet (mass=%.1f kg >= %.0f) -- relaying damage=%.1f + knock=(%.0f,%.0f,%.0f) to slot %d",
                mass, kHeavyPropMassKg, damage, kx, ky, kz, ownerSlot);
        SendPlayerDamageWithImpulse(ownerSlot, damage, kx, ky, kz);
    });
    return true;  // cancel the local body synchronously; the owner applies it to the real player
}

// ATV impactDamageCPP PRE: the ATV has its own impactDamageCPP (separate from mainPlayer_C).
// When the ATV hits a puppet, self=ATV, params->Actor=puppet. We check if the hit actor is a
// puppet, and if so, relay heavy-tier damage+knock to the puppet's owner. The ATV is always
// heavy (>=30kg), so no mass-tier check needed -- it's always a knock.
bool OnAtvImpactDamageCppPre(void* self, void* params) {
    if (!self || !params) return false;
    const uint8_t* bytes = static_cast<const uint8_t*>(params);
    void* hitActor = *reinterpret_cast<void* const*>(bytes + kImpactDamageCpp_ActorOffset);
    const float* impactVec = reinterpret_cast<const float*>(bytes + kImpactDamageCpp_ImpactOffset);
    const float damage = *reinterpret_cast<const float*>(bytes);
    const float ivx = impactVec[0], ivy = impactVec[1], ivz = impactVec[2];

    if (!hitActor) return false;  // ATV hit the world, not a puppet -- let it through
    // Synchronous puppet check against the Tick-published snapshot: only cancel the ATV's
    // native impact body when the hit actor is a puppet. Cancelling for every hit (the
    // first version did, deciding membership later on the game thread) silently ate the
    // ATV's own impact damage against walls, props and the ground.
    if (!IsPuppetSnapshot(hitActor)) return false;

    // Game thread: authoritative slot lookup, then relay to the puppet's owner.
    GT::Post([self, hitActor, damage, ivx, ivy, ivz] {
        if (!R::IsLive(hitActor) || !R::IsLive(self)) return;
        const int ownerSlot = SlotForPuppetActor(hitActor);
        if (ownerSlot < 0) return;  // Not a puppet -- let the ATV's normal impact run
        // It IS a puppet. The ATV is heavy -- relay damage + knock to the owner.
        const float mass = E::GetActorRootMass(self);
        UE_LOGI("player_damage: ATV hit puppet (mass=%.1f kg) -- relaying damage=%.1f to slot %d",
                mass, damage, ownerSlot);
        // Knock direction: away from the ATV, using the impact vector or positional fallback.
        float ix = ivx, iy = ivy, iz = ivz;
        float len = std::sqrt(ix*ix + iy*iy + iz*iz);
        if (!(len > 1e-3f)) {
            ue_wrap::FVector atvPos{}, puppetPos{};
            if (E::TryGetActorLocation(self, atvPos) && E::TryGetActorLocation(hitActor, puppetPos)) {
                ix = puppetPos.X - atvPos.X;
                iy = puppetPos.Y - atvPos.Y;
                iz = 0.f;
                len = std::sqrt(ix*ix + iy*iy);
            }
        }
        float kx = 0.f, ky = 0.f, kz = 0.f;
        if (len > 1e-3f) {
            // ATV is heavy -- use max knock speed, scaled slightly by mass.
            float speed = kKnockVelMax;
            if (mass > 0.f && mass < 500.f) {
                // Lighter than expected? Scale down proportionally, but keep it punchy.
                speed = kKnockVelMin + (kKnockVelMax - kKnockVelMin) * (mass / 500.f);
            }
            kx = ix / len * speed;
            ky = iy / len * speed;
            kz = iz / len * speed;
        }
        SendPlayerDamageWithImpulse(ownerSlot, damage, kx, ky, kz);
    });
    return true;  // cancel the ATV's local impact body; the owner applies it
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    // Publish the local pawn and the puppet snapshot for the interceptors' any-thread
    // compares (GT here; Registry::Local() is GT-only and cached). This runs every Tick,
    // before either latch's early-out.
    auto& reg = coop::players::Registry::Get();
    g_localPawn.store(reg.Local(), std::memory_order_release);
    {
        int n = 0;
        for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot) {
            coop::RemotePlayer* pup = reg.Puppet(slot);
            if (pup && pup->GetActor()) g_puppetSnapshot[n++] = pup->GetActor();
        }
        g_puppetSnapshotCount.store(n, std::memory_order_release);
    }
    if (!g_impactInterceptorsDone) {
        void* cls = ue_wrap::object_index::ClassByName(L"mainPlayer_C");
        if (cls) {
            // A Blueprint class loads with its functions, so the first attempt with the class present is the
            // last: a name that does not resolve is a recooked Blueprint, and a full table is a capacity fault
            // to fix, not a slot to wait for.
            g_impactInterceptorsDone = true;
            bool all = true;
            for (int i = 0; i < 3; ++i) {
                g_impactFns[i] = R::FindFunction(cls, kImpactEntryNames[i]);
                // impactDamageCPP gets the mass-tier dispatcher (reads the hitting Actor from the params);
                // the other entries keep the plain non-local-body cancel.
                auto* cb = (i == 1) ? &OnImpactDamageCppPre : &OnImpactEntryPre;
                if (g_impactFns[i] && GT::RegisterInterceptor(g_impactFns[i], cb)) continue;
                all = false;
                UE_LOGE("player_damage: impact entry %ls %s -- a non-local body runs its damage body", kImpactEntryNames[i],
                        g_impactFns[i] ? "did not register (the interceptor table is full)"
                                       : "is not declared on mainPlayer_C (a recooked Blueprint)");
            }
            if (all) {
                UE_LOGI("player_damage: impact-entry PRE cancels installed (impactDamage=%p "
                        "impactDamageCPP=%p impactSquishCPP=%p) -- non-local bodies never run "
                        "the saveSlot damage body",
                        g_impactFns[0], g_impactFns[1], g_impactFns[2]);
            }
        }
    }
    // ATV interceptor: the ATV has its own impactDamageCPP. When it hits a puppet, we relay
    // heavy-tier damage+knock to the puppet's owner. Registered on its OWN latch with a
    // retry: ATV_C is world content and may not be loaded yet when mainPlayer_C first
    // resolves. The first version rode the mainPlayer one-shot and failed SILENTLY (no
    // retry, not even a log line) whenever ATV_C lost that race -- the Oct 22 playtest's
    // dead ATV damage is consistent with exactly that.
    if (!g_atvInterceptorDone) {
        void* atvCls = ue_wrap::object_index::ClassByName(L"ATV_C");
        if (!atvCls) {
            if (!g_atvWaitLogged) {
                g_atvWaitLogged = true;
                UE_LOGI("player_damage: ATV_C not loaded yet -- ATV impact interceptor will retry");
            }
        } else {
            g_atvInterceptorDone = true;
            g_atvImpactFn = R::FindFunction(atvCls, L"impactDamageCPP");
            if (g_atvImpactFn && GT::RegisterInterceptor(g_atvImpactFn, &OnAtvImpactDamageCppPre)) {
                UE_LOGI("player_damage: ATV impactDamageCPP interceptor installed (%p) -- ATV hits relay to puppet owners",
                        g_atvImpactFn);
            } else {
                UE_LOGE("player_damage: ATV impactDamageCPP %s -- ATV hits won't relay damage",
                        g_atvImpactFn ? "did not register (the interceptor table is full)"
                                      : "is not declared on ATV_C (a recooked Blueprint)");
            }
        }
    }
}

void OnWireDamage(const coop::net::PlayerDamagePayload& p) {
    if (!ValidDamage(p.damage)) {
        UE_LOGW("player_damage: dropping wire damage with invalid amount=%.3f", p.damage);
        return;
    }
    const uint32_t targetEid = p.targetElementId;
    const float damage = p.damage;
    const float ix = p.impulseX, iy = p.impulseY, iz = p.impulseZ;
    const bool hasKnock = (ix != 0.f || iy != 0.f || iz != 0.f);
    GT::Post([targetEid, damage, ix, iy, iz, hasKnock] {
        auto& reg = coop::players::Registry::Get();
        const coop::element::ElementId localEid = reg.LocalPlayerElementId();
        // Defense-in-depth: PlayerDamage is sent point-to-point to our slot, so it IS
        // for us; the element check guards a future fan-out/relay-path bug. Apply on a
        // match OR when our id isn't allocated yet (boot window); drop ONLY on a
        // definite mismatch against a valid local id.
        if (localEid != coop::element::kInvalidId && targetEid != localEid) {
            UE_LOGW("player_damage: wire damage targetEid=%u != our localEid=%u -- dropping "
                    "(not addressed to us)", targetEid, localEid);
            return;
        }
        void* mp = reg.Local();
        if (!mp || !R::IsLive(mp)) {
            UE_LOGW("player_damage: wire damage but no live local player to apply to -- dropping");
            return;
        }
        const bool possessed = (E::GetController(mp) != nullptr);
        const bool isPup = reg.IsPuppet(mp);
        float before = -1.f, after = -1.f;
        ue_wrap::vitals::Read(ue_wrap::vitals::Field::Health, &before);
        // The native blood path: AddPlayerDamage's `blood` parameter gates the game's own
        // bloodLoss effect block (the effect_bloodLoss_C actor, its blood particles/decals and
        // the red wash), which a synthetic hit otherwise never produces. This lambda is the
        // single application point of the PlayerDamage lane (one wire packet runs it once), so
        // one packet arms exactly one effect: no new protocol kind, no second damage lane.
        // Guard: only arm it when the hit can actually land (the player was alive going in), so
        // a packet arriving for an already-dead player cannot bleed a corpse. The before/after
        // reads below are the audit that health really dropped; a read failure (-1) fails
        // closed, arming nothing. The puppet-side red hurt flash is untouched.
        const bool wasAlive = (before > 0.f);
        const bool ok = E::InvokeAddPlayerDamage(mp, damage, /*blood=*/wasAlive);
        ue_wrap::vitals::Read(ue_wrap::vitals::Field::Health, &after);
        UE_LOGI("player_damage: applied %.1f to own player (targetEid=%u) -> ok=%d, "
                "saveSlot.health %.2f -> %.2f blood=%d [mp possessed=%d isPuppet=%d]",
                damage, targetEid, ok ? 1 : 0, before, after, wasAlive ? 1 : 0,
                possessed ? 1 : 0, isPup ? 1 : 0);
        // Heavy-prop knock-over: launch the character along the relayed impulse. This is the
        // owner's REAL player (not a puppet), so the game's own Character movement handles it.
        if (hasKnock && wasAlive) {
            void* charClass = R::FindClass(L"Character");
            void* launchFn = charClass ? R::FindFunction(charClass, L"LaunchCharacter") : nullptr;
            if (launchFn) {
                ue_wrap::ParamFrame frame(launchFn);
                if (frame.valid()) {
                    // LaunchVelocity is an FVector at offset 0; set the three floats directly.
                    float vel[3] = { ix, iy, iz };
                    frame.SetRaw(L"LaunchVelocity", vel, sizeof(vel));
                    const bool xy = true, z = true;
                    frame.Set(L"bXYOverride", xy);
                    frame.Set(L"bZOverride", z);
                    if (ue_wrap::Call(mp, frame)) {
                        UE_LOGI("player_damage: knock-over LaunchCharacter(%.0f,%.0f,%.0f) on own player",
                                ix, iy, iz);
                    } else {
                        UE_LOGW("player_damage: LaunchCharacter call failed");
                    }
                }
            } else {
                UE_LOGW("player_damage: knock-over impulse (%.0f,%.0f,%.0f) but LaunchCharacter not found",
                        ix, iy, iz);
            }
        }
    });
}

void SendPlayerDamageWithImpulse(int ownerSlot, float damage, float ix, float iy, float iz) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) {
        UE_LOGW("player_damage: SendPlayerDamageWithImpulse but session is unset");
        return;
    }
    if (ownerSlot <= 0 || ownerSlot >= coop::players::kMaxPeers) {
        UE_LOGW("player_damage: SendPlayerDamageWithImpulse invalid ownerSlot=%d", ownerSlot);
        return;
    }
    if (!ValidDamage(damage)) {
        UE_LOGW("player_damage: SendPlayerDamageWithImpulse invalid amount=%.3f", damage);
        return;
    }
    auto& reg = coop::players::Registry::Get();
    coop::element::Player* el = reg.GetPlayerElement(static_cast<uint8_t>(ownerSlot));
    if (!el) {
        UE_LOGW("player_damage: SendPlayerDamageWithImpulse slot=%d has no Player Element yet -- skipping "
                "(peer not fully identified)", ownerSlot);
        return;
    }
    coop::net::PlayerDamagePayload p{};
    p.targetElementId = el->GetId();
    p.damage = damage;
    p.impulseX = ix;
    p.impulseY = iy;
    p.impulseZ = iz;
    const bool sent = s->SendReliableToSlot(
        ownerSlot, coop::net::ReliableKind::PlayerDamage, &p, sizeof(p), /*senderSlot=*/0);
    UE_LOGI("player_damage: sent PlayerDamage(%.1f, impulse=%.0f,%.0f,%.0f) to slot=%d (targetEid=%u) -> sent=%d",
            damage, ix, iy, iz, ownerSlot, p.targetElementId, sent ? 1 : 0);
}

void SendPlayerDamage(int ownerSlot, float damage) {
    SendPlayerDamageWithImpulse(ownerSlot, damage, 0.f, 0.f, 0.f);
}

bool DebugForceHitPuppet(int ownerSlot, float damage) {
    if (ownerSlot <= 0 || ownerSlot >= coop::players::kMaxPeers) {
        UE_LOGW("player_damage: DebugForceHitPuppet invalid ownerSlot=%d", ownerSlot);
        return false;
    }
    auto& reg = coop::players::Registry::Get();
    if (!reg.Puppet(static_cast<uint8_t>(ownerSlot))) {
        UE_LOGW("player_damage: DebugForceHitPuppet slot=%d has no puppet (peer not connected) "
                "-- cannot simulate a hit", ownerSlot);
        return false;
    }
    SendPlayerDamage(ownerSlot, damage);
    return true;
}

}  // namespace coop::player_damage
