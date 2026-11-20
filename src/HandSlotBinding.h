#pragma once
#include <cctype>
#include <string>
#include <string_view>
#include <cmath>
#include <optional>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace ysm {
// A YSM charge drives the grip. Suppress only the vanilla spear's own
// lowering roll, preserving any other animated axes.
inline bool usesAnimatedLanceRoll(bool vanillaCharge, glm::vec3 rotation) {
    return vanillaCharge && !rotation.x && !rotation.y && rotation.z != 0;
}
inline glm::mat4 slotRootAtGrip(glm::mat4 const& grip, glm::vec3 position, glm::vec3 rotation, glm::vec3 scale) {
    auto pose = glm::translate(grip, {position.x, -position.y, position.z});
    if (rotation.z) pose = glm::rotate(pose, glm::radians(rotation.z), {0,0,1});
    if (rotation.y) pose = glm::rotate(pose, glm::radians(rotation.y), {0,1,0});
    if (rotation.x) pose = glm::rotate(pose, glm::radians(rotation.x), {1,0,0});
    return glm::scale(pose, scale);
}
// Only the native equipment-slot query designates a hand here. Other
// expressions may intentionally bind equipment to the head or torso.
inline bool usesEquipmentSlotBone(std::string_view source) {
    if (source == "q.item_slot_to_bone_name(c.item_slot)") return true;
    std::string expression;
    for (unsigned char c : source)
        if (!std::isspace(c)) expression += static_cast<char>(std::tolower(c));
    if (expression.ends_with(';')) expression.pop_back();
    std::string_view query = expression;
    if (query.starts_with("query.")) query.remove_prefix(6);
    else if (query.starts_with("q.")) query.remove_prefix(2);
    else return false;
    return query == "item_slot_to_bone_name(c.item_slot)"
        || query == "item_slot_to_bone_name(context.item_slot)";
}

inline std::optional<glm::mat4> retargetSlotBone(glm::mat4 const& child, glm::mat4 const* oldParent,
    glm::mat4 const& target, bool localIdentity, bool sameParent) {
    if (localIdentity) return target;
    if (sameParent) return {};
    if (!oldParent) return target * child;
    if (std::abs(glm::determinant(*oldParent)) < 1e-10f) return {};
    return target * glm::inverse(*oldParent) * child;
}
}
