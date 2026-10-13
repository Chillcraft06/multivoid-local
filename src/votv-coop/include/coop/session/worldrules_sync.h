// coop/session/worldrules_sync.h -- host-editable world rules: the F1 rules panel's write path
// and the WorldRuleState=178 lane.
//
// The game's own menu is host-only; the mod's panel was read-only, so the host could not change
// rules mid-session. This module is the write + sync half: the panel queues edits here, the game
// thread validates and writes them (ue_wrap::game_rules), the host broadcasts each accepted edit
// as one WorldRulePayload, and a joiner gets the full set at its world-ready edge. Clients apply
// to their own process + saved copies and re-render. Only the host's slot authors
// (senderSlot == 0); anything else is dropped. Convergence is host-wins by construction: only
// the host can author, and the join snapshot precedes the live edits that follow it.
//
// Wiring the coordinator owns: event_dispatch_entity.cpp routes ReliableKind::WorldRuleState to
// OnRuleState, and subsystems.cpp's ConnectReplayForSlot calls QueueConnectBroadcastForSlot(slot)
// (one line each, in those shared files). The edit drain needs no tick: the panel posts it to the
// game thread right after queueing.

#pragma once

#include <cstdint>
#include <string>

#include "coop/net/protocol.h"
#include "ue_wrap/world/game_rules.h"

namespace coop::worldrules_sync {

// The panel's edit intake, render-thread safe: one edit, validated on the game thread before
// anything is written. `kind` is the rule's own kind (only the matching value argument is read).
// The queue is bounded; false means it was full and the edit was dropped (logged).
bool RequestRuleEdit(const std::string& key, ue_wrap::game_rules::Kind kind, bool bval, int ival,
                     float fval);

// Game thread: drain the edit queue. Each edit is re-validated against the live rules (kind
// match, enum ordinal in the game's name list, float finite and clamped) and written to the
// process + saved copies. Accepted edits broadcast as WorldRuleState when this peer hosts a live
// session, and apply locally only when solo (no session). A client-role call is dropped.
void DrainEditQueue();

// Game thread, host only: the full rule set to one slot, one WorldRuleState per rule, at its
// world-ready edge. Called from subsystems.cpp's ConnectReplayForSlot.
void QueueConnectBroadcastForSlot(int slot);

// Game thread: one WorldRuleState arrived. Host-authored only: senderSlot != 0 is dropped, as is
// any kind outside 0..2, a non-finite float, a bool outside 0/1, or an enum ordinal the game
// does not name for that rule. Accepted rules are written to the process + saved copies and the
// change generation is bumped so the panel re-renders. The coordinator wires this to
// ReliableKind::WorldRuleState.
void OnRuleState(const net::WorldRulePayload& p, int senderSlot);

// Lock-free generation, bumped on every applied rule change (a local edit or a remote apply).
// The panel polls it and re-snapshots when it moves. Any thread.
uint32_t ChangeGeneration();

}  // namespace coop::worldrules_sync
