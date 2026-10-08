// coop/world/weather_fog.h -- host-authoritative FOG sync (MTA CBlendedWeather::DoPulse).
//
// Fog is not a flag: enable_fog/enable_superfog are persistent CONFIG gates; the ACTIVE fog
// is an event ACTOR (AweatherFogController_C in the cycle's fogEventObject slot, AsuperFog_C
// and AblackFog_C found by class), each ramping height-fog density over its own Duration then
// destroying itself. BASE ambient density is shared and time-of-day driven, so host-clear to
// client-clear is actor destruction only, never a density float. flags2 carries kFogActive,
// kSuperFogActive, kBlackFogActive, kPermanentFog; clients suppress their own spawnFog and
// assert the host's bits. The rolling actor's END has no observable UFunction, so
// HostFogStateChanged supplies the edge; super-fog spawn is deferred (clear-only), black fog
// mirror-spawns through the gamemode's spawnBlackFog with the echo raised. Player page:
// docs/events-and-weather.md.

#pragma once

namespace coop::net { struct WeatherStatePayload; }

namespace coop::weather_fog {

// Resolve spawnFog off daynightCycle_C and, on the CLIENT only, register the
// echo-suppressed spawnFog PRE interceptor (so the client never spawns fog except
// when ApplyFromHost asks). The clear is a plain K2_DestroyActor (no SetFogDensity:
// density settles to the shared ambient via the cycle's own ReceiveTick), so
// spawnFog is the only ROLLING-fog UFunction resolved; spawnBlackFog is resolved
// off mainGamemode_C for the black-fog mirror spawn. Idempotent; safe every
// NetPumpTick. Returns true once spawnFog is resolved.
bool Install(bool isHost);

// HOST: stamp the active-fog bits into payload.flags2 from the live cycle -- kFogActive
// (fogEventObject is non-null), kSuperFogActive (a live AsuperFog_C), kBlackFogActive (a
// live AblackFog_C), kPermanentFog (the gamerule). Called from weather_sync::ReadCycleState.
// Game thread. Reads CountObjectsByClass for the super fog, but only on the occasional
// broadcast path, never per frame.
void ReadHostFogState(void* cycle, coop::net::WeatherStatePayload& out);

// CLIENT receiver: assert the host's fog state on the local cycle (MTA DoPulse,
// no diff-skip). Rolling fog: host clear + client actor live -> destroy + null
// the slot; host fog + client none -> echo-suppressed spawnFog(). Super fog:
// clear-only (destroy stray when host has none). Black fog: host fog + client
// none -> echo-raised spawnBlackFog() on the live gamemode; host clear + client
// live -> destroy the stray. Always mirror enable_fog / enable_superfog /
// permanentFog. Game thread. Called from weather_sync::ApplyFromHost.
void ApplyFromHost(void* cycle, const coop::net::WeatherStatePayload& payload);

// CLIENT per-tick reconcile heartbeat, throttled internally to about 3 s. When the host's
// last-known fog state was CLEAR, destroys any stray rolling-fog actor in the cycle's
// fogEventObject slot and any stray live blackFog_C: the MTA DoPulse backstop for an actor
// that leaked the pre-suppression connect window, which a clear and static host never
// re-broadcasts to clear. Self-gates to the client and host-clear; no-op otherwise.
// Game thread. Called from weather_sync::TickConnect.
void TickClientReconcile(void* cycle);

// HOST per-tick fog-edge detector (THROTTLED internally to ~3 Hz -- the super-fog
// check walks GUObjectArray). Returns true when the host's (kFogActive,
// kSuperFogActive, kBlackFogActive, kPermanentFog) tuple CHANGED since the last fire, so
// weather_sync re-broadcasts the (now fog-updated) state. This catches the
// rolling actor's silent self-destruct END that the scheduler observers miss.
// Game thread.
bool HostFogStateChanged(void* cycle);

// Disconnect hook: clear the echo-suppress flag + the cached host detector state.
void OnDisconnect();

// True while ApplyFromHost is mid mirror-spawn -- the spawnFog echo window. Read by
// coop/world/weather_event_births so the wire-commanded fog-controller birth passes the
// client birth-catch.
bool MirrorEchoActive();

// True while ApplyFromHost is mid black-fog mirror-spawn -- the spawnBlackFog echo window.
// Read by coop/world/weather_event_births so the wire-commanded blackFog_C birth passes
// the client birth-catch (which otherwise destroys every organic client birth of it).
bool BlackFogEchoActive();

}  // namespace coop::weather_fog
