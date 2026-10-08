#include "coop/player/client_model.h"

#include "coop/player/skin_effects.h"
#include "coop/player/skin_registry.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cstdint>
#include <cwctype>
#include <map>

namespace coop::client_model {

namespace {

// Per-name asset cache. A load MISS is remembered with a TIMESTAMP: re-probes
// are throttled (kMissRetryMs), never latched forever -- the forever-latch was
// the "peer sees my model with the wrong texture" field root: a
// skin applied EARLY on a joining machine (world still loading, pak mounting)
// could miss ONE load, latch `tried`, and render the mesh with no atlas for the
// whole session while the wearer's own machine (warm world) looked fine. A HIT
// is revalidated by GUObjectArray slot (IsLiveByIndex) because a level-change
// GC can collect a pak asset and recycle its address -- a stale hit re-loads
// instead of dereferencing an impostor.
struct CachedAsset {
    void*    ptr = nullptr;
    int32_t  idx = -1;
    bool     tried = false;
    uint64_t lastTryMs = 0;
};

// A missing asset re-probes at most this often (a burst of puppet spawns costs
// one LoadObjectByPath per name per window; a transient join-window miss heals
// on the next apply/converge pass instead of sticking for the session).
constexpr uint64_t kMissRetryMs = 5000;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::map<std::string, CachedAsset> g_meshCache;
std::map<std::string, CachedAsset> g_texCache;

std::wstring Widen(const std::string& ascii) {
    return std::wstring(ascii.begin(), ascii.end());  // names are validated ASCII
}

void* ResolveCached(std::map<std::string, CachedAsset>& cache, const std::string& name,
                    const std::wstring& path, const char* what) {
    namespace R = ue_wrap::reflection;
    CachedAsset& c = cache[name];
    if (c.ptr && R::IsLiveByIndex(c.ptr, c.idx)) return c.ptr;
    if (c.ptr) {
        UE_LOGI("client_model: cached %s for skin '%s' was GC'd -- re-loading", what, name.c_str());
        c.ptr = nullptr;
        c.tried = false;  // the asset existed before; re-probe
    }
    const uint64_t now = NowMs();
    if (c.tried && (now - c.lastTryMs) < kMissRetryMs) return nullptr;  // known-missing: throttled
    c.tried = true;
    c.lastTryMs = now;
    c.ptr = ue_wrap::asset_load::LoadObjectByPath(path.c_str());
    if (c.ptr) {
        c.idx = R::InternalIndexOf(c.ptr);
        UE_LOGI("client_model: skin '%s' %s ready (%p)", name.c_str(), what, c.ptr);
    } else {
        UE_LOGW("client_model: skin '%s' %s NOT loadable (pak absent on this machine?) -- "
                "native kel fallback", name.c_str(), what);
    }
    return c.ptr;
}

}  // namespace

bool IsNativeSkin(const std::string& name) {
    return name.empty() || name == coop::skins::kNativeSkinName;
}

bool UsesNativeMesh(const std::string& name) {
    return IsNativeSkin(name) || coop::skins::KelVariantTint(name) != nullptr;
}

// --- kel hair-tint discovery ---------------------------------------------------------
// The variant table names a tint; the game asset decides WHERE it goes. The hair slot index
// and the tintable vector parameter are discovered once per process from the live kel mesh
// and cached: the mesh asset is the same on every body, so re-discovering per puppet would
// be pure waste. Discovery is VERIFIED, not guessed: the slot is the first whose material
// name contains "hair", and the parameter is the first candidate that round-trips
// (set -> read back the same value) on a probe MID. A failed discovery logs exactly what
// was seen and the variant renders as plain dr_kel -- a visible, diagnosable no-op, never a
// mis-tint of the wrong slot.
namespace {

struct HairTarget {
    bool attempted = false;  // discovery ran (success or failure -- never re-run)
    bool verified = false;   // a slot AND a round-tripped parameter were found
    int32_t slot = -1;
    std::wstring param;
};
HairTarget g_hair;

bool NameContainsHair(const std::wstring& w) {
    std::wstring low = w;
    for (wchar_t& c : low) c = static_cast<wchar_t>(::towlower(c));
    return low.find(L"hair") != std::wstring::npos;
}

// The parameter vocabulary, tried in order. The round-trip below is the selector, so order
// is preference only: the engine's common tint names first.
constexpr const wchar_t* kTintParamCandidates[] = {
    L"TintColor", L"HairColor", L"Color", L"BaseColor", L"DiffuseColor",
};

void DiscoverHairTarget(void* comp) {
    namespace E = ue_wrap::engine;
    namespace R = ue_wrap::reflection;
    g_hair.attempted = true;
    if (!comp) {
        UE_LOGW("kel_tint: discovery with no mesh component -- variants render as plain dr_kel");
        return;
    }
    const int32_t n = E::GetNumMaterials(comp);
    if (n < 0) {
        UE_LOGW("kel_tint: GetNumMaterials unresolved -- variants render as plain dr_kel");
        return;
    }
    UE_LOGI("kel_tint: kel body has %d material slot(s)", n);
    int32_t hairSlot = -1;
    for (int32_t i = 0; i < n; ++i) {
        void* mat = E::GetMaterialAt(comp, i);
        const std::wstring name = mat ? R::ToString(R::NameOf(mat)) : L"<null>";
        UE_LOGI("kel_tint: slot %d -> '%ls'", i, name.c_str());
        if (hairSlot < 0 && mat && NameContainsHair(name)) hairSlot = i;
    }
    if (hairSlot < 0) {
        UE_LOGW("kel_tint: no material slot named '*hair*' -- variants render as plain dr_kel "
                "(see the slot list above; the table may need the game's actual hair material name)");
        return;
    }
    // Probe MID: verify a tint parameter by round-trip before trusting it on a real apply.
    void* probe = E::CreateDynamicMaterialInstance(comp, hairSlot);
    if (!probe) {
        UE_LOGW("kel_tint: probe MID failed on slot %d -- variants render as plain dr_kel", hairSlot);
        return;
    }
    const ue_wrap::FLinearColor test{0.31f, 0.47f, 0.53f, 1.0f};
    for (const wchar_t* cand : kTintParamCandidates) {
        if (!E::SetVectorParameterValue(probe, cand, test)) continue;
        ue_wrap::FLinearColor back{0.f, 0.f, 0.f, 0.f};
        if (!E::GetVectorParameterValue(probe, cand, back)) continue;
        const float d = (back.R - test.R) * (back.R - test.R) +
                        (back.G - test.G) * (back.G - test.G) +
                        (back.B - test.B) * (back.B - test.B);
        if (d < 1e-6f) {
            g_hair.verified = true;
            g_hair.slot = hairSlot;
            g_hair.param = cand;
            UE_LOGI("kel_tint: VERIFIED slot %d param '%ls' (round-trip) -- variants live",
                    hairSlot, cand);
            break;
        }
    }
    // Drop the probe MID so the slot renders its authored material until a real tint applies.
    E::SetComponentMaterial(comp, hairSlot, nullptr);
    if (!g_hair.verified)
        UE_LOGW("kel_tint: slot %d looks like hair but no tint parameter round-tripped -- variants "
                "render as plain dr_kel (tried: TintColor, HairColor, Color, BaseColor, DiffuseColor)",
                hairSlot);
}

// The tint apply: one MID per body slot on the discovered hair slot, the variant's color.
// Runs inside ApplySkinToBody's native-mesh branch, after the override clear.
void ApplyKelHairTint(void* comps[2], const coop::skins::KelVariant& variant) {
    namespace E = ue_wrap::engine;
    if (!g_hair.attempted) DiscoverHairTarget(comps[0] ? comps[0] : comps[1]);
    if (!g_hair.verified) return;  // already logged by the discovery
    const ue_wrap::FLinearColor tint{variant.r, variant.g, variant.b, 1.0f};
    int bound = 0;
    for (int i = 0; i < 2; ++i) {
        if (!comps[i]) continue;
        void* mid = E::CreateDynamicMaterialInstance(comps[i], g_hair.slot);
        if (mid && E::SetVectorParameterValue(mid, g_hair.param.c_str(), tint)) ++bound;
    }
    UE_LOGI("kel_tint: variant '%s' -> hair slot %d param '%ls' on %d/2 body slots",
            variant.name, g_hair.slot, g_hair.param.c_str(), bound);
}

}  // namespace

void* GetSkinMesh(const std::string& name) {
    if (IsNativeSkin(name) || !coop::skins::IsValidSkinName(name)) return nullptr;
    // Builtin kerfur skins: the game's own kerfurOmegaV1_Skeleton bodies,
    // loaded by asset path (no pak; materials ship with the mesh).
    if (const wchar_t* builtin = coop::skins::BuiltinSkinPath(name))
        return ResolveCached(g_meshCache, name, builtin, "mesh");
    const std::wstring w = Widen(name);
    // Package <name>, object kerfurOmega_KelSkin: the converter splices every
    // model into the kel-skin template and does not rename the export
    // under its own package; the package path disambiguates from the
    // game's own kerfurOmega_KelSkin.
    return ResolveCached(g_meshCache, name,
                         L"/Game/Mods/VOTVCoop/" + w + L".kerfurOmega_KelSkin", "mesh");
}

void* GetSkinTexture(const std::string& name) {
    if (IsNativeSkin(name) || !coop::skins::IsValidSkinName(name)) return nullptr;
    // Builtins and kel variants carry their own materials (no tex atlas by design).
    if (coop::skins::BuiltinSkinPath(name) || coop::skins::KelVariantTint(name)) return nullptr;
    const std::wstring w = Widen(name);
    return ResolveCached(g_texCache, name,
                         L"/Game/Mods/VOTVCoop/tex_" + w + L".tex_" + w, "texture");
}

namespace {
// Did the resolver DECLINE to ask, rather than ask and fail? Mirrors ResolveCached's own
// throttle test, so the two cannot drift into different ideas of "known missing".
bool ProbeWasThrottled(std::map<std::string, CachedAsset>& cache, const std::string& name) {
    auto it = cache.find(name);
    if (it == cache.end()) return false;           // never asked at all -> a real answer follows
    const CachedAsset& c = it->second;
    return !c.ptr && c.tried && (NowMs() - c.lastTryMs) < kMissRetryMs;
}
}  // namespace

Wearable CanWearSkin(const std::string& name) {
    // Ships with the game: no pak to be missing. Kel variants ride the native mesh.
    if (IsNativeSkin(name) || coop::skins::BuiltinSkinPath(name) ||
        coop::skins::KelVariantTint(name))
        return Wearable::Yes;
    // SAMPLED BEFORE THE PROBE, and the order is the whole point. `GetSkinMesh` REFRESHES
    // the throttle stamp when it asks and fails, so asking afterwards always says "throttled"
    // -- which made this function answer `Unknown` forever for a genuinely absent skin and
    // `No` never. Measured on a real run: a host whose ini named a missing pak never degraded
    // its body and never told the player.
    const bool meshDeclined = ProbeWasThrottled(g_meshCache, name);
    if (!GetSkinMesh(name)) return meshDeclined ? Wearable::Unknown : Wearable::No;
    // THE ATOMIC PAIR the apply insists on -- see ApplySkinToBody: a pak skin's mesh without
    // its atlas renders in the kel material, so the apply defers, and a gate that stopped at
    // the mesh would promise something the apply refuses.
    const bool texDeclined = ProbeWasThrottled(g_texCache, name);
    if (!GetSkinTexture(name)) return texDeclined ? Wearable::Unknown : Wearable::No;
    return Wearable::Yes;
}

bool ApplySkinToBody(void* mainPlayerActor, const std::string& name, void* nativeMesh) {
    namespace E = ue_wrap::engine;
    namespace Pup = ue_wrap::puppet;
    if (!mainPlayerActor) return false;

    void* comps[2] = { Pup::GetMeshPlayerVisibleComponent(mainPlayerActor),
                       Pup::GetNativeBodyMeshComponent(mainPlayerActor) };

    const coop::skins::KelVariant* variant = coop::skins::KelVariantTint(name);
    if (IsNativeSkin(name) || variant) {
        if (!nativeMesh) {
            UE_LOGW("client_model: dr_kel apply on %p but no native mesh captured yet -- skipped",
                    mainPlayerActor);
            return false;
        }
        int done = 0;
        for (void* comp : comps) {
            if (!comp) continue;
            if (E::SetSkeletalMesh(comp, nativeMesh)) ++done;
            // Clear a previous skin's MID override so kel renders its own material.
            E::SetComponentMaterial(comp, 0, nullptr);
        }
        UE_LOGI("client_model: %p -> native dr_kel body (%d/2 slots, material override cleared)",
                mainPlayerActor, done);
        // A kel variant tints the hair slot after the override clear above, so the tint MID
        // lands on the final material state.
        if (variant && done > 0) ApplyKelHairTint(comps, *variant);
        // Tear down a previous builtin skin's effect rig (face actor / light /
        // particles) -- dr_kel wears none.
        if (done > 0) coop::skin_effects::Apply(mainPlayerActor, name);
        return done > 0;
    }

    void* mesh = GetSkinMesh(name);
    if (!mesh) return false;  // pak missing here -- keep the current body (logged in resolver)
    // ATOMIC pair for pak skins: a pak skin's mesh without its atlas
    // texture is the "wrong texture / wrong UV" field report -- the mesh swap
    // landed and the tex miss cleared the slot-0 override, so the model rendered
    // in the kel material. Builtins carry their own materials (no tex by design);
    // for a pak skin, defer the WHOLE apply until both halves resolve -- the
    // caller keeps the previous body and the throttled retry heals the pair.
    if (!coop::skins::BuiltinSkinPath(name) && !GetSkinTexture(name)) {
        UE_LOGW("client_model: skin '%s' mesh ready but atlas tex not yet loadable -- "
                "deferring the apply (retry heals; no mesh-without-tex render)", name.c_str());
        return false;
    }
    int done = 0;
    for (void* comp : comps) {
        if (!comp) continue;
        if (E::SetSkeletalMesh(comp, mesh)) ++done;
    }
    // Slot-0 MID 'tex' override on both slots (inst_kel4_body is a MIC of
    // mat_object_sk whose diffuse is the 'tex' param -- no cooked material needed).
    // No texture (builtin kerfur skins carry their OWN materials) -> CLEAR the
    // slot-0 override instead, or a previous pak skin's MID atlas stays painted
    // over the builtin's material.
    int bound = 0;
    void* tex = GetSkinTexture(name);
    for (void* comp : comps) {
        if (!comp) continue;
        if (tex) {
            void* mid = E::CreateDynamicMaterialInstance(comp, 0);
            if (mid && E::SetTextureParameterValue(mid, L"tex", tex)) ++bound;
        } else {
            E::SetComponentMaterial(comp, 0, nullptr);
        }
    }
    UE_LOGI("client_model: %p -> skin '%s' (mesh %d/2 slots, %s)",
            mainPlayerActor, name.c_str(), done,
            tex ? "atlas tex bound" : "own materials (override cleared)");
    // The skin is more than the mesh for builtin kerfur skins: rebuild the
    // variant's effect rig (RT face / alive glow / particles / step FX) --
    // AFTER the material override pass above so the rig's slot writes land on
    // the final material state. Non-builtins tear a previous rig down.
    if (done > 0) coop::skin_effects::Apply(mainPlayerActor, name);
    return done > 0;
}

}  // namespace coop::client_model
