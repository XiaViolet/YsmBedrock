#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <optional>

namespace ysm {
// The native BoneOrientation routine itself maps logical (x,y,z) to (x,-y,z).
// The renderer then applies Rz(180), producing Java YSM's (-x,y,z) position.
// Keep authored offsets unchanged here; another Y flip would cancel the
// engine's conversion and lift an authored downward pose above the ground.
inline std::array<double, 3> nativeAnimationPosition(std::array<double, 3> const& authored) {
    return authored;
}

inline double sourcePlayerRenderYOffset(bool crouching, bool worldBody) {
    // ReplacePlayerRenderEvent inherits Java PlayerRenderer's -0.125 crouch
    // render offset; GeoReplacedEntityRenderer then adds its own +0.01 lift.
    // These are entity-origin adjustments, outside the author's model scale.
    return worldBody ? 0.01 - (crouching ? 0.125 : 0.0) : 0.0;
}

// A replicated ground bit can arrive several interpolated positions before the
// client reaches the block. The integrated server supplies the verified point.
struct ProjectileBlockContact {
    bool embedded = false;
    std::array<double, 3> position{};
};
inline bool projectileReachedBlock(bool nativeGround, std::optional<ProjectileBlockContact> const& contact,
    std::array<double, 3> const& renderedPosition) {
    if (!nativeGround || !contact) return nativeGround;
    if (!contact->embedded) return false;
    for (std::size_t axis = 0; axis < renderedPosition.size(); ++axis)
        if (!(std::abs(renderedPosition[axis] - contact->position[axis]) <= 1.0 / 4096.0)) return false;
    return true;
}

struct ProjectileRenderRotation { double yaw, pitch; };
inline ProjectileRenderRotation nativeProjectileRotation(double previousYaw, double yaw,
    double previousPitch, double pitch, double partialTick, double renderBodyYaw) {
    auto alpha = std::isfinite(partialTick) ? std::clamp(partialTick, 0.0, 1.0) : 0.0;
    auto interpolatedYaw = previousYaw + (yaw - previousYaw) * alpha;
    auto interpolatedPitch = previousPitch + (pitch - previousPitch) * alpha;
    // Native outer matrix: Ry(180-renderBodyYaw) * Rz(180).
    // YSM outer matrix: Ry(interpolatedYaw-90) * Rz(interpolatedPitch).
    // Conjugating by Rz(180) gives these two native carrier rotations.
    // renderBodyYaw MUST be Actor::getInterpolatedBodyYaw, the native renderer's
    // slot 0x78, rather than the distinct getInterpolatedBodyRot (slot 0x68).
    return {-interpolatedYaw - renderBodyYaw - 90.0, interpolatedPitch};
}
}
