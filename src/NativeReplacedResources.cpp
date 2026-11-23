#include "NativeReplacedResources.h"
#include "ll/api/mod/NativeMod.h"
#include "mc/client/model/models/SkinnedMesh.h"
#include "mc/client/model/models/SkinnedMeshGroup.h"
#include "NativeModelFactory.h"
#include "ClientGeometry.h"
#include "ContentHash.h"
#include "AnimationControllers.h"
#include "EntityQueries.h"
#include "NativeBlockQueries.h"
#include "mc/world/actor/FishingHook.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/attribute/AttributeInstanceConstRef.h"
#include "mc/world/attribute/AttributeInstance.h"
#include "mc/world/attribute/SharedAttributes.h"
#include "MolangPhysics.h"
#include "NativeRenderBindings.h"
#include "NativeTextureUpload.h"
#include "ReplacedModelAssignments.h"
#include "ModelLibrary.h"
#include "mc/deps/nbt/CompoundTag.h"
#include <future>
#include "JavaVanillaInputs.h"
#include "RenderSpace.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/model/GeometryGroup.h"
#include "mc/client/model/GeometryInfo.h"
#include "mc/client/model/Geometry.h"
#include "mc/client/model/models/DataDrivenModel.h"
#include "mc/client/model/models/DataDrivenGeometry.h"
#include "mc/client/model/GeometryPtr.h"
#include "mc/client/model/geom/ModelPart.h"
#include "mc/client/renderer/actor/ActorResourceDefinition.h"
#include "mc/client/renderer/actor/ActorRenderer.h"
#include "mc/client/renderer/actor/ActorRenderDispatcher.h"
#include "mc/client/renderer/actor/DataDrivenRenderer.h"
#include "mc/client/renderer/controller/RenderController.h"
#include "mc/client/world/actor/animation/ClientAnimationComponent.h"
#include "mc/client/renderer/controller/RenderControllerToProcess.h"
#include "mc/world/actor/RenderParams.h"
#include "mc/world/actor/animation/BoneOrientation.h"
#include "mc/world/actor/animation/ActorAnimationPlayer.h"
#include "mc/world/actor/animation/AnimationVisitor.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/world/level/Level.h"
#include "mc/profile/ProfilerLite.h"
#include "mc/world/level/Tick.h"
#include "mc/resources/MinEngineVersion.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorCategory.h"
#include "mc/world/actor/ActorType.h"
#include "mc/world/actor/provider/ActorRiding.h"
#include "mc/deps/vanilla_components/StrictActorIDEntityContextPair.h"
#include "ShoulderLocator.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/entity/components_json_legacy/ProjectileComponent.h"
#include "mc/deps/ecs/WeakEntityRef.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/legacy/ActorUniqueID.h"
#include <fstream>
#include <set>
#include <cmath>
#include <limits>

namespace ysm {
static MolangValue projectileOwner(Actor const* owner) {
    if (!owner) return {};
    std::map<std::string, MolangValue> values;
    values["query.is_alive"] = owner->isAlive() ? 1.0 : 0.0;
    values["query.is_on_ground"] = owner->isOnGround() ? 1.0 : 0.0;
    values["query.is_sneaking"] = owner->isSneaking() ? 1.0 : 0.0;
    values["query.is_on_fire"] = owner->isOnFire() ? 1.0 : 0.0;
    values["query.is_in_water"] = owner->isInWater() ? 1.0 : 0.0;
    values["query.is_riding"] = owner->getVehicle() ? 1.0 : 0.0;
    values["query.has_rider"] = owner->hasPassenger() ? 1.0 : 0.0;
    if (auto attribute = owner->getAttribute(SharedAttributes::HEALTH()); attribute.mPtr) {
        values["query.health"] = static_cast<double>(attribute.mPtr->mCurrentValue);
        values["query.max_health"] = static_cast<double>(attribute.mPtr->mCurrentMaxValue);
    }
    if (owner->isPlayer()) if (auto attribute = owner->getAttribute(Player::LEVEL()); attribute.mPtr)
        values["query.player_level"] = static_cast<double>(attribute.mPtr->mCurrentValue);
    return MolangValue("projectile_owner", std::move(values));
}

struct TargetPlayback {
    ClientGeometry geometry;
    AnimationSampler sampler;
    AnimationControllers controllers;
    AnimationEvaluation evaluation;
    EntityQueries queries;
    ReplacedModelKind kind;
    std::set<std::string> roots;
    std::int64_t epochTick = -1;
    std::optional<std::int64_t> groundSince;
    double time = 0;
    std::uint64_t calls = 0;
    std::uint64_t nativeCalls = 0;
    std::size_t lastApplied = 0;
    std::mutex mutex;
    Pose pose;
    ReplacedAnimationInput inputSnapshot;
    Json projectileSnapshot;
    Json nativePoseSnapshot;
    float projectileYaw = 0, projectilePitch = 0;
    std::weak_ptr<DataDrivenModel> model;
    bool forceCutout;
    bool visualMaterials;
    std::unordered_map<SkinnedMesh const*, std::weak_ptr<SkinnedMesh>> meshes;
    std::uint64_t meshDraws = 0, visualDraws = 0, meshMisses = 0;
    std::uint64_t passengerDraws = 0;
    int lastPassengerSeat = -1;
    std::array<float, 3> lastPassengerOffset{};
    TargetPlayback(ModelPack const& pack, ReplacedModelTarget const& target, ClientGeometry built)
        : geometry(std::move(built)), sampler(pack.targetAnimations(target), target.geometry),
          controllers(target.controllers, ControllerPolicy::SourcePackage), kind(target.kind),
          forceCutout(pack.descriptor.at("properties").value("all_cutout", false)),
          visualMaterials(target.individualBoneMaterials || forceCutout || !geometry.oneSidedParts.empty()) {
        evaluation.expressions.physics = std::make_shared<MolangPhysics>();
        evaluation.expressions.roaming.emplace(controllers.roamingFields(sampler, false, RoamingLimit::LocalOnly), RoamingRole::Consumer, RoamingLimit::LocalOnly);
        for (auto const& bone : geometry.document["minecraft:geometry"][0]["bones"])
            if (!bone.contains("parent")) roots.insert(bone.at("name").get<std::string>());
    }
    void applyRoaming(RoamingPayload const& values) {
        RoamingPayload accepted;
        for (auto const& [name, value] : values)
            if (evaluation.expressions.roaming->fields().contains(name)) accepted[name] = value;
        evaluation.expressions.roaming->applyFull(accepted);
    }
    BoneVisual visual(std::string const& part) const {
        auto surface = geometry.opacityParts.find(part);
        auto bone = geometry.bodyBones.find(surface == geometry.opacityParts.end() ? part : surface->second);
        BoneVisual result;
        if (bone != geometry.bodyBones.end()) {
            auto const& values = evaluation.expressions.boneVisuals;
            if (auto value = values.find(bone->second); value != values.end()) result = value->second;
        }
        result.cutout = forceCutout || geometry.oneSidedParts.contains(part);
        return result;
    }
    Json materialReport() const {
        Json values = Json::object();
        for (auto const& [name, visual] : evaluation.expressions.boneVisuals)
            if (visual != BoneVisual{})
                values[name] = {{"color", visual.color}, {"alpha", visual.alpha}, {"glow", visual.glow}};
        return {{"enabled", visualMaterials}, {"bound_meshes", meshes.size()}, {"mesh_draws", meshDraws},
            {"visual_draws", visualDraws}, {"mesh_misses", meshMisses}, {"bone_visuals", std::move(values)}};
    }
    Pose sample(Actor& actor, RenderParams& params, ReplacedModelAssignment const& assignment,
        std::optional<ProjectileBlockContact> const& contact) {
        auto tick = static_cast<std::int64_t>(actor.getLevel().getCurrentTick().tickID);
        if (epochTick < 0) epochTick = tick;
        auto alpha = params.mBaseActorRenderContext ? static_cast<double>(params.mBaseActorRenderContext->mFrameAlpha) : 0;
        if (!std::isfinite(alpha)) alpha = 0;
        if (kind == ReplacedModelKind::Projectile) {
            auto const& rotation = *actor.mBuiltInComponents->mActorRotationComponent;
            auto renderBodyYaw = actor.getInterpolatedBodyYaw(static_cast<float>(std::clamp(alpha, 0.0, 1.0)));
            auto carriers = nativeProjectileRotation(rotation.mRotPrev->y, rotation.mRot->y,
                rotation.mRotPrev->x, rotation.mRot->x, alpha, renderBodyYaw);
            projectileYaw = static_cast<float>(carriers.yaw);
            projectilePitch = static_cast<float>(carriers.pitch);
            projectileSnapshot = {{"native_yaw", -carriers.yaw - renderBodyYaw - 90.0},
                {"native_pitch", carriers.pitch}, {"native_render_body_yaw", renderBodyYaw},
                {"native_body_rot", actor.getInterpolatedBodyRot(static_cast<float>(std::clamp(alpha, 0.0, 1.0)))},
                {"previous", {rotation.mRotPrev->x, rotation.mRotPrev->y}},
                {"current", {rotation.mRot->x, rotation.mRot->y}}};
        }
        time = std::max(0.0, (double(tick - epochTick) + std::clamp(alpha, 0.0, 1.0)) / 20.0);
        EntityQueryInput input;
        input.player = false;
        input.living = actor.hasCategory(ActorCategory::Mob);
        auto position = actor.getFeetPos(), velocity = actor.getPosDelta();
        input.position = {position.x, position.y, position.z};
        auto nativePosition = actor.getPosition(), interpolated = actor.getInterpolatedPosition(static_cast<float>(std::clamp(alpha, 0.0, 1.0)));
        input.renderPosition = {position.x + (interpolated.x - nativePosition.x), position.y + (interpolated.y - nativePosition.y),
            position.z + (interpolated.z - nativePosition.z)};
        input.velocity = {velocity.x, velocity.y, velocity.z};
        input.yaw = input.viewYaw = input.bodyYaw = actor.getRotation().y;
        input.headPitch = -actor.getRotation().x;
        input.onGround = actor.isOnGround();
        input.onFire = actor.isOnFire();
        input.inWater = actor.isInWater();
        input.inRain = !input.inWater && actor.isInRain();
        input.hasRider = actor.hasPassenger();
        input.passenger = actor.getVehicle() != nullptr;
        input.alive = actor.isAlive();
        input.tick = tick;
        input.renderTicks = time * 20;
        input.worldTime = actor.getLevel().getTime();
        input.framesPerSecond = ProfilerLite::gProfilerLiteInstance().mFrameData->mFPS;
        sampleNativeSceneQueries(input, actor, params.mBaseActorRenderContext);
        queries.bind(evaluation.expressions, input);
        ReplacedAnimationInput replaced;
        replaced.present = input.alive;
        replaced.onGround = input.onGround;
        replaced.inWater = input.inWater;
        replaced.onFire = input.onFire;
        replaced.hasPassengers = input.hasRider;
        replaced.velocity = input.velocity;
        // In this Bedrock client StickInGroundSubcomponent sets the actor's
        // OnGroundFlagComponent, including wall hits. Restrict that host
        // signal to the AbstractArrow family, as YSM's IArrowExtraInfo does;
        // ordinary grounded vehicles/projectiles are not embedded arrows.
        replaced.inGround = kind == ReplacedModelKind::Projectile
            && actor.hasType(ActorType::AbstractArrow)
            && projectileReachedBlock(input.onGround, contact, {interpolated.x, interpolated.y, interpolated.z});
        if (kind == ReplacedModelKind::Projectile && actor.hasType(ActorType::AbstractArrow))
            replaced.shootItemId = assignment.shootItemId;
        if (kind == ReplacedModelKind::Projectile) {
            auto& level = actor.getLevel();
            if (replaced.inGround) {
                if (!groundSince || tick < *groundSince) groundSince = tick;
            } else groundSince.reset();
            auto component = actor.getEntityContext().tryGetComponent<ProjectileComponent>();
            if (groundSince) {
                auto observed = static_cast<int>(std::min<std::int64_t>(tick - *groundSince, std::numeric_limits<int>::max()));
                replaced.onGroundTicks = component ? std::max(observed, static_cast<int>(component->mOnGroundTime)) : observed;
            }
            replaced.projectileOwner = projectileOwner(level.fetchEntity(ActorUniqueID{assignment.owner}, false));
            auto const& type = actor.getActorIdentifier().mCanonicalName->getString();
            static std::map<std::string_view, std::string_view> const items{
                {"minecraft:snowball", "minecraft:snowball"}, {"minecraft:egg", "minecraft:egg"},
                {"minecraft:ender_pearl", "minecraft:ender_pearl"}, {"minecraft:xp_bottle", "minecraft:experience_bottle"},
                {"minecraft:splash_potion", "minecraft:splash_potion"}, {"minecraft:lingering_potion", "minecraft:lingering_potion"}};
            auto item = items.find(type);
            if (item != items.end()) replaced.throwableItem = item->second;
            replaced.spectralArrow = type == "minecraft:spectral_arrow";
            if (actor.hasType(ActorType::FishingHook)) {
                auto const& hook = static_cast<FishingHook const&>(actor);
                replaced.biting = hook.mTimeUntilNibble > 0;
                if (auto* hooked = level.fetchEntity(hook.mTargetId, false))
                    replaced.hookedEntity = javaEntityId(hooked->getActorIdentifier().mCanonicalName->getString());
            }
        }
        inputSnapshot = replaced;
        if (kind != ReplacedModelKind::Projectile) projectileSnapshot = Json();
        if (kind == ReplacedModelKind::Projectile)
            if (auto component = actor.getEntityContext().tryGetComponent<ProjectileComponent>())
                projectileSnapshot.update(Json{{"no_physics", static_cast<bool>(component->mNoPhysics)},
                    {"was_on_ground", static_cast<bool>(component->mWasOnGround)},
                    {"on_ground_time", static_cast<int>(component->mOnGroundTime)},
                    {"flight_time", static_cast<int>(component->mFlightTime)},
                    {"shake_time", static_cast<int>(component->mShakeTime)}});
        ++calls;
        NativeBlockQueryScope blocks(evaluation.expressions, actor);
        return controllers.sampleReplaced(sampler, kind, time, evaluation, replaced);
    }
    void apply(RenderParams& params, std::unordered_map<SkeletalHierarchyIndex, std::vector<BoneOrientation>>& skeletons) {
        if (params.mModel.lock() != model.lock()) return;
        std::scoped_lock lock(mutex);
        lastApplied = 0;
        if (nativeCalls % 64 == 0) nativePoseSnapshot = Json::object();
        for (auto& [hierarchy, bones] : skeletons) {
            if (bones.size() != geometry.document["minecraft:geometry"][0]["bones"].size()) continue;
            if (!std::all_of(bones.begin(), bones.end(), [&](auto const& bone) {
                auto name = bone.mName->getString();
                return geometry.bodyBones.contains(name) || geometry.opacityParts.contains(name)
                    || name == geometry.projectileYawCarrier || name == geometry.projectilePitchCarrier;
            })) continue;
            for (auto& bone : bones) {
                auto name = bone.mName->getString();
                auto& transform = bone.mTransform->mData;
                auto const& baseline = bone.mDefaultTransform->mData;
                if ((nativeCalls % 64 == 0) && (name == geometry.projectileYawCarrier || name == geometry.projectilePitchCarrier
                    || roots.contains(name) || (geometry.bodyBones.contains(name)
                        && geometry.bodyBones.at(name) == "Root"))) {
                    auto const& matrix = *bone.mMatrix->_m;
                    Json columns = Json::array();
                    for (int c = 0; c < 4; ++c)
                        columns.push_back({matrix[c][0], matrix[c][1], matrix[c][2], matrix[c][3]});
                    nativePoseSnapshot[name] = {{"baseline_rotation", {baseline[1]->x, baseline[1]->y, baseline[1]->z}},
                        {"previous_matrix", std::move(columns)}};
                }
                for (int channel = 0; channel < 3; ++channel) *transform[channel] = *baseline[channel];
                if (name == geometry.projectileYawCarrier) transform[1]->y += projectileYaw;
                if (name == geometry.projectilePitchCarrier) transform[1]->z += projectilePitch;
                auto logical = geometry.bodyBones.find(name);
                if (logical != geometry.bodyBones.end()) if (auto selected = pose.find(logical->second); selected != pose.end()) {
                    auto const& value = selected->second;
                    auto position = nativeAnimationPosition(value.position);
                    transform[0]->x += float(position[0]); transform[0]->y += float(position[1]); transform[0]->z += float(position[2]);
                    transform[1]->x += float(value.rotation[0]); transform[1]->y += float(value.rotation[1]); transform[1]->z += float(value.rotation[2]);
                    transform[2]->x *= float(value.scale[0]); transform[2]->y *= float(value.scale[1]); transform[2]->z *= float(value.scale[2]);
                }
                ++lastApplied;
            }
        }
        ++nativeCalls;
        if (params.mAnimationComponent) params.mAnimationComponent->mNeedToUpdateQueryableBoneOrientations = true;
    }
};
class TargetNumericPlayer final : public ActorAnimationPlayer {
    std::weak_ptr<TargetPlayback> playback;
    HashedString name{"animation.ysm_bedrock.target_numeric"};
public:
    explicit TargetNumericPlayer(std::shared_ptr<TargetPlayback> const& state) : playback(state) {
        mFriendlyName = name;
        mBlendExpression->mImpl->emplace<float>(1.0f);
        mExpanded = true;
        mAnimationFinished = false;
    }
    void applyToPose(ApplyAnimationContext const&, RenderParams& params,
        std::unordered_map<SkeletalHierarchyIndex, std::vector<BoneOrientation>>& skeletons, float) override {
        if (auto state = playback.lock()) state->apply(params, skeletons);
    }
    void resetAnimation() override {}
    void buildBoneToPartMapping(AnimationComponent&) override {}
    void bindParticleEffects(std::unordered_map<HashedString, HashedString> const&) override {}
    void bindSoundEffects(std::unordered_map<HashedString, std::string> const&) override {}
    bool hasAnimationFinished() const override { return playback.expired(); }
    std::shared_ptr<ActorAnimationPlayer> findAnimation(HashedString const&) override { return {}; }
    ActorAnimationType getAnimationType() const override { return ActorAnimationType::Empty; }
    HashedString const& getRawName() const override { return name; }
    void visit(AnimationVisitor&& visitor) override { visitor.mBase.get()(*this); }
};
struct NativeReplacedResources::Impl {
    ReplacedModelAssignments assignments;
    Level const* authority = nullptr;
    Level const* clientLevel = nullptr;
    std::set<std::string> missingSources;
    std::map<std::string, std::weak_ptr<ModelPack const>> knownSources;
    std::uint64_t restoredCount = 0;
    unsigned retirementTicks = 0;
    std::shared_ptr<ModelPack const> ownerPack;
    std::optional<std::int64_t> ownerId;
    std::set<std::int64_t> peerOwnerIds;
    struct PeerArrow {
        std::int64_t owner;
        bool crossbow;
        std::uint64_t seen;
        unsigned attempts{};
        std::chrono::steady_clock::time_point retry{};
    };
    std::map<std::int64_t,PeerArrow> peerArrows;
    std::uint64_t peerUpdate{};
    unsigned shotReports{};
    std::uint64_t projectileOwnerEvents = 0;
    std::uint64_t projectileClientSpawnEvents = 0;
    std::weak_ptr<DataDrivenModel> prototype;
    std::chrono::steady_clock::time_point nextPrototypeLookup{};
    std::map<std::string, std::weak_ptr<NativeTextureUpload>> textures;
    struct RenderEntry {
        std::weak_ptr<ActorResourceDefinition> source;
        NativeModelInstance instance;
        std::shared_ptr<ActorRenderer> renderer;
        std::weak_ptr<ActorRenderDispatcher> dispatcher;
        std::optional<HashedString> previousRendererId;
        std::shared_ptr<NativeTextureUpload> texture;
        std::unique_ptr<NativeRenderBindings> bindings;
        std::shared_ptr<TargetPlayback> playback;
        std::shared_ptr<TargetNumericPlayer> player;
        std::shared_ptr<ReplacedModelAssignment const> assignment;
        std::optional<ProjectileBlockContact> blockContact;
        std::string targetIdentity;
        std::uint64_t roamingRevision = 0;
    };
    std::map<std::pair<Actor const*, std::int64_t>, RenderEntry> renders;
    std::unordered_map<RenderController const*, std::weak_ptr<TargetPlayback>> rendererPlaybacks;
    std::map<std::pair<Actor const*, std::int64_t>, Json> routes;
    std::map<std::pair<Actor const*, std::int64_t>, WeakEntityRef> actorRefs;
    std::uint64_t retiredRenders = 0;
    std::map<std::tuple<Actor const*, std::int64_t, std::string>, std::string> failed;
    std::set<void const*> framesInspected;
    std::map<void const*, double> frameReports;
    Json renderReports = Json::array();
    void saveRenderReport(std::filesystem::path const& path, Json report) {
        renderReports.push_back(std::move(report));
        if (renderReports.size() > 128) renderReports.erase(renderReports.begin());
        auto pending = path;
        pending += ".tmp";
        std::ofstream stream(pending, std::ios::binary | std::ios::trunc);
        stream << renderReports.dump(2);
        stream.close();
        if (!stream) return;
        // Readers and crash recovery see the last complete report.
        std::error_code error;
        std::filesystem::rename(pending, path, error);
    }
    void retireRender(std::pair<Actor const*, std::int64_t> const& key) {
        auto found = renders.find(key);
        if (found == renders.end()) return;
        auto& entry = found->second;
        auto model = entry.instance.model.get();
        if (entry.previousRendererId) {
            auto const& id = *model->mName;
            if (auto ref = actorRefs.find(key); ref != actorRefs.end()) {
                auto context = ref->second.lock();
                auto actor = context.tryUnwrap<Actor>();
                if (actor && &*actor == key.first && actor->getActorRendererId() == id)
                    actor->setActorRendererId(*entry.previousRendererId);
            }
            if (auto dispatcher = entry.dispatcher.lock()) {
                auto& registry = *dispatcher->mDataDrivenRenderers;
                auto slot = registry.find(id);
                if (slot != registry.end() && slot->second == entry.renderer) registry.erase(slot);
            }
        }
        for (auto const& [controller, condition] : *model->mRenderControllers)
            rendererPlaybacks.erase(controller.get());
        framesInspected.erase(model);
        frameReports.erase(model);
        renders.erase(found);
        ++retiredRenders;
    }
};
struct NativeReplacedResources::Storage {
    using Key = std::pair<Actor const*, std::int64_t>;
    struct Entry { WeakEntityRef actor; SavedReplacedAssignment value; };
    std::map<Key, Entry> records;
    using SeatKey = std::pair<Level const*, std::int64_t>;
    struct Seat { WeakEntityRef bird; std::int64_t owner; bool left; bool mounted; };
    std::map<SeatKey, Seat> shoulderSeats;
    void trackShoulders(Level& level);
    // AddActor can precede player/model installation and even the first client
    // tick. Tie that observation to the actual client registry generation.
    struct SpawnHand { WeakEntityRef actor; std::int64_t owner; std::string hand; int selected, sent; };
    std::map<std::int64_t, SpawnHand> spawnHands;
    std::set<Key> toPublish;
    std::future<ModelChoice> reader;
    std::string requested;
    std::map<std::string, std::chrono::steady_clock::time_point> retry;
};
static constexpr char AssignmentTag[] = "ysm_bedrock:replaced_model";
static constexpr char ShoulderTag[] = "ysm_bedrock:parrot_shoulder";
NativeReplacedResources::NativeReplacedResources() : storage(std::make_unique<Storage>()), impl(std::make_unique<Impl>()) {}
void NativeReplacedResources::attachAuthority(Level const* level, Level const* clientLevel) {
    std::scoped_lock lock(mutex);
    if (level != impl->authority || clientLevel != impl->clientLevel)
        for (auto& [key, render] : impl->renders) render.blockContact.reset();
    impl->authority = level;
    impl->clientLevel = clientLevel;
}
void NativeReplacedResources::entityLoaded(Actor const& actor, CompoundTag const& tag) {
    if (actor.getLevel().isClientSide()) return;
    if (actor.isType(ActorType::Parrot) && tag.contains(ShoulderTag, Tag::String)) {
        try {
            auto bytes = static_cast<std::string_view>(tag.at(ShoulderTag));
            if (bytes.size() <= 96) {
                auto data = Json::parse(bytes);
                Storage::Seat seat{actor.getEntityContext().getWeakRef(), data.at("owner").get<std::int64_t>(), data.at("left").get<bool>(), false};
                std::scoped_lock lock(mutex);
                storage->shoulderSeats.insert_or_assign({&actor.getLevel(),actor.getOrCreateUniqueID().rawID}, std::move(seat));
            }
        } catch (std::exception const&) {}
    }
    if (!tag.contains(AssignmentTag, Tag::String)) return;
    try {
        auto text = static_cast<std::string_view>(tag.at(AssignmentTag));
        if (text.size() > 128 * 1024) return;
        auto record = SavedReplacedAssignment::decode(Json::parse(text));
        auto key = Storage::Key{&actor, actor.getOrCreateUniqueID().rawID};
        std::scoped_lock lock(mutex);
        storage->records.insert_or_assign(key, Storage::Entry{actor.getEntityContext().getWeakRef(), std::move(record)});
        storage->toPublish.insert(key);
    } catch (std::exception const&) {
        // Ignore malformed optional metadata on this native entity.
    }
}
void NativeReplacedResources::entitySaving(Actor const& actor, CompoundTag& tag) {
    if (actor.getLevel().isClientSide()) return;
    std::scoped_lock lock(mutex);
    if (actor.isType(ActorType::Parrot)) {
        auto seat = storage->shoulderSeats.find({&actor.getLevel(), actor.getOrCreateUniqueID().rawID});
        if (seat != storage->shoulderSeats.end()) {
            auto native = seat->second.bird.lock().tryUnwrap<Actor>();
            if (native && &*native == &actor)
                tag.putString(ShoulderTag, Json{{"owner",seat->second.owner},{"left",seat->second.left}}.dump());
        }
    }
    if (storage->records.empty()) return;
    auto key = Storage::Key{&actor, actor.getOrCreateUniqueID().rawID};
    auto found = storage->records.find(key);
    if (found == storage->records.end() || !found->second.actor.lock().tryUnwrap<Actor>()) return;
    tag.putString(AssignmentTag, found->second.value.encode().dump());
}
void NativeReplacedResources::Storage::trackShoulders(Level& level) {
    level.forEachPlayer([&](Player& owner) {
        auto ownerId = owner.getOrCreateUniqueID().rawID;
        std::array<Actor*, 2> birds{};
        std::array<std::int64_t, 2> ids{};
        std::size_t count = 0;
        ShoulderOccupants slots;
        for (auto const& rider : ActorRiding::getPassengers(owner.getEntityContext())) {
            auto bird = level.fetchEntity(*rider.mActorID, false);
            if (!bird || !bird->isType(ActorType::Parrot)) continue;
            birds[count] = bird;
            ids[count++] = rider.mActorID->rawID;
            auto it = shoulderSeats.find({&level, rider.mActorID->rawID});
            if (it != shoulderSeats.end() && it->second.owner == ownerId
                && it->second.bird.lock().tryUnwrap<Actor>()) slots.sides[it->second.left ? 0 : 1] = ids[count-1];
            if (count == birds.size()) break;
        }
        slots.update(std::span(ids).first(count));
        for (std::size_t i = 0; i < count; ++i) {
            auto& bird = *birds[i];
            shoulderSeats.insert_or_assign({&level,ids[i]}, Seat{bird.getEntityContext().getWeakRef(),ownerId,slots.sides[0]==ids[i],true});
        }
        // During chunk construction the NBT can arrive before either passenger
        // link. Do not retire that saved side until an actual mount was seen.
        std::erase_if(shoulderSeats, [&](auto const& entry) {
            if (entry.first.first != &level || entry.second.owner != ownerId || !entry.second.mounted
                || std::find(ids.begin(), ids.begin()+count, entry.first.second) != ids.begin()+count) return false;
            auto bird = entry.second.bird.lock().template tryUnwrap<Actor>();
            return bird && bird->getVehicle() != &owner;
        });
        return true;
    });
    if (level.getCurrentTick().tickID % 80 == 0)
        std::erase_if(shoulderSeats, [](auto const& item) { return !item.second.bird.lock().template tryUnwrap<Actor>(); });
}
std::optional<bool> NativeReplacedResources::savedShoulderSeat(Actor const& bird, Actor const& owner) const {
    std::scoped_lock lock(mutex);
    if (!impl->authority || &bird.getLevel() != impl->clientLevel || &owner.getLevel() != impl->clientLevel) return {};
    auto found = storage->shoulderSeats.find({impl->authority,bird.getOrCreateUniqueID().rawID});
    if (found == storage->shoulderSeats.end() || found->second.owner != owner.getOrCreateUniqueID().rawID
        || !found->second.bird.lock().tryUnwrap<Actor>()) return {};
    return found->second.left;
}
void NativeReplacedResources::serverTick(Level& level) {
    std::scoped_lock lock(mutex);
    storage->trackShoulders(level);
    // Bind persistent records to authoritative ECS generations, including
    // across chunk unloads and worlds with recycled runtime IDs.
    if (level.getCurrentTick().tickID % 80 == 0)
        std::erase_if(storage->records, [&](auto const& item) {
            if (item.second.actor.lock().template tryUnwrap<Actor>()) return false;
            storage->toPublish.erase(item.first);
            return true;
        });
    if (&level != impl->authority) return;
    for (auto& [key, render] : impl->renders) {
        if (!render.playback || render.playback->kind != ReplacedModelKind::Projectile) continue;
        render.blockContact.reset();
        auto* actor = level.fetchEntity(ActorUniqueID{key.second}, false);
        if (!actor || !actor->hasType(ActorType::AbstractArrow)) continue;
        auto component = actor->getEntityContext().tryGetComponent<ProjectileComponent>();
        if (!component) continue;
        auto& contact = render.blockContact.emplace();
        if (!actor->isOnGround() || component->mNoPhysics) continue;
        if (component->mHitResult->mType == HitResultType::Tile) {
            auto const& pos = *component->mCollisionPos;
            contact = {true, {pos.x, pos.y, pos.z}};
        } else if (component->mWasOnGround || component->mOnGroundTime > 0) {
            // A deserialized, already embedded arrow may lack its transient ray hit.
            auto pos = actor->getPosition();
            contact = {true, {pos.x, pos.y, pos.z}};
        }
    }
    for (auto const& key : storage->toPublish) {
        auto entry = storage->records.find(key);
        if (entry == storage->records.end()) continue;
        auto& record = entry->second;
        auto actor = record.actor.lock().tryUnwrap<Actor>();
        if (!actor || &actor->getLevel() != &level) continue;
        auto const& source = record.value.source;
        auto known = impl->knownSources.find(source);
        auto pack = known != impl->knownSources.end() ? known->second.lock() : nullptr;
        if (!source.empty() && !pack) impl->missingSources.insert(source);
        impl->assignments.restore(key.second, record.value, std::move(pack));
        if (record.value.kind == ReplacedModelKind::Projectile) {
            auto observed = storage->spawnHands.find(key.second);
            if (observed != storage->spawnHands.end()) {
                auto clientActor = observed->second.actor.lock().tryUnwrap<Actor>();
                if (clientActor && &clientActor->getLevel() == impl->clientLevel)
                    impl->assignments.restoredClientHand(key.second, observed->second.owner, observed->second.hand);
            }
        }
        ++impl->restoredCount;
    }
    storage->toPublish.clear();
    for (auto const& key : impl->assignments.takeChanged()) {
        auto* actor = level.fetchEntity(ActorUniqueID{key.second}, false);
        auto value = impl->assignments.saved(key);
        if (!actor || !value) continue;
        storage->records.insert_or_assign(Storage::Key{actor, key.second},
            Storage::Entry{actor->getEntityContext().getWeakRef(), std::move(*value)});
    }
}
void NativeReplacedResources::resolveSavedModels(ModelLibrary const& library) {
    std::scoped_lock lock(mutex);
    auto now = std::chrono::steady_clock::now();
    if (storage->reader.valid()) {
        if (storage->reader.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        auto source = std::exchange(storage->requested, {});
        auto loaded = storage->reader.get();
        if (loaded.pack && loaded.source == source) {
            auto pack = std::make_shared<ModelPack const>(std::move(*loaded.pack));
            impl->assignments.loadedSource(source, pack);
            impl->knownSources[source] = pack;
            impl->missingSources.erase(source);
        } else storage->retry[source] = now + std::chrono::seconds(30);
    }
    for (auto const& source : impl->missingSources) {
        if (auto r = storage->retry.find(source); r != storage->retry.end() && now < r->second) continue;
        storage->requested = source;
        storage->reader = std::async(std::launch::async, [library, source] { return library.select(source); });
        break;
    }
}
NativeReplacedResources::~NativeReplacedResources() = default;
std::vector<ModelEffects> NativeReplacedResources::takeEffects() {
    std::scoped_lock lock(mutex);
    std::vector<ModelEffects> result;
    for (auto const& [key, entry] : impl->renders) {
        std::scoped_lock lockPlayback(entry.playback->mutex);
        auto& context = entry.playback->evaluation.expressions;
        if (context.soundCues.empty() && context.particleCues.empty()) continue;
        auto sounds = std::exchange(context.soundCues, std::vector<SoundCue>{});
        auto ref = impl->actorRefs.find(key);
        result.push_back({entry.playback, entry.assignment->pack, ref == impl->actorRefs.end() ? WeakEntityRef{} : ref->second,
            std::move(sounds), context.takeParticleCues()});
    }
    return result;
}
void NativeReplacedResources::reset() {
    std::scoped_lock lock(mutex);
    if (impl->authority)
        for (auto const& [key, record] : storage->records) storage->toPublish.insert(key);
    while (!impl->renders.empty()) impl->retireRender(impl->renders.begin()->first);
    impl = std::make_unique<Impl>();
}
static auto assignedTarget(Actor const& actor, ReplacedModelAssignments const& assignments)
    -> std::pair<std::shared_ptr<ReplacedModelAssignment const>, ReplacedModelTarget const*> {
    auto const& id = actor.getActorIdentifier().mCanonicalName->getString();
    if (id == "minecraft:player") return {};
    auto entity = actor.getOrCreateUniqueID().rawID;
    for (auto kind : {ReplacedModelKind::Vehicle, ReplacedModelKind::Projectile}) {
        auto assignment = assignments.find(kind, entity);
        if (assignment && assignments.canPresent(*assignment)) {
            auto variant = (id == "minecraft:boat" || id == "minecraft:chest_boat") ? actor.getVariant() : -1;
            if (auto* target = assignment->pack->findTarget(kind, id, {}, variant))
                return {std::move(assignment), target};
        }
    }
    return {};
}
bool NativeReplacedResources::hasActiveTarget(Actor const& actor) const {
    std::scoped_lock lock(mutex);
    auto* target = assignedTarget(actor, impl->assignments).second;
    return target && (target->kind == ReplacedModelKind::Vehicle ? showVehicles : showProjectiles);
}
std::optional<glm::mat4> NativeReplacedResources::passengerTransform(
    Actor const& rider, Actor const& vehicle, float alpha, bool customPlayer) const {
    if (!std::isfinite(alpha)) return {};
    auto const& passengers = ActorRiding::getPassengers(vehicle.getEntityContext());
    auto id = rider.getOrCreateUniqueID().rawID;
    auto occupant = std::find_if(passengers.begin(), passengers.end(), [id](auto const& entry) { return entry.mActorID->rawID == id; });
    if (occupant == passengers.end()) return {};
    auto seat = static_cast<int>(std::distance(passengers.begin(), occupant));
    std::scoped_lock lock(mutex);
    if (!showVehicles) return {};
    auto key = std::pair<Actor const*, std::int64_t>{&vehicle, vehicle.getOrCreateUniqueID().rawID};
    auto found = impl->renders.find(key);
    if (found == impl->renders.end()) return {};
    auto const& entry = found->second;
    auto [assignment, target] = assignedTarget(vehicle, impl->assignments);
    if (!target || target->kind != ReplacedModelKind::Vehicle || !entry.instance.model
        || entry.targetIdentity != assignment->pack->localContentKey + "/" + target->id) return {};
    std::scoped_lock poseLock(entry.playback->mutex);
    auto const& ctx = entry.playback->evaluation.expressions;
    if (!entry.playback->meshDraws) return {};
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    auto feet = [alpha](Actor const& a) {
        auto p = a.getInterpolatedPosition(alpha);
        return glm::vec3(p.x, p.y + a.getFeetPos().y - a.getPosition().y, p.z);
    };
    auto matrix = passengerLocator(ctx, seat, vehicle.getInterpolatedBodyYaw(alpha), feet(vehicle) - feet(rider), customPlayer);
    if (!matrix) return {};
    ++entry.playback->passengerDraws;
    entry.playback->lastPassengerSeat = seat;
    auto translation = (*matrix)[3];
    entry.playback->lastPassengerOffset = {translation.x, translation.y, translation.z};
    return matrix;
}
void NativeReplacedResources::observeRenderRoute(Actor const& actor, bool enabled) {
    auto const& id = actor.getActorIdentifier().mCanonicalName->getString();
    if (id == "minecraft:player") return;
    auto projectile = actor.hasType(ActorType::Projectile) || actor.hasType(ActorType::FishingHook)
        || actor.hasType(ActorType::FireworksRocket) || actor.getEntityContext().tryGetComponent<ProjectileComponent>();
    bool boat = actor.hasType(ActorType::BoatRideable) || actor.hasType(ActorType::ChestBoatRideable);
    if (!projectile && !boat && !actor.hasPassenger()) return;
    auto key = std::pair<Actor const*, std::int64_t>{&actor, actor.getOrCreateUniqueID().rawID};
    auto kind = projectile ? ReplacedModelKind::Projectile : ReplacedModelKind::Vehicle;
    std::scoped_lock lock(mutex);
    if (impl->routes.size() >= 64 && !impl->routes.contains(key)) impl->routes.erase(impl->routes.begin());
    auto& route = impl->routes[key];
    auto visible = kind == ReplacedModelKind::Vehicle ? showVehicles : showProjectiles;
    auto variant = id == "minecraft:boat" || id == "minecraft:chest_boat" ? actor.getVariant() : -1;
    route = {{"actor_id", key.second}, {"entity", id}, {"java_entity", javaEntityId(id, variant)},
        {"kind", replacedModelKindName(kind)}, {"stage", "feature_switch"}, {"result", "disabled"},
        {"last_seen_tick", actor.getLevel().getCurrentTick().tickID}};
    if (!enabled || !visible) {
        route["reason"] = !enabled ? "target_render_switch" : (projectile ? "disable_projectile_model" : "disable_vehicle_model");
        return;
    }
    auto assignment = impl->assignments.find(kind, key.second);
    if (assignment && assignment->restored && !assignment->pack && !assignment->savedSource.empty()) {
        route["owner_id"] = assignment->owner;
        route["saved_source"] = assignment->savedSource;
        route["stage"] = "resource_load";
        auto retry = storage->retry.find(assignment->savedSource);
        route["result"] = retry != storage->retry.end() && std::chrono::steady_clock::now() < retry->second
            ? "saved_model_source_unavailable" : "saved_model_loading";
        return;
    }
    if (!assignment || !assignment->pack) {
        route["stage"] = "ownership";
        route["result"] = "no_active_model_owner";
        if (projectile) route["native_owner"] = actor.getOwnerId().rawID;
        else if (auto* rider = actor.getFirstPassenger()) route["first_rider"] = rider->getOrCreateUniqueID().rawID;
        return;
    }
    if (!impl->assignments.canPresent(*assignment)) {
        route["stage"] = "ownership";
        route["result"] = "server_model_owner_not_visible";
        route["owner_id"] = assignment->owner;
        return;
    }
    route["owner_id"] = assignment->owner;
    route["pack"] = assignment->pack->localContentKey;
    if (auto* target = assignment->pack->findTarget(kind, id, {}, variant)) {
        route["target"] = target->id;
        route["stage"] = "draw";
        route["result"] = "replacement_model_not_prepared";
        auto failure = std::tuple{key.first, key.second, assignment->pack->localContentKey + "/" + target->id};
        if (auto found = impl->failed.find(failure); found != impl->failed.end()) {
            route["result"] = "native_creation_failed";
            route["reason"] = found->second;
        }
        else if (impl->prototype.expired()) route["result"] = "waiting_for_native_prototype";
    } else {
        route["stage"] = "target_match";
        route["result"] = "no_matching_target_in_assigned_pack";
    }
}
void NativeReplacedResources::presentation(bool vehicles, bool projectiles) {
    std::scoped_lock lock(mutex);
    if ((showVehicles && !vehicles) || (showProjectiles && !projectiles)) {
        for (auto it = impl->renders.begin(); it != impl->renders.end();) {
            auto current = it++;
            bool vehicle = current->second.playback->kind == ReplacedModelKind::Vehicle;
            if (!(vehicle ? vehicles : projectiles)) impl->retireRender(current->first);
        }
    }
    showVehicles = vehicles;
    showProjectiles = projectiles;
}
void NativeReplacedResources::updateOwner(std::int64_t owner, ModelPack const& pack, RoamingPayload const& roaming) {
    std::scoped_lock lock(mutex);
    if (!impl->ownerPack || impl->ownerPack->localContentKey != pack.localContentKey || impl->ownerPack->librarySource != pack.librarySource)
        impl->ownerPack = std::make_shared<ModelPack const>(pack);
    impl->knownSources[pack.librarySource] = impl->ownerPack;
    if (impl->missingSources.erase(pack.librarySource)) impl->assignments.loadedSource(pack.librarySource, impl->ownerPack);
    impl->ownerId = owner;
    impl->assignments.updateOwner(owner, impl->ownerPack, roaming);
}
void NativeReplacedResources::clearOwner(std::int64_t owner) {
    std::scoped_lock lock(mutex);
    impl->assignments.removeOwner(owner);
    impl->ownerPack.reset();
    impl->ownerId.reset();
}
void NativeReplacedResources::updatePeerOwners(std::span<PeerOwner const> peers) {
    std::scoped_lock lock(mutex);
    std::set<std::int64_t> active;
    auto seen=++impl->peerUpdate;
    for (auto const& peer : peers) {
        if (!peer.pack || peer.id == impl->ownerId) continue;
        active.insert(peer.id);
        for (auto const& [entity, crossbow] : peer.arrows) {
            auto [it, inserted] = impl->peerArrows.try_emplace(entity, Impl::PeerArrow{peer.id, crossbow, seen});
            if (inserted && impl->shotReports < 16) {
                ++impl->shotReports;
                if (auto mod = ll::mod::NativeMod::current()) mod->getLogger().info(
                    "Room arrow observed entity={} owner={} item={}", entity, peer.id, crossbow ? "crossbow" : "bow");
            }
            if (!inserted && (it->second.owner != peer.id || it->second.crossbow != crossbow))
                it->second = {peer.id, crossbow, seen};
            it->second.seen = seen;
        }
        if (!peer.pack->librarySource.empty()) {
            impl->knownSources[peer.pack->librarySource] = peer.pack;
            if (impl->missingSources.erase(peer.pack->librarySource))
                impl->assignments.loadedSource(peer.pack->librarySource, peer.pack);
        }
        impl->assignments.updatePeer(peer.id, peer.pack, peer.roaming, peer.mount);
    }
    std::erase_if(impl->peerArrows,[&](auto const& e){return e.second.seen!=seen;});
    for (auto id : impl->peerOwnerIds) if (!active.contains(id) && id != impl->ownerId) impl->assignments.removeOwner(id);
    impl->peerOwnerIds = std::move(active);
}
// A network arrow can already be in the vanilla batch when its room message arrives.
void NativeReplacedResources::preparePeerArrows(IClientInstance& client, std::filesystem::path const& path) {
    auto* level = client.getLevel();
    std::array<std::int64_t, 4> work{};
    size_t count = 0;
    auto now = std::chrono::steady_clock::now();
    {
        std::scoped_lock lock(mutex);
        if (impl->peerArrows.empty() || !showProjectiles || !level || level != impl->clientLevel) return;
        for (auto& [id, shot] : impl->peerArrows) {
            if (shot.attempts >= 30 || now < shot.retry) continue;
            ++shot.attempts;
            shot.retry = now + std::chrono::milliseconds(150);
            work[count++] = id;
            if (count == work.size()) break;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        auto* actor = level->fetchEntity(ActorUniqueID{work[i]}, false);
        if (!actor || actor->getActorIdentifier().mCanonicalName->getString() != "minecraft:arrow") continue;
        observeProjectile(*actor);
        bool matched = hasActiveTarget(*actor);
        if (matched && !resolveRenderer(*actor, {}, client, path)) continue;
        std::scoped_lock lock(mutex);
        if (auto it = impl->peerArrows.find(work[i]); it != impl->peerArrows.end())
            it->second.attempts = 30;
    }
}
void NativeReplacedResources::observeMount(Actor const& rider) {
    auto vehicle = rider.getVehicle();
    auto first = vehicle ? vehicle->getFirstPassenger() : nullptr;
    std::scoped_lock lock(mutex);
    if (!impl->assignments.ownerActive(rider.getOrCreateUniqueID().rawID)) return;
    if (vehicle && first) impl->assignments.mounted(vehicle->getOrCreateUniqueID().rawID,
        rider.getOrCreateUniqueID().rawID, first->getOrCreateUniqueID().rawID);
    else impl->assignments.dismounted(rider.getOrCreateUniqueID().rawID);
}
void NativeReplacedResources::projectileOwnerSet(Actor const& projectile, std::int64_t owner) {
    std::scoped_lock lock(mutex);
    ++impl->projectileOwnerEvents;
    if (impl->assignments.ownerActive(owner))
        impl->assignments.projectileOwnerSet(projectile.getOrCreateUniqueID().rawID, owner);
}
void NativeReplacedResources::projectileClientSpawned(Actor const& projectile, LocalPlayer const* localPlayer) {
    bool arrow = projectile.getActorIdentifier().mCanonicalName->getString() == "minecraft:arrow";
    if (!arrow && !projectile.hasType(ActorType::AbstractArrow)) return;
    auto entity = projectile.getOrCreateUniqueID().rawID;
    auto owner = projectile.getOwnerId().rawID;
    std::scoped_lock lock(mutex);
    ++impl->projectileClientSpawnEvents;
    if (arrow && &projectile.getLevel() == impl->clientLevel) {
        if (auto it = impl->peerArrows.find(entity); it != impl->peerArrows.end()) {
            it->second.attempts = 0;
            it->second.retry = {};
            impl->assignments.projectileServerShot(entity, it->second.owner, it->second.crossbow);
            return;
        }
    }
    auto assigned = impl->assignments.find(ReplacedModelKind::Projectile, entity);
    auto* shooter = projectile.getLevel().fetchEntity(ActorUniqueID{owner}, false);
    if (!shooter && assigned) {
        owner = assigned->owner;
        shooter = projectile.getLevel().fetchEntity(ActorUniqueID{owner}, false);
    }
    if (!shooter || !(shooter->hasCategory(ActorCategory::Mob) || shooter->hasType(ActorType::Player))) return;
    auto itemName = [](ItemStack const& stack) -> std::string {
        return stack.isNull() || !stack.mItem ? "minecraft:air" : stack.mItem->mFullName->getString();
    };
    auto const* stack = &shooter->getCarriedItem();
    auto equipped = itemName(*stack);
    int selected = -1, sent = -1;
    if (localPlayer && owner == localPlayer->getOrCreateUniqueID().rawID) {
        selected = localPlayer->getSelectedItemSlot();
        sent = localPlayer->mSentSelectedSlot;
        equipped = itemName(localPlayer->getSelectedItem());
        // Crossbow replication can restore the firing slot while handling
        // the spawn. Read the local input that was already sent at that point.
        int slot = localPlayer->mSentSelectedSlot;
        auto const& inventory = localPlayer->getInventory();
        if (slot >= 0 && slot < 9 && slot < inventory.getContainerSize()) stack = &inventory.getItem(slot);
    }
    if (shooter->hasType(ActorType::Player))
        storage->spawnHands.insert_or_assign(entity, Storage::SpawnHand{projectile.getEntityContext().getWeakRef(), owner, equipped, selected, sent});
    impl->assignments.projectileClientOwnerSet(entity, owner, itemName(*stack));
    impl->assignments.restoredClientHand(entity, owner, equipped);
}
void NativeReplacedResources::observeProjectile(Actor const& projectile) {
    auto component = projectile.getEntityContext().tryGetComponent<ProjectileComponent>();
    bool arrow=projectile.hasType(ActorType::AbstractArrow)
        || projectile.getActorIdentifier().mCanonicalName->getString()=="minecraft:arrow";
    if (!component && !arrow && !projectile.hasType(ActorType::FishingHook)
        && !projectile.hasType(ActorType::FireworksRocket) && !projectile.hasType(ActorType::Projectile)) return;
    auto owner = component ? component->mOwnerId->rawID : projectile.getOwnerId().rawID;
    std::scoped_lock lock(mutex);
    if(arrow && &projectile.getLevel()==impl->clientLevel){
        auto entity=projectile.getOrCreateUniqueID().rawID;
        auto shot=impl->peerArrows.find(entity);
        if(shot!=impl->peerArrows.end()){
            impl->assignments.projectileServerShot(entity,shot->second.owner,shot->second.crossbow);
            return;
        }
    }
    if (component && !impl->assignments.ownerActive(owner)) owner = projectile.getOwnerId().rawID;
    if (!impl->assignments.ownerActive(owner)) return;
    auto entity = projectile.getOrCreateUniqueID().rawID;
    if (impl->assignments.find(ReplacedModelKind::Projectile, entity)) return;
    // The client can first observe an entity after receiving its owner packet.
    // Don't infer the launch item from what that owner happens to hold now.
    impl->assignments.projectileOwnerSet(entity, owner);
}
Json NativeReplacedResources::assignmentReport(bool enabled) const {
    std::scoped_lock lock(mutex);
    auto result = impl->assignments.report();
    result["restored_from_server_entities"] = impl->restoredCount;
    result["pending_saved_models"] = impl->missingSources;
    result["authority_available"] = impl->authority != nullptr;
    result["server_entity_records"] = storage->records.size();
    result["client_spawn_hands"] = Json::array();
    for (auto const& [entity, observed] : storage->spawnHands)
        result["client_spawn_hands"].push_back({{"entity",entity},{"equipped",observed.hand},
            {"selected_slot",observed.selected},{"sent_slot",observed.sent}});
    result["projectile_owner_events"] = impl->projectileOwnerEvents;
    result["projectile_client_spawn_events"] = impl->projectileClientSpawnEvents;
    result["render_entries"] = impl->renders.size();
    result["tracked_render_entities"] = impl->actorRefs.size();
    result["failed_render_entries"] = impl->failed.size();
    result["retired_render_entries"] = impl->retiredRenders;
    result["texture_entries"] = impl->textures.size();
    result["switches"] = {{"target_render", enabled}, {"vehicles", showVehicles}, {"projectiles", showProjectiles}};
    auto routes = Json::array();
    for (auto const& [key, snapshot] : impl->routes) {
        auto route = snapshot;
        if (snapshot.contains("target")) if (auto found = impl->renders.find(key); found != impl->renders.end()) {
            auto& playback = *found->second.playback;
            std::scoped_lock playbackLock(playback.mutex);
            route["animation_samples"] = playback.calls;
            route["bones_applied"] = playback.lastApplied;
            if (playback.kind == ReplacedModelKind::Vehicle)
                route["passenger_locator"] = {{"draws", playback.passengerDraws}, {"seat", playback.lastPassengerSeat},
                    {"offset", playback.lastPassengerOffset}};
            auto controllers = playback.controllers.report();
            auto const& coded = controllers.at("coded");
            bool selected = std::any_of(coded.begin(), coded.end(), [](auto const& controller) {
                return !controller.value("animation", std::string()).empty();
            });
            auto const& definitions = controllers.at("states");
            selected = selected || std::any_of(definitions.begin(), definitions.end(), [](auto const& controller) {
                auto const& animations = controller.at("animations");
                return std::any_of(animations.begin(), animations.end(), [](auto const& animation) {
                    return animation.value("applied", false);
                });
            });
            route["animation_selection"] = selected ? "clip_selected" : "no_active_clip";
            route["controllers"] = std::move(controllers);
            route["material"] = playback.materialReport();
            if (playback.calls == 0) {
                route["stage"] = "animation";
                route["result"] = "no_frame_for_replacement";
            } else if (playback.lastApplied == 0 && playback.meshDraws > 0 && selected) {
                route["stage"] = "animation";
                route["result"] = "selected_pose_not_applied_to_skeleton";
            } else if (playback.meshDraws == 0) {
                route["result"] = playback.meshes.empty() ? "mesh_not_bound" : "replacement_mesh_not_drawn";
            } else if (playback.visualMaterials && playback.visualDraws == 0) {
                route["result"] = "replacement_mesh_missing_visual_palette";
            } else {
                route["result"] = "replacement_mesh_drawn";
            }
        }
        routes.push_back(std::move(route));
    }
    result["entity_routes"] = std::move(routes);
    return result;
}
void NativeReplacedResources::collectRetired() {
    std::scoped_lock lock(mutex);
    if (++impl->retirementTicks % 80 == 0)
        std::erase_if(storage->spawnHands, [](auto const& observed) {
            return !observed.second.actor.lock().template tryUnwrap<Actor>();
        });
    // Track engine registry generations, never dereference a cached Actor*.
    // Chunk unload retires client drawing state, not the connection's model
    // assignment: the same entity may return with its captured pack later.
    std::erase_if(impl->actorRefs, [&](auto const& tracked) {
        auto context = tracked.second.lock();
        auto actor = context.template tryUnwrap<Actor>();
        bool present = actor && &*actor == tracked.first.first
            && actor->getOrCreateUniqueID().rawID == tracked.first.second;
        if (present) {
            auto [assignment, target] = assignedTarget(*actor, impl->assignments);
            if (auto entry = impl->renders.find(tracked.first); entry != impl->renders.end()) {
                auto identity = target ? assignment->pack->localContentKey + "/" + target->id : std::string();
                if (entry->second.targetIdentity != identity) impl->retireRender(tracked.first);
            }
            return false;
        }
        impl->retireRender(tracked.first);
        std::erase_if(impl->failed, [&](auto const& failure) {
            return std::get<0>(failure.first) == tracked.first.first && std::get<1>(failure.first) == tracked.first.second;
        });
        return true;
    });
    std::erase_if(impl->textures, [](auto const& item) { return item.second.expired(); });
}
void NativeReplacedResources::prepareFrame(RenderParams& params, std::filesystem::path const& path) {
    auto model = params.mModel.lock();
    if (!model || !model->mName->getString().starts_with("ysm.replaced.")) return;
    if (params.mAnimationComponent) if (auto client = params.mAnimationComponent->tryGetClient()) {
        for (auto const& geometry : *model->mGeometries) if (geometry)
            client->getBoneOrientationsFromGeometry(*geometry);
        client->updateRenderControllersToProcess(model, params);
    }
    std::scoped_lock lock(mutex);
    if (!params.mActor) return;
    auto key = std::pair<Actor const*, std::int64_t>{params.mActor, params.mActor->getOrCreateUniqueID().rawID};
    auto found = impl->renders.find(key);
    if (found == impl->renders.end() || found->second.instance.model != model) return;
    auto& owned = found->second;
    if (owned.bindings && owned.texture) owned.bindings->bind(params, owned.texture->textures(), {});
    auto& playback = *owned.playback;
    if (params.mAnimationComponent && params.mAnimationComponent->mAnimationComponentInitialized) {
        auto& players = *params.mAnimationComponent->mComponentAnimationPlayers;
        std::erase_if(players, [](auto const& player) {
            return player && player->getRawName().getString() == "animation.ysm_bedrock.target_numeric"
                && player->hasAnimationFinished();
        });
        if (std::find(players.begin(), players.end(), owned.player) == players.end()) players.push_back(owned.player);
    }
    std::scoped_lock playbackLock(playback.mutex);
    if (owned.roamingRevision != owned.assignment->revision) {
        playback.applyRoaming(owned.assignment->roaming);
        owned.roamingRevision = owned.assignment->revision;
    }
    auto previousInput = playback.inputSnapshot;
    playback.pose = playback.sample(*params.mActor, params, *owned.assignment, owned.blockContact);
    auto const& pose = playback.pose;
    auto applied = playback.lastApplied;
    bool first = impl->framesInspected.insert(model.get()).second;
    auto& lastReport = impl->frameReports[model.get()];
    auto const& input = playback.inputSnapshot;
    auto phaseChanged = playback.kind == ReplacedModelKind::Projectile
        && (input.inGround != previousInput.inGround || input.inWater != previousInput.inWater
            || input.onFire != previousInput.onFire || input.shootItemId != previousInput.shootItemId);
    if (!first && !phaseChanged && playback.time - lastReport < 2.0) return;
    lastReport = playback.time;
    Json entry{{"stage", "native_frame_prepared"}, {"model", model->mName->getString()},
        {"actor_id", key.second}, {"tick", params.mActor->getLevel().getCurrentTick().tickID},
        {"actor", params.mActor ? params.mActor->getActorIdentifier().mCanonicalName->getString() : ""},
        {"controllers", Json::array()}, {"scripts", Json::object()}, {"skeletons", Json::array()}};
    entry["time"] = playback.time;
    entry["pose_calls"] = playback.calls;
    entry["native_pose_calls"] = playback.nativeCalls;
    entry["bones_applied"] = applied;
    entry["pose"] = AnimationSampler::report(pose);
    entry["expression_failures"] = playback.evaluation.expressions.failures;
    entry["world_queries"] = {{"ysm.weather", playback.evaluation.expressions.read("ysm.weather").number()},
        {"ysm.is_open_air", playback.evaluation.expressions.read("ysm.is_open_air").number()}};
    entry["animation_controllers"] = playback.controllers.report();
    entry["material"] = playback.materialReport();
    if (playback.kind == ReplacedModelKind::Projectile)
        entry["projectile_rotation"] = {{"yaw_carrier", playback.projectileYaw}, {"pitch_carrier", playback.projectilePitch}};
    if (!playback.nativePoseSnapshot.empty()) entry["native_pose"] = playback.nativePoseSnapshot;
    entry["animation_input"] = {{"present", input.present}, {"on_ground", input.onGround},
        {"in_ground", input.inGround}, {"in_water", input.inWater}, {"on_fire", input.onFire},
        {"has_passengers", input.hasPassengers}, {"velocity", input.velocity}, {"shoot_item_id", input.shootItemId}};
    if (owned.blockContact) {
        float alpha = params.mBaseActorRenderContext ? params.mBaseActorRenderContext->mFrameAlpha : 0.0f;
        auto rendered = params.mActor->getInterpolatedPosition(std::isfinite(alpha) ? std::clamp(alpha, 0.0f, 1.0f) : 0.0f);
        entry["authoritative_block_contact"] = {{"embedded", owned.blockContact->embedded},
            {"rendered_position", {rendered.x, rendered.y, rendered.z}}};
        if (owned.blockContact->embedded) entry["authoritative_block_contact"]["position"] = owned.blockContact->position;
    }
    if (!playback.projectileSnapshot.is_null()) entry["projectile_input"] = playback.projectileSnapshot;
    if (playback.kind == ReplacedModelKind::Projectile) {
        auto& actor = *params.mActor;
        if (auto component = actor.getEntityContext().tryGetComponent<ProjectileComponent>()) {
            auto position = actor.getPosition(), collision = *component->mCollisionPos;
            auto hit = [](HitResult const& result) {
                Json value{{"type", static_cast<int>(result.mType)}};
                if (result.mType == HitResultType::Tile || result.mType == HitResultType::Entity)
                    value["position"] = {result.mPos.x, result.mPos.y, result.mPos.z};
                if (result.mType == HitResultType::Tile)
                    value["block"] = {result.mBlock.x, result.mBlock.y, result.mBlock.z};
                return value;
            };
            entry["contact"] = {{"actor_position", {position.x, position.y, position.z}},
                {"collision_position", {collision.x, collision.y, collision.z}},
                {"hit", hit(*component->mHitResult)}, {"cached_hit", hit(*component->mCachedHitResult)},
                {"waiting_for_server_ground", static_cast<bool>(component->mWaitingForServerHitGround)},
                {"damaged_or_effects_applied", static_cast<bool>(component->mHitState->mDamageOrEffectsApplied)},
                {"actor_hit_count", static_cast<int>(component->mActorHitCount)}};
        }
    }
    entry["owner_id"] = owned.assignment->owner;
    entry["assigned_pack"] = owned.assignment->pack->localContentKey;
    entry["assignment_revision"] = owned.assignment->revision;
    entry["roaming"] = playback.evaluation.expressions.roaming->namedValues();
    entry["textures"] = owned.bindings->report(params).value("controller_textures", Json::object());
    for (auto const& [controller, condition] : *model->mRenderControllers) if (controller)
        entry["controllers"].push_back({{"name", controller->mName->getString()},
            {"condition", condition.getExpressionString()}});
    if (auto definition = model->mResourceDefinition.lock())
        for (auto const& [name, script] : *definition->mActorScripts)
            entry["scripts"][name.getString()] = script.getExpressionString();
    if (params.mAnimationComponent) {
        entry["initialized"] = static_cast<bool>(params.mAnimationComponent->mAnimationComponentInitialized);
        for (auto const& [index, bones] : *params.mAnimationComponent->mBoneOrientations)
            entry["skeletons"].push_back({{"index", static_cast<int>(index)}, {"bones", bones.size()}});
        if (auto client = params.mAnimationComponent->tryGetClient())
            entry["controllers_to_process"] = client->mRenderControllersToProcess->size();
    }
    impl->saveRenderReport(path, std::move(entry));
}
void NativeReplacedResources::prepareGeometry(RenderController const& controller,
    std::shared_ptr<DataDrivenGeometry> const& geometry, RenderParams const& params) {
    auto model = params.mModel.lock();
    if (!model || !model->mName->getString().starts_with("ysm.replaced.")) return;
    std::scoped_lock lock(mutex);
    auto binding = impl->rendererPlaybacks.find(&controller);
    auto playback = binding == impl->rendererPlaybacks.end() ? nullptr : binding->second.lock();
    if (!playback || playback->model.lock() != model) return;
    std::scoped_lock playbackLock(playback->mutex);
    std::erase_if(playback->meshes, [](auto const& item) { return item.second.expired(); });
    for (auto const& group : *geometry->mSkinnedMeshGroups)
        for (auto const& mesh : *group.mSkinnedMeshes)
            if (mesh) playback->meshes.insert_or_assign(mesh.get(), mesh);
}
std::span<BoneVisual const> NativeReplacedResources::visuals(SkinnedMesh const& mesh, RenderController const* controller,
    std::span<BoneVisual> scratch, std::vector<BoneVisual>& overflow) {
    if (!controller) return {};
    std::shared_ptr<TargetPlayback> playback;
    {
        std::scoped_lock lock(mutex);
        if (auto binding = impl->rendererPlaybacks.find(controller); binding != impl->rendererPlaybacks.end())
            playback = binding->second.lock();
    }
    if (!playback) return {};
    std::scoped_lock playbackLock(playback->mutex);
    auto binding = playback->meshes.find(&mesh);
    if (binding == playback->meshes.end() || binding->second.lock().get() != &mesh) {
        ++playback->meshMisses;
        return {};
    }
    ++playback->meshDraws;
    if (!playback->visualMaterials) return {};
    auto const& parts = *mesh.mSkinnedParts;
    if (parts.size() > scratch.size()) {
        overflow.resize(parts.size());
        scratch = overflow;
    }
    auto result = scratch.first(parts.size());
    for (std::size_t i = 0; i < parts.size(); ++i)
        result[i] = parts[i] ? playback->visual(parts[i]->mName->getString()) : BoneVisual{};
    if (!result.empty()) ++playback->visualDraws;
    return result;
}
void NativeReplacedResources::rememberPrototype(std::shared_ptr<DataDrivenModel> const& model) {
    if (!model) return;
    std::scoped_lock lock(mutex);
    if (!impl->prototype.expired() || !NativeModelFactory::supports(*model)) return;
    auto resource = model->mResourceDefinition.lock();
    if (!resource || resource->mGeometryMap->empty()) return;
    bool surface = std::any_of(model->mRenderControllers->begin(), model->mRenderControllers->end(), [](auto const& entry) {
        return entry.first && entry.first->mName->getString() == "controller.render.player.third_person";
    });
    if (!surface) return;
    impl->prototype = model;
}
std::shared_ptr<ActorRenderer> NativeReplacedResources::resolveRenderer(Actor& actor,
    std::shared_ptr<ActorRenderer> original, IClientInstance& client,
    std::filesystem::path const& path) {
    if (!hasActiveTarget(actor)) return original;
    std::shared_ptr<DataDrivenModel> prototype;
    {
        std::scoped_lock lock(mutex);
        prototype = impl->prototype.lock();
        if (!prototype) {
            auto now = std::chrono::steady_clock::now();
            if (now < impl->nextPrototypeLookup) return original;
            impl->nextPrototypeLookup = now + std::chrono::milliseconds(250);
        }
    }
    if (!prototype) {
        // Free cameras and first-person views need not enqueue a body draw.
        // Ask the native dispatcher for the same player renderer explicitly.
        auto dispatcher = client.getEntityRenderDispatcher();
        auto* local = client.getLocalPlayer();
        if (!dispatcher || !local) return original;
        auto take = [this](std::shared_ptr<ActorRenderer> const& renderer) {
            if (!renderer) return;
            std::shared_ptr<Model> model = renderer->mModel;
            if (model && NativeModelFactory::supports(*model))
                rememberPrototype(std::static_pointer_cast<DataDrivenModel>(model));
        };
        take(dispatcher->_getRenderer(*local));
        take(dispatcher->getDataDrivenRenderer(HashedString("minecraft:player")));
        std::scoped_lock lock(mutex);
        prototype = impl->prototype.lock();
    }
    if (!prototype) return original;
    auto model = resolveModel(actor, prototype, client, path);
    if (model == prototype) return original;
    std::scoped_lock lock(mutex);
    auto key = std::pair<Actor const*, std::int64_t>{&actor, actor.getOrCreateUniqueID().rawID};
    auto found = impl->renders.find(key);
    if (found == impl->renders.end()) return original;
    auto& entry = found->second;
    auto failureKey = std::tuple{key.first, key.second, entry.targetIdentity};
    if (impl->failed.contains(failureKey)) return original;
    if (entry.renderer) return entry.renderer;
    try {
        // The dispatcher can leave the individual renderer empty for batchable mobs.
        auto textures = original ? std::shared_ptr<mce::TextureGroup>(original->mTextureGroup) : client.getTextureGroup();
        auto renderer = NativeModelFactory::createRenderer(entry.instance, textures);
        // A queued draw can outlive the cache entry. Keep uploaded texture
        // locations alive until that last renderer reference is released.
        entry.renderer = std::shared_ptr<ActorRenderer>(renderer.get(),
            [renderer, texture = entry.texture, definition = entry.instance.definition](ActorRenderer*) {});
        // The world draw list can resolve the renderer by actor ID after
        // dispatch. Register the same instance for projectiles and vehicles.
        if (auto dispatcher = client.getEntityRenderDispatcher()) {
            auto const& id = *entry.instance.model->mName;
            auto [slot, installed] = dispatcher->mDataDrivenRenderers->try_emplace(id,
                std::static_pointer_cast<DataDrivenRenderer>(entry.renderer));
            if (installed) {
                entry.previousRendererId = actor.getActorRendererId();
                entry.dispatcher = dispatcher;
                actor.setActorRendererId(id);
            }
        }
        impl->saveRenderReport(path, {{"entity", actor.getActorIdentifier().mCanonicalName->getString()},
            {"actor_id", key.second}, {"stage", "submitted_independent_renderer"}});
        return entry.renderer;
    } catch (std::exception const& error) {
        impl->failed.emplace(failureKey, error.what());
        impl->saveRenderReport(path, {{"actor_id", key.second}, {"stage", "independent_renderer_failed"},
            {"error", error.what()}});
        return original;
    }
}
std::shared_ptr<DataDrivenModel> NativeReplacedResources::resolveModel(Actor& actor,
    std::shared_ptr<DataDrivenModel> original, IClientInstance& client,
    std::filesystem::path const& path) {
    if (!original) return original;
    std::scoped_lock lock(mutex);
    auto [assignment, target] = assignedTarget(actor, impl->assignments);
    if (!target) return original;
    if (!(target->kind == ReplacedModelKind::Vehicle ? showVehicles : showProjectiles)) return original;
    auto const& pack = *assignment->pack;
    auto identity = pack.localContentKey + "/" + target->id;
    if (original->mName->getString().starts_with("ysm.replaced.")) return original;
    // Cold-loaded actors can request their vanilla model before the local
    // player has drawn. Only the player prototype owns our surface controller.
    auto prototype = impl->prototype.lock();
    if (!prototype) return original;
    auto source = prototype->mResourceDefinition.lock();
    if (!source) return original;
    auto key = std::pair<Actor const*, std::int64_t>{&actor, actor.getOrCreateUniqueID().rawID};
    auto failureKey = std::tuple{key.first, key.second, identity};
    impl->actorRefs.insert_or_assign(key, actor.getEntityContext().getWeakRef());
    if (impl->failed.contains(failureKey)) return original;
    if (auto found = impl->renders.find(key); found != impl->renders.end() && found->second.targetIdentity == identity) {
        found->second.assignment = std::move(assignment);
        return found->second.instance.model;
    }
    if (impl->renders.size() >= 128 && !impl->renders.contains(key)) return original;
    Json report{{"target", target->id}, {"entity", actor.getActorIdentifier().mCanonicalName->getString()},
        {"actor_id", key.second}, {"stage", "model_preparation"}};
    try {
        auto built = ClientGeometry::buildTarget(*target);
        auto bytes = built.document.dump();
        auto name = "geometry.ysm_bedrock.target_" + contentSha256(std::span(
            reinterpret_cast<unsigned char const*>(bytes.data()), bytes.size())).substr(0, 24);
        built.document["minecraft:geometry"][0]["description"]["identifier"] = name;
        auto group = client.getGeometryGroup();
        group->loadModelPackFromStringSync(name, built.document.dump(), MinEngineVersion::fromString("1.21.0"));
        auto found = group->mGeometries->find(HashedString(name));
        if (found == group->mGeometries->end()) throw std::runtime_error("Render-probe geometry was not registered");
        std::set<std::string> bones;
        for (auto const& bone : built.document["minecraft:geometry"][0]["bones"]) bones.insert(bone.at("name").get<std::string>());
        Impl::RenderEntry next;
        next.source = source;
        next.assignment = std::move(assignment);
        next.targetIdentity = identity;
        next.instance = NativeModelFactory::createTarget(*prototype, found->second, bones,
            "ysm.replaced.probe.actor_" + std::to_string(key.second),
            // The 2.6.5 vehicle renderer submits geometry directly after yaw.
            target->kind == ReplacedModelKind::Vehicle ? 1.f : 0.7f);
        auto texture = impl->textures[identity].lock();
        if (!texture) texture = NativeTextureUpload::create(client.getTextureGroup(), target->texture,
            pack.pbrEncoding, contentSha256(std::span(reinterpret_cast<unsigned char const*>(pack.localContentKey.data()),
                pack.localContentKey.size())).substr(0, 24) + "_" + std::to_string(static_cast<int>(target->kind))
                + "_" + target->id.substr(target->id.find('/') + 1));
        impl->textures[identity] = texture;
        next.texture = texture;
        next.bindings = std::make_unique<NativeRenderBindings>();
        if (impl->authority && &actor.getLevel() == impl->clientLevel && actor.hasType(ActorType::AbstractArrow))
            next.blockContact.emplace();
        next.playback = std::make_shared<TargetPlayback>(pack, *target, std::move(built));
        next.playback->model = next.instance.model;
        next.player = std::make_shared<TargetNumericPlayer>(next.playback);
        auto result = next.instance.model;
        report["model"] = result->mName->getString();
        report["geometry"] = name;
        report["bones"] = bones.size();
        report["stage"] = "submitted_to_native_render_parameters";
        impl->retireRender(key);
        for (auto const& [controller, condition] : *result->mRenderControllers)
            if (controller) impl->rendererPlaybacks[controller.get()] = next.playback;
        impl->renders.insert_or_assign(key, std::move(next));
        impl->saveRenderReport(path, std::move(report));
        return result;
    } catch (std::exception const& error) {
        impl->failed.emplace(failureKey, error.what());
        report["error"] = error.what();
        impl->saveRenderReport(path, std::move(report));
        return original;
    }
}
}
