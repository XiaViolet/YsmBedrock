#include "EntityQueries.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace ysm {
static int arrowLifetime(int count) { return std::max(1, 20 * (30 - count)); }
int AttachedArrows::sample(std::int64_t tick, bool alive) {
    if (!alive || tick < lastTick) count = 0;
    lastTick = tick;
    while (count && tick >= expireAt) {
        --count;
        if (count) expireAt += arrowLifetime(count);
    }
    return count;
}
void AttachedArrows::hit(std::int64_t tick) {
    sample(tick);
    if (count == 0) expireAt = tick + arrowLifetime(1);
    if (count < 127) ++count;
}
MolangValue blockQueryResult(std::string_view blockId, std::span<MolangValue const> tags,
    std::function<bool(std::string const&)> const& hasNativeTag, bool requireAll) {
    if (tags.empty()) return std::string(blockId);
    for (auto const& argument : tags) {
        if (!argument.isString()) return {};
        auto tag = canonicalRegistryId(argument.string());
        if (tag.empty()) return {};
        bool matches = hasJavaBlockTag(blockId, tag) || (hasNativeTag && hasNativeTag(tag));
        if (matches != requireAll) return requireAll ? 0.0 : 1.0;
    }
    return requireAll ? 1.0 : 0.0;
}

std::string EntityQueryInput::mainAnimation() const {
    // AnimationRegister and PlayerMainPredicate define the source priority.
    if (passenger) return {};
    if (!alive) return "death";
    if (riptide) return "riptide";
    if (sleeping) return "sleep";
    if (swimming) return "swim";
    bool moving = walkAnimationSpeed ? std::abs(static_cast<float>(*walkAnimationSpeed)) > 0.05f
        : std::hypot(velocity[0], velocity[2]) * 20 > 0.1;
    if (crawling) return moving ? "climb" : "climbing";
    if (onClimbable) return velocity[1] > 0 ? "ladder_up" : velocity[1] < 0 ? "ladder_down" : "ladder_stillness";
    if (flying) return "fly";
    if (gliding) return "elytra_fly";
    if (inWater && !onGround) return "swim_stand";
    if (hurt) return "attacked";
    if (!onGround && !inWater) return "jump";
    if (onGround && crouching) return moving ? "sneak" : "sneaking";
    if (onGround && sprinting) return "run";
    // Walking uses the signed limb amount; crawling and sneaking above use
    // its absolute value (AnimationRegister.MIN_SPEED is a Java float).
    bool walking = walkAnimationSpeed ? static_cast<float>(*walkAnimationSpeed) > 0.05f : moving;
    if (onGround && walking) return "walk";
    return "idle";
}
void EntityQueryInput::setHeadAngles(double rawPitch, double interpolatedBodyYaw, double interpolatedHeadYaw) {
    bodyYaw = interpolatedBodyYaw;
    headPitch = -rawPitch;
    auto net = std::fmod(interpolatedHeadYaw - interpolatedBodyYaw, 360.0);
    if (net >= 180) net -= 360;
    if (net < -180) net += 360;
    headYaw = -std::clamp(net, -85.0, 85.0);
}
void EntityQueryInput::setBedrockWeather(double rainStrength, double lightningStrength) {
    // Bedrock's weather packets may carry weak rain/thunder (observed 0.05).
    // These are active weather types, not Java's 0..1 transition amplitudes.
    // Normalize host types before applying the source query's Java thresholds.
    auto active = [](double strength) { return std::isfinite(strength) ? (strength > 0 ? 1.0 : 0.0) : strength; };
    rainLevel = active(rainStrength);
    thunderLevel = active(lightningStrength);
}
// Adapted from the pinned YSM EntityBuiltins and PlayerStateTracker (Apache-2.0).
void EntityQueries::bind(MolangContext& context, EntityQueryInput const& input) {
    context.particleOrigin = {input.position, input.yaw, input.bodyYaw, input.player};
    auto finite = [&](double value, char const* field) {
        if (std::isfinite(value)) return value;
        context.diagnose(std::string("Non-finite entity input: ") + field);
        return 0.0;
    };
    auto flag = [&](char const* key, bool value) { context.queries[key] = value ? 1.0 : 0.0; };
    auto number = [&](char const* key, double value) { context.queries[key] = finite(value, key); };
    auto wrap = [](double value) {
        auto result = std::fmod(value, 360.0);
        if (result >= 180) result -= 360;
        if (result < -180) result += 360;
        return result;
    };
    auto yaw = finite(input.yaw, "yaw");
    // Local Java players sample yaw speed on entity ticks. Render/hand passes
    // within one tick must observe the same value, independent of render FPS.
    if (!observed || input.tick < lastTick) yawSpeed = 0;
    else if (input.tick > lastTick) {
        auto ticks = static_cast<long double>(input.tick) - static_cast<long double>(lastTick);
        yawSpeed = finite(static_cast<double>((static_cast<long double>(yaw) - lastYaw) * 20 / ticks), "yaw_speed");
    }
    if (!observed || input.tick != lastTick) {
        observed = true;
        lastTick = input.tick;
        lastYaw = yaw;
    }
    number("query.yaw_speed", yawSpeed);
    auto x = finite(input.velocity[0], "velocity_x"), y = finite(input.velocity[1], "velocity_y"),
         z = finite(input.velocity[2], "velocity_z");
    auto groundSpeed = finite(input.ownerGroundSpeed.value_or(std::hypot(x, z)*20), "query.ground_speed");
    context.queries["query.ground_speed"] = groundSpeed;
    auto renderTick = finite(input.renderTicks, "render_ticks");
    auto position = input.renderPosition.value_or(input.position);
    for (auto& axis : position) axis = finite(axis, "position");
    context.entityPosition = position;
    if (input.renderPosition && !input.renderingInInventory) {
        auto sampleTick = finite(input.positionSampleTicks.value_or(renderTick), "position_sample_ticks");
        if (!previousRenderPosition || sampleTick < positionTick) {
            renderDelta = {};
            positionTickDelta = 0;
        } else if (sampleTick > positionTick) {
            for (std::size_t axis = 0; axis < renderDelta.size(); ++axis)
                renderDelta[axis] = finite(position[axis] - (*previousRenderPosition)[axis], "render_position_delta");
            positionTickDelta = finite(sampleTick - positionTick, "render_tick_delta");
        }
        // Repeated passes at the same owner time observe the current position
        // but keep the delta and baseline from the accepted logical update.
        if (!previousRenderPosition || sampleTick != positionTick) {
            previousRenderPosition = position;
            positionTick = sampleTick;
        }
    }
    context.positionDelta = input.renderPosition ? renderDelta : std::array{x, y, z};
    auto const& delta = context.positionDelta;
    auto tickSpan = input.renderPosition ? positionTickDelta : 1;
    auto horizontal = std::hypot(delta[0], delta[2]);
    auto trackedSpeed = tickSpan > 0 ? horizontal * 20 / tickSpan : 0;
    number("ysm.ground_speed2", input.renderPosition ? trackedSpeed : groundSpeed);
    number("query.vertical_speed", tickSpan > 0 ? delta[1] * 20 / tickSpan : 0);
    auto viewYaw = finite(input.viewYaw, "query.eye_target_y_rotation");
    double lateral = 0, forward = 0;
    if(input.playerAxes){
        lateral=(*input.playerAxes)[0];
        forward=(*input.playerAxes)[1];
    }else if (horizontal >= 1.0e-4) {
        constexpr double radians = std::numbers::pi / 180;
        auto motionAngle = std::atan2(delta[2], delta[0]) / radians;
        auto relative = wrap(motionAngle - (90 - wrap(-viewYaw))) * radians;
        lateral = std::sin(relative);
        forward = std::cos(relative);
    }
    number("ysm.input_horizontal", lateral);
    number("ysm.input_vertical", forward);
    // The source reads LivingEntity.yya / tracked moveY, not forward zza or
    // world velocity. Keep the signed local input and its original units.
    number("ysm.yya", input.living ? input.moveInput[1] : 0);
    number("ysm.fps", std::max(0.0, std::floor(finite(input.framesPerSecond, "fps"))));
    number("query.body_y_rotation", wrap(finite(input.bodyYaw, "body_yaw")));
    number("query.eye_target_y_rotation", viewYaw);
    // The upstream query names intentionally expose net yaw as x and pitch as y.
    number("query.head_x_rotation", input.headYaw);
    number("query.head_y_rotation", input.headPitch);
    number("ysm.head_pitch", input.headPitch);
    number("ysm.head_yaw", input.headYaw);
    auto roll = input.elytraRotationZ ? *input.elytraRotationZ : input.emulateElytra
        ? elytra.sample(input.renderFrame, input.renderElytraLayer && !input.renderingInInventory,
            input.elytraEquipped, input.gliding, input.crouching, input.velocity) : 0.0f;
    number("ysm.elytra_rot_z", roll);
    static constexpr int directions[]{3, 4, 2, 5}; // south, west, north, east (Java Direction IDs)
    auto quadrant = static_cast<int>(std::floor(wrap(yaw) / 90 + 0.5));
    number("query.cardinal_facing_2d", directions[(quadrant % 4 + 4) % 4]);
    number("query.health", input.living ? input.health : 0);
    number("query.max_health", input.living ? input.maxHealth : 0);
    number("ysm.food_level", input.player ? input.foodLevel : 0);
    number("query.time_stamp", input.worldTime);
    number("query.player_level", input.player ? input.playerLevel : 0);
    auto cameraDistance = 0.0;
    if (input.cameraPosition && !input.renderingInInventory) {
        auto const& camera = *input.cameraPosition;
        cameraDistance = std::hypot(camera[0] - position[0], camera[1] - position[1], camera[2] - position[2]);
    }
    number("query.distance_from_camera", cameraDistance);
    context.cameraRotation = input.renderingInInventory ? std::array<double, 2>{} : input.cameraRotation;
    context.biomeId = input.biomeId;
    context.queries["ysm.hit_target_id"] = input.renderingInInventory ? "" : input.hitTargetId;
    context.queries["ysm.hit_target_type"] = input.renderingInInventory ? "" : input.hitTargetType;
    auto useMaximum = input.usingItem ? std::max(0, input.itemMaxUseTicks) : 0;
    auto useElapsed = std::clamp(input.itemUseTicks, 0, useMaximum);
    number("query.item_in_use_duration", useElapsed / 20.0);
    number("query.item_max_use_duration", useMaximum / 20.0);
    number("query.item_remaining_use_duration", (useMaximum - useElapsed) / 20.0);
    number("query.equipment_count", input.equipmentCount);
    flag("query.is_using_item", input.usingItem);
    flag("query.is_spectator", input.spectator);
    flag("query.is_on_fire", input.onFire);
    flag("query.has_rider", input.hasRider);
    flag("query.is_on_ground", input.onGround);
    flag("query.is_sneaking", input.onGround && input.crouching);
    flag("query.is_jumping", input.player && !input.flying && !input.passenger && !input.onGround && !input.inWater);
    flag("query.is_in_water", input.inWater);
    flag("query.is_in_water_or_rain", input.inWater || input.inRain);
    flag("query.is_riding", input.passenger);
    flag("query.is_swimming", input.swimming);
    flag("query.is_flying", input.flying);
    flag("query.is_gliding", input.gliding);
    flag("query.is_sprinting", input.sprinting);
    flag("query.is_crawling", input.crawling);
    flag("query.is_sleeping", input.sleeping);
    flag("query.is_alive", input.alive);
    flag("query.is_eating", input.living && input.eating);
    flag("ysm.is_fishing", input.player && input.fishing);
    flag("ysm.is_open_air", input.skyVisible && input.blockingHeight <= input.blockY);
    flag("ysm.rendering_in_inventory", input.renderingInInventory);
    number("ysm.arrow_count", input.living && input.alive ? input.arrowCount : 0);
    flag("query.is_first_person", !input.renderingInInventory && input.viewPerspective == 0);
    number("ysm.person_view", input.viewPerspective);
    auto rain = finite(input.rainLevel, "rain_level"), thunder = finite(input.thunderLevel, "thunder_level");
    number("ysm.weather", rain * thunder > 0.9 ? 2 : rain > 0.2 ? 1 : 0);
    // Java Math.abs(long) deliberately leaves MIN_VALUE negative. Preserve its
    // remainder without overflowing signed C++ arithmetic.
    auto offset = input.uuidLeast == std::numeric_limits<std::int64_t>::min() ? -8
        : (input.uuidLeast < 0 ? -input.uuidLeast : input.uuidLeast) % 10;
    auto blink = std::fmod(renderTick + offset, 90.0);
    flag("ysm.is_close_eyes", input.living && (input.sleeping || (blink > 85 && blink < 90)));
    number("global.o", 0);
}
}
