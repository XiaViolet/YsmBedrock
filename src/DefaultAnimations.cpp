#include "DefaultAnimations.h"
#include "ContentHash.h"
#include <algorithm>

namespace ysm {
namespace {
#include "DefaultAnimations.inc"

std::string_view boneWithoutNumber(std::string_view name) {
    auto end = name.find_last_not_of("0123456789");
    return end == name.npos ? std::string_view{} : name.substr(0, end + 1);
}

struct BoneAlias {
    std::string numbered, base;
    bool alternateHandRig;
};
std::vector<BoneAlias> numberedBoneAliases(Json const& geometry) {
    if (geometry.is_null()) return {};
    std::map<std::string, std::string> parents;
    for (auto const& bone : geometry.at("minecraft:geometry").at(0).at("bones"))
        parents.emplace(bone.at("name").get<std::string>(), bone.value("parent", ""));
    std::vector<BoneAlias> aliases;
    for (auto const& [name, parent] : parents) {
        auto source = boneWithoutNumber(name);
        if (source.empty() || source == name) continue;
        auto ancestor = parents.find(parent);
        while (ancestor != parents.end() && boneWithoutNumber(ancestor->first) != source)
            ancestor = parents.find(ancestor->second);
        // A numbered child already inherits its family's animated transform.
        if (ancestor != parents.end()) continue;
        // Separate outfits can have a complete numbered arm. A second item
        // locator on the same arm remains independently animated.
        bool alternateArm = false;
        auto suffix = std::string_view(name).substr(source.size());
        for (auto it = parents.find(name); it != parents.end(); it = parents.find(it->second)) {
            auto base = boneWithoutNumber(it->first);
            if ((base == "RightArm" || base == "LeftArm") && std::string_view(it->first).substr(base.size()) == suffix) {
                alternateArm = true;
                for (auto outer = parents.find(it->second); outer != parents.end(); outer = parents.find(outer->second))
                    if (boneWithoutNumber(outer->first) == base) { alternateArm = false; break; }
                break;
            }
        }
        aliases.push_back({name, std::string(source), alternateArm});
    }
    return aliases;
}
}
Json withDefaultAnimations(Json document, bool firstPerson, Json const& geometry) {
    static Json const body = Json::parse(DefaultBodyAnimations);
    static Json const arm = Json::parse(DefaultFirstPersonAnimations);
    auto const& defaults = firstPerson ? arm : body;
    // ordered_json stores object members in a vector. Insert both keys before
    // borrowing either child; adding provenance must not invalidate clips.
    if (!document.contains("animations")) document["animations"] = Json::object();
    if (!document.contains("ysm_fallback_animations")) document["ysm_fallback_animations"] = Json::array();
    auto& clips = document.at("animations");
    auto& fallback = document.at("ysm_fallback_animations");
    auto aliases = numberedBoneAliases(geometry);
    for (auto const& [name, clip] : defaults.items()) if (!clips.contains(name)) {
        clips[name] = clip;
        bool handClip = name.starts_with("hold_mainhand") || name.starts_with("hold_offhand")
            || name.starts_with("swing") || name.starts_with("use_mainhand") || name.starts_with("use_offhand");
        bool lance = name.ends_with(":lance");
        if ((lance || (!firstPerson && handClip)) && clip.contains("bones") && !aliases.empty()) {
            auto const& source = clip.at("bones");
            auto& bones = clips.at(name).at("bones");
            for (auto const& [numbered, base, alternateArm] : aliases) {
                // A second grip can be an independently toggled item. Copying
                // the primary lance's scale here would unhide that extra item.
                bool itemLocator = base == "RightHandLocator" || base == "LeftHandLocator";
                if (!alternateArm && (!lance || itemLocator)) continue;
                if (auto track = source.find(base); track != source.end() && !bones.contains(numbered)) bones[numbered] = *track;
            }
        }
        fallback.push_back(name);
    }
    return document;
}
std::string defaultTargetAnimationDomain(ReplacedModelKind kind, std::vector<std::string> matches) {
    std::sort(matches.begin(), matches.end());
    std::string canonical;
    bool first = true;
    for (auto const& match : matches) {
        if (!first) canonical += '\0';
        first = false;
        canonical += match;
    }
    auto digest = contentSha256(std::span<unsigned char const>(
        reinterpret_cast<unsigned char const*>(canonical.data()), canonical.size()));
    return std::string(replacedModelKindName(kind)) + "/" + digest.substr(0, 16) + "/main";
}
Json withDefaultTargetAnimations(Json document, ReplacedModelKind kind, std::vector<std::string> const& matches) {
    static Json const sets = Json::parse(DefaultTargetAnimationSets);
    auto domain = defaultTargetAnimationDomain(kind, matches);
    auto found = sets.find(domain);
    if (found == sets.end() && kind == ReplacedModelKind::Vehicle && matches.size() == 1 && matches.front() == "minecraft:boat") {
        static auto const boatDomain = defaultTargetAnimationDomain(ReplacedModelKind::Vehicle, {"#minecraft:boat"});
        found = sets.find(boatDomain);
    }
    if (!document.contains("animations")) document["animations"] = Json::object();
    if (!document.contains("ysm_fallback_animations")) document["ysm_fallback_animations"] = Json::array();
    document["ysm_default_animation_domain"] = domain;
    document["ysm_default_domain_available"] = found != sets.end();
    auto& clips = document.at("animations");
    auto& fallback = document.at("ysm_fallback_animations");
    if (found != sets.end()) for (auto const& [name, clip] : found->items())
        if (!clips.contains(name)) { clips[name] = clip; fallback.push_back(name); }
    return document;
}
}
