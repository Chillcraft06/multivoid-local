// coop/props/prop_hold_sync.cpp -- see coop/props/prop_hold_sync.h.
//
// How the lane works:
//   SEND: each tick the local grab is reduced to the same "held world prop" notion
//   local_streams uses (physics-handle grab first, holding_actor fallback when keyed;
//   ATVs and the hotbar hand item excluded). A keyed world prop's grab edge broadcasts
//   action=1 with just this peer; if the holder table already lists another holder, the
//   edge is action=2 with both holders (ascending element id). The release edge is
//   action=0.
//   RECEIVE: the per-key holder table is maintained (latest wins). An action=1 for a key
//   we hold is answered with action=2; an action=0 for a key we still hold is answered
//   with action=1 (de-contest). Answers are queued and drained on Tick, never sent from
//   the dispatch path.
//   CONFLICT RULE (DESIGN DECISION): the lowest element id among a key's holders is the
//   streamer -- every peer computes the same answer with no negotiation, so exactly one
//   PropPose stream drives the mirrors. The other holder is a contestant: its local hold
//   is untouched, but local_streams suppresses its pose stream for that prop via
//   ShouldSuppressPropPose.
//
// IMPLEMENTED, UNVERIFIED -- no live two-client test has run.

#include "coop/props/prop_hold_sync.h"

#include "coop/element/element.h"
#include "coop/element/player.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace coop::prophold_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// Wire actions (must match protocol.h's PropHoldPayload comment).
constexpr uint8_t kRelease = 0;
constexpr uint8_t kGrab    = 1;
constexpr uint8_t kContest = 2;

struct HolderEntry {
    uint32_t holders[2] = {0, 0};
    uint8_t  count = 0;
    uint8_t  action = 0;
};

// The announced holder set per prop key (the WireKey bytes as a string). Latest wins;
// entries are erased on action=0 and on peer disconnect.
std::map<std::string, HolderEntry> g_table;

// This peer's current hold in this lane: the key we announced, or nothing.
coop::net::WireKey g_myKey{};
bool g_myHeld = false;

// Contest/de-contest answers queued by OnHoldState, drained on Tick (never sent from
// the dispatch path).
std::vector<coop::net::PropHoldPayload> g_pendingAnswers;

std::string KeyString(const coop::net::WireKey& key) {
    return std::string(key.data, key.data + key.len);
}

bool KeyEqual(const coop::net::WireKey& a, const coop::net::WireKey& b) {
    if (a.len != b.len) return false;
    for (uint8_t i = 0; i < a.len; ++i)
        if (a.data[i] != b.data[i]) return false;
    return true;
}

uint32_t MyEid() {
    const coop::element::ElementId eid =
        coop::players::Registry::Get().LocalPlayerElementId();
    return static_cast<uint32_t>(eid);
}

// The same "held world prop" notion local_streams::Tick uses: the physics-handle grab
// first, holding_actor as the fallback when it is a keyed interactable that is not the
// hotbar hand item; ATVs are not props in this lane.
void* HeldWorldProp(void* local) {
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!local || !E::ReadMainPlayerGrabState(local, gs)) return nullptr;
    void* heldActor = nullptr;
    if (gs.grabbingActor && R::IsLive(gs.grabbingActor))
        heldActor = gs.grabbingActor;
    if (gs.holdingActor && R::IsLive(gs.holdingActor) &&
        ue_wrap::prop::IsKeyedInteractable(gs.holdingActor)) {
        if (ue_wrap::prop::IsDescendantOfProp(gs.holdingActor)) {
            // The hotbar hand item: player expression, respawned per switch -- not a
            // world entity, so not in this lane.
        } else if (!heldActor) {
            heldActor = gs.holdingActor;
        }
    }
    if (heldActor && ue_wrap::atv::IsAtv(heldActor)) heldActor = nullptr;
    return heldActor;
}

bool SendHold(coop::net::Session& session, const coop::net::WireKey& key, uint8_t action,
              const uint32_t* holders, uint8_t count) {
    const uint32_t myEid = MyEid();
    if (myEid == 0 || myEid == coop::element::kInvalidId) {
        UE_LOGW("prophold: not sending action=%u -- no local player element id yet",
                static_cast<unsigned>(action));
        return false;
    }
    coop::net::PropHoldPayload p{};
    p.key = key;
    for (uint8_t i = 0; i < count && i < 2; ++i) p.holderElementId[i] = holders[i];
    p.holderCount = count;
    p.action = action;
    const bool ok = session.SendReliable(
        coop::net::ReliableKind::PropHoldState, &p, sizeof(p));
    if (!ok) {
        UE_LOGW("prophold: SendReliable action=%u failed (channel busy?)",
                static_cast<unsigned>(action));
    } else {
        UE_LOGI("prophold: sent action=%u key='%.*s' holders=%u",
                static_cast<unsigned>(action), static_cast<int>(key.len), key.data,
                static_cast<unsigned>(count));
    }
    return ok;
}

// Queue a contest/de-contest answer for the Tick drain; latest wins per key.
void QueueAnswer(const coop::net::WireKey& key, uint8_t action, const uint32_t* holders,
                 uint8_t count) {
    const std::string ks = KeyString(key);
    for (auto& a : g_pendingAnswers) {
        if (KeyString(a.key) == ks) {
            a.action = action;
            a.holderCount = count;
            a.holderElementId[0] = count > 0 ? holders[0] : 0;
            a.holderElementId[1] = count > 1 ? holders[1] : 0;
            return;
        }
    }
    coop::net::PropHoldPayload p{};
    p.key = key;
    p.action = action;
    p.holderCount = count;
    if (count > 0) p.holderElementId[0] = holders[0];
    if (count > 1) p.holderElementId[1] = holders[1];
    g_pendingAnswers.push_back(p);
}

void DrainPending(coop::net::Session& session) {
    for (auto it = g_pendingAnswers.begin(); it != g_pendingAnswers.end();) {
        if (SendHold(session, it->key, it->action, it->holderElementId, it->holderCount))
            it = g_pendingAnswers.erase(it);
        else
            break;  // channel busy: retry the rest next tick, in order
    }
}

void EraseMeFromKey(const std::string& ks, uint32_t myEid) {
    auto it = g_table.find(ks);
    if (it == g_table.end()) return;
    HolderEntry& e = it->second;
    uint8_t w = 0;
    for (uint8_t r = 0; r < e.count; ++r)
        if (e.holders[r] != myEid) e.holders[w++] = e.holders[r];
    e.count = w;
    if (w == 0)
        g_table.erase(it);
    else if (e.count == 1)
        e.action = kGrab;
}

}  // namespace

void OnSessionStart() {
    g_table.clear();
    g_myKey = {};
    g_myHeld = false;
    g_pendingAnswers.clear();
}

void Tick(coop::net::Session& session, void* local) {
    if (!session.connected()) return;
    const uint32_t myEid = MyEid();
    if (myEid == 0 || myEid == coop::element::kInvalidId) {
        // No wire identity: do not participate in the lane at all (a hold we cannot
        // announce must not suppress anyone's stream either).
        if (g_myHeld) { g_myHeld = false; g_myKey = {}; }
        g_pendingAnswers.clear();
        return;
    }

    void* held = HeldWorldProp(local);
    coop::net::WireKey key{};
    bool haveKey = false;
    if (held) {
        const std::wstring keyW = ue_wrap::prop::GetInteractableKeyString(held);
        // DESIGN DECISION: keyed world props only. "None" is the trash clump's FName --
        // every clump stringifies to it, so it is not a cross-peer identity -- and an
        // empty key is no identity at all.
        if (!keyW.empty() && keyW != L"None") {
            key.len = 0;
            for (size_t i = 0; i < keyW.size() && i < 31; ++i) {
                // Save keys are ASCII; the narrowing is lossless (same as local_streams).
                key.data[key.len++] = static_cast<char>(keyW[i]);
            }
            haveKey = key.len > 0;
        }
    }

    // The release edge (a direct switch from one prop to another releases first, then
    // grabs below).
    if (g_myHeld && (!haveKey || !KeyEqual(g_myKey, key))) {
        SendHold(session, g_myKey, kRelease, nullptr, 0);
        EraseMeFromKey(KeyString(g_myKey), myEid);
        // A queued contest/de-contest answer for this key is stale now: we no longer
        // hold it, so answering would announce a hold we just ended.
        const std::string released = KeyString(g_myKey);
        g_pendingAnswers.erase(
            std::remove_if(g_pendingAnswers.begin(), g_pendingAnswers.end(),
                           [&](const coop::net::PropHoldPayload& a) {
                               return KeyString(a.key) == released;
                           }),
            g_pendingAnswers.end());
        g_myHeld = false;
        g_myKey = {};
    }
    // The grab edge.
    if (haveKey && !g_myHeld) {
        g_myHeld = true;
        g_myKey = key;
        // Am I contesting? The table may already list another holder for this key.
        uint32_t others[2] = {0, 0};
        uint8_t nOthers = 0;
        auto it = g_table.find(KeyString(key));
        if (it != g_table.end()) {
            for (uint8_t i = 0; i < it->second.count && nOthers < 2; ++i) {
                const uint32_t h = it->second.holders[i];
                if (h != myEid && h != 0 && h != coop::element::kInvalidId)
                    others[nOthers++] = h;
            }
        }
        if (nOthers > 0) {
            // Contest at grab time: both holders, ascending element id (the convergent
            // order -- DESIGN DECISION).
            uint32_t hs[2] = {others[0], myEid};
            if (hs[0] > hs[1]) std::swap(hs[0], hs[1]);
            SendHold(session, key, kContest, hs, 2);
        } else {
            SendHold(session, key, kGrab, &myEid, 1);
        }
    }

    DrainPending(session);
}

void OnHoldState(const coop::net::PropHoldPayload& p, int senderSlot) {
    (void)senderSlot;
    // Trust-boundary validation (the coordinator checked the payload size).
    if (p.holderCount > 2) {
        UE_LOGW("prophold: holderCount=%u out of range -- dropping",
                static_cast<unsigned>(p.holderCount));
        return;
    }
    if (p.action > kContest) {
        UE_LOGW("prophold: action=%u out of range -- dropping",
                static_cast<unsigned>(p.action));
        return;
    }
    if (p.key.len > 31) {
        UE_LOGW("prophold: key.len=%u out of range -- dropping",
                static_cast<unsigned>(p.key.len));
        return;
    }
    // The action mirrors the count edge: 0->0, 1->1, 2->2.
    const bool countOk = (p.action == kRelease && p.holderCount == 0) ||
                         (p.action == kGrab && p.holderCount == 1) ||
                         (p.action == kContest && p.holderCount == 2);
    if (!countOk) {
        UE_LOGW("prophold: action=%u with holderCount=%u -- dropping",
                static_cast<unsigned>(p.action), static_cast<unsigned>(p.holderCount));
        return;
    }
    for (uint8_t i = 0; i < p.holderCount; ++i) {
        if (p.holderElementId[i] == 0 ||
            p.holderElementId[i] == coop::element::kInvalidId) {
            UE_LOGW("prophold: holder %u has no element id -- dropping",
                    static_cast<unsigned>(i));
            return;
        }
    }

    const uint32_t myEid = MyEid();
    const std::string ks = KeyString(p.key);
    const bool iHoldThis = g_myHeld && KeyEqual(g_myKey, p.key);

    UE_LOGI("prophold: recv action=%u key='%.*s' holders=%u iHoldThis=%d",
            static_cast<unsigned>(p.action), static_cast<int>(p.key.len), p.key.data,
            static_cast<unsigned>(p.holderCount), iHoldThis ? 1 : 0);

    if (p.action == kRelease) {
        // A holder let go: the set is stale until the remaining holder re-announces.
        // Erase; if I still hold the key, answer action=1 (de-contest) so the set
        // converges without a central authority.
        g_table.erase(ks);
        if (iHoldThis && myEid != 0 && myEid != coop::element::kInvalidId)
            QueueAnswer(p.key, kGrab, &myEid, 1);
        return;
    }

    // Record the announced set (latest wins). Self-echoes are harmless: they rewrite the
    // set we already hold.
    HolderEntry e{};
    e.count = p.holderCount;
    e.action = p.action;
    for (uint8_t i = 0; i < p.holderCount; ++i) e.holders[i] = p.holderElementId[i];
    g_table[ks] = e;

    if (p.action == kGrab && p.holderCount == 1 && iHoldThis &&
        p.holderElementId[0] != myEid && myEid != 0 &&
        myEid != coop::element::kInvalidId) {
        // Contest: another holder announced a grab for a key I hold. Answer with both
        // holders, ascending element id. Only action=1 is ever answered -- answering a
        // contest would loop.
        uint32_t hs[2] = {p.holderElementId[0], myEid};
        if (hs[0] > hs[1]) std::swap(hs[0], hs[1]);
        QueueAnswer(p.key, kContest, hs, 2);
    }
}

bool ShouldSuppressPropPose(const coop::net::WireKey& key) {
    if (!g_myHeld || !KeyEqual(g_myKey, key)) return false;
    const uint32_t myEid = MyEid();
    if (myEid == 0 || myEid == coop::element::kInvalidId) return false;
    auto it = g_table.find(KeyString(key));
    if (it == g_table.end()) return false;
    // DESIGN DECISION: the lowest element id among the holders streams. Every peer
    // computes the same answer, so exactly one PropPose stream drives the mirrors and
    // the two streams never fight.
    for (uint8_t i = 0; i < it->second.count; ++i) {
        const uint32_t h = it->second.holders[i];
        if (h != myEid && h != 0 && h != coop::element::kInvalidId && h < myEid)
            return true;
    }
    return false;
}

void OnDisconnect() {
    OnSessionStart();  // the same clear: no hold state survives into the next session
}

void OnDisconnectForSlot(int peerSlot) {
    if (peerSlot < 0 || peerSlot >= coop::players::kMaxPeers) return;
    coop::element::Player* el = coop::players::Registry::Get().GetPlayerElement(
        static_cast<uint8_t>(peerSlot));
    if (!el) return;  // already dropped; nothing to clean
    const uint32_t eid = el->GetId();
    if (eid == 0 || eid == coop::element::kInvalidId) return;
    for (auto it = g_table.begin(); it != g_table.end();) {
        HolderEntry& e = it->second;
        uint8_t w = 0;
        for (uint8_t r = 0; r < e.count; ++r)
            if (e.holders[r] != eid) e.holders[w++] = e.holders[r];
        e.count = w;
        if (w == 0)
            it = g_table.erase(it);
        else {
            if (w == 1) e.action = kGrab;
            ++it;
        }
    }
    UE_LOGI("prophold: peer slot %d disconnected -- removed 0x%08x from the holder table",
            peerSlot, static_cast<unsigned>(eid));
}

}  // namespace coop::prophold_sync
