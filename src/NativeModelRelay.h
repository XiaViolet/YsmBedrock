#pragma once
#include "AnimationRuntime.h"
#include "ModelLibrary.h"
#include "ModelEffects.h"

class IClientInstance;
class Player;
class Level;
class ActorRenderData;
class ActorResourceDefinition;
class BaseActorRenderContext;
class ClientSkinInfoData;
class SerializedSkinRef;

namespace ysm {
class NativeReplacedResources;
class NativeModelRelay {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    NativeModelRelay();
    ~NativeModelRelay();
    void configure(std::filesystem::path directory);
    void boundary();
    void serverTick(Level& level);
    void roomCommand(std::string command);
    Json roomInfo() const;
    void tick(IClientInstance& client, Player* player, ModelPack const* model, AnimationRuntime& local, NativeReplacedResources const& resources);
    std::array<std::int64_t,2> shoulderSeats(Actor const& owner) const;
    void stop(IClientInstance* client);
    SharedModelCatalog modelChoices() const;
    Json catalogRows() const;
    Json playerRows(Player const* local) const;
    void setVisibility(bool hideOthers, Json const& hiddenPlayers);
    bool canSelect(std::string const& source) const;
    void frame();
    std::vector<ModelEffects> takeEffects(Player const& local);
    void updateEntityOwners(NativeReplacedResources& resources, Player const& local) const;
    void prepareForRender(ActorRenderData const& render, AnimationComponent& component);
    void prepareGeometry(RenderController& controller, std::shared_ptr<DataDrivenGeometry> const& geometry, RenderParams& params);
    void updateTexture(IClientInstance& client, ClientSkinInfoData& skin, SerializedSkinRef const& serialized);
    void bindTexture(SerializedSkinRef const& serialized, std::shared_ptr<ActorResourceDefinition> const& resource);
    bool needsSurfaces(Actor const& actor) const;
    bool rendersPlayer(Actor const& actor) const;
    unsigned handDrawMask(Actor const& actor, bool offhand, bool nativeUi) const;
    bool drivesLanceCharge(Actor const& actor) const;
    bool drawParrot(Actor const& owner, BaseActorRenderContext& context, ActorRenderData& data,
        std::function<void()> const& draw);
    bool suppressGlide(Actor const& actor, bool firstPerson, std::array<float, 2> const& rotation);
    std::span<BoneVisual const> visuals(SkinnedMesh const& mesh, RenderController const* controller,
        std::span<BoneVisual> scratch, std::vector<BoneVisual>& overflow, Actor const* owner = nullptr);
};
}
