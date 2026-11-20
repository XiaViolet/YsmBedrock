#include "NativeBlockQueries.h"
#include "EntityQueries.h"
#include "NativeWeather.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/biome/Biome.h"
#include "mc/world/phys/HitResult.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/deps/renderer/Camera.h"
#include <cmath>
#include <numbers>

namespace ysm {
BedrockWeatherSnapshot sampleNativeSceneQueries(EntityQueryInput& input, Actor const& subject, BaseActorRenderContext* render) {
    auto& region = subject.getDimension().getBlockSourceFromMainChunkSource();
    auto feet = subject.getFeetBlockPos();
    auto weather = readNativeWeather(subject.getLevel());
    input.setBedrockWeather(weather.rain, weather.lightning);
    input.blockY = feet.y;
    if (region.getChunkAt(feet)) {
        input.skyVisible = region.canSeeSky(feet);
        // The lighting heightmap excludes clear glass. Java also requires
        // the column above motion-blocking blocks and fluids to be open.
        input.blockingHeight = region.getAboveTopSolidBlock(feet.x, feet.z, true, true);
        input.biomeId = canonicalRegistryId(region.getBiome(feet).mHash->getString());
    }
    input.spectator = subject.isSpectator();
    input.onFire = subject.isOnFire();
    input.hasRider = subject.hasPassenger();
    if (!render) return weather;
    auto const& position = *render->mImpl->mCameraPosition;
    input.cameraPosition = {position.x, position.y, position.z};
    auto& client = render->mClientInstance;
    auto const& forward = *client.getCamera().mForward;
    auto horizontal = std::hypot(forward.x, forward.z);
    if (std::isfinite(horizontal) && std::isfinite(forward.y) && (horizontal > 0 || forward.y != 0)) {
        constexpr double degrees = 180 / std::numbers::pi;
        input.cameraRotation = {-std::atan2(forward.y, horizontal) * degrees,
            std::atan2(-forward.x, forward.z) * degrees};
    }
    if (client.getLocalPlayer() != &subject) return weather;
    auto const& hit = client.getLatestHitResult();
    if (hit.mType == HitResultType::Entity) {
        if (auto* target = hit.getEntity()) {
            input.hitTargetId = target->getActorIdentifier().mCanonicalName->getString();
            input.hitTargetType = "entity";
        }
    } else if (hit.mType == HitResultType::Tile && region.getChunkAt(hit.mBlock)) {
        input.hitTargetId = region.getBlock(hit.mBlock).getTypeName();
        input.hitTargetType = "block";
    }
    return weather;
}

NativeBlockQueryScope::NativeBlockQueryScope(MolangContext& target, Actor const& subject)
    : context(target), previous(std::move(target.blockLookup)) {
    context.blockLookup = [this, &subject](auto const& coordinate, auto tags, bool requireAll) -> MolangValue {
        BlockPos position{coordinate[0], coordinate[1], coordinate[2]};
        auto& region = subject.getDimension().getBlockSourceFromMainChunkSource();
        if (position.y < region.getMinHeight() || position.y >= region.getMaxHeight())
            return blockQueryResult("minecraft:void_air", tags, {}, requireAll);
        auto [entry, inserted] = sampled.try_emplace(coordinate, nullptr);
        if (inserted && region.getChunkAt(position)) entry->second = &region.getBlock(position);
        auto block = entry->second;
        if (!block) return {};
        return blockQueryResult(block->getTypeName(), tags, [block](auto const& tag) {
            return block->hasTag(HashedString(tag));
        }, requireAll);
    };
}
NativeBlockQueryScope::~NativeBlockQueryScope() { context.blockLookup = std::move(previous); }
}
