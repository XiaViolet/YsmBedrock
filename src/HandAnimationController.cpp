#include "HandAnimationController.h"
#include "JavaVanillaInputs.h"
#include <algorithm>
#include <cmath>
#include <charconv>
#include <limits>

namespace ysm {
namespace {
bool usesFileHandLoop(std::string version) {
    // The behavior boundary is ModelOriginVersion.FILE_HAND_LOOP (19),
    // released as 2.4.2-snapshot-32. Missing/blank source versions are current.
    auto first = version.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return true;
    version = AnimationSampler::normalized(version.substr(first, version.find_last_not_of(" \t\r\n") - first + 1));
    if (auto metadata = version.find('+'); metadata != std::string::npos) version.resize(metadata);
    std::array<int, 3> numbers{};
    char const* cursor = version.data();
    char const* end = cursor + version.size();
    for (int part = 0; part < 3; ++part) {
        auto [next, error] = std::from_chars(cursor, end, numbers[part]);
        if (error == std::errc::result_out_of_range) numbers[part] = std::numeric_limits<int>::max();
        if (next == cursor) break;
        cursor = next;
        if (part == 2 || cursor == end || *cursor != '.') break;
        ++cursor;
    }
    constexpr std::array<int, 3> boundary{2, 4, 2};
    if (numbers != boundary) return numbers > boundary;
    while (cursor != end && (*cursor == '-' || *cursor == '.')) ++cursor;
    if (cursor == end) return true;
    std::string qualifier(cursor, end);
    if (qualifier.starts_with("snapshot")) {
        cursor += 8;
        while (cursor != end && (*cursor == '-' || *cursor == '.')) ++cursor;
        int snapshot = 0;
        auto parsed = std::from_chars(cursor, end, snapshot);
        return parsed.ec == std::errc::result_out_of_range || snapshot >= 32;
    }
    // Maven orders alpha/beta/milestone/rc before snapshot, and the release,
    // service-pack and unknown qualifiers after it.
    for (auto const* prefix : {"alpha", "beta", "milestone", "rc", "cr"})
        if (qualifier.starts_with(prefix)) return false;
    if (qualifier.size() > 1 && (qualifier[0] == 'a' || qualifier[0] == 'b' || qualifier[0] == 'm')
        && qualifier[1] >= '0' && qualifier[1] <= '9') return false;
    return true;
}
}
void adaptBedrockHandItem(HandItem& item) {
    appendJavaItemTags(item.id, item.tags);
    item.category = javaItemCategory(item.category, item.tags);

    static constexpr std::array<std::string_view, 2> spearTags{"minecraft:is_spear", "minecraft:spears"};
    static constexpr std::string_view vanillaSpears[]{
        "minecraft:wooden_spear", "minecraft:stone_spear", "minecraft:copper_spear", "minecraft:iron_spear",
        "minecraft:golden_spear", "minecraft:diamond_spear", "minecraft:netherite_spear"};
    auto hasTag = [&](std::string_view tag) { return std::ranges::find(item.tags, tag) != item.tags.end(); };
    if (std::ranges::find(vanillaSpears, item.id) == std::end(vanillaSpears)
        && std::ranges::none_of(spearTags, hasTag)) return;
    for (auto tag : spearTags) if (!hasTag(tag)) item.tags.emplace_back(tag);
    item.category = "lance";
    item.useAction = "lance";
}
HandAnimationController::HandAnimationController(Json const& animations) {
    auto version = animations.value("ysm_origin_version", Json(""));
    if (version.is_number_unsigned()) {
        // The legacy v3 header stores a behavior level, not a release string.
        authoredHandLoop = version.get<std::uint32_t>() >= 19;
    } else authoredHandLoop = usesFileHandLoop(version.get<std::string>());
    for (auto const& name : animations.value("ysm_fallback_animations", Json::array()))
        fallbackClips.insert(name.get<std::string>());
    for (auto const& [name, clip] : animations.at("animations").items()) {
        clips.insert(name);
        conditions.add(name);
    }
}
std::string HandAnimationController::select(std::string const& prefix, HandItem const& item, bool holding) const {
    auto present = [&](std::string const& name) { return clips.contains(name) ? name : std::string(); };
    if (item.id.empty()) return holding ? present(prefix + ":empty") : "";
    if (holding && item.charged) if (auto name = present(prefix + ":charged_crossbow"); !name.empty()) return name;
    if (holding && item.fishing) if (auto name = present(prefix + ":fishing"); !name.empty()) return name;
    if (auto name = present(prefix + "$" + item.id); !name.empty()) return name;
    if (auto clip = conditions.matchingTag(prefix, item.tags); !clip.empty()) return clip;
    if (item.isLance()) {
        auto lance = present(prefix + ":lance");
        if (!lance.empty() && !fallbackClips.contains(lance)) return lance;
        // Keep preview-era authored clips ahead of the new default route.
        if (auto legacy = present(prefix + ":melee_spear"); !legacy.empty()) return legacy;
        return lance;
    }
    if (!item.category.empty()) if (auto name = present(prefix + ":" + item.category); !name.empty()) return name;
    if (!item.useAction.empty() && item.useAction != "none") return present(prefix + ":" + item.useAction);
    return "";
}
void HandAnimationController::choose(Playback& playback, std::string name, std::string identity, bool reload) {
    if (name == playback.name && identity == playback.identity && !reload) return;
    playback = {std::move(name), std::move(identity), 0, true};
}
std::vector<AnimationLayer> HandAnimationController::update(double time, HandInput const& input) {
    if (!std::isfinite(time) || time < 0 || !std::isfinite(input.swingProgress)
        || input.usingHand < -1 || input.usingHand > 1 || input.swingingHand < -1 || input.swingingHand > 1)
        throw std::runtime_error("Invalid hand animation input");
    // A rewind is a new entity timeline, including its discrete input history.
    if (lastTime >= 0 && time < lastTime) {
        holds = {}; use = {}; swing = {}; previousSwingHand = -1; swingOwner = -1;
    }
    double delta = lastTime < 0 || time < lastTime ? 0 : time - lastTime;
    lastTime = time;
    nativeSpearPose = false;
    if (!input.enabled) {
        holds = {}; use = {}; swing = {}; previousSwingHand = -1; swingOwner = -1;
        return {};
    }
    auto identity = [](HandItem const& item) { return item.identity.empty() ? item.id : item.identity; };
    for (int hand = 0; hand < 2; ++hand) {
        auto& playback = holds[hand];
        // PAUSE advances the player while hiding its bone queues. The same
        // item resumes the elapsed clock; only a real selection reloads it.
        playback.time += delta;
        if (input.usingHand == hand || input.swingingHand == hand) continue;
        choose(playback, select(hand == 0 ? "hold_mainhand" : "hold_offhand", input.items[hand], true), identity(input.items[hand]));
    }
    use.time += delta;
    if (input.usingHand >= 0 && !input.sleeping) {
        auto const& item = input.items[input.usingHand];
        std::string prefix = input.usingHand == 0 ? "use_mainhand" : "use_offhand";
        auto selected = select(prefix, item, false);
        if (selected.empty() && !(input.usingHand == 0 && item.isLance()) && clips.contains(prefix)) selected = prefix;
        choose(use, selected, std::to_string(input.usingHand) + ":" + identity(item));
    } else choose(use, "", "");
    // Equipment changes retire the old item's swing. Otherwise a held final
    // frame can keep its authored weapon visible after the item was put away.
    if (swingOwner >= 0 && swing.identity != std::to_string(swingOwner) + ":" + identity(input.items[swingOwner])) {
        choose(swing, "", "");
        swingOwner = -1;
    }
    swing.time += delta;
    bool chargingSpear = input.usingHand == 0 && input.items[0].isLance();
    if (chargingSpear && (input.swingingHand == 0 || (input.swingingHand < 0 && swingOwner == 0))) {
        choose(swing, "", "");
        swingOwner = -1;
    } else if (input.swingingHand >= 0 && !input.sleeping) {
        auto const& item = input.items[input.swingingHand];
        std::string prefix = input.swingingHand == 0 ? "swing" : "swing_offhand";
        auto selected = select(prefix, item, false);
        auto fallback = input.swingingHand == 0 ? "swing_hand" : "swing_offhand";
        if (selected.empty() && !(input.swingingHand == 0 && item.isLance()) && clips.contains(fallback)) selected = fallback;
        bool started = previousSwingHand != input.swingingHand || input.swingProgress + 1e-6 < previousSwingProgress;
        choose(swing, selected, std::to_string(input.swingingHand) + ":" + identity(item), started);
        swingOwner = input.swingingHand;
    }
    previousSwingHand = input.swingingHand;
    previousSwingProgress = input.swingProgress;
    if (!input.sleeping && input.items[0].isLance()) {
        // Bedrock's charge state takes precedence over jabbing. Each action
        // falls back to its native pose only when its own YSM selector missed.
        auto const& active = input.usingHand == 0 ? use : input.swingingHand == 0 ? swing : holds[0];
        nativeSpearPose = active.name.empty();
    }
    std::vector<AnimationLayer> result;
    auto append = [&](Playback& playback, char const* owner) {
        if (!playback.name.empty()) {
            AnimationLayer layer{playback.name, playback.time, false, owner, playback.restart};
            if (!authoredHandLoop && !fallbackClips.contains(playback.name))
                layer.loop = std::string_view(owner) == "hand.swing" ? AnimationLoop::Once : AnimationLoop::Loop;
            result.push_back(std::move(layer));
        }
        playback.restart = false;
    };
    // YSM's player collection resolves offhand, mainhand, swing, then use.
    if (input.usingHand != 1 && input.swingingHand != 1) append(holds[1], "hand.hold_offhand");
    if (input.usingHand != 0 && input.swingingHand != 0) append(holds[0], "hand.hold_mainhand");
    append(swing, "hand.swing");
    append(use, "hand.use");
    return result;
}
}
