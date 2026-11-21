#pragma once

#include "Molang.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace ysm {
struct ShoulderOccupants {
    std::array<std::int64_t, 2> sides{};

    void update(std::span<std::int64_t const> passengers, std::array<std::int64_t, 2> restored = {},
        std::array<std::int64_t, 2> const& remote = {}) {
        // Only current passengers may be placed using a peer's snapshot.
        // A local authoritative save takes precedence over either relay slot.
        for (std::size_t side = 0; side < restored.size(); ++side) {
            auto id = remote[side];
            if (id && !restored[side] && restored[1 - side] != id &&
                std::find(passengers.begin(), passengers.end(), id) != passengers.end()) restored[side] = id;
        }
        for (auto& id : sides)
            if (std::find(passengers.begin(), passengers.end(), id) == passengers.end()) id = 0;
        // An authoritative saved side may arrive after the first client frame.
        for (std::size_t side = 0; side < restored.size(); ++side) {
            auto id = restored[side];
            if (!id || std::find(passengers.begin(), passengers.end(), id) == passengers.end()) continue;
            if (sides[1 - side] == id) sides[1 - side] = 0;
            sides[side] = id;
        }
        for (auto id : passengers) {
            if (!id || std::find(sides.begin(), sides.end(), id) != sides.end()) continue;
            auto vacancy = std::find(sides.begin(), sides.end(), 0);
            if (vacancy != sides.end()) *vacancy = id;
        }
    }
};

// Java 2.6.5 bakes one root-to-locator path per shoulder. Numbered sibling
// bones belong to the author and are not additional attachment points.
inline std::optional<glm::mat4> animatedLocator(MolangContext const& context, std::string_view name) {
    if (!context.boneRig) return {};
    auto const& rig = *context.boneRig;
    auto bone = rig.find(name);
    if (bone == rig.end()) return {};
    glm::mat4 matrix(1);
    bool endpoint = true;
    for (std::size_t count = 0; bone != rig.end(); ++count) {
        if (count >= rig.size()) return {};
        auto const& [name, basis] = *bone;
        auto point = [](std::array<double, 3> const& v) {
            return glm::vec3(-v[0], v[1], v[2]) / 16.0f;
        };
        auto pivot = point(basis.pivot);
        MolangContext::BoneChannels identity;
        auto found = context.boneChannels.find(name);
        auto const& pose = found == context.boneChannels.end() ? identity : found->second;
        auto local = glm::translate(glm::mat4(1), pivot + point(pose.position));
        auto const& r = pose.rotation;
        local = glm::rotate(local, glm::radians(float(r[2])), glm::vec3(0, 0, 1));
        local = glm::rotate(local, glm::radians(float(-r[1])), glm::vec3(0, 1, 0));
        local = glm::rotate(local, glm::radians(float(-r[0])), glm::vec3(1, 0, 0));
        local = glm::scale(local, glm::vec3(pose.scale[0], pose.scale[1], pose.scale[2]));
        if (!endpoint) local = glm::translate(local, -pivot);
        matrix = local * matrix;
        endpoint = false;
        if (basis.parent.empty()) break;
        bone = rig.find(basis.parent);
    }
    return matrix;
}

// The Java passenger pass rotates the complete locator into the vehicle's
// frame before cancelling the native riding offset. Animated seats can tilt.
inline std::optional<glm::mat4> passengerLocator(MolangContext const& context, int seat,
    float yawDegrees, glm::vec3 vehicleMinusRider, bool customPlayer) {
    if (seat < 0) return {};
    auto locator = animatedLocator(context, seat == 0 ? "PassengerLocator" : "PassengerLocator" + std::to_string(seat + 1));
    if (!locator) return {};
    auto angle = glm::radians(180.f - yawDegrees);
    auto yaw = glm::rotate(glm::mat4(1), angle, glm::vec3(0, 1, 0));
    if (customPlayer) vehicleMinusRider.y -= 0.5f;
    auto result = glm::translate(yaw * *locator * glm::transpose(yaw), vehicleMinusRider);
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            if (!std::isfinite(result[col][row])) return {};
    return result;
}

inline std::optional<glm::mat4> shoulderLocator(MolangContext const& context, bool left) {
    return animatedLocator(context, left ? "LeftShoulderLocator" : "RightShoulderLocator");
}
}

