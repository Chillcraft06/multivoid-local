// coop/world/weather_fog.cpp -- host-authoritative fog sync; see coop/world/weather_fog.h.

#include "coop/world/weather_fog.h"

#include "coop/net/protocol.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace coop::weather_fog {
namespace {

namespace P  = ue_wrap::profile;
namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::engine;

// Resolved once. spawnFog spawns the fog controller into the cycle's fog slot and drives the
// height-fog density; SetFogDensity pushes the final density into the cycle's height-fog
// component, used by the late-joiner snap so a written density takes effect on the apply
// frame. spawnBlackFog is the gamemode's black-fog birth verb, mirror-called on the client.
void* g_spawnFogFn      = nullptr;
void* g_setFogDensityFn = nullptr;  // the cycle's SetFogDensity (pushes the final density into the height fog)
void* g_spawnBlackFogFn = nullptr;  // AmainGamemode_C::spawnBlackFog
bool  g_installed  = false;
bool  g_clientInterceptorReg = false;

// The black-fog echo for the client birth-catch: ApplyFromHost raises it around its own
// spawnBlackFog call so the wire-commanded birth passes weather_event_births' catch, which
// otherwise destroys every organic client birth of blackFog_C. Separate from the spawnFog
// echo: different verb, different birth class.
std::atomic<bool> g_blackFogEcho{false};

// The client echo-suppress for spawnFog: while false, the client's spawnFog interceptor
// cancels the blueprint body (the client never independently makes fog); ApplyFromHost sets
// it true around its own mirror spawn so that one call passes. The same shape as the rain
// sync's echo-suppress, and atomic to match it.
std::atomic<bool> g_spawnFogEchoSuppress{false};

// The current role, true on a client. The interceptor is gated at runtime on this, not at
// registration, so a process that reconnects with a different role cannot keep suppressing
// (a client-to-host reconnect goes inert on the host). Install updates it every call, before
// the latch early-out.
std::atomic<bool> g_isClient{false};

// The host detector cache and throttle. Game thread only; 0xFF means never sampled.
uint8_t   g_lastHostFogBits = 0xFF;
long long g_lastDetectMs    = 0;

// The client reconcile cache (set by ApplyFromHost, read by TickClientReconcile). The apply's
// clear runs only when the host broadcasts, and a clear and static host never re-broadcasts,
// so a rolling-fog actor that leaked during the pre-suppression connect window (before the
// interceptor latched) would ride out its duration while the host is clear. The heartbeat
// continuously re-asserts host-clear (the MTA weather pulse shape), independent of
// broadcasts. Cheap: a slot pointer read, no object-array walk.
bool      g_haveHostFog        = false;  // received >=1 host fog state
bool      g_lastHostFogActive  = false;  // host's last-known rolling-fog presence (kFogActive)
bool      g_lastHostBlackFogActive = false;  // host's last-known black-fog presence (kBlackFogActive)
long long g_lastReconcileMs    = 0;

// The super-fog presence cache. The 3 Hz fog-edge detector once counted objects by class, a
// full object-array walk with a string allocation per entry, the single biggest mod cost on
// the host. Super fog is a rare, minutes-long encounter, so: cache the actor and revalidate
// by index (constant time, and it catches the despawn edge every detection), and walk to
// find a freshly spawned one only on a coarse throttle (a 5 s edge latency is fine for a
// multi-minute event, and the apply path is clear-only anyway).
void*     g_superFogActor   = nullptr;
int32_t   g_superFogIdx     = -1;
long long g_lastSuperScanMs = 0;

// The black-fog presence cache: the same shape as the super-fog one. Black fog is a rare,
// minutes-long event actor found by class (no cycle slot holds it), so the same coarse
// 5 s rescan applies; the actor-index revalidate catches the despawn edge every detection.
void*     g_blackFogActor   = nullptr;
int32_t   g_blackFogIdx     = -1;
long long g_lastBlackFogScanMs = 0;

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// True iff a live super-fog actor exists. Constant time when one is cached (an index
// revalidate); a single by-class find at most once per 5 s when none is cached.
bool HostHasSuperFog() {
    if (g_superFogActor && R::IsLiveByIndex(g_superFogActor, g_superFogIdx)) return true;
    g_superFogActor = nullptr;
    g_superFogIdx   = -1;
    const long long now = NowMs();
    if (now - g_lastSuperScanMs < 5000) return false;  // coarse rescan (super-fog is minutes-long)
    g_lastSuperScanMs = now;
    void* sf = R::FindObjectByClass(P::name::SuperFogClass);
    if (sf && R::IsLive(sf)) { g_superFogActor = sf; g_superFogIdx = R::InternalIndexOf(sf); return true; }
    return false;
}

// True iff a live black-fog actor exists. Same discipline as HostHasSuperFog: constant time
// on the cached index revalidate, a single by-class find at most once per 5 s otherwise.
bool HostHasBlackFog() {
    if (g_blackFogActor && R::IsLiveByIndex(g_blackFogActor, g_blackFogIdx)) return true;
    g_blackFogActor = nullptr;
    g_blackFogIdx   = -1;
    const long long now = NowMs();
    if (now - g_lastBlackFogScanMs < 5000) return false;
    g_lastBlackFogScanMs = now;
    void* bf = R::FindObjectByClass(P::name::BlackFogClass);
    if (bf && R::IsLive(bf)) { g_blackFogActor = bf; g_blackFogIdx = R::InternalIndexOf(bf); return true; }
    return false;
}

// Compute the host's active-fog bits from the live cycle. All reads are cheap: the fog slot
// is a pointer read, super fog goes through the cache, the permanent flag is a bool read.
uint8_t ComputeHostFogBits(void* cycle) {
    using namespace coop::net::fog_flags2;
    auto* b = reinterpret_cast<uint8_t*>(cycle);
    uint8_t f2 = 0;
    if (*reinterpret_cast<void**>(b + P::off::AdaynightCycle_fogEventObject) != nullptr)
        f2 |= kFogActive;
    if (HostHasSuperFog())
        f2 |= kSuperFogActive;
    if (HostHasBlackFog())
        f2 |= kBlackFogActive;
    if (*reinterpret_cast<bool*>(b + P::off::AdaynightCycle_permanentFog))
        f2 |= kPermanentFog;
    return f2;
}

// The live (non-CDO) mainGamemode instance: the event_trigger ambient-verb pattern -- first
// live object of the class that is not a Default__. The black-fog mirror spawn calls the
// gamemode's own verb on it, maximally native.
void* FindLiveGamemode() {
    for (void* obj : R::FindObjectsByClass(P::name::GamemodeClass)) {
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
            return obj;
    }
    return nullptr;
}

// The spawnFog interceptor; true cancels the blueprint body. Cancels organic spawnFog only on
// a client (so a client-to-host reconnect cannot suppress the host's own fog) and only when
// ApplyFromHost is not mid mirror-spawn. On the host it is a pass-through.
bool OnSpawnFogPreSuppress(void* /*self*/, void* /*params*/) {
    return g_isClient.load(std::memory_order_acquire) &&
           !g_spawnFogEchoSuppress.load(std::memory_order_acquire);
}

}  // namespace

bool Install(bool isHost) {
    // Update the role every call, before the latch early-out: a process can reconnect with a
    // different role, and the interceptor is gated at runtime on it, so it goes inert on the
    // host without re-registration.
    g_isClient.store(!isHost, std::memory_order_release);
    if (g_installed) return true;

    void* cls = R::FindClass(P::name::DaynightCycleClass);
    if (!cls) return false;  // cycle class not loaded yet -- retry next tick.
    if (!g_spawnFogFn) g_spawnFogFn = R::FindFunction(cls, P::name::DaynightCycle_spawnFogFn);
    if (!g_spawnFogFn) return false;
    // SetFogDensity for the late-joiner density snap; best effort, since an unresolved verb only
    // delays the snapped density to the cycle's next tick.
    if (!g_setFogDensityFn) g_setFogDensityFn = R::FindFunction(cls, P::name::DaynightCycle_setFogDensityFn);
    // The black-fog mirror verb off the gamemode class; required like spawnFog, retried the
    // same way -- a client that cannot mirror-spawn must not latch installed.
    if (!g_spawnBlackFogFn) {
        if (void* gmCls = R::FindClass(P::name::GamemodeClass))
            g_spawnBlackFogFn = R::FindFunction(gmCls, P::name::MainGamemode_SpawnBlackFogFn);
    }
    if (!g_spawnBlackFogFn) return false;

    // Register the spawnFog interceptor once, for both roles (it self-gates on the role, a
    // pass-through on the host). Do not latch installed until it succeeds: a silently dropped
    // registration would leave a client able to make uncommanded fog, so return false to retry.
    if (!g_clientInterceptorReg) {
        if (!GT::RegisterInterceptor(g_spawnFogFn, &OnSpawnFogPreSuppress)) {
            UE_LOGW("weather_fog: spawnFog interceptor registration FAILED (table full?) "
                    "-- NOT latching; retry next tick");
            return false;
        }
        g_clientInterceptorReg = true;
        UE_LOGI("weather_fog: spawnFog PRE interceptor registered (@%p; role-gated on "
                "g_isClient + echo-suppressed -- a client never makes uncommanded fog)",
                g_spawnFogFn);
    }

    g_installed = true;
    return true;
}

void ReadHostFogState(void* cycle, coop::net::WeatherStatePayload& out) {
    if (!cycle || !R::IsLive(cycle)) return;
    out.flags2 = ComputeHostFogBits(cycle);
    // The late-joiner snap: stamp the host's current fog level so a joiner matches it at once
    // instead of ramping from zero. The final density is the visible (eased) height-fog density;
    // the rolling-fog actor's alpha is the ramp driver and its strength the per-spawn scale (the
    // thick fog is their product). Alpha and strength stay 0 with no rolling-fog actor, and the
    // receiver then skips the actor snap.
    auto* b = reinterpret_cast<uint8_t*>(cycle);
    out.finalFogDensity = *reinterpret_cast<float*>(b + P::off::AdaynightCycle_finalFogDensity);
    void* actor = *reinterpret_cast<void**>(b + P::off::AdaynightCycle_fogEventObject);
    if (actor && R::IsLive(actor)) {
        auto* ab = reinterpret_cast<uint8_t*>(actor);
        out.fogAlpha    = *reinterpret_cast<float*>(ab + P::off::WeatherFogController_Alpha);
        out.fogStrength = *reinterpret_cast<float*>(ab + P::off::WeatherFogController_Strength);
    }
}

void ApplyFromHost(void* cycle, const coop::net::WeatherStatePayload& payload) {
    if (!GT::IsGameThread()) return;
    if (!cycle || !R::IsLive(cycle)) return;
    using namespace coop::net::weather_flags;  // kEnableFog, kEnableSuperfog
    using namespace coop::net::fog_flags2;      // kFogActive, kSuperFogActive, kPermanentFog
    auto* b = reinterpret_cast<uint8_t*>(cycle);

    const bool hostFogActive   = (payload.flags2 & kFogActive)   != 0;
    const bool hostSuperActive = (payload.flags2 & kSuperFogActive) != 0;

    // The cache for the client reconcile heartbeat: a clear and static host will not
    // re-broadcast, so the heartbeat re-asserts host-clear from this snapshot to clear a fog
    // actor that leaked the pre-suppression window.
    g_haveHostFog       = true;
    g_lastHostFogActive = hostFogActive;

    // Rolling fog: assert the host's actor presence (a pulse, no diff-skip).
    void** slot = reinterpret_cast<void**>(b + P::off::AdaynightCycle_fogEventObject);
    const bool clientHasRolling = (*slot != nullptr) && R::IsLive(*slot);
    if (!hostFogActive && clientHasRolling) {
        // Host clear: destroy the client's stray or mirror rolling fog. The cycle's own tick then
        // eases the final density back to the shared time-of-day ambient (the destroyed actor was
        // the only thing pushing the density target up).
        E::DestroyActor(*slot);
        *slot = nullptr;
        UE_LOGI("weather_fog: host CLEAR -> destroyed client rolling-fog actor");
    } else if (hostFogActive) {
        // Host fog: ensure the client's own rolling-fog actor exists (echo-suppressed past the
        // client interceptor), then snap it to the host's current level. Without the snap a fresh
        // mirror actor ramps its density from zero over its duration, minutes, the late joiner's
        // warm-up. Writing the actor's alpha is accepted (a plain accumulator, not timeline-locked)
        // and the actor keeps ramping from the written value, so the mirror tracks the host in
        // lockstep.
        if (!clientHasRolling && g_spawnFogFn) {
            g_spawnFogEchoSuppress.store(true, std::memory_order_release);
            ue_wrap::ParamFrame f(g_spawnFogFn);
            ue_wrap::Call(cycle, f);
            g_spawnFogEchoSuppress.store(false, std::memory_order_release);
            UE_LOGI("weather_fog: host FOG -> mirror-spawned client rolling-fog actor");
        }
        // Copy the host actor's ramp state (alpha the intensity, strength the per-spawn density
        // scale; their product is recomputed by the actor's own tick). Re-read the slot, since the
        // spawn above just populated it. Guard on non-zero, so a host with no actor (a race) does
        // not zero the client's.
        void* actor = *slot;
        if (actor && R::IsLive(actor) &&
            (payload.fogAlpha != 0.f || payload.fogStrength != 0.f)) {
            auto* ab = reinterpret_cast<uint8_t*>(actor);
            *reinterpret_cast<float*>(ab + P::off::WeatherFogController_Alpha)    = payload.fogAlpha;
            *reinterpret_cast<float*>(ab + P::off::WeatherFogController_Strength) = payload.fogStrength;
        }
        // The visible height-fog density snap is unconditional below: a host-clear apply also pins
        // the client's base haze, not just the fog case.
        UE_LOGI("weather_fog: host FOG snap -> Alpha=%.4f Strength=%.4f finalFogDensity=%.4f",
                payload.fogAlpha, payload.fogStrength, payload.finalFogDensity);
    }

    // Super fog: clear-only (destroy a stray when the host has none). The spawn is deferred: the
    // super-fog actor owns a UFO encounter, and its spawn was not reliably reproducible in the
    // probe. A bounded loop, since the by-class find returns one.
    if (!hostSuperActive) {
        for (int k = 0; k < 8; ++k) {
            void* sf = R::FindObjectByClass(P::name::SuperFogClass);
            if (!sf || !R::IsLive(sf)) break;
            E::DestroyActor(sf);
            UE_LOGI("weather_fog: host CLEAR -> destroyed stray client super-fog actor");
        }
    }

    // Black fog: presence mirror through the gamemode's own verb (a pulse, no diff-skip).
    // Host fog + client none -> echo-raised spawnBlackFog on the live gamemode, so the
    // birth-catch passes the commanded birth; host clear + client live -> destroy the stray.
    // The actor owns its duration and self-destructs; there is no ramp state to snap, and no
    // cycle slot to null -- presence is by class, like the super fog.
    const bool hostBlackFogActive = (payload.flags2 & kBlackFogActive) != 0;
    g_lastHostBlackFogActive = hostBlackFogActive;
    {
        void* bf = R::FindObjectByClass(P::name::BlackFogClass);
        const bool clientHasBlackFog = bf && R::IsLive(bf);
        if (!hostBlackFogActive && clientHasBlackFog) {
            E::DestroyActor(bf);
            UE_LOGI("weather_fog: host CLEAR -> destroyed client black-fog actor");
        } else if (hostBlackFogActive && !clientHasBlackFog && g_spawnBlackFogFn) {
            if (void* gm = FindLiveGamemode()) {
                g_blackFogEcho.store(true, std::memory_order_release);
                ue_wrap::ParamFrame f(g_spawnBlackFogFn);
                ue_wrap::Call(gm, f);
                g_blackFogEcho.store(false, std::memory_order_release);
                UE_LOGI("weather_fog: host BLACKFOG -> mirror-spawned client black-fog actor");
            } else {
                UE_LOGW("weather_fog: host BLACKFOG but no live gamemode -- mirror spawn "
                        "deferred to the next apply");
            }
        }
    }

    // The base ambient height-fog density: snap to the host's value unconditionally (also when
    // the host is clear), then push it into the height-fog component. The cycle's clear-weather
    // floor is small but non-zero, and a freshly joined client reads zero until its own tick
    // catches up, so without this the client stays a touch clearer than the host even with no
    // fog actor; in the fog-actor case this is the same value the mirror's ramp targets.
    *reinterpret_cast<float*>(b + P::off::AdaynightCycle_finalFogDensity) = payload.finalFogDensity;
    if (g_setFogDensityFn) {
        ue_wrap::ParamFrame f(g_setFogDensityFn);
        ue_wrap::Call(cycle, f);
    }

    // The config bits: canonical static-config writes (no blueprint listeners need a UFunction
    // fan-out for these), mirroring the host exactly.
    *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_enable_fog)      = (payload.flags  & kEnableFog)      != 0;
    *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_enable_superfog) = (payload.flags  & kEnableSuperfog) != 0;
    *reinterpret_cast<bool*>(b + P::off::AdaynightCycle_permanentFog)    = (payload.flags2 & kPermanentFog)   != 0;
}

void TickClientReconcile(void* cycle) {
    // The client-only backstop: continuously enforce host-clear, so a fog actor that leaked
    // the pre-suppression connect window (which a clear and static host never re-broadcasts
    // to clear) does not persist. Runs only as the client and only after at least one host
    // fog state. Cheap: slot pointer reads and one by-class find; throttled to 3 s, since
    // fog is slow.
    if (!GT::IsGameThread()) return;
    if (!g_isClient.load(std::memory_order_acquire)) return;
    if (!g_haveHostFog) return;
    if (!cycle || !R::IsLive(cycle)) return;
    const long long now = NowMs();
    if (now - g_lastReconcileMs < 3000) return;
    g_lastReconcileMs = now;

    auto* b = reinterpret_cast<uint8_t*>(cycle);
    // Rolling fog: act only when the host's rolling fog is known-CLEAR -- with host fog the
    // legitimate mirror actor must survive.
    if (!g_lastHostFogActive) {
        void** slot = reinterpret_cast<void**>(b + P::off::AdaynightCycle_fogEventObject);
        if (*slot && R::IsLive(*slot)) {
            E::DestroyActor(*slot);
            *slot = nullptr;
            UE_LOGI("weather_fog: client reconcile -> destroyed stray rolling-fog actor (host is clear)");
        }
    }
    // Black fog: independent of the rolling fog -- the host can hold one while clear of the
    // other, so it reads its own last-known bit.
    if (!g_lastHostBlackFogActive) {
        if (void* bf = R::FindObjectByClass(P::name::BlackFogClass)) {
            if (R::IsLive(bf)) {
                E::DestroyActor(bf);
                UE_LOGI("weather_fog: client reconcile -> destroyed stray black-fog actor (host is clear)");
            }
        }
    }
}

bool MirrorEchoActive() {
    return g_spawnFogEchoSuppress.load(std::memory_order_acquire);
}

bool BlackFogEchoActive() {
    return g_blackFogEcho.load(std::memory_order_acquire);
}

bool HostFogStateChanged(void* cycle) {
    if (!cycle || !R::IsLive(cycle)) return false;
    // Throttle to 3 Hz: the super-fog check can walk the object array; never per frame.
    const long long now = NowMs();
    if (now - g_lastDetectMs < 300) return false;
    g_lastDetectMs = now;

    const uint8_t bits = ComputeHostFogBits(cycle);
    if (bits == g_lastHostFogBits) return false;
    g_lastHostFogBits = bits;
    return true;
}

void OnDisconnect() {
    g_spawnFogEchoSuppress.store(false, std::memory_order_release);
    g_blackFogEcho.store(false, std::memory_order_release);
    g_lastHostFogBits = 0xFF;
    g_lastDetectMs    = 0;
    g_superFogActor   = nullptr;  // session-scoped; a reconnect re-scans
    g_superFogIdx     = -1;
    g_lastSuperScanMs = 0;
    g_blackFogActor   = nullptr;  // session-scoped; a reconnect re-scans
    g_blackFogIdx     = -1;
    g_lastBlackFogScanMs = 0;
    g_haveHostFog       = false;  // the next session re-learns the host's fog state
    g_lastHostFogActive = false;
    g_lastHostBlackFogActive = false;
    g_lastReconcileMs   = 0;
    // The installed and registered flags stay set across sessions: the spawnFog function is
    // stable (the same class) and the interceptor self-gates at runtime on the role, which
    // Install refreshes every call, so a reconnect with a different role needs no re-resolve or
    // re-register. The interceptor is also conditional on the echo flag (false outside the
    // apply). The rain sync keeps its interceptor the same way.
}

}  // namespace coop::weather_fog
