// coop/session/worldrules_sync.cpp -- see coop/session/worldrules_sync.h. The host's rule edits
// reach every client: one WorldRuleState per accepted edit, and the full set as the join
// snapshot. A client applies the host's rule to its own process + saved copies and re-renders
// its panel; it never authors. Host-wins by construction: only slot 0 can author, so the only
// race is an edit against the join snapshot, and the snapshot is what the joiner starts from.

#include "coop/session/worldrules_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"        // kMaxPeers, Role, Session, SendReliable*
#include "coop/save/slot_file_read.h"  // Crc32 for the ruleKeyHash
#include "harness/session_runtime.h"   // the process's Session (stable address, no bind needed)

#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/world/game_rules.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace coop::worldrules_sync {
namespace {

namespace GR = ue_wrap::game_rules;
using coop::net::kMaxPeers;
using coop::net::ReliableKind;
using coop::net::Role;
using coop::net::Session;
using coop::net::WorldRulePayload;

struct Edit {
    std::string key;
    GR::Kind    kind = GR::Kind::Bool;
    bool        bval = false;
    int         ival = 0;
    float       fval = 0.f;
};

std::mutex              g_editMx;
std::vector<Edit>       g_edits;  // guarded by g_editMx
constexpr size_t        kEditQueueMax = 64;
std::atomic<uint32_t>   g_changeGen{0};

uint32_t KeyHash(const std::string& key) {
    return coop::slot_file_read::Crc32(reinterpret_cast<const uint8_t*>(key.data()),
                                       key.size());
}

uint8_t KindToWire(GR::Kind k) {
    return k == GR::Kind::Bool ? 0 : k == GR::Kind::Enum ? 1 : 2;
}

void FillPayload(WorldRulePayload& p, const GR::RuleField& f) {
    p = WorldRulePayload{};
    p.ruleKeyHash = KeyHash(f.key);
    p.kind = KindToWire(f.kind);
    if (f.kind == GR::Kind::Bool) p.value.bval = f.bval ? 1 : 0;
    else if (f.kind == GR::Kind::Enum) p.value.ival = f.ival;
    else p.value.fval = f.fval;
}

// The rule the payload names, or null. ~36 rules; one CRC each per message, a person's act.
const GR::RuleField* FindByHash(const std::vector<GR::RuleField>& fields, uint32_t hash) {
    for (const GR::RuleField& f : fields)
        if (KeyHash(f.key) == hash) return &f;
    return nullptr;
}

const GR::RuleField* FindByKey(const std::vector<GR::RuleField>& fields, const std::string& key) {
    for (const GR::RuleField& f : fields)
        if (f.key == key) return &f;
    return nullptr;
}

std::string ValueText(const GR::RuleField& f) {
    char buf[32];
    switch (f.kind) {
        case GR::Kind::Bool: return f.bval ? "on" : "off";
        case GR::Kind::Float:
            std::snprintf(buf, sizeof(buf), "%f", static_cast<double>(f.fval));
            return buf;
        case GR::Kind::Enum:
            if (!f.valueName.empty()) return f.valueName;
            std::snprintf(buf, sizeof(buf), "#%d", f.ival);
            return buf;
    }
    return "?";
}

void DropMalformed(const char* why) {
    UE_LOGW("worldrules: WorldRuleState dropped -- %s", why);
}

}  // namespace

bool RequestRuleEdit(const std::string& key, GR::Kind kind, bool bval, int ival, float fval) {
    std::lock_guard<std::mutex> lk(g_editMx);
    if (g_edits.size() >= kEditQueueMax) {
        UE_LOGW("worldrules: edit queue full -- dropped '%s'", key.c_str());
        return false;
    }
    Edit e;
    e.key = key;
    e.kind = kind;
    e.bval = bval;
    e.ival = ival;
    e.fval = fval;
    g_edits.push_back(std::move(e));
    return true;
}

void DrainEditQueue() {
    UE_ASSERT_GAME_THREAD("worldrules::DrainEditQueue");
    std::vector<Edit> edits;
    {
        std::lock_guard<std::mutex> lk(g_editMx);
        edits.swap(g_edits);
    }
    if (edits.empty()) return;

    Session& s = harness::session_runtime::Session();
    const bool live = s.running();
    const bool hosting = live && s.role() == Role::Host;
    if (live && !hosting) {
        UE_LOGW("worldrules: %zu edit(s) dropped -- this peer is a client and cannot author rules",
                edits.size());
        return;
    }
    GR::Snapshot snap;
    if (!GR::ReadLocal(snap) || !snap.valid) {
        UE_LOGW("worldrules: %zu edit(s) dropped -- the rules are not readable right now",
                edits.size());
        return;
    }
    for (const Edit& e : edits) {
        const GR::RuleField* cur = FindByKey(snap.fields, e.key);
        if (!cur) {
            UE_LOGW("worldrules: edit dropped -- '%s' names no rule", e.key.c_str());
            continue;
        }
        if (cur->kind != e.kind) {
            UE_LOGW("worldrules: edit dropped -- '%s' is not a %s rule", e.key.c_str(),
                    e.kind == GR::Kind::Bool ? "bool" : e.kind == GR::Kind::Enum ? "enum" : "float");
            continue;
        }
        GR::RuleField v = *cur;
        if (e.kind == GR::Kind::Bool) v.bval = e.bval;
        else if (e.kind == GR::Kind::Enum) v.ival = e.ival;
        else v.fval = e.fval;
        // WriteRule re-validates (enum ordinal in the game's name list, float finite + clamped)
        // and writes the process + saved copies; false writes nothing.
        if (!GR::WriteRule(e.key, v)) {
            UE_LOGW("worldrules: edit of '%s' rejected by validation -- nothing written",
                    e.key.c_str());
            continue;
        }
        UE_LOGI("worldrules: '%s' set to %s%s", e.key.c_str(), ValueText(v).c_str(),
                hosting ? " (broadcasting)" : " (solo: local only)");
        if (hosting) {
            WorldRulePayload p;
            FillPayload(p, v);
            if (!s.SendReliable(ReliableKind::WorldRuleState, &p, sizeof(p)))
                UE_LOGW("worldrules: broadcast of '%s' failed to send", e.key.c_str());
        }
        g_changeGen.fetch_add(1, std::memory_order_release);
    }
}

void QueueConnectBroadcastForSlot(int slot) {
    UE_ASSERT_GAME_THREAD("worldrules::QueueConnectBroadcastForSlot");
    Session& s = harness::session_runtime::Session();
    if (!s.running() || s.role() != Role::Host) return;
    if (slot < 1 || slot >= kMaxPeers) return;
    GR::Snapshot snap;
    if (!GR::ReadLocal(snap) || !snap.valid) {
        UE_LOGW("worldrules: connect-snapshot to slot %d skipped -- the rules are not readable",
                slot);
        return;
    }
    int sent = 0;
    for (const GR::RuleField& f : snap.fields) {
        WorldRulePayload p;
        FillPayload(p, f);
        if (s.SendReliableToSlot(slot, ReliableKind::WorldRuleState, &p, sizeof(p))) ++sent;
    }
    UE_LOGI("worldrules: connect-snapshot -- sent %d rule(s) to slot %d (of %zu)", sent, slot,
            snap.fields.size());
}

void OnRuleState(const WorldRulePayload& p, int senderSlot) {
    UE_ASSERT_GAME_THREAD("worldrules::OnRuleState");
    // Host-authored, never relayed: the host's slot alone may author.
    if (senderSlot != 0) {
        UE_LOGW("worldrules: WorldRuleState dropped -- sender is not the host (slot %d)",
                senderSlot);
        return;
    }
    Session& s = harness::session_runtime::Session();
    if (s.running() && s.role() == Role::Host) {
        DropMalformed("a host never takes it");
        return;
    }
    if (p.kind > 2) {
        char why[48];
        std::snprintf(why, sizeof(why), "bad kind %u", static_cast<unsigned>(p.kind));
        DropMalformed(why);
        return;
    }
    GR::Snapshot snap;
    if (!GR::ReadLocal(snap) || !snap.valid) {
        DropMalformed("the rules are not readable right now");
        return;
    }
    const GR::RuleField* cur = FindByHash(snap.fields, p.ruleKeyHash);
    if (!cur) {
        char why[48];
        std::snprintf(why, sizeof(why), "hash %08X names no rule", p.ruleKeyHash);
        DropMalformed(why);
        return;
    }
    const GR::Kind want = p.kind == 0 ? GR::Kind::Bool : p.kind == 1 ? GR::Kind::Enum : GR::Kind::Float;
    if (cur->kind != want) {
        DropMalformed("kind disagrees with the rule");
        return;
    }
    GR::RuleField v = *cur;
    if (want == GR::Kind::Bool) {
        if (p.value.bval > 1) {
            DropMalformed("bool outside 0/1");
            return;
        }
        v.bval = p.value.bval != 0;
    } else if (want == GR::Kind::Enum) {
        v.ival = p.value.ival;
    } else {
        if (!std::isfinite(p.value.fval)) {
            DropMalformed("non-finite float");
            return;
        }
        v.fval = p.value.fval;
    }
    // WriteRule re-validates (enum ordinal in the game's name list, float clamped) and writes the
    // process + saved copies, so the client's two copies keep agreeing with the host's.
    if (!GR::WriteRule(cur->key, v)) {
        UE_LOGW("worldrules: host's '%s' rejected by validation -- nothing written",
                cur->key.c_str());
        return;
    }
    UE_LOGI("worldrules: host set '%s' to %s", cur->key.c_str(), ValueText(v).c_str());
    g_changeGen.fetch_add(1, std::memory_order_release);
}

uint32_t ChangeGeneration() { return g_changeGen.load(std::memory_order_acquire); }

}  // namespace coop::worldrules_sync
