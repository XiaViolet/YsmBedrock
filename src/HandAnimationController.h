#pragma once
#include "AnimationSampler.h"

namespace ysm {
struct HandItem {
    std::string id;
    std::string category;
    std::string useAction;
    std::vector<std::string> tags;
    std::string identity;
    bool charged = false, fishing = false;
    int maxDurability = 0, damage = 0;
    std::map<std::string, int> enchantments;
    bool isLance() const { return category == "lance" || category == "melee_spear"; }
    bool matchesAnimationCategory(std::string_view value) const {
        return category == value || useAction == value || (isLance() && (value == "lance" || value == "melee_spear"));
    }
};
void adaptBedrockHandItem(HandItem& item);
struct HandInput {
    struct Mount {
        std::string id, legacyAnimation;
        std::vector<std::string> tags;
        bool alive = true;
    };
    std::optional<Mount> vehicle, passenger;
    std::array<HandItem, 2> items;
    std::map<std::string, HandItem> armor;
    int usingHand = -1, swingingHand = -1, armorValue = 0;
    double swingProgress = 0;
    bool sleeping = false, enabled = true, shieldBlockCooldown = false;
    HandItem const& equipment(std::string const& slot) const {
        if (slot == "mainhand") return items[0];
        if (slot == "offhand") return items[1];
        auto found = armor.find(slot);
        static HandItem const empty;
        return found == armor.end() ? empty : found->second;
    }
};

// Owns coded hold/use/swing clocks. Animation definitions remain immutable;
// separate controllers can request the same clip without sharing event cursors.
class HandAnimationController {
    struct Playback {
        std::string name, identity;
        double time = 0;
        bool restart = false;
    };
    std::set<std::string> clips;
    std::set<std::string> fallbackClips;
    ConditionalClips conditions;
    std::array<Playback, 2> holds;
    Playback use, swing;
    double lastTime = -1, previousSwingProgress = 0;
    int previousSwingHand = -1;
    int swingOwner = -1;
    bool authoredHandLoop = true;
    bool nativeSpearPose = false;
    std::string select(std::string const& prefix, HandItem const& item, bool holding) const;
    void choose(Playback& playback, std::string name, std::string identity, bool reload = false);
public:
    explicit HandAnimationController(Json const& animations);
    std::vector<AnimationLayer> update(double time, HandInput const& input);
    bool usesNativeSpearPose() const { return nativeSpearPose; }
};
}
