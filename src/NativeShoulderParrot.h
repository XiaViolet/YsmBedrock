#pragma once
#include <functional>
#include <array>
#include <cstdint>
#include <glm/mat4x4.hpp>
#include <optional>

class Actor;
class ActorRenderData;
class BaseActorRenderContext;
namespace ysm {
class AnimationRuntime;
class NativeReplacedResources;
Actor* shoulderParrotOwner(Actor const& bird);
std::array<std::int64_t, 2> observeShoulderPassengers(Actor const& owner, NativeReplacedResources const& resources,
    std::array<std::int64_t, 2> const& remote = {});
std::array<int, 2> shoulderParrotVariants(Actor const& owner);
void clearShoulderPassengers();
void observeShoulderPreview(BaseActorRenderContext const* context, ActorRenderData const& data);
void drawShoulderParrot(AnimationRuntime const& model, Actor const& owner, BaseActorRenderContext& context,
    ActorRenderData& data, std::function<void()> const& draw);
}

