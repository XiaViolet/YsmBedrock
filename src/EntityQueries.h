#pragma once
#include "Molang.h"
#include "JavaVanillaInputs.h"
#include <cstdint>

namespace ysm {
// An empty tag list requests a block ID. The optional predicate keeps the
// world's own tags available alongside the pinned Java compatibility tags.
MolangValue blockQueryResult(std::string_view blockId, std::span<MolangValue const> tags,
    std::function<bool(std::string const&)> const& hasNativeTag = {}, bool requireAll = false);

// Host snapshot in game units. Head angles have already been converted to
// YSM's convention; world yaw stays in Minecraft's world convention.
struct EntityQueryInput {
    std::array<double, 3> position{};
    std::optional<std::array<double, 3>> renderPosition;
    std::optional<std::array<double, 3>> cameraPosition;
    std::array<double, 2> cameraRotation{};
    std::string hitTargetId, hitTargetType, biomeId;
    bool player = true;
    bool living = true;
    std::array<double, 3> velocity{};
    // Local movement input axes, independent of physical displacement.
    std::array<double, 3> moveInput{};
    std::optional<double> walkAnimationSpeed, ownerGroundSpeed;
    std::optional<std::array<double,2>> playerAxes;
    double framesPerSecond = 0;
    bool nativeMoveInputAvailable = false;
    double yaw = 0, viewYaw = 0, bodyYaw = 0, headPitch = 0, headYaw = 0;
    double health = 0, maxHealth = 0, foodLevel = 0, worldTime = 0;
    double renderTicks = 0, rainLevel = 0, thunderLevel = 0;
    std::int64_t tick = 0, uuidLeast = 0;
    int blockY = 0, blockingHeight = 0, viewPerspective = 1, arrowCount = 0;
    int playerLevel = 0, itemUseTicks = 0, itemMaxUseTicks = 0, equipmentCount = 0;
    bool usingItem = false, spectator = false, onFire = false, hasRider = false;
    bool onGround = false, crouching = false, passenger = false;
    bool inWater = false, inRain = false, swimming = false, flying = false, gliding = false;
    bool sprinting = false, crawling = false, sleeping = false, alive = true;
    bool eating = false, fishing = false, skyVisible = false;
    bool renderingInInventory = false;
    bool onClimbable = false, riptide = false, hurt = false;
    std::optional<double> elytraRotationZ;
    bool emulateElytra = false, elytraEquipped = false, renderElytraLayer = true;
    std::uint64_t renderFrame = 0;
    std::string mainAnimation() const;
    void setHeadAngles(double rawPitch, double interpolatedBodyYaw, double interpolatedHeadYaw);
    void setBedrockWeather(double rainStrength, double lightningStrength);
};
class AttachedArrows {
    int count = 0;
    std::int64_t lastTick = -1, expireAt = 0;
public:
    int sample(std::int64_t tick, bool alive = true);
    void hit(std::int64_t tick);
};

class EntityQueries {
    bool observed = false;
    std::int64_t lastTick = 0;
    double lastYaw = 0, yawSpeed = 0;
    std::optional<std::array<double, 3>> previousRenderPosition;
    std::array<double, 3> renderDelta{};
    double positionTick = 0, positionTickDelta = 0;
    JavaElytraInput elytra;
public:
    void bind(MolangContext& context, EntityQueryInput const& input);
};
}
