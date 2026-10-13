// coop/interactables/atv_sync.cpp -- see coop/interactables/atv_sync.h. The ATV (AATV_C) rig sync.
// The mirror simulates: a peer that does not author an ATV runs the rig natively, physics and tick
// on, and is corrected toward the authority through velocity, never frozen or teleported. AATV_C is
// a five-body constraint rig whose visible output is suspension travel, and SetActorLocation moves
// the root only, so a kinematic apply dragged four constrained bodies behind one teleported root.
// The one thing a mirror may not do is author collision damage (the hit guard). MTA's shape (a hard
// velocity write per packet, a warp past a speed-scaled threshold, an elected syncer for an
// unoccupied vehicle that sends only on change) with one divergence: correction through velocity,
// since their vehicle is one rigid body. Two predicates: IsPoseAuthor (I drive or carry it) and
// OwnsTick (that, or I am the host and nobody authors it); the seat (occupantSlot, which the
// mount deny reads) and the author (authorSlot, who streams) stay separate, since a grabbing
// peer must not deny the seat. Keyed by (spawner, spawn index): "save#<key>" for save-placed
// ATVs ("save#<key>#<n>" when two share one Key string, ordered by spawn site), "menu#<n>" for
// the host's spawn-menu spawner, "menu@<slot>#<n>" for a client's (see atv_spawner_keys.h). The
// seat is self-elected, and two peers mounting inside one round-trip both elect themselves:
// the lower slot wins in OnReliable, a tie-break, not authority.

#include "coop/interactables/atv_sync.h"
#include "coop/interactables/atv_corrector.h"
#include "coop/dev/atv_eject_drill.h"
#include "coop/interactables/atv_condition_sync.h"
#include "coop/interactables/atv_hit_guard.h"
#include "coop/interactables/atv_spawner_keys.h"
#include "coop/interactables/atv_sync_internal.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString / StringFromWireKey / FnvKey (shared)
#include "coop/player/players_registry.h"   // Registry::Local / LocalPeerId / kMaxPeers
#include "coop/player/roster_ledger.h"      // SubscribeSlotReplaced: a departed author must not hold an ATV

#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"          // ReadMainPlayerGrabState (grabber authority) + Get/SetActorRootPhysicsVelocity
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"     // the generation-stamped index
#include "coop/element/object_scan_hub.h"      // the shared sliced scan pass
#include "ue_wrap/core/types.h"           // FVector, FRotator, NormalizeAxis

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace coop::atv_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace A = ue_wrap::atv;
namespace SK = coop::atv_spawner_keys;
using ue_wrap::FVector;
using ue_wrap::FRotator;
using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using coop::net::FnvKey;

constexpr uint64_t kDriveSendMs = 50;   // about 20 Hz while a peer authors it
constexpr uint64_t kIdleSendMs  = 200;  // the idle syncer's ceiling, 5 Hz, and only when it changed
// Never nothing: the change gate asks whether the sender's copy moved and is blind to the
// receiver, whose mirror can roll while the host's copy is parked, and a dropped connect snapshot
// would otherwise be permanent for an idle ATV. The gate lowers the rate; this floor keeps it
// non-zero.
constexpr uint64_t kIdleKeepaliveMs = 2000;

// The idle syncer's change gate (MTA's WriteVehicleInformation in our units): a parked ATV sends
// nothing.
constexpr float kIdleMovedCm   = 1.0f;
constexpr float kIdleTurnedDeg = 0.5f;
constexpr float kIdleMovingCmS = 1.0f;


// Per-ATV state; a mirror is a simulating body whose velocity is biased at packet arrival, with
// no interpolation state. Game thread only.

std::atomic<coop::net::Session*> g_session{nullptr};

// g_atvs is game-thread only: Install, Tick, OnReliable, the connect snapshot and OnDisconnect run
// serially inside the pump. Tick mutates entry fields only; inserts and erases happen in the hub
// pass (scan_hub::Tick runs before this Tick) and in OnReliable (the drain, before the ticks).
std::unordered_map<std::wstring, AtvEntry> g_atvs;
size_t   g_lastLogCount = SIZE_MAX;
uint64_t g_lastLogHash  = 0;
bool     g_installed    = false;  // latches the one-time index; Install is the per-tick ensure path

// Per-spawner mirror identity (see coop/interactables/atv_spawner_keys.h). Every ATV is born
// from one spawner -- "save" (the world/save loader), "menu" (the host's spawn-menu spawner),
// "menu@<slot>" (a client's spawn-menu spawner) -- and the g_atvs key is the composite
// "<spawner>#<local id>", so two peers looking at ATVs from different spawners agree on which
// ATV is which. A save-placed ATV keeps its deterministic Key inside the save spawner's local
// id ("save#<key>"); two ATVs sharing one Key string get ordinals by spawn-site order
// ("save#<key>#<n>"). Nothing sells an ATV, but list_props row 'atv' spawns an ATV_C from the
// spawn menu at runtime; such an ATV has no save twin on the other peers and its own save key
// is minted randomly per peer, so the host mints "menu#N" and announces it, and clients
// fresh-spawn a native AATV_C under that key.
std::unordered_set<std::wstring>        g_savePlacedKeys;    // real keys known save-placed (host: seen before any client connected; client: seen before the connect snapshot)
std::unordered_set<void*>               g_savePlacedActors;  // host: the actors present before any client, so a save ATV that mints its key late is not misread as a runtime spawn
std::unordered_map<void*, std::wstring> g_keyForActor;       // actor -> composite key (menu-spawned ATVs: host runtime + client mirrors)
uint32_t                                g_menuCounter = 0;   // host: the monotonic menu-spawner id
uint32_t                                g_clientMenuCounter = 0;  // client: the local menu@slot spawner id
bool                                    g_clientSawSnapshot = false;  // client: the host's connect snapshot arrived; ATVs appearing later are menu spawns

// Save spawner: realKey -> sorted spawn sites. The duplicate-Key ordinal source; the sorted
// insert is order-independent, which is what makes the ordinals peer-deterministic (both peers
// loaded the same save bytes, so both compute identical sites).
std::map<std::wstring, std::vector<SK::SpawnSite>> g_saveSites;

// The per-spawner mirror table: spawner key -> composite keys of its live ATV mirrors. The
// mirror state itself (actor/transform, occupantSlot, authorSlot) lives in the g_atvs entries;
// rebuilt after every structural change.
std::unordered_map<std::wstring, std::vector<std::wstring>> g_bySpawner;

void RebuildSpawnerIndex() {
    g_bySpawner.clear();
    for (const auto& kv : g_atvs) {
        if (!kv.second.spawnerKey.empty())
            g_bySpawner[kv.second.spawnerKey].push_back(kv.first);
    }
}

// Client-side snapshot adoption: host composite keys seen in AtvState but not yet bound to a
// local actor. The snapshot can beat the hub pass, and the host's duplicate-key ordinals can
// differ from the local rule's (anchors are captured at world load on the host but at join time
// on the client, so mid-session movement can flip the site order). Retried on every hub pass;
// the bind is by realKey + proximity, never by ordinal.
struct PendingAdopt {
    std::wstring compositeKey;
    std::wstring realKey;
    FVector      pos;  // the snapshot pose: the local twin is the same rig, so it is close
};
std::vector<PendingAdopt> g_pendingAdopts;
constexpr size_t kMaxPendingAdopts = 64;
constexpr float  kAdoptBindCm = 500.0f;  // a snapshot binds the local ATV within this distance

// The g_atvs entry for this actor, or g_atvs.end(). N is a handful of ATVs; a linear scan is fine.
std::unordered_map<std::wstring, AtvEntry>::iterator FindByActor(void* obj) {
    for (auto it = g_atvs.begin(); it != g_atvs.end(); ++it)
        if (it->second.actor == obj) return it;
    return g_atvs.end();
}

float Dist2(const FVector& a, float x, float y, float z) {
    const float dx = a.X - x, dy = a.Y - y, dz = a.Z - z;
    return dx * dx + dy * dy + dz * dz;
}

// Bind a host composite key to a local actor (client snapshot adoption). The local entry is
// re-keyed to the host's key with all its fields preserved; the entry is marked adopted so the
// hub pass does not re-key it back to the local rule's ordinal. True on bind.
bool AdoptKeyToActor(const std::wstring& compositeKey, const std::wstring& realKey, void* actor,
                     float distCm) {
    auto it = FindByActor(actor);
    if (it == g_atvs.end()) return false;
    if (it->first == compositeKey) return true;  // already there
    UE_LOGI("atv: mirror adopted -- host key='%ls' bound to local actor=%p (realKey='%ls', "
            "dist=%.0f cm; was '%ls')",
            compositeKey.c_str(), actor, realKey.c_str(), static_cast<double>(distCm),
            it->first.c_str());
    AtvEntry e = std::move(it->second);
    const auto kf = g_keyForActor.find(actor);
    g_atvs.erase(it);
    e.keyAdopted = true;
    e.spawnerKey = SK::kSaveSpawner;
    e.realKey = realKey;
    g_atvs[compositeKey] = std::move(e);
    if (kf != g_keyForActor.end()) kf->second = compositeKey;
    RebuildSpawnerIndex();
    return true;
}

void StashPendingAdopt(const std::wstring& compositeKey, const std::wstring& realKey,
                       const FVector& pos) {
    for (const auto& p : g_pendingAdopts)
        if (p.compositeKey == compositeKey) return;  // already waiting
    if (g_pendingAdopts.size() >= kMaxPendingAdopts)
        g_pendingAdopts.erase(g_pendingAdopts.begin());  // FIFO: the oldest never bound
    g_pendingAdopts.push_back({ compositeKey, realKey, pos });
}

// Try to bind one pending host key to a freshly indexed local actor.
void DrainPendingAdopts() {
    if (g_pendingAdopts.empty()) return;
    for (auto pi = g_pendingAdopts.begin(); pi != g_pendingAdopts.end();) {
        void* bindActor = nullptr;
        float bindDist = 0.f;
        for (auto& kv : g_atvs) {
            AtvEntry& e = kv.second;
            if (e.spawnerKey != SK::kSaveSpawner || e.realKey != pi->realKey) continue;
            if (e.keyAdopted) continue;  // already bound to a (possibly different) host key
            if (!R::IsLiveByIndex(e.actor, e.idx)) continue;
            FVector loc; FRotator rot;
            if (!A::GetRootTransform(e.actor, loc, rot)) continue;
            const float d2 = Dist2(loc, pi->pos.X, pi->pos.Y, pi->pos.Z);
            if (d2 <= kAdoptBindCm * kAdoptBindCm) { bindActor = e.actor; bindDist = std::sqrt(d2); break; }
        }
        const bool bound = bindActor != nullptr &&
                           AdoptKeyToActor(pi->compositeKey, pi->realKey, bindActor, bindDist);
        if (bound) pi = g_pendingAdopts.erase(pi);
        else ++pi;
    }
}

// A WireClassName from a wide class name (ASCII), truncated at 63.
void FillWireClassName(coop::net::WireClassName& out, const std::wstring& name) {
    out.len = 0;
    for (size_t i = 0; i < name.size() && i < sizeof(out.data); ++i)
        out.data[out.len++] = static_cast<char>(name[i]);
}
std::wstring WireClassNameToString(const coop::net::WireClassName& in) {
    std::wstring s;
    const uint8_t n = in.len <= sizeof(in.data) ? in.len : static_cast<uint8_t>(sizeof(in.data));
    s.reserve(n);
    for (uint8_t i = 0; i < n; ++i) s.push_back(static_cast<wchar_t>(static_cast<unsigned char>(in.data[i])));
    return s;
}

// Announces a runtime ATV so the other side fresh-spawns a native mirror; slot < 0 broadcasts (a
// new ATV mid-session), slot >= 0 seeds one joiner. Reads the class and the current pose. The
// host uses it for its menu spawner; a client uses it for the host-directed announce of its own
// menu@slot spawn (a client has only the host connection, so slot < 0 reaches the host).
void SendAtvSpawn(const std::wstring& menuKey, void* actor, int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !actor) return;
    void* cls = R::ClassOf(actor);
    if (!cls) return;
    FVector loc; FRotator rot;
    if (!A::GetRootTransform(actor, loc, rot)) return;
    coop::net::AtvSpawnPayload p{};
    WireKeyFromString(menuKey, p.synthKey);
    FillWireClassName(p.className, R::ToString(R::NameOf(cls)));
    p.x = loc.X; p.y = loc.Y; p.z = loc.Z;
    p.pitch = rot.Pitch; p.yaw = rot.Yaw; p.roll = rot.Roll;
    if (slot < 0) s->SendReliable(coop::net::ReliableKind::AtvSpawn, &p, sizeof(p));
    else          s->SendReliableToSlot(slot, coop::net::ReliableKind::AtvSpawn, &p, sizeof(p));
}

void SendAtvDestroy(const std::wstring& menuKey) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    coop::net::AtvDestroyPayload p{};
    WireKeyFromString(menuKey, p.synthKey);
    s->SendReliable(coop::net::ReliableKind::AtvDestroy, &p, sizeof(p));
}


// True if this peer's player is seated in `actor`, per the local engine.
bool IsLocalOccupant(void* actor, void* localPlayer) {
    return localPlayer && A::IsDriven(actor) && A::GetOccupantPlayer(actor) == localPlayer;
}

// True if this peer can claim or holds driver authority; a seat another peer holds cannot be
// claimed (the double-mount race).
bool CanClaimOrIsDriver(void* actor, void* localPlayer, uint8_t occupantSlot, uint8_t localSlot) {
    if (!IsLocalOccupant(actor, localPlayer)) return false;
    return occupantSlot == 0xFF || occupantSlot == localSlot;
}

// True if this peer's player is grabbing `actor` with the grav hand (carried, not seated). The
// ATV has no grabbed flag of its own (isDriven and Player are written only on the seating path),
// so the grabber identity is the player's grabbing_actor or holding_actor, both accepted.
bool IsLocalGrabber(void* actor, void* localPlayer) {
    if (!localPlayer || !actor) return false;
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!ue_wrap::engine::ReadMainPlayerGrabState(localPlayer, gs)) return false;
    return gs.grabbingActor == actor || gs.holdingActor == actor;
}

// Pose authority: this peer streams `actor`, driving or carrying it. Not tick ownership.
bool IsPoseAuthor(void* actor, void* localPlayer, uint8_t occupantSlot, uint8_t localSlot) {
    return CanClaimOrIsDriver(actor, localPlayer, occupantSlot, localSlot) ||
           IsLocalGrabber(actor, localPlayer);
}

// Tick ownership: the peer that authors the ATV, else the host (MTA's unoccupied-vehicle
// election). A different set from pose authority: on the host with nobody driving, IsPoseAuthor
// is false and this is true. Its two jobs are electing the idle syncer and feeding the collision
// guard's owned set; there is no brain-off (parking the tick was measured useless, since
// SetCenterOfMass runs unconditionally per tick, and the wheel torque and battery terms are
// already single-peer by the game's own isDriven gates).
bool OwnsTickFor(bool isPoseAuthor, bool isHost, uint8_t authorSlot) {
    return isPoseAuthor || (isHost && authorSlot == 0xFF);
}

// An AtvStatePayload from a live read; false if the transform read fails. `grabbed` marks the
// authority as the grav-hand grabber (bit 2); `authorSlot` names who holds it (0xFF = nobody,
// which elects the host as its idle syncer).
bool ReadPayload(void* actor, const std::wstring& key, uint8_t occupantSlot, uint8_t authorSlot,
                 bool adopt, coop::net::AtvStatePayload& p, bool grabbed = false) {
    FVector loc; FRotator rot;
    if (!A::GetRootTransform(actor, loc, rot)) return false;
    FVector lin{}, ang{};
    // Best effort: a failed read leaves zeros, the honest value and also a body at rest.
    ue_wrap::engine::GetActorRootPhysicsVelocity(actor, lin, ang);
    std::memset(&p, 0, sizeof(p));
    WireKeyFromString(key, p.key);
    p.x = loc.X; p.y = loc.Y; p.z = loc.Z;
    p.pitch = rot.Pitch; p.yaw = rot.Yaw; p.roll = rot.Roll;
    p.linVelX = lin.X; p.linVelY = lin.Y; p.linVelZ = lin.Z;
    p.angVelX = ang.X; p.angVelY = ang.Y; p.angVelZ = ang.Z;
    p.occupantSlot = occupantSlot;
    p.authorSlot   = authorSlot;
    uint8_t sb = 0;
    if (A::IsDriven(actor)) sb |= 0x1;
    if (A::GetBrake(actor)) sb |= 0x2;
    if (grabbed)            sb |= 0x4;
    p.stateBits = sb;
    p.adopt = adopt ? 1 : 0;
    // The condition block (tires, spare, dirt, fuel, health), filled at this one site so the
    // authority, the idle syncer and the adopt seed cannot fork; a failed read leaves it zeroed
    // with tiresValid 0, and receivers touch nothing.
    coop::atv_condition_sync::FillPayload(actor, p);
    return true;
}




// The idle syncer's change gate: true if the ATV moved, turned or is moving; the baseline updates
// on a yes, so a slow drift accumulates into a send instead of being rounded away.
bool IdleWorthSending(AtvEntry& e) {
    FVector loc; FRotator rot;
    if (!A::GetRootTransform(e.actor, loc, rot)) return false;
    FVector lin{}, ang{};
    ue_wrap::engine::GetActorRootPhysicsVelocity(e.actor, lin, ang);
    bool send = !e.haveLastSync || Len(lin) > kIdleMovingCmS;
    if (!send) {
        const FVector d{ loc.X - e.lastSyncPos.X, loc.Y - e.lastSyncPos.Y, loc.Z - e.lastSyncPos.Z };
        send = Len(d) > kIdleMovedCm ||
               std::fabs(ue_wrap::NormalizeAxis(rot.Pitch - e.lastSyncRot.Pitch)) > kIdleTurnedDeg ||
               std::fabs(ue_wrap::NormalizeAxis(rot.Yaw   - e.lastSyncRot.Yaw))   > kIdleTurnedDeg ||
               std::fabs(ue_wrap::NormalizeAxis(rot.Roll  - e.lastSyncRot.Roll))  > kIdleTurnedDeg;
    }
    if (send) { e.lastSyncPos = loc; e.lastSyncRot = rot; e.haveLastSync = true; }
    return send;
}


// The scan-hub consumer: the hub's shared sliced pass drives these callbacks, preserving the
// sender state for keys that persist (only the actor and index update) and classifying identity
// by spawner (save-placed ATVs keep their Key inside the save spawner's local id; a host-side
// menu-spawned ATV gets a menu key and an announce). A spawn landing inside the pass window at
// a join is announced on the next pass, when the joiner is connected.
uint32_t g_indexGen = 0;  // world gen of the last completed pass (stale-gen index = EMPTY)
bool IndexCurrent() { return g_indexGen == ue_wrap::world_identity::Generation(); }
struct ScanFound {
    void*        obj;
    int32_t      idx;
    std::wstring spawnerKey;    // "save" | "menu" | "menu@<slot>"
    std::wstring realKey;       // save spawner: the game's Key (the identity WITHIN the spawner)
    SK::SpawnSite site;         // save spawner: first-sighting site (reused from the entry when known)
    bool         siteValid = false;
    uint32_t     ordinal = 0;   // save spawner: duplicate-Key disambiguation (assigned in Complete)
    std::wstring compositeKey;  // menu spawners: final here; save spawner: assigned in Complete
    bool         isNewSpawn = false;  // first classification this session (the spawn log line)
};
std::vector<ScanFound> g_scanFound;   // pass scratch (GT-only)
bool g_scanIsHost = false;            // pass context, captured at pass begin
bool g_scanCapturing = false;

void HubPassBegin(void*, bool) {
    g_scanFound.clear();
    auto* s = g_session.load(std::memory_order_acquire);
    g_scanIsHost = s && s->role() == coop::net::Role::Host;
    const bool isHost = g_scanIsHost;
    // The baseline window: before any client is connected, every keyed ATV the host has is
    // save-placed; after that a newly appearing key is a runtime spawn. Accumulated, not a
    // single-frame latch, so a save ATV slow to mint its key still lands in the set.
    g_scanCapturing = isHost && (!s || !s->connected());
}

void HubMatch(void*, void* obj) {
    const bool isHost = g_scanIsHost;
    const bool capturing = g_scanCapturing;
    auto& found = g_scanFound;
    {
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) return;  // skip CDO
        if (!R::IsLive(obj)) return;
        if (capturing) g_savePlacedActors.insert(obj);  // capture the ACTOR (even before it mints its key)
        // A menu-spawned ATV the hub has already keyed keeps its composite key (a host runtime
        // spawn, or this client's mirror of one announced via AtvSpawn).
        const auto kf = g_keyForActor.find(obj);
        if (kf != g_keyForActor.end()) {
            std::wstring spawner, local;
            SK::SplitCompositeKey(kf->second, spawner, local);
            found.push_back(ScanFound{ obj, R::InternalIndexOf(obj), std::move(spawner),
                                       std::wstring(), SK::SpawnSite(), false, 0, kf->second,
                                       false });
            return;
        }
        // An already-indexed ATV keeps its spawner and its first-sighting site; the save
        // spawner's composite is recomputed in Complete from the stable site, and an adopted
        // entry keeps the host's key.
        const auto ei = FindByActor(obj);
        std::wstring spawnerKey;
        std::wstring realKey;
        SK::SpawnSite site;
        bool siteValid = false;
        bool isNew = false;
        if (ei != g_atvs.end() && !ei->second.spawnerKey.empty()) {
            spawnerKey = ei->second.spawnerKey;
            realKey    = ei->second.realKey;
            site       = ei->second.spawnSite;
            siteValid  = ei->second.siteValid;
        } else {
            realKey = A::GetKeyString(obj);
            if (realKey.empty() || realKey == L"None") return;  // not yet keyed -- the next pass picks it up
            isNew = true;
            if (capturing) {
                g_savePlacedKeys.insert(realKey);              // baseline: a save-placed ATV
                spawnerKey = SK::kSaveSpawner;
            } else if (isHost && g_savePlacedKeys.find(realKey) == g_savePlacedKeys.end() &&
                       g_savePlacedActors.find(obj) == g_savePlacedActors.end()) {
                // Host: a mid-session ATV not in the save set is a spawn-menu spawn: a
                // menu-spawner key and an announce, so the clients fresh-spawn a mirror.
                spawnerKey = SK::kHostMenuSpawner;
                const std::wstring compositeKey =
                    SK::BuildCompositeKey(spawnerKey, std::to_wstring(++g_menuCounter));
                g_keyForActor[obj] = compositeKey;
                SendAtvSpawn(compositeKey, obj, /*slot*/ -1);  // broadcast to all connected clients
                UE_LOGI("atv: spawner keyed -- key='%ls' spawner='menu' class='%ls' (host-announced AtvSpawn)",
                        compositeKey.c_str(), R::ToString(R::NameOf(R::ClassOf(obj))).c_str());
                found.push_back(ScanFound{ obj, R::InternalIndexOf(obj), std::move(spawnerKey),
                                           std::wstring(), SK::SpawnSite(), false, 0,
                                           compositeKey, true });
                return;
            } else if (!isHost && g_clientSawSnapshot &&
                       g_savePlacedKeys.find(realKey) == g_savePlacedKeys.end()) {
                // Client: an ATV appearing after the connect snapshot did not come from the
                // transferred save, so it is this client's own spawn-menu spawn. It gets a
                // stable menu@slot identity, streams under it, and announces the spawn to the
                // host (AtvSpawn, client-originated); the host mirrors it and re-announces it
                // so the other clients spawn mirrors too.
                const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
                spawnerKey = SK::ClientMenuSpawnerForSlot(localSlot);
                const std::wstring compositeKey =
                    SK::BuildCompositeKey(spawnerKey, std::to_wstring(++g_clientMenuCounter));
                g_keyForActor[obj] = compositeKey;
                UE_LOGI("atv: spawner keyed -- key='%ls' spawner='%ls' class='%ls' (client menu spawn; announcing to host)",
                        compositeKey.c_str(), spawnerKey.c_str(),
                        R::ToString(R::NameOf(R::ClassOf(obj))).c_str());
                SendAtvSpawn(compositeKey, obj, /*slot*/ -1);  // client -> host (only connection)
                found.push_back(ScanFound{ obj, R::InternalIndexOf(obj), std::move(spawnerKey),
                                           std::wstring(), SK::SpawnSite(), false, 0,
                                           compositeKey, true });
                return;
            } else {
                // Save-placed: the host's baseline window, or the client's pre-snapshot world
                // (everything the transferred save materialised, keyed or still keyless).
                if (!isHost && !g_clientSawSnapshot) g_savePlacedKeys.insert(realKey);
                spawnerKey = SK::kSaveSpawner;
            }
            if (spawnerKey == SK::kSaveSpawner) {
                FVector loc; FRotator rot;
                if (A::GetRootTransform(obj, loc, rot)) {
                    site = SK::QuantizeSite(loc);  // the first-sighting site; never moves after
                    siteValid = true;
                }
            }
            if (isNew && spawnerKey == SK::kSaveSpawner)
                UE_LOGI("atv: spawner keyed -- realKey='%ls' spawner='save' actor=%p site=(%d,%d,%d)m",
                        realKey.c_str(), obj, site.x, site.y, site.z);
        }
        found.push_back(ScanFound{ obj, R::InternalIndexOf(obj), std::move(spawnerKey),
                                   std::move(realKey), site, siteValid, 0, std::wstring(), isNew });
    }
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    const bool isHost = g_scanIsHost;
    auto& found = g_scanFound;
    // Save-spawner ordinals. The site insert is order-independent, so a tail pass (which sees
    // only new arrivals) assigns the same ordinals as a full pass. An adopted entry keeps the
    // host's key instead of the local rule's; a site-less ATV (its transform was unreadable at
    // first sighting) falls back to the bare save key rather than dropping out of the index.
    for (auto& f : found) {
        if (f.spawnerKey != SK::kSaveSpawner) continue;
        if (!f.compositeKey.empty()) continue;  // menu path or an already-final key
        const auto ei = FindByActor(f.obj);
        if (ei != g_atvs.end() && ei->second.keyAdopted) {
            f.compositeKey = ei->first;  // the host's key stands; the local ordinal is not recomputed
            f.ordinal = ei->second.spawnOrdinal;
            continue;
        }
        if (!f.siteValid) {
            if (f.isNewSpawn)
                UE_LOGW("atv: no spawn site for realKey='%ls' actor=%p -- ordinal defaults to 0",
                        f.realKey.c_str(), f.obj);
            f.compositeKey = SK::BuildCompositeKey(SK::kSaveSpawner, f.realKey);
            continue;
        }
        f.ordinal = SK::InsertSiteOrdinal(g_saveSites[f.realKey], f.site);
        const std::wstring local = f.ordinal == 0
            ? f.realKey
            : f.realKey + L"#" + std::to_wstring(f.ordinal);
        if (4 + 1 + local.size() > 31)  // "save#" is 5 chars; BuildCompositeKey falls back past 31
            UE_LOGW("atv: composite key for save key '%ls' exceeded 31 chars -- deterministic FNV tag used",
                    f.realKey.c_str());
        f.compositeKey = SK::BuildCompositeKey(SK::kSaveSpawner, local);
    }
    // Defensive: a found ATV must never carry an empty key into the prune/add loops (an empty
    // key would prune a live entry on a full pass, or create an unkeyed one).
    for (auto& f : found) {
        if (!f.compositeKey.empty()) continue;
        const auto ei = FindByActor(f.obj);
        if (ei != g_atvs.end()) f.compositeKey = ei->first;
        else UE_LOGW("atv: found ATV actor=%p has no composite key -- skipping this pass", f.obj);
    }
    // Entries whose ATV vanished are dropped; a gone host menu-spawned ATV sends AtvDestroy so
    // the clients tear down their mirror. On a full pass `found` is authoritative; on a tail pass
    // it is only the new tail, so liveness by index prunes instead.
    for (auto it = g_atvs.begin(); it != g_atvs.end();) {
        bool keep;
        if (isFull) {
            keep = false;
            for (auto& f : found) if (!f.compositeKey.empty() && f.compositeKey == it->first) { keep = true; break; }
        } else {
            keep = R::IsLiveByIndex(it->second.actor, it->second.idx);
        }
        if (keep) { ++it; continue; }
        UE_LOGI("atv: ATV destroyed -- key='%ls' spawner='%ls' (entry dropped)",
                it->first.c_str(), it->second.spawnerKey.c_str());
        if (SK::IsMenuSpawnerKey(it->first)) {
            // Menu spawn teardown: the host tells the clients (its own spawner); a client tells
            // the host, but only for its OWN menu@slot spawner (the host authors its menu
            // spawner, and the host re-announces an adopted client spawn to the other clients).
            // Either way the actor->key map must not outlive the actor: a reused actor pointer
            // would otherwise inherit a dead composite key on the next pass.
            if (isHost && SK::IsHostMenuKey(it->first)) {
                SendAtvDestroy(it->first);
            } else if (!isHost) {
                uint8_t claimedSlot = 0xFF;
                if (SK::ParseClientMenuKey(it->first, claimedSlot) &&
                    claimedSlot == coop::players::Registry::Get().LocalPeerId())
                    SendAtvDestroy(it->first);  // client -> host (only connection)
            }
            if (it->second.actor) g_keyForActor.erase(it->second.actor);
        }
        it = g_atvs.erase(it);
    }
    // Update or add. A composite key can outlive its actor (a save-placed key is deterministic
    // and a client join runs two level loads), and the successor must not inherit the dead
    // actor's authority, seat, correction clock and change-gate baseline: the entry stays,
    // everything that described the actor resets. A duplicate-Key ordinal shift re-keys the
    // entry instead: the ACTOR is the same, so the seat, the author and the clocks move with it.
    for (auto& f : found) {
        if (f.compositeKey.empty()) continue;  // warned above; nothing to index it under
        auto it = g_atvs.find(f.compositeKey);
        if (it == g_atvs.end()) {
            const auto byActor = FindByActor(f.obj);
            if (byActor != g_atvs.end()) {
                UE_LOGI("atv: ATV re-keyed -- '%ls' -> '%ls' spawner='%ls' (duplicate-Key ordinal shift; seat/author preserved)",
                        byActor->first.c_str(), f.compositeKey.c_str(), f.spawnerKey.c_str());
                AtvEntry e = std::move(byActor->second);
                const auto kf = g_keyForActor.find(f.obj);
                g_atvs.erase(byActor);
                g_atvs[f.compositeKey] = std::move(e);
                if (kf != g_keyForActor.end()) kf->second = f.compositeKey;
                it = g_atvs.find(f.compositeKey);
            } else {
                AtvEntry e{};
                e.actor = f.obj;
                e.idx = f.idx;
                e.spawnerKey = f.spawnerKey;
                e.realKey = f.realKey;
                e.spawnOrdinal = f.ordinal;
                e.spawnSite = f.site;
                e.siteValid = f.siteValid;
                g_atvs[f.compositeKey] = std::move(e);
                UE_LOGI("atv: ATV spawned -- key='%ls' spawner='%ls' actor=%p class='%ls'",
                        f.compositeKey.c_str(), f.spawnerKey.c_str(), f.obj,
                        R::ToString(R::NameOf(R::ClassOf(f.obj))).c_str());
                it = g_atvs.find(f.compositeKey);
            }
        }
        AtvEntry& e = it->second;
        if (e.actor && e.actor != f.obj) {
            e.occupantSlot = 0xFF; e.authorSlot = 0xFF;
            e.wasPoseAuthor = false;
            e.haveLastSync = false; e.lastIdleSendMs = 0; e.lastPktMs = 0;
            e.lastErrCm = -1.f; e.stallPackets = 0; e.restReplaces = 0; e.lastRestPlaceMs = 0;
        }
        e.actor = f.obj;
        e.idx   = f.idx;
    }
    DrainPendingAdopts();  // a snapshot that beat the hub pass binds now
    RebuildSpawnerIndex();
    g_indexGen = worldGen;
    // The keys hash over the whole index, since on a tail pass `found` is only the new arrivals.
    uint64_t keysHash = 0;
    for (auto& kv : g_atvs) keysHash ^= FnvKey(kv.first);
    if (g_atvs.size() != g_lastLogCount || keysHash != g_lastLogHash) {
        g_lastLogCount = g_atvs.size();
        g_lastLogHash  = keysHash;
        UE_LOGI("atv: index rebuilt -- %zu live ATV(s), keysHash=0x%016llX (%s pass, +%zu new) "
                "(compare host vs client for cross-peer Key stability)",
                g_atvs.size(), static_cast<unsigned long long>(keysHash),
                isFull ? "full" : "tail", found.size());
        for (const auto& skv : g_bySpawner)
            UE_LOGI("atv: spawner '%ls' holds %zu mirror(s)",
                    skv.first.c_str(), skv.second.size());
    }
    g_scanFound.clear();
    return g_atvs.size();
}

// A peer whose seat or authorship we hold has left or been replaced (slots recycle lowest-free,
// so a slot can change occupant with no absence between, which is why this hangs off the
// ledger's row transition). Every ATV it held is released: an authorSlot stuck on a departed peer
// means nobody ever syncs that ATV again.
void OnSlotReplaced(int slot, const coop::roster_ledger::Row& /*outgoing*/,
                    const coop::roster_ledger::Row& /*incoming*/) {
    if (slot < 0 || slot > 0xFE) return;
    const uint8_t s8 = static_cast<uint8_t>(slot);
    int freed = 0;
    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        if (e.occupantSlot == s8) { e.occupantSlot = 0xFF; ++freed; }
        if (e.authorSlot   == s8) { e.authorSlot   = 0xFF; ++freed; }
        // The change gate re-arms: the other peers still hold the departed claim, and healing them
        // depends on the host sending something.
        e.haveLastSync = false;
    }
    if (freed > 0)
        UE_LOGI("atv: slot %d departed -- freed %d ATV seat/author reservation(s)", slot, freed);
}

void SubscribeDepartures() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::roster_ledger::SubscribeSlotReplaced(&OnSlotReplaced);
}

void RegisterWithScanHub() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "atv", nullptr, &A::EnsureResolved, &A::IsAtv,
        &HubPassBegin, &HubMatch, &HubPassComplete, /*settleScans*/ 15});
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // Install is the per-tick ensure path (net_pump re-calls it until the class loads), latched so
    // the one-time registration happens once.
    if (!g_installed && A::EnsureResolved()) {
        RegisterWithScanHub();  // the hub builds the index on its own cadence
        // The condition layout is warmed here, with ATV_C resident, so its one-shot resolve (thirteen
        // offsets and four function lookups) lands at session setup rather than in the first apply frame.
        ue_wrap::atv_condition::Resolve();
        coop::atv_hit_guard::InstallHitGuard();      // the seven ComponentHit interceptors -- Tick refuses to run without them
        SubscribeDepartures();  // a departed author must not hold an ATV hostage
        g_installed = true;
    }
}

// A peer may name itself as an ATV's holder, never anyone else: authorSlot elects the idle syncer
// and every other peer's mount deny reads it, so an unattributed one is an authority assertion,
// and one packet a second from any client would pin an empty ATV as occupied for everyone. Client
// scoped: the host legitimately speaks for other peers in its snapshot and its relay, and the
// relay preserves the origin slot.
bool SenderMaySpeakFor(uint8_t senderSlot, uint8_t claimedSlot) {
    if (senderSlot == 0 || senderSlot == 0xFF) return true;  // host, or unattributed local path
    return claimedSlot == 0xFF || claimedSlot == senderSlot;
}

void OnReliable(const coop::net::AtvStatePayload& payload, uint8_t senderPeerSlot) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) { UE_LOGW("atv: OnReliable empty key -- dropping"); return; }
    if (!A::EnsureResolved()) return;
    if (!IndexCurrent()) return;  // a stale-generation index holds another world's ATVs; the stream re-sends
    if (payload.adopt != 0) {
        g_clientSawSnapshot = true;  // the host's connect snapshot is flowing
        // Seed the save-placed set from the snapshot's save-spawner keys, so a save ATV that
        // mints its Key after the snapshot still classifies save-placed, not a menu spawn.
        std::wstring spawner, local;
        if (SK::SplitCompositeKey(key, spawner, local) && spawner == SK::kSaveSpawner)
            g_savePlacedKeys.insert(SK::SaveLocalRealKey(local));
    }
    // A non-finite guard before the engine writes.
    if (!std::isfinite(payload.x) || !std::isfinite(payload.y) || !std::isfinite(payload.z) ||
        !std::isfinite(payload.pitch) || !std::isfinite(payload.yaw) || !std::isfinite(payload.roll)) {
        UE_LOGW("atv: OnReliable non-finite pose -- dropping key='%ls'", key.c_str());
        return;
    }
    auto it = g_atvs.find(key);
    if (it == g_atvs.end()) {
        // Snapshot adoption: the host's composite key for a save ATV may not match the local
        // rule's. Duplicate-Key ordinals are spawn-site ordered, and the client captures sites
        // at join time while the host captured them at world load, so mid-session movement can
        // flip the order. Bind by realKey + proximity instead of dropping the stream; when no
        // local actor is close yet the key waits for the hub pass (DrainPendingAdopts).
        bool bound = false;
        std::wstring spawner, local;
        if (SK::SplitCompositeKey(key, spawner, local) && spawner == SK::kSaveSpawner) {
            const std::wstring realKey = SK::SaveLocalRealKey(local);
            const FVector saw{ payload.x, payload.y, payload.z };
            void* bindActor = nullptr;
            float bindDist = 0.f;
            for (auto& kv : g_atvs) {
                AtvEntry& c = kv.second;
                if (c.spawnerKey != SK::kSaveSpawner || c.realKey != realKey) continue;
                if (c.keyAdopted) continue;  // already bound to a (different) host key
                if (!R::IsLiveByIndex(c.actor, c.idx)) continue;
                FVector loc; FRotator rot;
                if (!A::GetRootTransform(c.actor, loc, rot)) continue;
                const float d = std::sqrt(Dist2(loc, saw.X, saw.Y, saw.Z));
                if (d <= kAdoptBindCm) { bindActor = c.actor; bindDist = d; break; }
            }
            if (bindActor) bound = AdoptKeyToActor(key, realKey, bindActor, bindDist);
            else StashPendingAdopt(key, realKey, saw);
        }
        if (!bound) return;  // not indexed yet -- the throttled rebuild will pick it up
        it = g_atvs.find(key);
        if (it == g_atvs.end()) return;
    }
    AtvEntry& e = it->second;
    if (!R::IsLiveByIndex(e.actor, e.idx)) return;

    void* localPlayer = coop::players::Registry::Get().Local();
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();

    // The simultaneous-mount tie-break, before the authority early-return that reads the seat: two
    // peers mounting inside one round-trip both elect themselves and each treats the other's stream
    // as an echo, a permanent double drive. The lower slot wins, a total order both hold; only a
    // genuine claim from a peer we outrank takes the seat, so an echo can never demote us.
    if (payload.occupantSlot != 0xFF && payload.occupantSlot < localSlot &&
        IsLocalOccupant(e.actor, localPlayer) &&
        (e.occupantSlot == 0xFF || e.occupantSlot == localSlot)) {
        UE_LOGI("atv: seat contention on '%ls' -- slot %u outranks local slot %u; yielding pose authority",
                key.c_str(), static_cast<unsigned>(payload.occupantSlot),
                static_cast<unsigned>(localSlot));
        e.occupantSlot = payload.occupantSlot;   // a mirror now; Tick's release edge is suppressed for a yield
    }

    // Our own authority ignores the incoming pose, so a relayed copy cannot fight the live drive.
    if (IsPoseAuthor(e.actor, localPlayer, e.occupantSlot, localSlot)) return;

    // The seat and author, if the sender may name them.
    if (!SenderMaySpeakFor(senderPeerSlot, payload.authorSlot) ||
        !SenderMaySpeakFor(senderPeerSlot, payload.occupantSlot)) {
        UE_LOGW("atv: slot %u named holder author=%u occ=%u on '%ls' -- refusing (a peer speaks "
                "only for itself)", static_cast<unsigned>(senderPeerSlot),
                static_cast<unsigned>(payload.authorSlot),
                static_cast<unsigned>(payload.occupantSlot), key.c_str());
        return;
    }
    if (e.occupantSlot != payload.occupantSlot || e.authorSlot != payload.authorSlot)
        UE_LOGI("atv: occupant change -- key='%ls' seat %u -> %u author %u -> %u (from slot %u)",
                key.c_str(), static_cast<unsigned>(e.occupantSlot),
                static_cast<unsigned>(payload.occupantSlot),
                static_cast<unsigned>(e.authorSlot),
                static_cast<unsigned>(payload.authorSlot),
                static_cast<unsigned>(senderPeerSlot));
    e.occupantSlot = payload.occupantSlot;
    e.authorSlot   = payload.authorSlot;

    // A connect snapshot warps (the joiner has not seen the world); a live packet corrects. The rig
    // keeps simulating either way; the state machine between "adopt an idle ATV" and "mirror an
    // authored one" is where the release defect lived.
    coop::atv_corrector::ApplyCorrection(e, payload, /*snap*/ payload.adopt != 0);
    // The condition block applies after the pose, behind the same gates; presence consumption is
    // further gated inside on the host as sender (atv_condition_sync.h).
    coop::atv_condition_sync::ApplyPayload(e, payload, senderPeerSlot);
}

void OnAtvRelease(const coop::net::AtvReleasePayload& payload, uint8_t senderPeerSlot) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) { UE_LOGW("atv: OnAtvRelease empty key -- dropping"); return; }
    if (!A::EnsureResolved()) return;
    if (!IndexCurrent()) return;  // a stale-generation index holds another world's ATVs
    auto it = g_atvs.find(key);
    if (it == g_atvs.end()) return;  // not indexed yet -- nothing whose author we could clear
    AtvEntry& e = it->second;
    if (!R::IsLiveByIndex(e.actor, e.idx)) return;

    void* localPlayer = coop::players::Registry::Get().Local();
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();

    // Our own authority ignores a stale or echoed release.
    if (IsPoseAuthor(e.actor, localPlayer, e.occupantSlot, localSlot)) return;

    // Only the recorded author may clear the author (the host excepted), or any client could hand
    // itself the idle election or un-seat a driver.
    if (senderPeerSlot != 0 && senderPeerSlot != 0xFF &&
        e.authorSlot != 0xFF && e.authorSlot != senderPeerSlot) {
        UE_LOGW("atv: slot %u released '%ls' held by slot %u -- refusing",
                static_cast<unsigned>(senderPeerSlot), key.c_str(),
                static_cast<unsigned>(e.authorSlot));
        return;
    }

    // The whole handler: the seat and the author free. No physics write and no velocity, since
    // nothing was frozen and every AtvState carried the velocity; on the host this election makes
    // it the idle syncer on the next tick.
    e.occupantSlot = 0xFF;
    e.authorSlot   = 0xFF;
    UE_LOGI("atv: OnAtvRelease key='%ls' -- author cleared (the rig kept simulating throughout)",
            key.c_str());
}

void OnAtvSpawn(const coop::net::AtvSpawnPayload& payload, uint8_t /*senderPeerSlot*/) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return;  // client-only (the host owns the real ATV)
    if (!A::EnsureResolved()) return;
    std::wstring menuKey = StringFromWireKey(payload.synthKey);
    if (menuKey.empty()) { UE_LOGW("atv: OnAtvSpawn empty key -- dropping"); return; }
    if (g_atvs.find(menuKey) != g_atvs.end()) return;  // already spawned (re-announce / connect dup) -- idempotent
    if (!std::isfinite(payload.x) || !std::isfinite(payload.y) || !std::isfinite(payload.z) ||
        !std::isfinite(payload.pitch) || !std::isfinite(payload.yaw) || !std::isfinite(payload.roll)) {
        UE_LOGW("atv: OnAtvSpawn non-finite pose key='%ls' -- dropping", menuKey.c_str());
        return;
    }
    std::wstring className = WireClassNameToString(payload.className);
    if (className.empty()) { UE_LOGW("atv: OnAtvSpawn empty className key='%ls' -- dropping", menuKey.c_str()); return; }
    const FVector loc{ payload.x, payload.y, payload.z };
    const FRotator rot{ payload.pitch, payload.yaw, payload.roll };
    void* spawned = A::SpawnMirror(className, loc, rot);  // physics LEFT ON -- a native idle grabbable ATV
    if (!spawned) {
        UE_LOGW("atv: OnAtvSpawn SpawnMirror failed key='%ls' class='%ls'", menuKey.c_str(), className.c_str());
        return;
    }
    AtvEntry e{};
    e.actor = spawned;
    e.idx = R::InternalIndexOf(spawned);
    e.isClientSpawnedMirror = true;
    // The spawner rides the key: the host's "menu#N" or another client's "menu@<slot>#N" that the
    // host re-announced. Derive it rather than assuming the host's menu spawner.
    std::wstring spawnerKey, localId;
    if (SK::SplitCompositeKey(menuKey, spawnerKey, localId)) e.spawnerKey = spawnerKey;
    else e.spawnerKey = SK::kHostMenuSpawner;
    g_atvs[menuKey] = std::move(e);
    g_keyForActor[spawned] = menuKey;
    RebuildSpawnerIndex();
    g_clientSawSnapshot = true;  // an announce means the host's snapshot is flowing
    UE_LOGI("atv: mirror adopted -- spawned runtime-ATV mirror key='%ls' spawner='%ls' class='%ls' actor=%p loc=(%.0f, %.0f, %.0f)",
            menuKey.c_str(), g_atvs[menuKey].spawnerKey.c_str(), className.c_str(), spawned, loc.X, loc.Y, loc.Z);
}

void OnAtvDestroy(const coop::net::AtvDestroyPayload& payload, uint8_t /*senderPeerSlot*/) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return;  // client-only
    std::wstring menuKey = StringFromWireKey(payload.synthKey);
    if (menuKey.empty()) return;
    if (!IndexCurrent()) return;  // the pass prunes a dead-world entry itself
    auto it = g_atvs.find(menuKey);
    if (it == g_atvs.end()) return;
    void* actor = it->second.actor;
    const bool destroyed = it->second.isClientSpawnedMirror && R::IsLiveByIndex(actor, it->second.idx) &&
                           A::DestroyMirror(actor);  // our own fresh spawn
    if (actor) g_keyForActor.erase(actor);
    const std::wstring spawnerKey = it->second.spawnerKey;
    g_atvs.erase(it);
    RebuildSpawnerIndex();
    UE_LOGI("atv: ATV destroyed -- key='%ls' spawner='%ls' (host-announced teardown; mirror %s)",
            menuKey.c_str(), spawnerKey.c_str(),
            destroyed ? "destroyed" : "dropped from the lane, not destroyed here");
}

void OnClientAtvSpawn(const coop::net::AtvSpawnPayload& payload, uint8_t senderPeerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;  // host-only (the client announce lands here)
    if (!A::EnsureResolved()) return;
    std::wstring menuKey = StringFromWireKey(payload.synthKey);
    if (menuKey.empty()) { UE_LOGW("atv: OnClientAtvSpawn empty key -- dropping"); return; }
    // The key must name the sender's own menu spawner: "menu@<slot>#<n>" with slot == sender.
    // Anything else is a forged or misrouted announce.
    uint8_t claimedSlot = 0xFF;
    if (!SK::ParseClientMenuKey(menuKey, claimedSlot) || claimedSlot != senderPeerSlot) {
        UE_LOGW("atv: OnClientAtvSpawn key='%ls' not the sender's menu spawner (sender=%u) -- dropping",
                menuKey.c_str(), static_cast<unsigned>(senderPeerSlot));
        return;
    }
    if (g_atvs.find(menuKey) != g_atvs.end()) return;  // re-announce / dup -- idempotent
    if (!std::isfinite(payload.x) || !std::isfinite(payload.y) || !std::isfinite(payload.z) ||
        !std::isfinite(payload.pitch) || !std::isfinite(payload.yaw) || !std::isfinite(payload.roll)) {
        UE_LOGW("atv: OnClientAtvSpawn non-finite pose key='%ls' -- dropping", menuKey.c_str());
        return;
    }
    std::wstring className = WireClassNameToString(payload.className);
    if (className.empty()) { UE_LOGW("atv: OnClientAtvSpawn empty className key='%ls' -- dropping", menuKey.c_str()); return; }
    const FVector loc{ payload.x, payload.y, payload.z };
    const FRotator rot{ payload.pitch, payload.yaw, payload.roll };
    void* spawned = A::SpawnMirror(className, loc, rot);  // the host's mirror of the client's ATV
    if (!spawned) {
        UE_LOGW("atv: OnClientAtvSpawn SpawnMirror failed key='%ls' class='%ls'", menuKey.c_str(), className.c_str());
        return;
    }
    AtvEntry e{};
    e.actor = spawned;
    e.idx = R::InternalIndexOf(spawned);
    e.isClientSpawnedMirror = true;  // fresh-spawned by us (the host), torn down on destroy
    e.spawnerKey = SK::ClientMenuSpawnerForSlot(senderPeerSlot);
    e.realKey.clear();
    g_atvs[menuKey] = std::move(e);
    g_keyForActor[spawned] = menuKey;
    RebuildSpawnerIndex();
    // Re-announce so the OTHER clients spawn mirrors. The originator drops it as a duplicate
    // (it already keyed the ATV locally).
    SendAtvSpawn(menuKey, spawned, /*slot*/ -1);
    UE_LOGI("atv: client menu spawn adopted -- key='%ls' from slot=%u, host mirror actor=%p (re-announced)",
            menuKey.c_str(), static_cast<unsigned>(senderPeerSlot), spawned);
}

void OnClientAtvDestroy(const coop::net::AtvDestroyPayload& payload, uint8_t senderPeerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;  // host-only
    std::wstring menuKey = StringFromWireKey(payload.synthKey);
    if (menuKey.empty()) return;
    uint8_t claimedSlot = 0xFF;
    if (!SK::ParseClientMenuKey(menuKey, claimedSlot) || claimedSlot != senderPeerSlot) {
        UE_LOGW("atv: OnClientAtvDestroy key='%ls' not the sender's menu spawner (sender=%u) -- dropping",
                menuKey.c_str(), static_cast<unsigned>(senderPeerSlot));
        return;
    }
    if (!IndexCurrent()) return;  // the pass prunes a dead-world entry itself
    auto it = g_atvs.find(menuKey);
    if (it == g_atvs.end()) return;
    void* actor = it->second.actor;
    const bool destroyed = it->second.isClientSpawnedMirror && R::IsLiveByIndex(actor, it->second.idx) &&
                           A::DestroyMirror(actor);  // the host's fresh spawn
    if (actor) g_keyForActor.erase(actor);
    g_atvs.erase(it);
    RebuildSpawnerIndex();
    // Re-announce so the other clients tear down their mirrors (the originator already pruned).
    SendAtvDestroy(menuKey);
    UE_LOGI("atv: client menu destroy adopted -- key='%ls' from slot=%u (mirror %s; re-announced)",
            menuKey.c_str(), static_cast<unsigned>(senderPeerSlot),
            destroyed ? "destroyed" : "dropped from the lane, not destroyed here");
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;  // host-only snapshot
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    // The hub keeps the index a pass fresh; a spawn inside that window is announced on the next
    // pass.
    void* localPlayer = coop::players::Registry::Get().Local();
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    int sent = 0, spawns = 0;
    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        if (!R::IsLiveByIndex(e.actor, e.idx)) continue;
        // A menu-spawned ATV is announced first on the same lane, so the joiner fresh-spawns it
        // before the pose lands. Both the host's menu spawner and the adopted client menu@slot
        // spawners seed the joiner.
        if (SK::IsMenuSpawnerKey(kv.first)) { SendAtvSpawn(kv.first, e.actor, peerSlot); ++spawns; }
        // The change gate re-arms for the joiner: the snapshot is fire-once and a fresh joiner is
        // the peer most likely to drop it, and a satisfied gate would then send nothing until the
        // host's copy moved.
        e.haveLastSync = false;
        // The joiner gets pose and velocity and warps to it. It needs who holds the ATV: the host
        // itself if it authors, else the recorded author (or nobody, which makes the host its
        // syncer). An ATV in the air at a join arrives moving and lands.
        const bool hostAuthors = IsPoseAuthor(e.actor, localPlayer, e.occupantSlot, localSlot);
        const uint8_t authorSlot = hostAuthors ? localSlot : e.authorSlot;
        coop::net::AtvStatePayload p{};
        if (!ReadPayload(e.actor, kv.first, e.occupantSlot, authorSlot, /*adopt*/true, p)) continue;
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::AtvState, &p, sizeof(p));
        ++sent;
    }
    UE_LOGI("atv: connect-snapshot -- sent %d ATV pose(s) (%d runtime-ATV announce(s)) to slot %d (of %zu indexed)",
            sent, spawns, peerSlot, g_atvs.size());
}

void Tick() {
    // Every early return means this module is not deciding who owns what, and the collision guard
    // must then be inert: it suppresses damage by default, and armed against a stale set it would
    // make the local ATV invulnerable. Disarmed first; the path that earns it re-arms at the end.
    struct DisarmUnlessArmed {
        bool armed = false;
        ~DisarmUnlessArmed() {
            if (armed) return;
            coop::atv_hit_guard::SetActive(false);
            // The set clears too: OwnsTick is a public read, and a stale set would label a mirror
            // sample as an owner sample.
            coop::atv_hit_guard::PublishOwned(nullptr, 0);
        }
    } scope;

    if (!A::EnsureResolved()) return;
    RegisterWithScanHub();  // safety net for any order where Tick precedes Install
    if (!IndexCurrent()) return;  // index belongs to a dead world -- wait for the hub's next pass
    auto* s = g_session.load(std::memory_order_acquire);

    if (!s || !s->connected()) return;
    void* localPlayer = coop::players::Registry::Get().Local();
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    const uint64_t nowMs = NowMs();

    // Fail closed: without the hit interceptors a non-owner would author damage on a simulating
    // rig, so nothing is mirrored and the ERROR at Install says why.
    if (!coop::atv_hit_guard::Armed()) return;

    const bool isHost = s->role() == coop::net::Role::Host;
    void* owned[coop::atv_hit_guard::kMaxOwned];
    int   ownedN = 0;

    for (auto& kv : g_atvs) {
        AtvEntry& e = kv.second;
        if (!R::IsLiveByIndex(e.actor, e.idx)) continue;

        const bool isDriver  = CanClaimOrIsDriver(e.actor, localPlayer, e.occupantSlot, localSlot);
        const bool isGrabber = !isDriver && IsLocalGrabber(e.actor, localPlayer);  // mutually exclusive
        const bool authority = isDriver || isGrabber;

        // The authority-lost edge, with two causes handled oppositely: a dismount or ungrab frees
        // the ATV (the seat and author clear, and a release goes out); a yield (still seated, but
        // outranked by a lower slot) means someone else holds it, and clearing would erase the
        // winner's claim, re-claim next tick and flap.
        const bool yielded = IsLocalOccupant(e.actor, localPlayer) &&
                             e.occupantSlot != 0xFF && e.occupantSlot != localSlot;
        if (e.wasPoseAuthor && !authority && !yielded) {
            e.occupantSlot = 0xFF;
            e.authorSlot   = 0xFF;
            coop::net::AtvReleasePayload rp{};
            WireKeyFromString(kv.first, rp.key);
            s->SendReliable(coop::net::ReliableKind::AtvRelease, &rp, sizeof(rp));
            UE_LOGI("atv: authority released key='%ls' -- author cleared; the host now syncs it idle",
                    kv.first.c_str());
        }
        e.wasPoseAuthor = authority;

        if (authority) {
            if (isDriver && e.occupantSlot != localSlot)
                UE_LOGI("atv: occupant change -- key='%ls' seat %u -> %u (local mount)",
                        kv.first.c_str(), static_cast<unsigned>(e.occupantSlot),
                        static_cast<unsigned>(localSlot));
            if (isDriver) e.occupantSlot = localSlot;   // claim the seat locally
            if (e.authorSlot != localSlot)
                UE_LOGI("atv: occupant change -- key='%ls' author %u -> %u (local authority)",
                        kv.first.c_str(), static_cast<unsigned>(e.authorSlot),
                        static_cast<unsigned>(localSlot));
            e.authorSlot = localSlot;
        }

        // Tick ownership, a different question from pose authority.
        const bool ownsTick = OwnsTickFor(authority, isHost, e.authorSlot);
        // The eject drill, env-gated, once per process.
        coop::atv_eject_drill::MaybeFire(e.actor, kv.first.c_str(), nowMs, isHost, authority, ownsTick);
        if (ownsTick) {
            if (ownedN < coop::atv_hit_guard::kMaxOwned) {
                owned[ownedN++] = e.actor;
            } else {
                static bool sSaturated = false;
                if (!sSaturated) { sSaturated = true;
                    UE_LOGE("atv: owned-set saturated at %d -- ATV '%ls' and any beyond it will "
                            "have their OWN collisions suppressed on the peer that owns them",
                            coop::atv_hit_guard::kMaxOwned, kv.first.c_str()); }
            }
        }

        if (authority) {
            if (nowMs - e.lastSentMs >= kDriveSendMs) {
                e.lastSentMs = nowMs;
                coop::net::AtvStatePayload p{};
                const uint8_t occSlot = isDriver ? localSlot : uint8_t{0xFF};  // grabber: no seated driver
                if (ReadPayload(e.actor, kv.first, occSlot, localSlot, /*adopt*/false, p, /*grabbed*/isGrabber)) {
                    s->SendReliable(coop::net::ReliableKind::AtvState, &p, sizeof(p));
                    coop::atv_condition_sync::NoteSent(e, p);  // keep the idle gate's baseline fresh across a drive-to-idle handoff
                }
            }
        } else if (ownsTick) {
            // The idle syncer (the host, nobody driving): a slower cadence and a change gate, so a
            // parked ATV costs nothing while one rolling down a hill still converges.
            if (nowMs - e.lastSentMs >= kIdleSendMs) {
                // The clock bumps once the window elapses, as MTA's does, not only on a send:
                // bumped only on a send it stayed stale for the parked ATV, and the gate then ran
                // its dispatches at the pump rate.
                e.lastSentMs = nowMs;
                // The payload is built before the gate so the condition block can vote: a parked
                // host eject (a mask flip, no motion) would otherwise wait out the keepalive.
                coop::net::AtvStatePayload p{};
                const bool readable  = ReadPayload(e.actor, kv.first, e.occupantSlot, /*authorSlot*/0xFF, /*adopt*/false, p);
                const bool changed   = IdleWorthSending(e);
                const bool condMoved = readable && coop::atv_condition_sync::CondChangedSinceLastSend(e, p);
                const bool keepalive = nowMs - e.lastIdleSendMs >= kIdleKeepaliveMs;
                if (readable && (changed || condMoved || keepalive)) {
                    e.lastIdleSendMs = nowMs;
                    s->SendReliable(coop::net::ReliableKind::AtvState, &p, sizeof(p));
                    coop::atv_condition_sync::NoteSent(e, p);
                }
            }
        }
        // A mirror does nothing here; it is corrected at packet arrival.
    }

    // Published before arming, so the guard never runs against an unrefreshed set.
    coop::atv_hit_guard::PublishOwned(owned, ownedN);
    coop::atv_hit_guard::SetActive(true);
    scope.armed = true;
}

void OnDisconnect() {
    // Disarmed first: nothing publishes an owned set from here, and a live hit must reach the game.
    coop::atv_hit_guard::SetActive(false);
    coop::atv_hit_guard::PublishOwned(nullptr, 0);
    for (auto& kv : g_atvs) {
        const bool live = R::IsLiveByIndex(kv.second.actor, kv.second.idx);
        if (kv.second.isClientSpawnedMirror) {
            if (live) A::DestroyMirror(kv.second.actor);   // a fresh-spawned runtime mirror is a coop artifact
        }
        // Nothing to restore: the lane disables neither tick nor physics, so a session leaves every
        // save ATV as it found it.
    }
    const size_t n = g_atvs.size();
    g_atvs.clear();
    g_keyForActor.clear();
    g_savePlacedKeys.clear();
    g_savePlacedActors.clear();
    g_saveSites.clear();
    g_bySpawner.clear();
    g_pendingAdopts.clear();
    g_menuCounter = 0;
    g_clientMenuCounter = 0;
    g_clientSawSnapshot = false;
    g_installed = false;  // a new session re-indexes via the next Install (latched again)
    const auto c  = coop::atv_corrector::ReadCounters();
    const auto hg = coop::atv_hit_guard::ReadCounters();
    if (n > 0)
        UE_LOGI("atv: OnDisconnect -- cleared %zu ATV(s) (brains restored; runtime mirrors destroyed); "
                "hit guard: %s, %llu neutered / %llu allowed / %llu UNRESOLVED; "
                "corrector: %llu nudged / %llu warped "
                "/ %llu cut-on-stall / %llu parked-replace",
                n, hg.armed ? "armed" : "NEVER ARMED",
                static_cast<unsigned long long>(hg.neutered),
                static_cast<unsigned long long>(hg.allowed),
                static_cast<unsigned long long>(hg.unresolved),
                static_cast<unsigned long long>(c.corrections),
                static_cast<unsigned long long>(c.warps),
                static_cast<unsigned long long>(c.stallWarps),
                static_cast<unsigned long long>(c.restPlaces));
}

bool OwnsTick(void* actor) {
    // The set, the latch and the answer live with the collision guard (atv_hit_guard::Owns).
    return coop::atv_hit_guard::Owns(actor);
}

// Whether a remote peer occupies this ATV; the mount deny reads it.
bool IsOccupiedByOther(void* actor, uint8_t* outOccupantSlot) {
    if (!actor) return false;
    if (!IndexCurrent()) return false;   // a stale-generation index holds another world's ATVs
    const uint8_t localSlot = coop::players::Registry::Get().LocalPeerId();
    for (const auto& kv : g_atvs) {
        if (kv.second.actor == actor) {
            const uint8_t occ = kv.second.occupantSlot;
            if (occ != 0xFF && occ != localSlot) {
                if (outOccupantSlot) *outOccupantSlot = occ;
                return true;
            }
            return false;
        }
    }
    return false;
}

}  // namespace coop::atv_sync
