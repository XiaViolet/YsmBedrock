#include "NativeShoulderParrot.h"
#include "AnimationRuntime.h"
#include "NativeReplacedResources.h"
#include "RenderSpace.h"
#include "ShoulderLocator.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/provider/ActorRiding.h"
#include "mc/deps/vanilla_components/StrictActorIDEntityContextPair.h"
#include "mc/world/level/Level.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/actor/ActorRenderData.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/deps/renderer/Camera.h"
#include <glm/gtc/matrix_transform.hpp>
#include <mutex>

namespace ysm {
namespace {
std::mutex seatsMutex;
std::map<std::pair<Level const*, std::int64_t>, ShoulderOccupants> seatsByOwner;
struct PreviewOwner {
    BaseActorRenderContext const* context{};
    Actor const* actor{};
    glm::vec3 position{};
};
thread_local PreviewOwner preview;
}

std::array<std::int64_t, 2> observeShoulderPassengers(Actor const& owner, NativeReplacedResources const& resources,
    std::array<std::int64_t, 2> const& remote) {
    auto ownerKey = std::pair{&owner.getLevel(), owner.getOrCreateUniqueID().rawID};
    auto const& riders = ActorRiding::getPassengers(owner.getEntityContext());
    std::array<std::int64_t, 2> parrots{};
    std::array<std::int64_t, 2> restored{};
    std::size_t count = 0;
    for (auto const& rider : riders) {
        auto* actor = owner.getLevel().fetchEntity(*rider.mActorID, false);
        if (!actor || !actor->isType(ActorType::Parrot)) continue;
        auto birdId = rider.mActorID->rawID;
        parrots[count++] = birdId;
        if (auto left = resources.savedShoulderSeat(*actor, owner)) restored[*left ? 0 : 1] = birdId;
        if (count == parrots.size()) break;
    }
    auto mounted = std::span(parrots).first(count);
    std::scoped_lock lock(seatsMutex);
    if (count) {
        auto& seats = seatsByOwner[ownerKey];
        seats.update(mounted, restored, remote);
        return seats.sides;
    }
    seatsByOwner.erase(ownerKey);
    return {};
}

std::array<int, 2> shoulderParrotVariants(Actor const& owner) {
    std::array<int, 2> variants{-1, -1};
    auto& level = owner.getLevel();
    std::array<std::int64_t, 2> ids{};
    {
        std::scoped_lock lock(seatsMutex);
        auto it = seatsByOwner.find({&level, owner.getOrCreateUniqueID().rawID});
        if (it != seatsByOwner.end()) ids = it->second.sides;
    }
    for (auto const& rider : ActorRiding::getPassengers(owner.getEntityContext())) {
        auto found = std::find(ids.begin(), ids.end(), rider.mActorID->rawID);
        if (found == ids.end()) continue;
        auto* bird = level.fetchEntity(*rider.mActorID, false);
        if (bird && bird->isType(ActorType::Parrot)) variants[found-ids.begin()] = bird->getVariant();
    }
    return variants;
}

void clearShoulderPassengers() {
    std::scoped_lock lock(seatsMutex);
    seatsByOwner.clear();
    preview = {};
}

void observeShoulderPreview(BaseActorRenderContext const* context, ActorRenderData const& data) {
    if (!data.mActor || !data.mActor->isPlayer()) return;
    if (context && (context->mIsOnScreen || data.mIsInUI)) {
        auto& screen = static_cast<ScreenContext&>(context->mScreenContext);
        auto const& camera = static_cast<mce::Camera&>(screen.camera);
        auto const& projection = *camera.projectionMatrixStack->top()._m;
        // Menu controls use an orthographic projection (constant clip w).
        // Bedrock also sets mIsOnScreen for world and first-person passes.
        if (projection[0][3] == 0.f && projection[1][3] == 0.f &&
            projection[2][3] == 0.f && projection[3][3] != 0.f) {
            auto p = *data.position;
            preview = {context, data.mActor, {p.x, p.y, p.z}};
            return;
        }
    }
    preview = {};
}

Actor* shoulderParrotOwner(Actor const& bird) {
    if (!bird.isType(ActorType::Parrot)) return nullptr;
    auto* owner = bird.getVehicle();
    return owner && owner->isPlayer() ? owner : nullptr;
}

static glm::vec3 interpolatedFeet(Actor const& actor, float alpha) {
    auto p = actor.getInterpolatedPosition(alpha);
    auto feet = actor.getFeetPos();
    return {p.x, p.y + feet.y - actor.getPosition().y, p.z};
}

void drawShoulderParrot(AnimationRuntime const& model, Actor const& owner, BaseActorRenderContext& context,
    ActorRenderData& data, std::function<void()> const& draw) {
    auto& bird = *static_cast<Actor*>(data.mActor);
    int seat = owner.getPassengerIndex(bird);
    if (seat < 0 || !context.mImpl) {
        draw();
        return;
    }
    float alpha = context.mFrameAlpha;
    float yaw = glm::radians(owner.getInterpolatedBodyYaw(alpha));
    // Bedrock compacts both seat indices and seat offsets when seat 0 leaves.
    // Java keeps the surviving shoulder NBT in its original independent side.
    bool left = seat == 0;
    {
        std::scoped_lock lock(seatsMutex);
        auto found = seatsByOwner.find({&owner.getLevel(), owner.getOrCreateUniqueID().rawID});
        if (found != seatsByOwner.end()) left = found->second.sides[0] == bird.getOrCreateUniqueID().rawID;
    }
    bool inventory = (context.mIsOnScreen || data.mIsInUI) && preview.context == &context && preview.actor == &owner;
    auto locator = model.shoulderTransform(left, inventory);
    if (!locator) return;

    auto rotateY = [](float angle) { return glm::rotate(glm::mat4(1), angle, glm::vec3(0, 1, 0)); };
    glm::vec3 relative;
    if (inventory) {
        // LivePlayerRenderer submits the player and bird in the same UI
        // matrix. Its camera position still describes the world behind the
        // menu. Use the player's preview position and native feet offset.
        relative = preview.position;
        relative.y += owner.getFeetPos().y - owner.getPosition().y;
    } else {
        auto feet = interpolatedFeet(owner, alpha);
        feet.y += static_cast<float>(sourcePlayerRenderYOffset(owner.isSneaking(), true));
        auto camera = *context.mImpl->mCameraPosition;
        relative = feet - glm::vec3(camera.x, camera.y, camera.z);
    }
    auto world = glm::translate(glm::mat4(1), relative) * rotateY(glm::pi<float>() - yaw) * *locator;
    // The native renderer supplies its own entity yaw and mesh orientation.
    // Remove that yaw before its call; keep the original bird's variant,
    // riding animation, lighting and RenderDragon submission.
    world *= rotateY(glm::radians(bird.getInterpolatedBodyYaw(alpha)) - glm::pi<float>());
    auto& screen = static_cast<ScreenContext&>(context.mScreenContext);
    auto& cameraState = static_cast<mce::Camera&>(screen.camera);
    auto stackEntry = cameraState.worldMatrixStack->push(false);
    auto& current = cameraState.worldMatrixStack->getTop();
    *current._m *= world;
    struct RestorePosition {
        ActorRenderData& data;
        Vec3 previous;
        ~RestorePosition() { *data.position = previous; }
    } restore{data, *data.position};
    *data.position = Vec3(0, 0, 0);
    draw();
}
}

