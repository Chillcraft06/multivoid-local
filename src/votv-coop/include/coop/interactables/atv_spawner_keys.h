// coop/interactables/atv_spawner_keys.h -- the ATV per-spawner identity scheme.
//
// Every ATV in the world is born from exactly one spawner, and every peer holds a mirror of
// each spawned ATV keyed by (spawner id, spawn index), so two peers looking at ATVs from
// different spawners agree on which ATV is which. The spawners the code can see:
//
//   "save"        -- the world/save loader. Save-placed ATVs carry the game's deterministic
//                    Key, which is stable across peers because the joiner boots from the
//                    host's transferred save (docs/join.md). The Key is the identity within
//                    the spawner; two ATVs sharing one Key string are disambiguated by an
//                    ordinal over their spawn sites (below).
//   "menu"        -- the host's spawn-menu spawner (the list_props 'atv' row, at runtime).
//                    The host mints a monotonic counter and announces it (AtvSpawn).
//   "menu@<slot>" -- a client's spawn-menu spawner, by peer slot. The client announces the
//                    spawn to the host (AtvSpawn, client-originated); the host spawns its own
//                    mirror and re-announces it so the other clients mirror it too.
//
// The composite wire identity is "<spawner>#<spawner-local id>":
//   save (unique key):   "save#<realKey>"
//   save (dup keys):     "save#<realKey>#<ordinal>"
//   menu (host):         "menu#<counter>"
//   menu (client):       "menu@<slot>#<counter>"
// It rides the existing AtvState/AtvRelease `key` field (WireKey, 31 chars); no new wire kind
// was needed. A composite longer than 31 chars falls back to a deterministic FNV-1a tag so two
// peers still agree (and it logs, because it means a save key longer than ~24 chars showed up).
//
// Spawn-site choice: the anchor is the first-sighting position quantized to a 1 m grid, with
// the full-precision position as the tiebreak. Both peers load the same save bytes, so both
// compute identical sites for save-placed ATVs; the anchor is captured once and never moves,
// so the ordinal cannot flip when two ATVs drive past each other. Raw floats are the tiebreak
// rather than first-seen order because first-seen order is pass-slicing dependent and NOT
// peer-deterministic. Assumes save Key strings never end in "#<digits>" (the ordinal parse).

#pragma once

#include "coop/net/wire_key_util.h"  // FnvKey
#include "ue_wrap/core/types.h"      // FVector

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::atv_spawner_keys {

inline constexpr wchar_t kSaveSpawner[] = L"save";
inline constexpr wchar_t kHostMenuSpawner[] = L"menu";  // the host's spawn-menu spawner
// A client's spawn-menu spawner key: L"menu@" + the peer slot, e.g. L"menu@2".

// A spawn site: the quantized anchor (the stable sort key) plus the full-precision position
// (the anchor tiebreak; identical across peers for the same save bytes).
struct SpawnSite {
    int32_t x = 0, y = 0, z = 0;  // first-sighting position, 1 m grid
    float rawX = 0.f, rawY = 0.f, rawZ = 0.f;
    bool operator<(const SpawnSite& o) const {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        if (z != o.z) return z < o.z;
        if (rawX != o.rawX) return rawX < o.rawX;
        if (rawY != o.rawY) return rawY < o.rawY;
        return rawZ < o.rawZ;
    }
    bool operator==(const SpawnSite& o) const {
        return x == o.x && y == o.y && z == o.z &&
               rawX == o.rawX && rawY == o.rawY && rawZ == o.rawZ;
    }
};

inline SpawnSite QuantizeSite(const ue_wrap::FVector& p) {
    SpawnSite s;
    s.x = static_cast<int32_t>(std::lround(static_cast<double>(p.X) / 100.0));
    s.y = static_cast<int32_t>(std::lround(static_cast<double>(p.Y) / 100.0));
    s.z = static_cast<int32_t>(std::lround(static_cast<double>(p.Z) / 100.0));
    s.rawX = p.X; s.rawY = p.Y; s.rawZ = p.Z;
    return s;
}

// Sorted-unique insert of a spawn site; returns the site's ordinal (its index). Order-independent:
// inserting the same set of sites in any order yields the same ordinals, which is what makes the
// duplicate-key disambiguation peer-deterministic.
inline uint32_t InsertSiteOrdinal(std::vector<SpawnSite>& sites, const SpawnSite& site) {
    uint32_t i = 0;
    for (; i < sites.size(); ++i) {
        if (sites[i] == site) return i;
        if (site < sites[i]) break;
    }
    sites.insert(sites.begin() + i, site);
    return i;
}

// The composite (spawner, index) identity. Deterministic FNV fallback past 31 chars (WireKey).
inline std::wstring BuildCompositeKey(const std::wstring& spawner, const std::wstring& localId) {
    std::wstring key = spawner;
    key.push_back(L'#');
    key += localId;
    if (key.size() <= 31) return key;
    const uint32_t h = static_cast<uint32_t>(coop::net::FnvKey(key) & 0xFFFFFFFFu);
    static const wchar_t* const hexd = L"0123456789ABCDEF";
    std::wstring tag = spawner;
    tag += L"#h";
    for (int i = 7; i >= 0; --i) tag.push_back(hexd[(h >> (i * 4)) & 0xFu]);
    return tag;  // spawner keys are short; the tag always fits
}

// Split "<spawner>#<local>" at the first '#'. False when there is no '#'.
inline bool SplitCompositeKey(const std::wstring& key, std::wstring& spawnerOut,
                              std::wstring& localOut) {
    const size_t p = key.find(L'#');
    if (p == std::wstring::npos) return false;
    spawnerOut = key.substr(0, p);
    localOut = key.substr(p + 1);
    return true;
}

// The save spawner's local id is "<realKey>" or "<realKey>#<ordinal>": recover the realKey.
inline std::wstring SaveLocalRealKey(const std::wstring& local) {
    const size_t p = local.rfind(L'#');
    if (p == std::wstring::npos || p + 1 >= local.size()) return local;
    for (size_t i = p + 1; i < local.size(); ++i)
        if (local[i] < L'0' || local[i] > L'9') return local;  // '#' belongs to the real key
    return local.substr(0, p);
}

// True for the host's spawn-menu spawner keys ("menu#N"). A client's ("menu@<slot>#N") is not.
inline bool IsHostMenuKey(const std::wstring& key) {
    std::wstring spawner, local;
    return SplitCompositeKey(key, spawner, local) && spawner == kHostMenuSpawner;
}

// True for any spawn-menu spawner key, host ("menu#N") or client ("menu@<slot>#N").
inline bool IsMenuSpawnerKey(const std::wstring& key) {
    std::wstring spawner, local;
    if (!SplitCompositeKey(key, spawner, local)) return false;
    return spawner == kHostMenuSpawner || spawner.rfind(L"menu@", 0) == 0;
}

// A client's spawn-menu spawner key for its peer slot: L"menu@<slot>".
inline std::wstring ClientMenuSpawnerForSlot(uint8_t slot) {
    return std::wstring(L"menu@") + std::to_wstring(slot);
}

// Parse a client menu composite key ("menu@<slot>#<n>"): true with the claimed slot when the
// shape holds. The host checks the claimed slot against the packet's sender slot.
inline bool ParseClientMenuKey(const std::wstring& key, uint8_t& slotOut) {
    std::wstring spawner, local;
    if (!SplitCompositeKey(key, spawner, local)) return false;
    if (spawner.rfind(L"menu@", 0) != 0) return false;
    const std::wstring slotStr = spawner.substr(5);
    if (slotStr.empty()) return false;
    unsigned long v = 0;
    for (wchar_t c : slotStr) {
        if (c < L'0' || c > L'9') return false;
        v = v * 10 + static_cast<unsigned long>(c - L'0');
        if (v > 255) return false;
    }
    slotOut = static_cast<uint8_t>(v);
    return !local.empty();
}

}  // namespace coop::atv_spawner_keys
