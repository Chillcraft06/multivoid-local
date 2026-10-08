// coop/world/balance_sync.cpp -- see coop/world/balance_sync.h.

#include "coop/world/balance_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/roster_ledger.h"
#include "ue_wrap/world/economy.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace coop::balance_sync {
namespace {

namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::economy;

std::atomic<coop::net::Session*> g_session{nullptr};

// Host broadcast dedup -- game-thread only (Tick host branch). Sentinel via a separate
// flag so any int32 Points value (including 0 / negative) is a legitimate balance.
bool    g_haveSent = false;
int32_t g_lastSent = 0;

// Client mirror: the latest host balance to apply, RETRIED each tick until the saveSlot
// resolves -- a connect-edge BalanceSync can arrive before the client's gamemode/saveSlot
// is loaded (the WritePoints would otherwise silently fail + the mirror never lands).
// Atomic because ApplyFromHost runs off the event_feed dispatch while Tick applies on the
// game thread. Latest-wins (a newer BalanceSync overwrites a not-yet-applied pending).
std::atomic<int32_t> g_pendingTotal{0};
std::atomic<bool>    g_havePending{false};

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- Client earnings forwarding (the equality lane) ----
//
// A connected client's local game credits it for things no intent lane carries: point sacks,
// chests, achievements, coins the E-press credited. Without a lane those earnings die at the
// host's next BalanceSync. So the Tick client branch diffs the live Points against this
// baseline; a positive delta is an observed earning and goes to the host as ClientCredit.
// A negative delta is a phantom debit (the laptop order button's addPoints(-price), which is
// EX_LocalVirtualFunction and cannot be suppressed); it is never forwarded -- the host's
// broadcast heals it.
//
// Bounds are anti-runaway, not anti-cheat: this is a trusted small-group lane, and the caps
// exist to catch baseline corruption, not to police the players.
int32_t g_earnBaseline = 0;
bool    g_earnBaselineInit = false;

// Suppression for forwarded coin collects: the E-press entry forwards (CoinCollect) AND its
// local credit still runs (uncancellable), so NoteForwardedCredit records the coin's value here
// and the earnings poll consumes it before forwarding -- one coin, one credit. The overlap
// entry cancels the local credit outright and never notes. Entries expire: a note whose credit
// never landed must not eat a later real earning.
int32_t   g_suppressedCoinCredit = 0;
long long g_suppressedAtMs = 0;

constexpr int32_t   kMaxClientCredit = 1000000;  // per-intent bound
constexpr uint32_t  kMaxCreditsPerWindow = 20;   // per-slot: intents per window
constexpr int64_t   kMaxCreditSumPerWindow = 5000000;  // per-slot: total per window
constexpr long long kCreditWindowMs = 60000;
constexpr long long kSuppressionTtlMs = 5000;

void ForwardLocalEarnings(coop::net::Session* s) {
    int32_t local = 0;
    if (!E::ReadPoints(&local)) return;  // not resolved yet (booting / at menu)
    if (!g_earnBaselineInit) {
        // First sighting: adopt, don't forward. Anything earned before we started watching is
        // already inside this number and indistinguishable from the starting balance.
        g_earnBaseline = local;
        g_earnBaselineInit = true;
        return;
    }
    const int32_t delta = local - g_earnBaseline;
    g_earnBaseline = local;  // always advance: phantom debits must not accumulate either
    if (delta <= 0) return;

    // Consume the coin-collect suppression first (see NoteForwardedCredit).
    int32_t forward = delta;
    if (g_suppressedCoinCredit > 0) {
        if (NowMs() - g_suppressedAtMs >= kSuppressionTtlMs) {
            g_suppressedCoinCredit = 0;  // stale: the credit never landed; don't eat earnings
        } else {
            const int32_t eaten = forward < g_suppressedCoinCredit ? forward : g_suppressedCoinCredit;
            forward -= eaten;
            g_suppressedCoinCredit -= eaten;
        }
    }
    if (forward <= 0) {
        UE_LOGI("balance_sync: local earning %+d covered by a forwarded coin credit -- not re-forwarded", delta);
        return;
    }
    if (forward > kMaxClientCredit) {
        UE_LOGW("balance_sync: local earning %+d exceeds per-intent bound %d -- DROPPING (baseline kept)",
                forward, kMaxClientCredit);
        return;
    }
    coop::net::ClientCreditPayload p{};
    p.amount = forward;
    s->SendReliable(coop::net::ReliableKind::ClientCredit, &p, sizeof(p));
    UE_LOGI("balance_sync: client earning %+d forwarded as ClientCredit (local=%d)", forward, local);
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (s->role() == coop::net::Role::Host) {
        // HOST: poll the canonical balance; broadcast on change (dedup).
        int32_t points = 0;
        if (!E::ReadPoints(&points)) return;  // not resolved yet (booting / at menu)
        if (g_haveSent && points == g_lastSent) return;  // unchanged -> no broadcast
        g_haveSent = true;
        g_lastSent = points;
        coop::net::BalancePayload p{points};
        s->SendReliable(coop::net::ReliableKind::BalanceSync, &p, sizeof(p));
        UE_LOGI("balance_sync: host Points -> %d, broadcast BalanceSync to all", points);
    } else {
        // CLIENT, two jobs, in this order: the mirror first (so the earnings baseline below
        // starts from truth), then the earnings forward.
        if (g_havePending.load(std::memory_order_acquire)) {
            const int32_t total = g_pendingTotal.load(std::memory_order_acquire);
            if (E::WritePoints(total)) {
                // WritePoints is side-effect-free, so the on-screen credit number would stay frozen:
                // text_points is push-updated only by the blueprint's addPoints -> SetText. Re-run that
                // repaint with a zero add so the mirrored value actually shows.
                E::RefreshPointsHud();
                g_havePending.store(false, std::memory_order_release);
                g_earnBaseline = total;
                g_earnBaselineInit = true;
                UE_LOGI("balance_sync: mirrored host balance -> Points=%d (HUD repainted)", total);
            }
            // else: saveSlot still resolving -- silently retry next tick.
        }
        ForwardLocalEarnings(s);
    }
}

void SendCurrentToSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    int32_t points = 0;
    if (!E::ReadPoints(&points)) {
        UE_LOGW("balance_sync: direct send to slot %d -- Points not resolved yet (no replay)", slot);
        return;
    }
    coop::net::BalancePayload p{points};
    s->SendReliableToSlot(slot, coop::net::ReliableKind::BalanceSync, &p, sizeof(p));
    UE_LOGI("balance_sync: sent current Points=%d directly to slot %d", points, slot);
}

void ApplyFromHost(int32_t total) {
    if (IsHost()) return;  // the host is authoritative -- never mirror our own broadcast
    // Stash the latest; Tick (game thread) writes it + RETRIES until the saveSlot
    // resolves (a connect-edge BalanceSync can arrive before the gamemode is ready).
    g_pendingTotal.store(total, std::memory_order_release);
    g_havePending.store(true, std::memory_order_release);
}

void NoteForwardedCredit(int32_t amount) {
    if (amount <= 0) return;
    g_suppressedCoinCredit += amount;
    g_suppressedAtMs = NowMs();
    UE_LOGI("balance_sync: noted forwarded coin credit %d (suppression now %d)", amount,
            g_suppressedCoinCredit);
}

void OnClientCredit(const uint8_t* payload, int len, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGW("balance_sync: ClientCredit received off the HOST -- dropping");
        return;
    }
    if (!payload || len < static_cast<int>(sizeof(coop::net::ClientCreditPayload))) {
        UE_LOGW("balance_sync: ClientCredit payload too small (len=%d) -- dropping", len);
        return;
    }
    coop::net::ClientCreditPayload p{};
    std::memcpy(&p, payload, sizeof(p));
    if (p.amount <= 0 || p.amount > kMaxClientCredit) {
        UE_LOGW("balance_sync: ClientCredit from slot=%u amount=%d out of (0, %d] -- dropping",
                static_cast<unsigned>(senderSlot), p.amount, kMaxClientCredit);
        return;
    }
    // Per-slot sliding-window rate limit: count and sum. Reached from the trust boundary at
    // whatever rate a sender likes, so the refusal is the whole defence here.
    struct Window { long long startMs = 0; uint32_t count = 0; int64_t sum = 0; };
    static coop::roster_ledger::PerSlotState<Window> s_windows;
    const long long nowMs = NowMs();
    Window& w = s_windows[senderSlot];
    if (nowMs - w.startMs >= kCreditWindowMs) { w.startMs = nowMs; w.count = 0; w.sum = 0; }
    if (w.count >= kMaxCreditsPerWindow || w.sum + p.amount > kMaxCreditSumPerWindow) {
        UE_LOGW("balance_sync: ClientCredit from slot=%u REFUSED -- rate limit (count=%u sum=%lld in window)",
                static_cast<unsigned>(senderSlot), w.count, w.sum);
        return;
    }
    w.count++;
    w.sum += p.amount;
    // Credit on the game thread, like CreditLocal does; the next Tick poll sees the new Points
    // and broadcasts it to every peer, the earner included.
    GT::Post([amount = p.amount, senderSlot] {
        E::AddPoints(amount);
        UE_LOGI("balance_sync: applied ClientCredit +%d from slot=%u", amount,
                static_cast<unsigned>(senderSlot));
    });
}

void CreditLocal(int32_t amount) {
    auto* s = g_session.load(std::memory_order_acquire);
    const bool host      = s && s->role() == coop::net::Role::Host;
    const bool connected = s && s->connected();
    if (host || !connected) {
        // Host OR solo (no live session): apply locally. If hosting, the next TickHost
        // poll sees the new Points and broadcasts it to all clients.
        GT::Post([amount] { E::AddPoints(amount); });
        UE_LOGI("balance_sync: local credit %+d (%s)", amount,
                host ? "host -> will broadcast" : "solo");
        return;
    }
    // A connected CLIENT cannot AUTHOR the shared balance, and that half is deliberate: the lane
    // that let it -- a delta the host applied through AddPoints with no value bound -- was retired
    // whole rather than clamped, because the balance is host-authoritative everywhere. Game
    // earnings reach it through ClientCredit (bounded, rate-limited, add-only); the dev button is
    // not an earning, so a direct credit from here is refused: defence in depth, since
    // coop::dev_gate already refuses the only caller on a client.
    UE_LOGW("balance_sync: local credit %+d REFUSED -- a client cannot author the shared "
            "balance; game earnings travel as ClientCredit", amount);
}

void OnDisconnect() {
    g_haveSent = false;
    g_lastSent = 0;
    g_havePending.store(false, std::memory_order_release);
    g_earnBaselineInit = false;
    g_earnBaseline = 0;
    g_suppressedCoinCredit = 0;
}

}  // namespace coop::balance_sync
