#pragma once
#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <vector>

namespace ysm {
// Entity-owned attributes. They affect this bone's surface, never its children.
struct BoneVisual {
    std::array<int, 3> color{255, 255, 255};
    int alpha = 255;
    int glow = -1;
    bool cutout = false;
    bool operator==(BoneVisual const&) const = default;
};
// The opaque eye layer needs depth from a matching cutout draw. Glint,
// damage overlays, custom alpha and tint keep their native materials.
inline bool supportsArkeGlowLayer(BoneVisual const& v, std::string_view material) {
    auto base = material.substr(0, material.find('.'));
    return v.glow == 15 && v.alpha == 255 && v.color == BoneVisual{}.color
        && (base == "entity_alphatest" || base == "entity_alphatest_one_sided");
}
// A cloned native mesh has no stable address in the preparation cache.
// The caller already established the draw owner. Require every non-null
// palette slot to belong to it and at least one YSM-generated part name.
template<class Parts, class Resolve>
bool resolveOwnedPalette(Parts const& parts, std::span<BoneVisual> result, Resolve&& resolve) {
    bool recognized = false;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (!parts[i]) return false;
        auto const& name = parts[i]->mName->getString();
        if (!resolve(name, result[i])) return false;
        recognized |= name.starts_with("__ysm_");
    }
    return recognized;
}
using BoneVisuals = std::map<std::string, BoneVisual>;
struct BoneVisualPass {
    BoneVisual visual;
    std::vector<std::size_t> bones;
};
std::vector<BoneVisualPass> planBoneVisualPasses(std::span<BoneVisual const> visuals);
struct BoneDrawPlan {
    std::optional<BoneVisual> uniform;
    std::vector<BoneVisualPass> groups;
    std::size_t passCount() const { return uniform ? (uniform->alpha == 0 ? 0 : 1) : groups.size(); }
};
BoneDrawPlan planBoneDraws(std::span<BoneVisual const> visuals);
// The renderer supplies a predicate only for rigid skinning palettes whose
// current and previous transforms both collapse the bone to a point.
template <class Collapsed>
std::size_t omitCollapsedMaterialPasses(BoneDrawPlan& plan, std::span<BoneVisual const> visuals, Collapsed collapsed) {
    if (plan.uniform || plan.groups.empty()) return 0;
    auto omitted = std::erase_if(plan.groups, [&](auto const& group) {
        return std::all_of(group.bones.begin(), group.bones.end(), collapsed);
    });
    if (plan.groups.size() == 1) {
        // Zero-alpha geometry has no pass in the original plan. A full
        // palette would expose it if it can still cover any pixels.
        for (std::size_t i = 0; i < visuals.size(); ++i)
            if (visuals[i].alpha == 0 && !collapsed(i)) return omitted;
        plan.uniform = plan.groups.front().visual;
        plan.groups.clear();
    }
    return omitted;
}

// A collapsed affine transform has three zero basis columns and a valid
// homogeneous translation. An unpopulated palette entry is not evidence of collapse.
bool collapsedSkinMatrix(std::span<float const, 16> matrix);
}
