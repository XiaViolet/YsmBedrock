#include "BoneVisual.h"
#include "NativePoseLayout.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>

using namespace ysm;

void require(bool passed, char const* label) {
    if (!passed) throw std::runtime_error(label);
}

void boneLayout() {
    std::vector<std::string> order{"Root", "Arm", "Grip"};
    int bindings[]{3, 7, 11};
    unsigned resolutions = 0;
    auto resolve = [&](std::string const& name) -> int const* {
        ++resolutions;
        if (name == "Root" || name == "ROOT") return &bindings[0];
        if (name == "Arm") return &bindings[1];
        if (name == "Grip") return &bindings[2];
        return nullptr;
    };
    NativePoseLayout<int> layout;
    auto bind = [&] {
        return layout.bind(order.size(), [&](std::size_t i) -> auto const& { return order[i]; }, resolve);
    };
    require(bind() && resolutions == 3, "cold bindings");
    std::vector<std::string>(order).swap(order);
    require(bind() && resolutions == 3, "rebuilt engine storage");
    std::swap(order[1], order[2]);
    require(bind() && resolutions == 5 && layout[1] == 11 && layout[2] == 7, "reordered bindings");
    order[0] = "ROOT";
    require(bind() && layout[0] == 3, "source name normalization");
    order[2] = "foreign";
    require(!bind(), "foreign layout of equal size");
    order[2] = "Arm";
    require(bind() && layout[2] == 7, "return to owner after failed bind");
    order.resize(1);
    require(bind() && layout[0] == 3, "shorter hierarchy");
    order = {"Grip", "Arm", "ROOT"};
    require(bind() && layout[0] == 11 && layout[2] == 3, "regrown hierarchy");
}

void compare(std::vector<BoneVisual> const& palette) {
    auto old = planBoneVisualPasses(palette);
    auto now = planBoneDraws(palette);
    require(now.passCount() == old.size(), "pass count");
    if (now.uniform) {
        require(now.groups.empty(), "uniform material allocated a bone list");
        require(std::all_of(palette.begin(), palette.end(), [&](auto const& v) {return v == *now.uniform;}), "merged different surfaces");
        if (old.empty()) return;
        require(old.size() == 1 && old[0].visual == *now.uniform && old[0].bones.size() == palette.size(), "uniform surface coverage");
        for (std::size_t i = 0; i < palette.size(); ++i) require(old[0].bones[i] == i, "uniform bone index");
    } else {
        for (std::size_t i = 0; i < old.size(); ++i) {
            require(now.groups[i].visual == old[i].visual, "material order or attributes");
            require(now.groups[i].bones == old[i].bones, "masked current/previous palette indices");
        }
    }
}

// Bit 0 is a collapsed current palette, bit 1 a collapsed previous palette.
void verifyCollapsedPlan(std::vector<BoneVisual> const& visuals, std::vector<unsigned> const& state) {
    auto reference = planBoneVisualPasses(visuals);
    auto plan = planBoneDraws(visuals);
    auto fullyCollapsed = [&](std::size_t bone) { return state[bone] == 3; };
    omitCollapsedMaterialPasses(plan, visuals, fullyCollapsed);
    std::vector<BoneVisualPass> visible;
    for (auto pass : reference) {
        std::erase_if(pass.bones, fullyCollapsed);
        if (!pass.bones.empty()) visible.push_back(std::move(pass));
    }
    std::vector<BoneVisualPass> actual;
    if (plan.uniform && plan.passCount()) {
        actual.push_back({*plan.uniform, {}});
        for (std::size_t i = 0; i < visuals.size(); ++i)
            if (!fullyCollapsed(i)) actual.back().bones.push_back(i);
        if (actual.back().bones.empty()) actual.clear();
    } else for (auto pass : plan.groups) {
        std::erase_if(pass.bones, fullyCollapsed);
        if (!pass.bones.empty()) actual.push_back(std::move(pass));
    }
    require(actual.size() == visible.size(), "per-frame visible material count");
    for (std::size_t i = 0; i < actual.size(); ++i) {
        require(actual[i].visual == visible[i].visual, "tint, alpha, glow, cutout and order");
        require(actual[i].bones == visible[i].bones, "interpolated material-to-bone assignment");
    }
}

int main() {
    boneLayout();
    std::array<BoneVisual, 6> materials{};
    materials[1].color = {99, 107, 215};
    materials[2].alpha = 128;
    materials[3].glow = 9;
    materials[4].cutout = true;
    materials[5].alpha = 0;
    compare({});
    // Exercise both the allocation-free native draw and mixed material submission.
    for (auto const& v : materials) {
        compare({v});
        compare(std::vector<BoneVisual>(1034, v));
        compare(std::vector<BoneVisual>(6896, v));
    }
    for (auto const& a : materials) for (auto const& b : materials) compare({a,b,a,b});
    // All 4 possible current/previous combinations for three mixed bones.
    // Includes nonzero-alpha materials, hidden opaque and visible zero-alpha bones.
    for (auto const& a : materials) for (auto const& b : materials)
        for (unsigned mask = 0; mask < 64; ++mask)
            verifyCollapsedPlan({a, b, BoneVisual{}}, {mask & 3u, (mask >> 2) & 3u, (mask >> 4) & 3u});
    auto withHiddenGlow = planBoneDraws(std::array{materials[0], materials[3]});
    require(omitCollapsedMaterialPasses(withHiddenGlow, std::array{materials[0], materials[3]},
        [](std::size_t i) { return i == 1; }) == 1 && withHiddenGlow.uniform == materials[0],
        "invisible glow pass removed and only visible palette submitted in full");
    auto withTransparent = planBoneDraws(std::array{materials[0], materials[3], materials[5]});
    require(omitCollapsedMaterialPasses(withTransparent, std::array{materials[0], materials[3], materials[5]},
        [](std::size_t i) { return i == 1; }) == 1 && !withTransparent.uniform,
        "transparent non-collapsed geometry stays masked");
    std::mt19937 rng(42);
    for(int i=0;i<500;++i) {
        std::vector<BoneVisual> palette(rng() % 97);
        for (auto& v : palette) v = materials[rng() % materials.size()];
        compare(palette);
    }
    std::cout << "2304 interpolated culling cases and 2 optimization checks passed; native pose layout 8 cases passed; 555 native material plans match reference (including 500 random palettes).\n";
}
