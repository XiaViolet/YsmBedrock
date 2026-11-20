#include "BoneVisual.h"
#include <algorithm>
#include <cmath>

namespace ysm {
bool collapsedSkinMatrix(std::span<float const, 16> matrix) {
    return matrix[15] == 1
        && std::all_of(matrix.begin(), matrix.begin() + 12, [](float value) { return value == 0; })
        && std::all_of(matrix.begin() + 12, matrix.begin() + 15, [](float value) { return std::isfinite(value); });
}
std::vector<BoneVisualPass> planBoneVisualPasses(std::span<BoneVisual const> visuals) {
    std::vector<BoneVisualPass> result;
    for (std::size_t i = 0; i < visuals.size(); ++i) {
        auto const& visual = visuals[i];
        if (visual.alpha == 0) continue;
        auto found = std::find_if(result.begin(), result.end(), [&](auto const& pass) { return pass.visual == visual; });
        if (found == result.end()) result.push_back({visual, {i}});
        else found->bones.push_back(i);
    }
    return result;
}
BoneDrawPlan planBoneDraws(std::span<BoneVisual const> visuals) {
    if (visuals.empty()) return {};
    if (std::all_of(visuals.begin() + 1, visuals.end(), [&](auto const& v) { return v == visuals.front(); }))
        return {visuals.front(), {}};
    return {std::nullopt, planBoneVisualPasses(visuals)};
}
}
