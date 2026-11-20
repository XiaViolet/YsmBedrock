#pragma once
#include "ModelPack.h"
#include "AnimationSampler.h"
#include <array>
#include <atomic>
#include <map>
#include <set>

namespace ysm {
class HandVisibilityByPass {
    std::array<std::atomic<unsigned>, 2> masks{};
public:
    void update(bool inventory, std::array<unsigned, 2> hands) {
        masks[inventory].store(hands[0] | (hands[1] << 8), std::memory_order_relaxed);
    }
    unsigned get(bool inventory, bool offhand) const {
        return (masks[inventory].load(std::memory_order_relaxed) >> (offhand ? 8 : 0)) & 255u;
    }
};
bool signedClosedCube(Json const& cube, double boneInflate);
double geometryTextureDimension(Json const& description, char const* key);

struct ClientGeometry {
    Json document;
    std::map<std::string, std::string> bodyBones;
    std::map<std::string, std::string> armBones;
    std::map<std::string, std::string> armParts;
    // Drawable part -> owning native bone, for non-inherited transparency.
    std::map<std::string, std::string> opacityParts;
    std::set<std::string> oneSidedParts;
    std::set<std::string> armCarriers;
    std::set<std::string> meleeSpearArms;
    std::map<std::string, std::string> itemLocators;
    std::array<unsigned, 2> handDrawMasks{};
    std::array<std::array<std::vector<std::string>, 8>, 2> handLocatorChains;
    std::array<unsigned, 2> visibleHandMasks(Pose const& pose) const;
    std::map<std::string, std::string> firstPersonItems;
    std::string headLocator;
    std::string bodyRoot;
    std::string projectileYawCarrier, projectilePitchCarrier;
    static ClientGeometry build(ModelPack const& pack);
    static ClientGeometry buildTarget(ReplacedModelTarget const& target);
    Json report() const;
};
}
