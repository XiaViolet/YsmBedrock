#include "JavaVanillaInputs.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <ranges>
#include <span>
#include <utility>

namespace ysm {
namespace {
using Entry = std::pair<std::string_view, std::string_view>;
constexpr Entry itemTags[]{
#define YSM_JAVA_ITEM_TAGS(id, tags) {id, tags},
#include "JavaVanillaTags.inc"
#undef YSM_JAVA_ITEM_TAGS
};
constexpr Entry entityTags[]{
#define YSM_JAVA_ENTITY_TAGS(id, tags) {id, tags},
#include "JavaVanillaTags.inc"
#undef YSM_JAVA_ENTITY_TAGS
};
constexpr Entry blockTags[]{
#define YSM_JAVA_BLOCK_TAGS(id, tags) {id, tags},
#include "JavaVanillaTags.inc"
#undef YSM_JAVA_BLOCK_TAGS
};
constexpr Entry biomeTags[]{
#define YSM_JAVA_BIOME_TAGS(id, tags) {id, tags},
#include "JavaVanillaTags.inc"
#undef YSM_JAVA_BIOME_TAGS
};
std::string_view tagsFor(std::span<Entry const> entries, std::string_view id) {
    auto entry = std::lower_bound(entries.begin(), entries.end(), id,
        [](auto const& pair, auto value) { return pair.first < value; });
    return entry != entries.end() && entry->first == id ? entry->second : std::string_view{};
}
void appendTags(std::span<Entry const> entries, std::string_view id, std::vector<std::string>& tags,
    bool includeLegacyItemTags = false) {
    auto values = tagsFor(entries, id);
    while (!values.empty()) {
        auto end = values.find(' ');
        auto tag = values.substr(0, end);
        if (std::find(tags.begin(), tags.end(), tag) == tags.end()) tags.emplace_back(tag);
        if (includeLegacyItemTags && tag.starts_with("ysm:")) {
            auto legacy = "yes_steve_model:" + std::string(tag.substr(4));
            if (std::find(tags.begin(), tags.end(), legacy) == tags.end()) tags.push_back(std::move(legacy));
        }
        if (end == std::string_view::npos) break;
        values.remove_prefix(end + 1);
    }
}
}
void appendJavaItemTags(std::string_view id, std::vector<std::string>& tags) { appendTags(itemTags, id, tags, true); }
std::string_view javaEntityId(std::string_view bedrockId, int boatVariant) {
    static constexpr std::string_view boats[]{
        "minecraft:oak_boat", "minecraft:spruce_boat", "minecraft:birch_boat", "minecraft:jungle_boat",
        "minecraft:acacia_boat", "minecraft:dark_oak_boat", "minecraft:mangrove_boat", "minecraft:bamboo_raft",
        "minecraft:cherry_boat", "minecraft:pale_oak_boat"
    };
    static constexpr std::string_view chestBoats[]{
        "minecraft:oak_chest_boat", "minecraft:spruce_chest_boat", "minecraft:birch_chest_boat", "minecraft:jungle_chest_boat",
        "minecraft:acacia_chest_boat", "minecraft:dark_oak_chest_boat", "minecraft:mangrove_chest_boat", "minecraft:bamboo_chest_raft",
        "minecraft:cherry_chest_boat", "minecraft:pale_oak_chest_boat"
    };
    if (boatVariant >= 0 && boatVariant < static_cast<int>(std::size(boats))) {
        if (bedrockId == "minecraft:boat") return boats[boatVariant];
        if (bedrockId == "minecraft:chest_boat") return chestBoats[boatVariant];
    }
    static constexpr Entry aliases[]{
        {"minecraft:breeze_wind_charge_projectile", "minecraft:breeze_wind_charge"},
        {"minecraft:evocation_fang", "minecraft:evoker_fangs"},
        {"minecraft:fireworks_rocket", "minecraft:firework_rocket"},
        {"minecraft:fishing_hook", "minecraft:fishing_bobber"},
        {"minecraft:lingering_potion", "minecraft:potion"},
        {"minecraft:splash_potion", "minecraft:potion"},
        {"minecraft:thrown_trident", "minecraft:trident"},
        {"minecraft:wind_charge_projectile", "minecraft:wind_charge"},
        {"minecraft:wither_skull_dangerous", "minecraft:wither_skull"},
        {"minecraft:xp_bottle", "minecraft:experience_bottle"}
    };
    auto match = std::ranges::find(aliases, bedrockId, &Entry::first);
    return match == std::end(aliases) ? bedrockId : match->second;
}
void appendJavaEntityTags(std::string_view id, std::vector<std::string>& tags) {
    appendTags(entityTags, id, tags);
    if (auto alias = javaEntityId(id); alias != id) appendTags(entityTags, alias, tags);
    // Java 26.1.2 separates the ten ordinary boat/raft types. Bedrock also
    // exposes one shared entity ID for these boats.
    static constexpr std::string_view boats[]{"minecraft:boat", "minecraft:oak_boat", "minecraft:spruce_boat",
        "minecraft:birch_boat", "minecraft:jungle_boat", "minecraft:acacia_boat", "minecraft:cherry_boat",
        "minecraft:dark_oak_boat", "minecraft:pale_oak_boat", "minecraft:mangrove_boat", "minecraft:bamboo_raft"};
    constexpr std::string_view boatTag = "minecraft:boat";
    if (std::ranges::find(boats, id) != std::end(boats) && std::ranges::find(tags, boatTag) == tags.end())
        tags.emplace_back(boatTag);
}
bool hasJavaBlockTag(std::string_view bedrockId, std::string_view tag) {
    static constexpr Entry aliases[]{
        {"minecraft:flowing_water", "minecraft:water"},
        {"minecraft:flowing_lava", "minecraft:lava"},
        {"minecraft:short_grass", "minecraft:grass"},
        {"minecraft:snow", "minecraft:snow_block"},
        {"minecraft:snow_layer", "minecraft:snow"},
        {"minecraft:slime", "minecraft:slime_block"},
        {"minecraft:web", "minecraft:cobweb"}
    };
    if (auto match = std::ranges::find(aliases, bedrockId, &Entry::first); match != std::end(aliases))
        bedrockId = match->second;
    auto values = tagsFor(blockTags, bedrockId);
    return std::ranges::any_of(values | std::views::split(' '),
        [&](auto member) { return std::ranges::equal(member, tag); });
}
bool hasJavaBiomeTag(std::string_view bedrockId, std::string_view tag) {
    static constexpr Entry aliases[]{
        {"minecraft:hell", "minecraft:nether_wastes"},
        {"minecraft:ice_plains", "minecraft:snowy_plains"},
        {"minecraft:mesa", "minecraft:badlands"},
        {"minecraft:swampland", "minecraft:swamp"},
        {"minecraft:extreme_hills", "minecraft:windswept_hills"},
        {"minecraft:roofed_forest", "minecraft:dark_forest"}
    };
    if (tag.empty()) return false;
    if (auto match = std::ranges::find(aliases, bedrockId, &Entry::first); match != std::end(aliases))
        bedrockId = match->second;
    return std::ranges::any_of(tagsFor(biomeTags, bedrockId) | std::views::split(' '),
        [&](auto member) { return std::ranges::equal(member, tag); });
}
std::string canonicalRegistryId(std::string_view value) {
    if (value.empty()) return {};
    auto result = value.find(':') == value.npos ? "minecraft:" + std::string(value) : std::string(value);
    auto colon = result.find(':');
    if (colon == 0 || colon + 1 == result.size() || result.find(':', colon + 1) != result.npos) return {};
    for (std::size_t i = 0; i < result.size(); ++i) {
        auto c = result[i];
        if (i == colon || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') continue;
        if (c == '/' && i > colon) continue;
        return {};
    }
    return result;
}
std::string javaItemCategory(std::string_view nativeCategory, std::vector<std::string> const& tags) {
    // Same order as YSM InnerClassify, including its own data tags.
    static constexpr Entry categories[]{
        {"sword", "ysm:swords"}, {"axe", "ysm:axes"}, {"pickaxe", "ysm:pickaxes"},
        {"shovel", "ysm:shovels"}, {"hoe", "ysm:hoes"}, {"shield", "ysm:shields"},
        {"crossbow", "ysm:crossbows"}, {"bow", "ysm:bows"}, {"fishing_rod", "ysm:fishing_rods"},
        {"spear", "ysm:tridents"}, {"throwable_potion", "ysm:throwable_potion"}};
    for (auto const& [category, tag] : categories)
        if (nativeCategory == category || std::find(tags.begin(), tags.end(), tag) != tags.end()) return std::string(category);
    return std::string(nativeCategory);
}
float JavaElytraPose::degrees() const {
    return static_cast<float>(static_cast<double>(roll) * 180.0 / std::numbers::pi);
}
void JavaElytraPose::advance(bool gliding, bool crouching, std::array<double, 3> const& velocity) {
    float target = -0.2617994f;
    if (gliding) {
        float blend = 1;
        if (velocity[1] < 0) {
            auto length = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
            auto vertical = length < 1.0e-4 ? 0.0 : velocity[1] / length;
            blend = 1.0f - static_cast<float>(std::pow(-vertical, 1.5));
        }
        target = blend * -1.5707964f + (1.0f - blend) * target;
    } else if (crouching) target = -0.7853982f;
    roll += (target - roll) * 0.1f;
}
float JavaElytraInput::sample(std::uint64_t frame, bool renderLayer, bool equipped,
    bool gliding, bool crouching, std::array<double, 3> const& velocity) {
    if (frame != lastFrame) {
        lastFrame = frame;
        query = pose.degrees();
        advanced = false;
    }
    // The source's animation queries run before its equipped wing layer calls
    // setupAnim. Freeze that query for every body/hand/shadow pass this frame.
    if (!advanced && renderLayer && equipped) {
        pose.advance(gliding, crouching, velocity);
        advanced = true;
    }
    return query;
}
}
