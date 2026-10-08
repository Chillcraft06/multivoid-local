// coop/world/balance_sync.h -- shared host-authoritative credit balance (saveSlot.Points).
//
// The HOST owns the canonical balance. It POLLS Points each game-thread tick -- catching every
// writer: shop orders, signal-disk sells, task rewards, the +1000 dev button -- and broadcasts
// BalanceSync on CHANGE, and to a joining client on its connect edge, so every peer MIRRORS the
// host's balance. The CLIENT writes the host's absolute value directly, with no AddPoints
// side-effects -- the MTA server-economy shape. SetSession, Tick and the connect edge run on the
// net-pump game thread, and a received value is stashed for Tick to write.
//
// THE WIRE IS HOST-AUTHORITATIVE. BalanceSync itself is one-way, host to client: the host polls
// Points and broadcasts the absolute value. The one exception is ClientCredit: a connected client
// may send a BOUNDED, rate-limited ADD -- never a set, never a debit -- naming an amount its own
// game just credited locally (a point sack, a chest, an achievement, an E-press coin) that no
// other lane carries. The host validates the bound and the rate and applies it through AddPoints;
// the next poll broadcasts the new total, so a client's earnings survive instead of being erased
// by the broadcast. This is the bounded successor to the retired BalanceDelta: the old lane let
// any peer set the balance to anything; this one only ever adds, within a bound, at a limited
// rate. (The old lane's other successor is OrderRequest, where the client sends list_store row
// names and the HOST prices and charges the order -- coop/items/order_sync.)

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::balance_sync {

void SetSession(coop::net::Session* session);

// Per-tick (net_pump game thread): on the HOST, poll Points + broadcast BalanceSync on
// change; on a CLIENT, retry applying the latest pending host balance until the saveSlot
// resolves (a connect-edge sync can arrive before the gamemode loads). No-op when not
// connected.
void Tick();

// HOST: send the current balance to ONE slot, right now. Two callers, one reason -- the
// change-polled broadcast in Tick() fires only when the host's Points MOVES, so anything that needs
// a specific peer to converge while the value is STATIC has to say so directly:
//   * the connect edge, since a joiner's balance would otherwise stay wrong until the host next
//     spent or earned something;
//   * an order REFUSAL, because the host's balance does not move when it refuses, yet the client
//     has already debited itself: the laptop's order button calls `lib_C::addPoints(-price)` before
//     the order is ever forwarded, and that call is EX_LocalVirtualFunction, so it cannot be
//     suppressed. Without this send the phantom debit persists until the host's balance happens to
//     change.
// The name says the ACTION rather than one trigger, because naming a trigger is how the refusal
// path came to be designed around a second function that did the same thing.
void SendCurrentToSlot(int slot);

// RECEIVER (client): take the host's ABSOLUTE balance for saveSlot.Points. No-op on the host
// (authoritative). The write itself is Tick's, on the game thread: this stashes the value, and Tick
// retries the write until the saveSlot resolves.
void ApplyFromHost(int32_t total);

// Apply a LOCAL credit (the +1000 dev button) on the host or solo, through AddPoints. On a
// connected CLIENT this REFUSES and logs: the dev button is not an earning, and the only
// client-to-host economy write is ClientCredit for observed game earnings (see OnClientCredit).
// Safe from the render thread (the menu).
void CreditLocal(int32_t amount);

// HOST receiver (event_feed thread): a client's observed local earning. Validates the
// per-intent bound and the per-slot rate, then credits the shared balance through AddPoints on
// the game thread; the next Tick poll broadcasts the new total. Drops anything out of bound or
// over rate with a log line. No-op unless this peer is the host.
void OnClientCredit(const uint8_t* payload, int len, uint8_t senderSlot);

// GAME THREAD: note that a forwarded coin collect (CoinCollect) is about to credit this client
// locally by `amount`. The E-press entry is uncancellable, so after the gate the game's own
// credit runs here; the host separately performs the forwarded collect. The earnings poll in
// Tick consumes this note before forwarding, so one coin credits the shared balance once, not
// twice. (The overlap entry cancels the local credit outright, so it never notes.)
void NoteForwardedCredit(int32_t amount);

// Reset the broadcast dedup on session teardown (so a reconnect re-broadcasts).
void OnDisconnect();

}  // namespace coop::balance_sync
