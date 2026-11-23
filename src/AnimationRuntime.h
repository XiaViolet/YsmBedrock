#pragma once
#include "ModelParticles.h"
#include "ModelPack.h"
#include "MolangRoamingSession.h"
#include "BoneVisual.h"
#include "NativeTextureSet.h"
#include "ModelRelay.h"
#include <glm/mat4x4.hpp>
#include <memory>
#include <set>
class Player;
class Actor;
struct BedrockTextureData;
class RenderController;
class DataDrivenGeometry;
class RenderParams;
class AnimationComponent;
class SkinnedMesh;
namespace ysm {
struct AnimationState;
struct CompiledPlayerModel;
class AnimationSampler;
struct PreviewPose;
class AnimationRuntime {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    using Prepared = std::shared_ptr<AnimationState>;
    struct RoamingPreparation { RoamingInstallationId installation; std::set<std::string> fields; };
    struct RoamingChanges { RoamingInstallationId installation; RoamingPayload delta; };
    AnimationRuntime();
    ~AnimationRuntime();
    static std::shared_ptr<CompiledPlayerModel const> compileModel(ModelPack const& pack);
    static Prepared prepare(ModelPack const& pack, std::filesystem::path diagnostics,
        std::shared_ptr<CompiledPlayerModel const> compiled = {});
    static std::shared_ptr<AnimationSampler const> preparedBodySampler(Prepared const& prepared);
    std::string const* skinGeometry() const;
    static std::vector<std::string> preparedDiagnostics(Prepared const& prepared);
    static RoamingPreparation roamingPreparation(Prepared const& prepared);
    static void restoreRoaming(Prepared const& prepared, RoamingPayload const& values);
    RoamingChanges takeRoamingChanges(bool retire = false);
    RoamingPayload roamingSnapshot() const;
    std::vector<SoundCue> takeSoundCues();
    std::vector<ParticleCue> takeParticleCues();
    std::shared_ptr<PreviewPose const> previewPose() const;
    std::vector<std::string> debugLines() const;
    void start(Prepared prepared);
    void start(ModelPack const& pack, std::filesystem::path diagnostics);
    void swap(AnimationRuntime& other) noexcept;
    void suspend(Player& player);
    void attach(Player& player);
    void detach(Player& player);
    void preview(std::string state);
    bool playExtra(std::string state);
    void stopExtra();
    void updateExtraMovement(bool locked, float strafe, float forward, bool jump, bool sneak);
    void relayEnabled(bool enabled);
    RelayPose outgoingPose(Player const& owner, std::uint64_t& eventSequence, bool takeEvents);
    void applyPeerPose(RelayPose const& peer, std::uint64_t& eventCursor, bool replaceAction, Player const& actor);
    void remoteFrame();
    bool hasWorldBodyPose() const;
    Json modelUiValues() const;
    bool modelUiAction(std::string const& route, double value, int label = -1);
    void setPaused(bool paused);
    void setHostInputs(std::set<int> keys, bool inGame, std::map<std::string, std::string> const& versions, bool newFrame = false);
    void setRenderTexture(NativeTextureSet texture, std::string const& name);
    void trackSeats(std::weak_ptr<class NativeReplacedResources> source);
    void prepareForRender(Player& player, AnimationComponent& component);
    void reportFirstPersonPass(bool rendered);
    void prepareGeometry(RenderController& controller, std::shared_ptr<DataDrivenGeometry> const& geometry, RenderParams& params);
    std::span<BoneVisual const> boneVisuals(SkinnedMesh const& mesh, RenderController const* controller,
        std::span<BoneVisual> local, std::vector<BoneVisual>& overflow, Actor const* owner = nullptr);
    bool needsIndividualSurfaces(Actor const& actor) const;
    bool rendersPlayer(Actor const& actor) const;
    unsigned handDrawMask(Actor const& actor, bool offhand, bool nativeUi) const;
    bool drivesLanceCharge(Actor const& actor) const;
    std::optional<glm::mat4> shoulderTransform(bool left, bool inventory = false) const;
    bool suppressNativeGlideRotation(Actor const& actor, bool firstPerson, std::array<float, 2> const& rotation);
    void reset(bool entityInputs = false);
};
void recordNativeArrowHit(Actor const& actor, std::int64_t arrowId);
}
