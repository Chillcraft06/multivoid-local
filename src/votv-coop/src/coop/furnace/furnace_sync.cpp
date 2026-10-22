// coop/furnace/furnace_sync.cpp -- see coop/furnace/furnace_sync.h.
//
// The furnace is Acremator_C (FACT: prop.h notes it derives from Aactor_save_C and carries
// a Key; docs/coop-dispatch-visibility.md notes its door is a prop_swinger_C whose
// setClosed calls the lid's open(bool)/close()). The door actor is runtime-created and
// keyless, so every packet here is keyed by the FURNACE actor's Key.
//
// The fire on/off verb and bool on Acremator_C are NOT in the tree (HYPOTHESIS ZONE):
// this TU discovers them from candidate lists at runtime, probe-logs every attempt, and
// re-reads + polls the fire state so a missed verb still converges. If nothing resolves,
// the fire lane says so loudly and the live two-client test must name the real verb/bool.

#include "coop/furnace/furnace_sync.h"

#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString / StringFromWireKey
#include "coop/player/players_registry.h"  // kMaxPeers, Registry::Get().LocalPlayerElementId()
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld

#include "ue_wrap/actors/prop.h"  // GetActorSaveKeyString (the Aactor_save_C::Key reader)
#include "ue_wrap/actors/swinger.h"  // the door: IsSwinger / TryReadOpen / CallOpen / CallClose
#include "ue_wrap/core/game_thread.h"  // Post: net callbacks -> game thread
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"  // ClassByName / ForEachInstance / ForEachClass
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"  // FindPropertyOffset
#include "ue_wrap/core/script_gate.h"  // WatchName / WatchClassName
#include "ue_wrap/core/types.h"  // FVector
#include "ue_wrap/engine/engine.h"  // ParentActorOf / TryGetActorLocation
#include "ue_wrap/world/world_singleton.h"  // Gamemode (for mainPlayer, the local pawn)

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace coop::furnace_sync {
namespace {

namespace sg = ue_wrap::script_gate;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace P  = ue_wrap::prop;
namespace SW = ue_wrap::swinger;
namespace OI = ue_wrap::object_index;
using coop::net::StringFromWireKey;
using coop::net::WireKeyFromString;

// Watch tags.
constexpr int kTagDoorOpen  = 0x46554F50;  // 'FUOP' -- a lid's open
constexpr int kTagDoorClose = 0x4655434C;  // 'FUCL' -- a lid's close
constexpr int kTagAction    = 0x46554149;  // 'FUAI' -- the cremator's actionOptionIndex
constexpr int kTagFireVerbBase = 0x46554630;  // 'FUF0'+i -- one tag per candidate fire verb

// HYPOTHESIS: the fire on/off verb and bool on Acremator_C. Neither name is in the tree;
// each candidate is tried in order, every attempt is probe-logged, and the live test must
// confirm which one the game actually uses. A candidate that resolves into a full gate
// table simply dies, logged, like any dead watch.
constexpr const wchar_t* kFireVerbs[] = {
    L"setFire", L"startFire", L"ignite", L"toggleFire", L"turnOn", L"setOn",
};
constexpr int kFireVerbCount = static_cast<int>(sizeof(kFireVerbs) / sizeof(kFireVerbs[0]));
constexpr const wchar_t* kFireBools[] = {
    L"Fire", L"fire", L"isOnFire", L"fireOn", L"burning",
    L"isBurning", L"FireOn", L"onFire", L"Active", L"active",
};
constexpr int kFireBoolCount = static_cast<int>(sizeof(kFireBools) / sizeof(kFireBools[0]));

// HYPOTHESIS: the furnace chamber is roughly person-sized. A generous box around the
// cremator's origin counts as "inside" for the occupant check; the live test must confirm
// the occupant is caught (and nobody outside is).
constexpr float kInsideXYCm = 160.0f;
constexpr float kInsideZCm  = 220.0f;

std::atomic<coop::net::Session*> g_session{nullptr};

// Game-thread state.
void* g_crematorCls = nullptr;   // cremator_C, once the class loads
int32_t g_fireBoolOff = -2;      // -2 = undiscovered, -1 = none resolved, else the offset
const wchar_t* g_fireBoolName = nullptr;
int32_t g_mainPlayerOff = -2;    // mainGamemode_C.mainPlayer, -2 = not tried
void* g_gmCls = nullptr;
int32_t g_isBurningOff = -2, g_burningTimeOff = -2;  // on the pawn's class, -2 = not tried
void* g_pawnCls = nullptr;
bool g_applyingDoor = false;     // our own OnDoorState apply: skip the verb echo
std::unordered_map<std::wstring, void*> g_doorByKey;  // cremator Key -> its door actor

// Last-sent state (one furnace per world; the packets are keyed so a second furnace
// would send fine, only this dedup is global).
bool g_haveDoor = false, g_lastDoorOpen = false;
bool g_haveFire = false, g_lastFireOn = false;
int  g_lastOccupant = -2;  // -2 = unknown, -1 = empty, else the peer slot

std::chrono::steady_clock::time_point g_lastFirePoll{};
std::chrono::steady_clock::time_point g_lastDoorIndex{};
std::chrono::steady_clock::time_point g_burnStuckSince{};
bool g_burnStuckArmed = false;

uint64_t g_doorEdges = 0, g_fireRechecks = 0, g_actions = 0, g_clears = 0, g_sends = 0;

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg g_regDoorOpen = Reg::Pending, g_regDoorClose = Reg::Pending, g_regAction = Reg::Pending;
Reg g_regFireVerb[kFireVerbCount] = {};
bool g_settled = false;

coop::net::Session* ConnectedAs(coop::net::Role role) {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->connected() && s->role() == role) ? s : nullptr;
}

bool IsCremator(void* obj) {
    if (!obj || !g_crematorCls) return false;
    void* cls = R::ClassOf(obj);
    return cls && R::IsDescendantOfAny(cls, &g_crematorCls, 1);
}

// The fire bool discovery. Runs once, the first Tick that sees cremator_C.
void DiscoverFireBool() {
    g_fireBoolOff = -1;
    g_fireBoolName = nullptr;
    for (int i = 0; i < kFireBoolCount; ++i) {
        const int32_t off = R::FindPropertyOffset(g_crematorCls, kFireBools[i]);
        UE_LOGI("furnace: fire-bool candidate '%ls' -> offset %d", kFireBools[i], off);
        if (off >= 0) {
            g_fireBoolOff = off;
            g_fireBoolName = kFireBools[i];
            break;
        }
    }
    if (g_fireBoolOff < 0) {
        UE_LOGW("furnace: NO fire bool resolved on cremator_C -- the fire lane is blind until "
                "the live session names the real bool (see the furnace report)");
    } else {
        UE_LOGI("furnace: fire bool is '%ls'@0x%04X -- HYPOTHESIS, confirm in the live two-client test",
                g_fireBoolName, g_fireBoolOff);
    }
}

bool EnsureResolved() {
    if (!g_crematorCls) {
        g_crematorCls = OI::ClassByName(L"cremator_C");
        if (g_crematorCls)
            UE_LOGI("furnace: resolved cremator_C=%p", g_crematorCls);
    }
    if (!g_crematorCls) return false;
    if (g_fireBoolOff == -2) DiscoverFireBool();
    if (g_mainPlayerOff == -2) {
        void* gm = ue_wrap::world_singleton::Gamemode();
        if (gm) {
            void* cls = R::ClassOf(gm);
            if (cls && cls != g_gmCls) {
                g_gmCls = cls;
                g_mainPlayerOff = R::FindPropertyOffset(cls, L"mainPlayer");
                UE_LOGI("furnace: mainGamemode_C.mainPlayer@%d", g_mainPlayerOff);
            }
        }
    }
    return true;
}

bool ReadFire(void* cremator, bool& on) {
    if (!cremator || g_fireBoolOff < 0) return false;
    on = *reinterpret_cast<const bool*>(reinterpret_cast<const uint8_t*>(cremator) +
                                        static_cast<size_t>(g_fireBoolOff));
    return true;
}

// The local pawn: the gamemode's mainPlayer, the same object vitals reads.
void* LocalPawn() {
    if (g_mainPlayerOff < 0) return nullptr;
    void* gm = ue_wrap::world_singleton::Gamemode();
    if (!gm) return nullptr;
    return *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(gm) +
                                           static_cast<size_t>(g_mainPlayerOff));
}

bool EnsureBurnOffsets(void* pawn) {
    void* cls = pawn ? R::ClassOf(pawn) : nullptr;
    if (!cls) return false;
    if (cls != g_pawnCls) {
        g_pawnCls = cls;
        g_isBurningOff = R::FindPropertyOffset(cls, L"isBurning");
        g_burningTimeOff = R::FindPropertyOffset(cls, L"burningTime");
        UE_LOGI("furnace: pawn burn offsets: isBurning@%d burningTime@%d",
                g_isBurningOff, g_burningTimeOff);
    }
    return g_isBurningOff >= 0 && g_burningTimeOff >= 0;
}

bool ReadLocalBurn(bool& burning, float& t) {
    void* pawn = LocalPawn();
    if (!pawn || !EnsureBurnOffsets(pawn)) return false;
    burning = *reinterpret_cast<const bool*>(reinterpret_cast<const uint8_t*>(pawn) +
                                             static_cast<size_t>(g_isBurningOff));
    t = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(pawn) +
                                        static_cast<size_t>(g_burningTimeOff));
    return true;
}

// The stuck-burn clear, SCOPED to the pawn's two burn fields. HYPOTHESIS: the burn
// visuals, burn audio and camera jerking are driven by isBurning/burningTime, so writing
// exactly these two is the whole clear; nothing else (no global audio/camera reset) is
// touched. Game thread.
bool ClearLocalBurnFields(const char* why) {
    void* pawn = LocalPawn();
    if (!pawn || !EnsureBurnOffsets(pawn)) {
        UE_LOGW("furnace: burn clear (%s) -- no pawn or no offsets, nothing written", why);
        return false;
    }
    *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(pawn) +
                             static_cast<size_t>(g_isBurningOff)) = false;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(pawn) +
                              static_cast<size_t>(g_burningTimeOff)) = 0.0f;
    ++g_clears;
    UE_LOGI("furnace: burn cleared locally (%s): isBurning=false burningTime=0", why);
    return true;
}

// --- Door index: cremator Key -> its prop_swinger_C door -----------------------------

struct IndexCtx {
    std::unordered_map<std::wstring, void*>* out;
    void* swingerCls;
    void* crematorCls;
};

void IndexInstanceFn(void* ctx, void* obj, int32_t /*index*/) {
    const IndexCtx* c = static_cast<const IndexCtx*>(ctx);
    void* parent = E::ParentActorOf(obj, nullptr);
    if (!parent) return;
    void* pcls = R::ClassOf(parent);
    if (!pcls || !R::IsDescendantOfAny(pcls, &c->crematorCls, 1)) return;
    const std::wstring key = P::GetActorSaveKeyString(parent);
    if (key.empty() || key == L"None") return;
    (*c->out)[key] = obj;
}

void IndexClassFn(void* ctx, void* cls, void* /*anyInstance*/) {
    const IndexCtx* c = static_cast<const IndexCtx*>(ctx);
    if (!R::IsDescendantOfAny(cls, &c->swingerCls, 1)) return;  // the door may subclass the lid
    OI::ForEachInstance(cls, IndexInstanceFn, ctx);
}

void IndexDoors() {
    if (!g_crematorCls || !SW::EnsureResolved()) return;
    void* swingerCls = OI::ClassByName(L"prop_swinger_C");
    if (!swingerCls) return;
    IndexCtx c{&g_doorByKey, swingerCls, g_crematorCls};
    OI::ForEachClass(IndexClassFn, &c);
    UE_LOGI("furnace: door index rebuilt: %zu furnace door(s)", g_doorByKey.size());
}

void* FindDoor(const std::wstring& key) {
    auto it = g_doorByKey.find(key);
    if (it != g_doorByKey.end() && R::IsLive(it->second)) return it->second;
    IndexDoors();
    it = g_doorByKey.find(key);
    return (it != g_doorByKey.end() && R::IsLive(it->second)) ? it->second : nullptr;
}

struct FirstCtx { void* out = nullptr; };
void FirstInstanceFn(void* ctx, void* obj, int32_t /*index*/) {
    FirstCtx* c = static_cast<FirstCtx*>(ctx);
    if (!c->out) c->out = obj;
}

// --- Occupant detection (HOST) --------------------------------------------------------
// Slot 0 is the local pawn; remote slots come from the pose stream. Returns the peer slot
// inside the furnace, -1 when empty, -2 when unknown (no cremator / no location).

int DetectOccupant(void* cremator) {
    if (!cremator) return -2;
    ue_wrap::FVector cl{};
    if (!E::TryGetActorLocation(cremator, cl)) return -2;
    auto inside = [&](const ue_wrap::FVector& p) {
        return std::fabs(p.X - cl.X) <= kInsideXYCm &&
               std::fabs(p.Y - cl.Y) <= kInsideXYCm &&
               std::fabs(p.Z - cl.Z) <= kInsideZCm;
    };
    void* pawn = LocalPawn();
    if (pawn) {
        ue_wrap::FVector pl{};
        if (E::TryGetActorLocation(pawn, pl) && inside(pl)) return 0;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (s) {
        for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
            net::PoseSnapshot pose{};
            if (!s->TryGetRemotePose(slot, pose)) continue;
            const ue_wrap::FVector pp{pose.x, pose.y, pose.z};
            if (inside(pp)) return slot;
        }
    }
    return -1;
}

bool IsLocalPawnInside(void* cremator) {
    if (!cremator) return false;
    ue_wrap::FVector cl{};
    void* pawn = LocalPawn();
    ue_wrap::FVector pl{};
    if (!E::TryGetActorLocation(cremator, cl) || !pawn ||
        !E::TryGetActorLocation(pawn, pl))
        return false;
    return std::fabs(pl.X - cl.X) <= kInsideXYCm &&
           std::fabs(pl.Y - cl.Y) <= kInsideXYCm &&
           std::fabs(pl.Z - cl.Z) <= kInsideZCm;
}

// --- Sends (HOST) ---------------------------------------------------------------------

void SendDoorState(const std::wstring& key, bool open) {
    auto* s = ConnectedAs(coop::net::Role::Host);
    if (!s) return;
    net::KeyedTogglePayload p{};
    WireKeyFromString(key, p.key);
    p.action = open ? 1 : 0;
    ++g_sends;
    if (s->SendReliable(net::ReliableKind::FurnaceDoorState, &p, sizeof(p)))
        UE_LOGI("furnace: sent FurnaceDoorState key='%ls' action=%s", key.c_str(),
                open ? "OPEN" : "SHUT");
    else
        UE_LOGW("furnace: FurnaceDoorState send FAILED (key='%ls')", key.c_str());
}

void SendFireState(const std::wstring& key, bool fireOn, int occupantSlot) {
    auto* s = ConnectedAs(coop::net::Role::Host);
    if (!s) return;
    if (occupantSlot < -1) occupantSlot = -1;  // never put the -2 "unknown" on the wire
    net::FurnaceFirePayload p{};
    WireKeyFromString(key, p.key);
    p.fireOn = fireOn ? 1 : 0;
    p.occupantSlot = static_cast<int8_t>(occupantSlot);
    ++g_sends;
    if (s->SendReliable(net::ReliableKind::FurnaceFireState, &p, sizeof(p)))
        UE_LOGI("furnace: sent FurnaceFireState key='%ls' fire=%s occupantSlot=%d", key.c_str(),
                fireOn ? "ON" : "OFF", occupantSlot);
    else
        UE_LOGW("furnace: FurnaceFireState send FAILED (key='%ls')", key.c_str());
}

void SendBurnClear(uint32_t elementId, int slot /* -1 = all */) {
    auto* s = ConnectedAs(coop::net::Role::Host);
    if (!s) return;
    net::FurnaceBurnClearPayload p{};
    p.playerElementId = elementId;
    const bool ok = (slot < 0)
        ? s->SendReliable(net::ReliableKind::FurnaceBurnClear, &p, sizeof(p))
        : s->SendReliableToSlot(slot, net::ReliableKind::FurnaceBurnClear, &p, sizeof(p));
    UE_LOGI("furnace: sent FurnaceBurnClear elementId=0x%08x to %s (%s)", elementId,
            slot < 0 ? "all" : "one slot", ok ? "ok" : "FAILED");
}

// --- Host edges -----------------------------------------------------------------------

// Damage-ownership correction (HYPOTHESIS): furnace damage is assumed to follow the
// pawn's burn flags. When the fire is on with a known occupant that is NOT the local
// player, the local player's burn is not the furnace's, so clear it: this is the
// 2026-10-13 "the presser took the damage instead of the occupant" fix at the burn-flag
// level. Only an ACTIVE burn is corrected here; a stuck one is the watchdog's.
void CorrectLocalBurnForOccupant(int occupantSlot) {
    if (occupantSlot <= 0) return;  // -2 unknown, -1 empty, 0 me: nothing to correct
    bool burning = false;
    float t = 0.0f;
    if (ReadLocalBurn(burning, t) && burning && t > 0.0f) {
        UE_LOGI("furnace: ownership correction -- fire is on with occupant slot %d but the "
                "local pawn is burning; clearing the local burn", occupantSlot);
        ClearLocalBurnFields("ownership-correction");
    }
}

void RecheckFire(void* cremator, const char* why) {
    if (!cremator) return;
    bool on = false;
    if (!ReadFire(cremator, on)) {
        UE_LOGW("furnace: fire recheck (%s) -- no fire bool resolved, blind", why);
        return;
    }
    const std::wstring key = P::GetActorSaveKeyString(cremator);
    if (key.empty() || key == L"None") {
        UE_LOGW("furnace: fire recheck (%s) -- the cremator has no Key, cannot key the packet",
                why);
        return;
    }
    const int occupant = DetectOccupant(cremator);
    const bool edge = !g_haveFire || g_lastFireOn != on || g_lastOccupant != occupant;
    g_haveFire = true;
    g_lastFireOn = on;
    g_lastOccupant = occupant;
    ++g_fireRechecks;
    if (edge) {
        UE_LOGI("furnace: fire edge (%s) key='%ls' fire=%s occupantSlot=%d", why, key.c_str(),
                on ? "ON" : "OFF", occupant);
        SendFireState(key, on, occupant);
        if (on) CorrectLocalBurnForOccupant(occupant);
    }
}

void OnFurnaceDoorEdge(void* door, void* cremator) {
    if (!door || !cremator) return;
    const std::wstring key = P::GetActorSaveKeyString(cremator);
    if (key.empty() || key == L"None") {
        UE_LOGW("furnace: door edge on a cremator with no Key -- cannot key the packet, dropped");
        return;
    }
    g_doorByKey[key] = door;  // learn the door from the edge
    bool open = false;
    if (!SW::TryReadOpen(door, open)) return;
    auto* s = g_session.load(std::memory_order_acquire);
    const bool isHost = s && s->connected() && s->role() == coop::net::Role::Host;
    if (!isHost) {
        // A client's own door press runs natively AND is sent to the host as an intent: the
        // host drives its own door verbs and its edge watch broadcasts FurnaceDoorState to
        // everyone, which converges the presser's copy too (the apply is idempotent).
        net::KeyedTogglePayload ip{};
        net::WireKeyFromString(key, ip.key);
        ip.action = open ? 1 : 0;
        if (s && s->SendReliable(net::ReliableKind::FurnaceDoorIntent, &ip, sizeof(ip))) {
            UE_LOGI("furnace: client door press key='%ls' -> %s -- intent sent to host",
                    key.c_str(), open ? "OPEN" : "SHUT");
        } else {
            UE_LOGW("furnace: client door press key='%ls' -- intent NOT sent (no session); "
                    "the host will not hear this press", key.c_str());
        }
        return;
    }
    if (g_haveDoor && g_lastDoorOpen == open) return;  // no edge
    g_haveDoor = true;
    g_lastDoorOpen = open;
    ++g_doorEdges;
    UE_LOGI("furnace: door edge key='%ls' -> %s", key.c_str(), open ? "OPEN" : "SHUT");
    SendDoorState(key, open);
}

// --- Verb watch callbacks ---------------------------------------------------------------

void OnDoorVerbPost(const sg::Call& call) {
    if (g_applyingDoor) return;  // our own OnDoorState apply
    if (!call.object || !SW::IsSwinger(call.object)) return;
    if (!EnsureResolved()) return;
    void* parent = E::ParentActorOf(call.object, nullptr);
    if (!parent || !IsCremator(parent)) return;  // not the furnace's door
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    // A client's world load is the host's save; its devices' own calls there are not plays.
    if (s->role() != coop::net::Role::Host && !coop::net_pump::IsInAnnouncedWorld(call.object))
        return;
    OnFurnaceDoorEdge(call.object, parent);
}

void OnCrematorActionPost(const sg::Call& call) {
    if (!call.object || !IsCremator(call.object)) return;
    if (!ConnectedAs(coop::net::Role::Host)) return;  // the host authors fire state
    ++g_actions;
    UE_LOGI("furnace: actionOptionIndex ran on the cremator (probe: a player interacted; "
            "re-reading the fire state)");
    RecheckFire(call.object, "action");
}

void OnFireVerbPost(const sg::Call& call) {
    if (!call.object || !IsCremator(call.object)) return;
    if (!ConnectedAs(coop::net::Role::Host)) return;
    UE_LOGI("furnace: candidate fire verb ran on the cremator (probe: watch tag 0x%X)", call.tag);
    RecheckFire(call.object, "fire-verb");
}

// --- Client applies -----------------------------------------------------------------------

void ApplyDoorState(const std::wstring& key, bool open) {
    void* door = FindDoor(key);
    if (!door) {
        UE_LOGW("furnace: OnDoorState key='%ls' -- no door indexed, dropped", key.c_str());
        return;
    }
    bool cur = false;
    if (SW::TryReadOpen(door, cur) && cur == open) {
        UE_LOGI("furnace: OnDoorState key='%ls' already %s -- no-op", key.c_str(),
                open ? "OPEN" : "SHUT");
        return;
    }
    g_applyingDoor = true;
    const bool ok = open ? SW::CallOpen(door, false) : SW::CallClose(door);
    g_applyingDoor = false;
    UE_LOGI("furnace: OnDoorState key='%ls' applied %s (%s)", key.c_str(),
            open ? "OPEN" : "SHUT", ok ? "ok" : "FAILED");
}

void ApplyFireState(const std::wstring& key, bool fireOn, int occupantSlot) {
    void* cremator = nullptr;
    if (g_crematorCls) {
        FirstCtx fc{};
        OI::ForEachInstance(g_crematorCls, FirstInstanceFn, &fc);
        // (One furnace per world; the first instance is it. A second furnace would need
        // a per-key scan here.)
        if (fc.out && P::GetActorSaveKeyString(fc.out) == key) cremator = fc.out;
    }
    if (!cremator) {
        UE_LOGW("furnace: OnFireState key='%ls' -- no cremator instance, dropped", key.c_str());
        return;
    }
    if (g_fireBoolOff >= 0) {
        *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(cremator) +
                                 static_cast<size_t>(g_fireBoolOff)) = fireOn;
        UE_LOGI("furnace: OnFireState key='%ls' applied fire=%s occupantSlot=%d (bool write)",
                key.c_str(), fireOn ? "ON" : "OFF", occupantSlot);
    } else {
        UE_LOGW("furnace: OnFireState key='%ls' fire=%s -- no fire bool resolved, cannot drive "
                "the local fire visual (blind lane)", key.c_str(), fireOn ? "ON" : "OFF");
    }
    // Burn ownership on this client, position-based (no my-slot accessor exists): am I
    // inside this furnace?
    const bool inside = IsLocalPawnInside(cremator);
    bool burning = false;
    float t = 0.0f;
    const bool have = ReadLocalBurn(burning, t);
    if (fireOn && inside) {
        UE_LOGI("furnace: OnFireState -- the local player is inside the burning furnace "
                "(occupantSlot=%d); the native burn proceeds", occupantSlot);
        // HYPOTHESIS backstop: if the fire bool is undiscovered the local fire never
        // turned on, so the native burn may never start; set the burn flags so the
        // occupant still takes the furnace's damage. Skipped when the bool is known and
        // was written above (the native simulation owns it then).
        if (g_fireBoolOff < 0 && have && (!burning || t <= 0.0f)) {
            void* pawn = LocalPawn();
            if (pawn && EnsureBurnOffsets(pawn)) {
                *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(pawn) +
                                         static_cast<size_t>(g_isBurningOff)) = true;
                *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(pawn) +
                                          static_cast<size_t>(g_burningTimeOff)) = 5.0f;
                UE_LOGI("furnace: OnFireState -- fire bool unknown, forced the occupant burn "
                        "flags (isBurning=true burningTime=5)");
            }
        }
    } else if (have && burning && t <= 0.0f) {
        // Burning with no time left while not the burning furnace's occupant: the stuck
        // burn from the playtest. Clear it.
        ClearLocalBurnFields("fire-state-stuck");
    } else {
        UE_LOGI("furnace: OnFireState -- local burn untouched (inside=%d burning=%d t=%.1f)",
                inside ? 1 : 0, burning ? 1 : 0, t);
    }
}

// --- Stuck-burn watchdog --------------------------------------------------------------------
// The observed failure: after the furnace death, isBurning stayed true with no damage
// behind it (burningTime exhausted), giving permanent burn audio + camera jerking +
// visuals. A burn with no time left for 10 straight seconds is never legitimate, so it
// is cleared. Host also broadcasts FurnaceBurnClear so a stuck client clears too.

void BurnWatchdog(std::chrono::steady_clock::time_point now, bool isHost) {
    bool burning = false;
    float t = 0.0f;
    if (!ReadLocalBurn(burning, t)) {
        g_burnStuckArmed = false;
        return;
    }
    if (burning && t <= 0.0f) {
        if (!g_burnStuckArmed) {
            g_burnStuckArmed = true;
            g_burnStuckSince = now;
            UE_LOGI("furnace: watchdog -- local burn has no time left; arming the 10s stuck check");
            return;
        }
        if (now - g_burnStuckSince >= std::chrono::seconds(10)) {
            g_burnStuckArmed = false;
            UE_LOGI("furnace: watchdog -- local burn stuck for 10s; clearing");
            ClearLocalBurnFields("watchdog-stuck");
            if (isHost)
                SendBurnClear(coop::players::Registry::Get().LocalPlayerElementId(), -1);
        }
    } else {
        g_burnStuckArmed = false;
    }
}

// --- Watch registration ---------------------------------------------------------------------

void SettleWatches() {
    int live = 0, dead = 0;
    auto nameOne = [&](const wchar_t* name, int tag, Reg r) {
        if (r == Reg::Registered && sg::NameWatchLive(name, tag)) ++live;
        else if (r == Reg::Refused || (r == Reg::Registered && sg::NameWatchSettled(name, tag)))
            ++dead;
    };
    auto clsOne = [&](const wchar_t* cls, const wchar_t* name, int tag, Reg r) {
        if (r == Reg::Registered && sg::ClassNameWatchLive(cls, name, tag)) ++live;
        else if (r == Reg::Refused ||
                 (r == Reg::Registered && sg::ClassNameWatchSettled(cls, name, tag)))
            ++dead;
    };
    nameOne(L"Open", kTagDoorOpen, g_regDoorOpen);
    nameOne(L"Close", kTagDoorClose, g_regDoorClose);
    clsOne(L"cremator_C", L"actionOptionIndex", kTagAction, g_regAction);
    for (int i = 0; i < kFireVerbCount; ++i)
        clsOne(L"cremator_C", kFireVerbs[i], kTagFireVerbBase + i, g_regFireVerb[i]);
    const int total = 3 + kFireVerbCount;
    if (live + dead == total && sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGI("furnace: watches settled: %d live, %d dead of %d (dead fire-verb candidates "
                "are expected guesses)", live, dead, total);
    }
}

void InstallWatchName(const wchar_t* name, int tag, Reg& reg, const char* what) {
    if (reg != Reg::Pending) return;
    if (sg::WatchName(name, tag, nullptr, &OnDoorVerbPost)) {
        reg = Reg::Registered;
    } else {
        reg = Reg::Refused;
        UE_LOGE("furnace: the script-body gate refused the watch on %s -- that edge is blind", what);
    }
}

void InstallWatchClass(const wchar_t* cls, const wchar_t* name, int tag, Reg& reg,
                       sg::PostFn post, const char* what) {
    if (reg != Reg::Pending) return;
    if (sg::WatchClassName(cls, name, tag, nullptr, post)) {
        reg = Reg::Registered;
    } else {
        reg = Reg::Refused;
        UE_LOGE("furnace: the script-body gate refused the watch on %s -- that edge is blind", what);
    }
}

}  // namespace

// --- Public API -------------------------------------------------------------------------------

bool IsFurnaceDoor(void* actor) {
    if (!actor) return false;
    // Resolve just the class here (not the full EnsureResolved discovery): this runs on
    // other lanes' index paths, possibly before this module's first Tick.
    if (!g_crematorCls) g_crematorCls = OI::ClassByName(L"cremator_C");
    if (!g_crematorCls) return false;
    if (!SW::IsSwinger(actor)) return false;
    void* parent = E::ParentActorOf(actor, nullptr);
    return parent && IsCremator(parent);
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    InstallWatchName(L"Open", kTagDoorOpen, g_regDoorOpen, "a lid's open (furnace door)");
    InstallWatchName(L"Close", kTagDoorClose, g_regDoorClose, "a lid's close (furnace door)");
    InstallWatchClass(L"cremator_C", L"actionOptionIndex", kTagAction, g_regAction,
                      &OnCrematorActionPost, "the cremator's actionOptionIndex");
    for (int i = 0; i < kFireVerbCount; ++i) {
        char what[96];
        std::snprintf(what, sizeof what, "the cremator's candidate fire verb %d", i);
        InstallWatchClass(L"cremator_C", kFireVerbs[i], kTagFireVerbBase + i, g_regFireVerb[i],
                          &OnFireVerbPost, what);
    }
    // subsystems::Install is a retry pump re-entered every net_pump tick; the watches
    // above are latched, so say so once. (The Oct 22 logs carried 31k copies of this line.)
    static bool s_announced = false;
    if (!s_announced) {
        s_announced = true;
        UE_LOGI("furnace: installed (door open/close, cremator action, %d fire-verb candidates)",
                kFireVerbCount);
    }
}

void Tick() {
    sg::ResolvePendingNames();
    if (!g_settled) SettleWatches();
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (!EnsureResolved()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastDoorIndex >= std::chrono::seconds(5)) {
        g_lastDoorIndex = now;
        IndexDoors();
    }
    if (s->role() == coop::net::Role::Host) {
        // Fire poll: catches edges the verb watches missed (e.g. an undiscovered fire
        // verb) and re-checks the occupant while the fire is on.
        if (now - g_lastFirePoll >= std::chrono::milliseconds(500)) {
            g_lastFirePoll = now;
            FirstCtx fc{};
            OI::ForEachInstance(g_crematorCls, FirstInstanceFn, &fc);
            if (fc.out) RecheckFire(fc.out, "poll");
        }
    }
    BurnWatchdog(now, s->role() == coop::net::Role::Host);
}

void OnDisconnect() {
    UE_LOGI("furnace: session end -- door edges %llu, fire rechecks %llu, cremator actions %llu, "
            "burn clears %llu, packets sent %llu", static_cast<unsigned long long>(g_doorEdges),
            static_cast<unsigned long long>(g_fireRechecks),
            static_cast<unsigned long long>(g_actions), static_cast<unsigned long long>(g_clears),
            static_cast<unsigned long long>(g_sends));
    g_doorEdges = g_fireRechecks = g_actions = g_clears = g_sends = 0;
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = ConnectedAs(coop::net::Role::Host);
    if (!s) return;
    if (peerSlot < 1 || peerSlot >= coop::players::kMaxPeers) return;
    if (!EnsureResolved()) return;
    IndexDoors();
    struct SnapCtx {
        coop::net::Session* s;
        int slot;
        int sent = 0;
    };
    SnapCtx c{s, peerSlot};
    // One furnace per world; iterate its instances and snapshot each by key.
    struct SnapFn {
        static void Call(void* ctx, void* obj, int32_t /*index*/) {
            SnapCtx* c = static_cast<SnapCtx*>(ctx);
            const std::wstring key = P::GetActorSaveKeyString(obj);
            if (key.empty() || key == L"None") return;
            bool open = false;
            if (void* door = FindDoor(key)) SW::TryReadOpen(door, open);
            bool fireOn = false;
            ReadFire(obj, fireOn);
            const int occupant = fireOn ? DetectOccupant(obj) : -1;
            net::KeyedTogglePayload dp{};
            WireKeyFromString(key, dp.key);
            dp.action = open ? 1 : 0;
            net::FurnaceFirePayload fp{};
            WireKeyFromString(key, fp.key);
            fp.fireOn = fireOn ? 1 : 0;
            fp.occupantSlot = static_cast<int8_t>(occupant < -1 ? -1 : occupant);
            if (c->s->SendReliableToSlot(c->slot, net::ReliableKind::FurnaceDoorState, &dp,
                                         sizeof(dp)))
                ++c->sent;
            if (c->s->SendReliableToSlot(c->slot, net::ReliableKind::FurnaceFireState, &fp,
                                         sizeof(fp)))
                ++c->sent;
            UE_LOGI("furnace: connect-snapshot key='%ls' door=%s fire=%s occupant=%d -> slot %d",
                    key.c_str(), open ? "OPEN" : "SHUT", fireOn ? "ON" : "OFF", occupant,
                    c->slot);
        }
    };
    OI::ForEachInstance(g_crematorCls, SnapFn::Call, &c);
    UE_LOGI("furnace: connect-snapshot -- sent %d packet(s) to slot %d", c.sent, peerSlot);
}

void NotifyLocalDeath() {
    ClearLocalBurnFields("local-death");
    if (ConnectedAs(coop::net::Role::Host))
        SendBurnClear(coop::players::Registry::Get().LocalPlayerElementId(), -1);
}

void NotifyLocalRevive() {
    ClearLocalBurnFields("local-revive");
}

void ClearLocalBurn(const char* why) {
    ClearLocalBurnFields(why && *why ? why : "explicit");
}

// --- Receive handlers (net dispatch -> game thread) ---------------------------------------------

void OnDoorState(const net::KeyedTogglePayload& p, int senderSlot) {
    if (senderSlot != 0) {
        UE_LOGW("furnace: FurnaceDoorState from slot %d refused -- host-authored", senderSlot);
        return;
    }
    if (p.action != 0 && p.action != 1) {
        UE_LOGW("furnace: FurnaceDoorState action=%u out of range -- dropping",
                static_cast<unsigned>(p.action));
        return;
    }
    const std::wstring key = StringFromWireKey(p.key);
    if (key.empty()) {
        UE_LOGW("furnace: FurnaceDoorState with an empty key -- dropping");
        return;
    }
    const bool open = p.action == 1;
    ue_wrap::game_thread::Post([key, open] {
        if (ConnectedAs(coop::net::Role::Host)) {
            UE_LOGI("furnace: FurnaceDoorState arrived on the HOST -- dropping");
            return;
        }
        if (!EnsureResolved()) return;
        UE_LOGI("furnace: OnDoorState key='%ls' action=%s", key.c_str(),
                open ? "OPEN" : "SHUT");
        ApplyDoorState(key, open);
    });
}

// FurnaceDoorIntent=179, client -> host: drive the host's door verbs WITHOUT the apply
// guard, so the normal edge watch fires and broadcasts FurnaceDoorState to everyone.
void OnDoorIntent(const net::KeyedTogglePayload& p, int senderSlot) {
    if (senderSlot == 0) {
        UE_LOGW("furnace: FurnaceDoorIntent from the host -- dropping (clients only)");
        return;
    }
    if (p.action != 0 && p.action != 1) {
        UE_LOGW("furnace: FurnaceDoorIntent action=%u out of range -- dropping",
                static_cast<unsigned>(p.action));
        return;
    }
    const std::wstring key = StringFromWireKey(p.key);
    if (key.empty()) {
        UE_LOGW("furnace: FurnaceDoorIntent with an empty key -- dropping");
        return;
    }
    const bool open = p.action == 1;
    ue_wrap::game_thread::Post([key, open, senderSlot] {
        if (!ConnectedAs(coop::net::Role::Host)) {
            UE_LOGI("furnace: FurnaceDoorIntent arrived off-host -- dropping");
            return;
        }
        if (!EnsureResolved()) return;
        void* door = FindDoor(key);
        if (!door) {
            UE_LOGW("furnace: OnDoorIntent key='%ls' -- no door indexed, dropped", key.c_str());
            return;
        }
        // Deliberately NOT under g_applyingDoor: the edge watch must see this drive and
        // broadcast the resulting FurnaceDoorState.
        const bool ok = open ? SW::CallOpen(door, false) : SW::CallClose(door);
        UE_LOGI("furnace: OnDoorIntent key='%ls' from slot %d -> %s (%s)", key.c_str(),
                senderSlot, open ? "OPEN" : "SHUT", ok ? "ok" : "FAILED");
    });
}

void OnFireState(const net::FurnaceFirePayload& p, int senderSlot) {
    if (senderSlot != 0) {        UE_LOGW("furnace: FurnaceFireState from slot %d refused -- host-authored", senderSlot);
        return;
    }
    if (p.fireOn != 0 && p.fireOn != 1) {
        UE_LOGW("furnace: FurnaceFireState fireOn=%u out of range -- dropping",
                static_cast<unsigned>(p.fireOn));
        return;
    }
    if (p.occupantSlot < -1 || p.occupantSlot >= coop::players::kMaxPeers) {
        UE_LOGW("furnace: FurnaceFireState occupantSlot=%d out of range -- dropping",
                static_cast<int>(p.occupantSlot));
        return;
    }
    const std::wstring key = StringFromWireKey(p.key);
    if (key.empty()) {
        UE_LOGW("furnace: FurnaceFireState with an empty key -- dropping");
        return;
    }
    const bool fireOn = p.fireOn == 1;
    const int occupant = p.occupantSlot;
    ue_wrap::game_thread::Post([key, fireOn, occupant] {
        if (ConnectedAs(coop::net::Role::Host)) {
            UE_LOGI("furnace: FurnaceFireState arrived on the HOST -- dropping");
            return;
        }
        if (!EnsureResolved()) return;
        UE_LOGI("furnace: OnFireState key='%ls' fire=%s occupantSlot=%d", key.c_str(),
                fireOn ? "ON" : "OFF", occupant);
        ApplyFireState(key, fireOn, occupant);
    });
}

void OnBurnClear(const net::FurnaceBurnClearPayload& p, int senderSlot) {
    if (senderSlot != 0) {
        UE_LOGW("furnace: FurnaceBurnClear from slot %d refused -- host-authored", senderSlot);
        return;
    }
    const uint32_t elementId = p.playerElementId;
    ue_wrap::game_thread::Post([elementId] {
        const uint32_t local =
            coop::players::Registry::Get().LocalPlayerElementId();
        if (local != 0 && elementId != 0 && local != elementId) {
            UE_LOGI("furnace: FurnaceBurnClear for element 0x%08x is not me (0x%08x) -- ignoring",
                    elementId, local);
            return;
        }
        UE_LOGI("furnace: OnBurnClear elementId=0x%08x", elementId);
        ClearLocalBurnFields("FurnaceBurnClear-from-host");
    });
}

}  // namespace coop::furnace_sync
