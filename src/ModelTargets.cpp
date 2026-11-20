#include "ModelPack.h"
#include "JavaVanillaInputs.h"
#include "DefaultAnimations.h"
#include <algorithm>

namespace ysm {
std::string_view replacedModelKindName(ReplacedModelKind kind) {
    return kind == ReplacedModelKind::Vehicle ? "vehicle" : "projectile";
}
bool ReplacedModelTarget::matchesEntity(std::string_view entityId, std::span<std::string const> registryTypeTags, int boatVariant) const {
    auto id = canonicalRegistryId(entityId);
    if (id.empty()) return false;
    std::vector<std::string> memberships(registryTypeTags.begin(), registryTypeTags.end());
    appendJavaEntityTags(id, memberships);
    auto alias = javaEntityId(id, boatVariant);
    if (boatVariant >= 0 && alias != id) appendJavaEntityTags(alias, memberships);
    for (auto const& match : matches) {
        bool tag = match.starts_with('#');
        auto name = canonicalRegistryId(tag ? std::string_view(match).substr(1) : std::string_view(match));
        if (name.empty()) continue;
        if (!tag ? name == id || name == alias
                : std::any_of(memberships.begin(), memberships.end(), [&](auto const& membership) {
                auto value = std::string_view(membership);
                if (value.starts_with('#')) value.remove_prefix(1);
                return canonicalRegistryId(value) == name;
            })) return true;
    }
    return false;
}
Json ReplacedModelTarget::summary() const {
    return {{"id", id}, {"kind", replacedModelKindName(kind)}, {"matches", matches},
        {"bones", boneCount}, {"cubes", cubeCount}, {"animation_clips", animations.at("animations").size()},
        {"animation_controllers", controllers.at("animation_controllers").size()},
        {"texture", {{"name", texture.name}, {"path", texture.uv->path},
                     {"width", texture.uv->width}, {"height", texture.uv->height},
                     {"normal", texture.normal ? Json(texture.normal->path) : Json()},
                     {"specular", texture.specular ? Json(texture.specular->path) : Json()}}},
        {"individual_bone_materials", individualBoneMaterials}, {"bone_transparency", boneTransparency}};
}
Json ModelPack::targetAnimations(ReplacedModelTarget const& target) const {
    auto result = target.animations;
    result["ysm_merge_multiline_expr"] = descriptor.at("properties").value("merge_multiline_expr", false);
    result["ysm_legacy_molang"] = true;
    result["ysm_origin_version"] = descriptor.value("properties", Json::object()).value("originVer", "");
    result["ysm_functions"] = functions;
    result["animation_controllers"] = target.controllers.at("animation_controllers");
    return withDefaultTargetAnimations(std::move(result), target.kind, target.matches);
}
ReplacedModelTarget const* ModelPack::findTarget(ReplacedModelKind kind, std::string_view entityId,
    std::span<std::string const> registryTypeTags, int boatVariant) const {
    // ClientCatalogManager.findRenderTarget selects the first matching entry.
    for (auto const& target : targets) if (target.kind == kind && target.matchesEntity(entityId, registryTypeTags, boatVariant)) return &target;
    return nullptr;
}
}
