// coop/furnace/furnace_sync.h -- the furnace (VOTV's cremator) sync: door state, fire
// state + occupant, damage ownership, and the stuck-burn clear.
//
// WHAT THIS IS
// A live playtest (2026-10-13) showed the furnace desynced across peers: the host locked
// their friend in the furnace and turned it on, the friend was unharmed while the host
// (the presser) took the damage and died, the host respawned at the gate with permanent
// burn audio + camera jerking + visual burning but no continued damage, and the friend lost
// their mesh and appeared as fire particles. A save/reload cleared it.
//
// WHAT WE KNOW (fact, from the tree)
// - The furnace is Acremator_C, an Aactor_save_C descendant with a save-persistent Key
//   (readable through ue_wrap::prop::GetActorSaveKeyString; see prop.h's note that the
//   generic three-lineage reader reported None for it).
// - Its door is a prop_swinger_C child actor (docs/coop-dispatch-visibility.md: the
//   cremator's setClosed calls one of the lid's open(bool)/close() verbs; the lid's own
//   state is `opened`). The door actor itself is top-level, runtime-created and keyless
//   (portable_identity.h), so it cannot be keyed by its own Key: every furnace packet is
//   keyed by the FURNACE actor's Key instead.
// - Each pawn carries the burn state as two fields: mainPlayer_C `isBurning` (bool) and
//   `burningTime` (float), both readable through ue_wrap::vitals (Burning is ReadOnly
//   there; this module writes them directly through reflection, scoped to those two).
//
// WHAT IS HYPOTHESIS (unverified, needs the live two-client test)
// - The wrong-player damage: the fire damaged the PRESSER rather than the OCCUPANT. The
//   correction below assumes damage follows the burn flags on a pawn; if the cremator
//   damages through some other path (a stored player pointer, a trigger volume), the
//   occupant-slot lane still carries the truth but the ownership correction may need a
//   different apply point. Flagged as hypothesis everywhere it matters.
// - The fire on/off verb and bool on Acremator_C are NOT known: no source in the tree
//   names them. This module DISCOVERS them at runtime (candidate lists in the .cpp,
//   every attempt probe-logged), re-reads the fire state after every cremator
//   interaction, and polls it on a timer. If nothing resolves, the fire lane logs that
//   it is blind and says exactly what the live session must confirm.
// - The burn visuals/audio/camera shake are assumed to be driven by the pawn's
//   isBurning/burningTime; the stuck-burn clear therefore writes exactly those two
//   fields and nothing else. Scoped by design: no global audio or camera resets.
//
// AUTHORITY
// Host-authored, never relayed. The host watches the furnace's door and fire edges and
// sends FurnaceDoorState (173) and FurnaceFireState (174) to all; clients apply. A
// client's own furnace interaction currently runs natively on that client (there is no
// client-to-host intent kind yet; see the report note), so the host's broadcast is the
// convergence point. FurnaceBurnClear (175) goes host -> the stuck player's slot (or all)
// and clears that pawn's burn flags locally.
//
// THREADING
// Install/Tick/QueueConnectBroadcastForSlot and the verb watches run on the game thread.
// The three On* handlers are called from the net dispatch; they validate and post to the
// game thread via ue_wrap::game_thread::Post, like every other lane.

#pragma once

#include "coop/net/protocol.h"  // KeyedTogglePayload, FurnaceFirePayload, FurnaceBurnClearPayload

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::furnace_sync {

// Register the furnace verb watches (door open/close, the cremator's interaction, the
// fire-verb candidates). Idempotent; safe to call once per session. Game thread.
void Install(coop::net::Session* session);

// Per-tick: settle the watches, run fire discovery, poll the fire state on the host,
// re-check the occupant while the fire is on, and run the stuck-burn watchdog.
// Game thread.
void Tick();

// The session ended: settle logs and counters. Game thread.
void OnDisconnect();

// Join snapshot (HOST): the current door state and fire state (+ occupant) for every
// indexed furnace, sent to one slot. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// The local player died: clear the local burn flags and, on the host, broadcast
// FurnaceBurnClear so a stuck peer clears too. Called from death_revive. Game thread.
void NotifyLocalDeath();

// The local player revived: clear the local burn flags again (the revive re-arm window).
// Called from death_revive. Game thread.
void NotifyLocalRevive();

// Clear the LOCAL pawn's burn flags (isBurning=false, burningTime=0), scoped to those
// two fields only. `why` names the caller for the probe log. Game thread.
void ClearLocalBurn(const char* why);

// --- Receive handlers (net dispatch -> game thread) ---------------------------------
// Each validates value ranges and trust-gates: the sender must be the host (slot 0).
// The coordinator wires these into the dispatch switch; they post to the game thread.
void OnDoorState(const net::KeyedTogglePayload& p, int senderSlot);
void OnFireState(const net::FurnaceFirePayload& p, int senderSlot);
void OnBurnClear(const net::FurnaceBurnClearPayload& p, int senderSlot);
// Client-to-host door intent (FurnaceDoorIntent=179): the sender must be a client (slot != 0)
// and this peer must be the host. Drives the host's door verbs WITHOUT the apply guard so
// the normal edge watch fires and broadcasts FurnaceDoorState to everyone.
void OnDoorIntent(const net::KeyedTogglePayload& p, int senderSlot);

}  // namespace coop::furnace_sync
