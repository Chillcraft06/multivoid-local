// coop/player/player_grab_sync.h -- player-to-player grab, drag and throw
// (ReliableKind::PlayerGrabState=176, PlayerGrabPayload).
//
// The lane is grabber-authored and host-relayed. The grabber watches its own physics-handle
// grab: when the held actor is a remote peer's ragdoll mirror body (RemotePlayer's spawned
// playerRagdoll_C), the grab edge, drag updates and the release/throw edge go out on the
// wire. The TARGET's client applies the drag to its own pawn -- it is the authority for its
// own position, and moving itself (rather than the grabber moving it) is what keeps the
// pose stream authoritative and prevents the snap-back to the pre-ragdoll position.
//
// Status: IMPLEMENTED, UNVERIFIED. No live two-client test has run; nothing here is known
// to work until one does. The hypotheses inside are labeled HYPOTHESIS in the .cpp.

#pragma once

#include <cstdint>

namespace coop::net { class Session; struct PlayerGrabPayload; }

namespace coop::playergrab_sync {

// Reset the send-side edge detectors and the receive-side drag latch. Called from
// local_streams::OnSessionStart (which net_pump invokes per session) so a stale grab edge
// or a stale drag latch cannot leak across a session restart.
void OnSessionStart();

// Per-tick worker, called from local_streams::Tick: game thread, session connected, pose
// authoritative. Does both directions:
//   SEND: edge-detects the local physics-handle grab of a peer's ragdoll mirror body and
//     emits action=1 (grab); while held, action=2 (drag) at ~12 Hz or on a significant
//     move, carrying the mirror body's position and the local yaw; on release, action=3
//     (throw, pos+vel) when the body is moving fast enough, else action=0 (release).
//   RECEIVE upkeep: expires a stale drag latch (a lost release, a departed grabber) and,
//     while a fresh drag latch is live, keeps the local player ragdolled (the stay-down
//     HYPOTHESIS -- see the .cpp).
void Tick(coop::net::Session& session, void* local);

// The wire receiver, wired by the coordinator's dispatch. Validates the action range
// (0-3), finite pos/vel/yaw floats, world-sanity bounds, and grabber != target; drops
// our own relayed echo. When the target is the local player the drag/throw is applied to
// the local pawn on the game thread (via game_thread::Post, so this stays correct whether
// the coordinator calls it on the game thread or a net thread). Third-party packets are
// observed (logged), never applied.
void OnGrabState(const net::PlayerGrabPayload& p, int senderSlot);

// Disconnect hook: clears the drag latch and the send-side edge state, so a dead
// session's grab cannot pin the local player down or leak a release onto the next one.
void OnDisconnect();

}  // namespace coop::playergrab_sync
