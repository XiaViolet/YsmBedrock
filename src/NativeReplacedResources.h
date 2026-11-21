#pragma once
#include "ModelPack.h"
#include "MolangRoaming.h"
#include "ModelEffects.h"
#include "BoneVisual.h"
#include <glm/mat4x4.hpp>
#include <memory>
#include <mutex>
class Actor;
class Level;
class CompoundTag;
class LocalPlayer;
class DataDrivenModel;
class IClientInstance;
class ActorRenderer;
class RenderParams;
class RenderController;
class SkinnedMesh;
class DataDrivenGeometry;
namespace ysm {
// Owns native model and animation resources for assigned entities.
class NativeReplacedResources : public std::enable_shared_from_this<NativeReplacedResources> {
    struct Impl;
    struct Storage;
    std::unique_ptr<Storage> storage;
    std::unique_ptr<Impl> impl;
    mutable std::mutex mutex;
    bool showVehicles = true, showProjectiles = true;
public:
    struct PeerOwner {
        std::int64_t id;
        std::shared_ptr<ModelPack const> pack;
        RoamingPayload roaming;
        std::optional<std::pair<std::int64_t, std::int64_t>> mount;
    };
    NativeReplacedResources();
    ~NativeReplacedResources();
    void attachAuthority(Level const* level, Level const* clientLevel);
    void entityLoaded(Actor const& actor, CompoundTag const& tag);
    void entitySaving(Actor const& actor, CompoundTag& tag);
    std::optional<bool> savedShoulderSeat(Actor const& bird, Actor const& owner) const;
    void serverTick(Level& level);
    void resolveSavedModels(class ModelLibrary const& library);
    bool hasActiveTarget(Actor const& actor) const;
    std::optional<glm::mat4> passengerTransform(Actor const& rider, Actor const& vehicle, float alpha, bool customPlayer) const;
    void updateOwner(std::int64_t owner, ModelPack const& pack, RoamingPayload const& roaming);
    void clearOwner(std::int64_t owner);
    void updatePeerOwners(std::span<PeerOwner const> peers);
    void observeMount(Actor const& rider);
    void projectileOwnerSet(Actor const& projectile, std::int64_t owner);
    void projectileClientSpawned(Actor const& projectile, LocalPlayer const* localPlayer);
    void observeProjectile(Actor const& projectile);
    void observeRenderRoute(Actor const& actor, bool enabled);
    Json assignmentReport(bool enabled) const;
    void collectRetired();
    std::vector<ModelEffects> takeEffects();
    void rememberPrototype(std::shared_ptr<DataDrivenModel> const& model);
    void prepareFrame(RenderParams& params, std::filesystem::path const& report);
    void prepareGeometry(RenderController const& controller, std::shared_ptr<DataDrivenGeometry> const& geometry,
        RenderParams const& params);
    std::span<BoneVisual const> visuals(SkinnedMesh const& mesh, RenderController const* controller,
        std::span<BoneVisual> scratch, std::vector<BoneVisual>& overflow);
    std::shared_ptr<ActorRenderer> resolveRenderer(Actor& actor, std::shared_ptr<ActorRenderer> original,
        IClientInstance& client, std::filesystem::path const& report);
    std::shared_ptr<DataDrivenModel> resolveModel(Actor& actor, std::shared_ptr<DataDrivenModel> original,
        IClientInstance& client, std::filesystem::path const& report);
    void reset();
    void presentation(bool vehicles, bool projectiles);
};
}
