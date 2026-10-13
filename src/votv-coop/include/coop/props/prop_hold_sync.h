// coop/props/prop_hold_sync.h -- shared prop holds / tug-of-war
// (ReliableKind::PropHoldState=177, PropHoldPayload).
//
// The lane is holder-authored and host-relayed. When the local player grabs a keyed world
// prop, the grab edge goes out (action=1); when a second holder is announced for the same
// key, action=2 carries both holders; on release the holder count is recomputed back to 1
// or 0. Conflict rule (a DESIGN DECISION, not physics): the lowest element id among the
// holders is the streamer -- both holders converge on it with no negotiation, so exactly
// one PropPose stream drives the mirrors. The other holder is a contestant, not an
// override: it keeps its native local hold, but its pose stream for that prop is
// suppressed (see ShouldSuppressPropPose, read by local_streams).
//
// Keying: the prop's Key FName string (max 31 chars). Keyed world props only -- the
// trash clump's "None" key is not a cross-peer identity (every clump stringifies to it),
// and the hotbar hand item is player expression, not a world entity, so neither enters
// this lane.
//
// Status: IMPLEMENTED, UNVERIFIED. No live two-client test has run; nothing here is known
// to work until one does.

#pragma once

#include <cstdint>

namespace coop::net { class Session; struct PropHoldPayload; struct WireKey; }

namespace coop::prophold_sync {

// Reset the send-side edge detector, the holder table and any pending answers. Called
// from local_streams::OnSessionStart (which net_pump invokes per session) so a stale
// hold edge cannot leak across a session restart.
void OnSessionStart();

// Per-tick worker, called from local_streams::Tick: game thread, session connected, pose
// authoritative. Edge-detects the local grab of a keyed world prop and broadcasts the
// holder-set edges (action 1/0, or 2 when the table already lists another holder); drains
// the pending contest/de-contest answers queued by OnHoldState.
void Tick(coop::net::Session& session, void* local);

// The wire receiver, wired by the coordinator's dispatch. Validates holderCount (0-2),
// action (0-2) and the key length; maintains the per-key holder table. Answering is
// deferred to Tick (never sent from the dispatch path): an action=1 for a key we hold is
// answered with action=2 (both holders, ascending element id); an action=0 for a key we
// still hold is answered with action=1 (de-contest), so the set converges without a
// central authority.
void OnHoldState(const net::PropHoldPayload& p, int senderSlot);

// True when the local peer holds `key` AND the holder table lists another holder for it
// AND this peer is not the lowest element id among the holders: this peer is the
// contestant, so its PropPose stream for the prop is suppressed and the lowest-id
// holder's stream drives the mirrors. Read by local_streams before emitting a held pose;
// the hold itself (the cache, the release edge) is untouched.
bool ShouldSuppressPropPose(const coop::net::WireKey& key);

// Disconnect hooks: clear the holder table, the send-side edge state and pending
// answers. A departed peer's hold must not linger in the table and suppress a later
// holder's stream.
void OnDisconnect();
void OnDisconnectForSlot(int peerSlot);

}  // namespace coop::prophold_sync
