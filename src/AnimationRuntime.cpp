#include "AnimationRuntime.h"
#include "NativePoseLayout.h"
#include "ExtraAction.h"
#include "ShoulderLocator.h"
#include "NativeReplacedResources.h"
#include "AnimationSampler.h"
#include "ClientGeometry.h"
#include "HandAnimationController.h"
#include "AnimationControllers.h"
#include "EntityQueries.h"
#include "JavaVanillaInputs.h"
#include "NativeWeather.h"
#include "NativeBlockQueries.h"
#include "MolangPhysics.h"
#include "NativeRenderBindings.h"
#include "NativeBoneVisual.h"
#include "RenderSpace.h"
#include "YsmUi.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/client/model/models/SkinnedMeshGroup.h"
#include "mc/client/model/models/SkinnedMesh.h"
#include "mc/client/model/geom/ModelPart.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/enchanting/ItemEnchants.h"
#include "mc/world/actor/RenderParams.h"
#include "mc/world/actor/animation/ActorAnimationPlayer.h"
#include "mc/world/actor/animation/AnimationComponent.h"
#include "mc/world/actor/animation/AnimationVisitor.h"
#include "mc/world/actor/animation/BoneOrientation.h"
#include "mc/client/renderer/actor/ActorRenderData.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/model/models/DataDrivenModel.h"
#include "mc/client/model/models/DataDrivenGeometry.h"
#include "mc/client/model/GeometryInfo.h"
#include "mc/client/model/Geometry.h"
#include "mc/client/renderer/actor/ActorResourceDefinition.h"
#include "mc/client/renderer/actor/ActorTextureInfo.h"
#include "mc/client/world/actor/animation/ClientAnimationComponent.h"
#include "mc/client/renderer/controller/RenderControllerToProcess.h"
#include "mc/client/renderer/controller/RenderController.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/ItemTag.h"
#include "mc/world/item/registry/ItemRegistryRef.h"
#include "mc/world/actor/player/PlayerItemInUse.h"
#include "mc/world/attribute/AttributeInstanceConstRef.h"
#include "mc/world/attribute/AttributeInstance.h"
#include "mc/world/attribute/SharedAttributes.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/platform/UUID.h"
#include "mc/profile/ProfilerLite.h"
#include "mc/entity/components/LocalMoveVelocityComponent.h"
#include "mc/entity/components/MobHurtTimeComponent.h"
#include "mc/entity/components/MobWalkAnimationComponent.h"
#include "mc/entity/components/RiptideTridentSpinAttackComponent.h"
#include "mc/deps/vanilla_components/OffsetsComponent.h"
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <set>
#include <atomic>
#include <mutex>
#include <thread>
#include <limits>
#include <unordered_map>
#include <Windows.h>

// This SDK leaves the default constructor undefined. A local constant node is
// sufficient for the base fields of our own animation player.
ExpressionNode::ExpressionNode() : mImpl(std::in_place_type<float>, 0.0f) {}
void ActorAnimationPlayer::buildBoneToPartMapping(AnimationComponent&) {}
void ActorAnimationPlayer::bindParticleEffects(std::unordered_map<HashedString, HashedString> const&) {}
void ActorAnimationPlayer::bindSoundEffects(std::unordered_map<HashedString, std::string> const&) {}
void ActorAnimationPlayer::visit(AnimationVisitor&& visitor) { visitor.mBase.get()(*this); }

namespace ysm {
static std::mutex elytraInputMutex;
static JavaElytraInput nativeElytraInput;
static std::atomic<std::uint64_t> nativeInputFrame = 0;
namespace {
struct ArrowHits {
    AttachedArrows arrows;
    std::int64_t lastArrow{};
};
std::mutex arrowHitMutex;
std::map<std::int64_t, ArrowHits> arrowHits;
}
void recordNativeArrowHit(Actor const& actor, std::int64_t arrowId) {
    auto identity = actor.getOrCreateUniqueID().rawID;
    std::scoped_lock lock(arrowHitMutex);
    if (arrowHits.size() >= 128 && !arrowHits.contains(identity)) return;
    auto& observed = arrowHits[identity];
    if (arrowId != observed.lastArrow) {
        observed.arrows.hit(actor.mTickCount);
        observed.lastArrow = arrowId;
    }
}
static Json nativeRenderStack() {
    void* frames[32]{};
    auto count = CaptureStackBackTrace(0, 32, frames, nullptr);
    auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"Minecraft.Windows.exe"));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64 const*>(base + dos->e_lfanew);
    Json result = Json::array();
    for (USHORT i = 0; i < count; ++i) {
        auto address = reinterpret_cast<std::uintptr_t>(frames[i]);
        if (address >= base && address - base < nt->OptionalHeader.SizeOfImage)
            result.push_back(address - base);
    }
    return result;
}
struct AnimationState {
    struct CpuCost {
        std::uint64_t samples = 0;
        double totalUs = 0, lastUs = 0, peakUs = 0;
        void record(std::chrono::steady_clock::time_point started) {
            lastUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
            totalUs += lastUs; peakUs = std::max(peakUs, lastUs); ++samples;
        }
        Json report() const { return {{"samples", samples}, {"last_us", lastUs},
            {"mean_us", samples ? totalUs / samples : 0}, {"peak_us", peakUs}}; }
    };
    CpuCost prepareCpu, logicCpu, poseCpu;
    ModelUiBindings uiBindings;
    std::shared_ptr<AnimationSampler const> bodySampler;
    AnimationSampler const& sampler;
    AnimationSampler armSampler;
    HandAnimationController hands;
    AnimationControllers controllers;
    AnimationControllers armControllers;
    HandInput handInput;
    std::vector<AnimationLayer> handLayers;
    std::set<std::string> handAnimationsSeen;
    Json lastMaceSwing = Json::array();
    bool samplingMaceSwing = false;
    double lastMaceProgress = 0;
    ClientGeometry geometry;
    std::string skinGeometryJson;
    NativeRenderBindings bindings;
    bool visualMaterials = false;
    bool forceCutout = false;
    struct PoseBinding;
    struct VisualPart {
        ModelPart const* part = nullptr;
        PoseBinding const* binding = nullptr;
        uint64_t nameHash = 0;
    };
    struct VisualMesh {
        std::weak_ptr<SkinnedMesh> mesh;
        std::vector<VisualPart> parts;
    };
    std::map<std::pair<SkinnedMesh const*, RenderController const*>, VisualMesh> visualMeshes;
    struct VisualGeometryCache {
        std::weak_ptr<DataDrivenGeometry> geometry;
        void const* parts = nullptr;
        std::size_t partCount = 0;
        bool namesValidated = false;
        std::vector<std::weak_ptr<SkinnedMesh>> meshes;
        std::set<RenderController const*> controllers;
    };
    std::map<DataDrivenGeometry const*, VisualGeometryCache> visualGeometryCaches;
    std::uint64_t visualGeometryCacheHits = 0, visualGeometryRefreshes = 0;
    std::uint64_t visualDrawMatches = 0, visualDrawMisses = 0, visualDrawRecovered = 0;
    Json visualMissSamples = Json::array();
    Json nativeRenderStacks = Json::object();
    NativeTextureSet texture;
    std::string textureName;
    struct PoseBinding {
        std::string const* source = nullptr;
        BoneVisual const* visual = nullptr;
        bool arm = false;
        bool carrier = false;
        bool item = false;
        bool staticBackground = false;
        bool scaleRoot = false;
        bool authoredRoot = false;
        bool opacitySurface = false;
        bool oneSided = false;
        bool meleeSpearArm = false;
    };
    // Keys and source names borrow the immutable prepared geometry, not the
    // engine's mutable orientation or model-part storage.
    std::unordered_map<std::string_view, PoseBinding> poseBindings;
    bool hasElytraLocator = false;
    double width, height;
    std::filesystem::path diagnostics;
    std::filesystem::path performanceReport;
    std::string modelId;
    Json initialDrawCounters;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point created = epoch, lastSample{};
    std::chrono::steady_clock::time_point lastReport{};
    std::string state = "idle";
    std::string preview;
    ExtraAction extra;
    std::chrono::steady_clock::time_point extraEpoch{};
    bool paused = false;
    std::chrono::steady_clock::time_point pauseStarted{};
    std::set<std::string> observedStates;
    std::mutex frameMutex;
    std::weak_ptr<NativeReplacedResources> seatSource;
    HandVisibilityByPass visibleHands;
    std::atomic<bool> ysmLanceCharge = false;
    std::atomic<bool> worldBodyPosed = false;
    std::size_t calls = 0;
    std::size_t attachments = 0;
    std::size_t preparationCalls = 0, handCalls = 0, bodyCalls = 0;
    std::size_t inventoryCalls = 0, mainUpdates = 0, armUpdates = 0, renderFrames = 0;
    std::atomic<std::uint64_t> suppressedGlideRotations = 0;
    std::atomic<float> nativeGlideX = 0, nativeGlideY = 0;
    std::size_t roamingProjections = 0;
    RoamingInstallationId roamingInstallation = std::make_shared<RoamingInstallation>();
    RoamingPayload pendingRoaming;
    bool roamingInitialized = false;
    double logicalTime = -1, armTime = -1, armEpoch = -1;
    std::size_t sampledFrame = std::numeric_limits<std::size_t>::max();
    double stateTime = 0;
    std::set<int> pressedKeys;
    std::map<std::string, std::string> modVersions;
    std::atomic<bool> inGame = false;
    bool remote = false;
    std::optional<unsigned> peerFlight;
    std::map<std::string,float> receivedFormValues;
    Json lastObservation = nullptr;
    std::size_t startupFirstPersonPasses = 0;
    std::size_t startupRenderPreparations = 0;
    std::size_t geometryDeferrals = 0;
    Json startupEvents = Json::array();
    std::atomic<bool> active = true;
    AnimationEvaluation evaluation;
    AnimationEvaluation armEvaluation;
    EntityQueries entityQueries;
    EntityQueryInput entitySnapshot;
    BedrockWeatherSnapshot weatherSnapshot;
    Json motionChanges = Json::array();
    Pose cachedPose;
    bool awaitingFormPose = false;
    std::array<std::optional<glm::mat4>, 2> inventoryShoulders;
    Pose cachedArmPose;
    PoseBinding const* poseBinding(std::string const& native) const {
        if (auto found = poseBindings.find(native); found != poseBindings.end()) return &found->second;
        if (auto found = poseBindings.find(AnimationSampler::normalized(native)); found != poseBindings.end())
            return &found->second;
        return nullptr;
    }
    void startupEvent(char const* stage, Json detail = Json::object()) {
        if (diagnostics.empty()) return;
        if (startupEvents.size() >= 48) return;
        detail["stage"] = stage;
        detail["thread"] = std::hash<std::thread::id>{}(std::this_thread::get_id());
        detail["first_person_depth"] = FirstPersonRenderScope::depth();
        detail["elapsed_us"] = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - created).count();
        startupEvents.push_back(std::move(detail));
        std::ofstream(diagnostics.parent_path() / "animation-startup.json") << Json{{"events", startupEvents}}.dump(2);
    }
    AnimationState(ModelPack const& pack, std::filesystem::path path)
        : AnimationState(pack, std::move(path), pack.bodyAnimations()) {}
    AnimationState(ModelPack const& pack, std::filesystem::path path, Json const& body)
    : uiBindings(pack.descriptor), bodySampler(std::make_shared<AnimationSampler const>(body, pack.mainGeometry)),
        sampler(*bodySampler), armSampler(pack.firstPersonAnimations(), pack.armGeometry), hands(body),
        controllers(pack.controllers, ControllerPolicy::SourcePackage), armControllers(pack.controllers, ControllerPolicy::SourcePackage), geometry(ClientGeometry::build(pack)),
        skinGeometryJson(geometry.document.dump()),
        textureName(pack.currentTexture().name), width(pack.widthScale), height(pack.heightScale), diagnostics(std::move(path)) {
        evaluation.expressions.physics = std::make_shared<MolangPhysics>();
        armEvaluation.expressions.physics = std::make_shared<MolangPhysics>();
        auto fields = controllers.roamingFields(sampler, false, RoamingLimit::LocalOnly);
        auto uiFields = uiBindings.roamingFields(); fields.insert(uiFields.begin(), uiFields.end());
        evaluation.expressions.roaming.emplace(std::move(fields), RoamingRole::Producer, RoamingLimit::LocalOnly);
        armEvaluation.expressions.roaming.emplace(armControllers.roamingFields(armSampler, true, RoamingLimit::LocalOnly), RoamingRole::Consumer, RoamingLimit::LocalOnly);
        connectRoamingArm(*evaluation.expressions.roaming, *armEvaluation.expressions.roaming);
        forceCutout = pack.descriptor.at("properties").value("all_cutout", false);
        visualMaterials = pack.individualBoneMaterials || forceCutout || !geometry.oneSidedParts.empty();
        // These installed samplers own the libraries for the entire animation
        // state. Binding now gives mesh lookups stable addresses for visuals.
        sampler.bindFunctions(evaluation);
        armSampler.bindFunctions(armEvaluation);
        modelId = pack.id;
        auto const& key = pack.localContentKey;
        if (!diagnostics.empty() && key.size() == 71 && key.starts_with("local1_") && key.find_first_not_of("0123456789abcdef", 7) == key.npos)
            performanceReport = diagnostics.parent_path() / "performance" / (key + ".json");
        visibleHands.update(false, geometry.handDrawMasks);
        visibleHands.update(true, geometry.handDrawMasks);
        std::set<std::string> authoredRoots;
        for (auto const& bone : pack.mainGeometry["minecraft:geometry"][0]["bones"])
            if (!bone.contains("parent")) authoredRoots.insert(bone.at("name").get<std::string>());
        auto const& nativeBones = geometry.document.at("minecraft:geometry").at(0).at("bones");
        poseBindings.reserve(nativeBones.size());
        for (auto const& bone : nativeBones) {
            auto const& name = bone.at("name").get_ref<std::string const&>();
            auto& binding = poseBindings[name];
            if (auto item = geometry.firstPersonItems.find(name); item != geometry.firstPersonItems.end()) {
                binding.source = &item->first;
                binding.item = true;
            } else if (auto arm = geometry.armBones.find(name); arm != geometry.armBones.end()) {
                binding.source = &arm->second;
                binding.arm = true;
            } else if (auto body = geometry.bodyBones.find(name); body != geometry.bodyBones.end()) {
                binding.source = &body->second;
                hasElytraLocator = hasElytraLocator || body->second == "ElytraLocator";
            }
            binding.carrier = geometry.armCarriers.contains(name);
            binding.scaleRoot = name == geometry.bodyRoot;
            binding.authoredRoot = binding.source && authoredRoots.contains(*binding.source);
            binding.opacitySurface = geometry.opacityParts.contains(name);
            binding.oneSided = geometry.oneSidedParts.contains(name);
            binding.meleeSpearArm = geometry.meleeSpearArms.contains(name);
            auto part = geometry.armParts.find(name);
            binding.staticBackground = part != geometry.armParts.end() && part->second == "__ysm_fp_background";
        }
        if (visualMaterials) for (auto& [name, binding] : poseBindings) {
            auto owner = poseBindings.find(name);
            if (auto surface = geometry.opacityParts.find(std::string(name)); surface != geometry.opacityParts.end())
                owner = poseBindings.find(surface->second);
            if (owner == poseBindings.end() || !owner->second.source || owner->second.item) continue;
            auto const& source = owner->second;
            auto const& values = !source.arm ? evaluation.expressions.boneVisuals
                : source.staticBackground ? armSampler.declaredVisuals() : armEvaluation.expressions.boneVisuals;
            if (auto found = values.find(*source.source); found != values.end()) binding.visual = &found->second;
        }
    }
};

static Json diagnosticVariables(MolangContext const& context) {
    auto result = Json::object();
    if (context.roaming) for (auto const& [name, value] : context.roaming->namedValues())
        result[std::string(roamingPrefix) + name] = value;
    std::map<std::string_view, MolangValue const*> ordered;
    for (auto const& [name, value] : context.variables) {
        if (!name.starts_with("variable.")) continue;
        ordered.emplace(name, &value);
        if (ordered.size() > 64) ordered.erase(std::prev(ordered.end()));
    }
    for (auto const& [name, entry] : ordered) {
        if (result.size() >= 64) break;
        auto const& value = *entry;
        if (value.isString()) result[name] = value.string().substr(0, 1024);
        else if (value.isArray()) result[name] = {{"type", "array"}, {"length", value.array().size()}};
        else if (value.isRecord()) {
            auto fields = Json::object();
            auto record = value.record();
            for (auto const& [field, item] : record.fields) {
                if (fields.size() >= 16) break;
                fields[field] = item.isString() ? Json(item.string().substr(0, 256)) : Json(item.number());
            }
            result[name] = {{"type", record.schema}, {"fields", fields}};
        }
        else result[name] = value.number();
    }
    return result;
}

static HandItem handItem(ItemStack const& stack, int slot) {
    HandItem result;
    if (stack.isNull() || !stack.mItem) return result;
    auto const& item = *stack.mItem;
    result.id = item.mFullName->getString();
    // Durability changes during an attack/use do not select a different item.
    auto variant = item.mMaxDamage > 0 ? 0 : stack.mAuxValue;
    result.identity = result.id + ":" + std::to_string(slot) + ":" + std::to_string(variant) + ":" + std::to_string(stack.mCount);
    result.maxDurability = std::max(0, static_cast<int>(item.mMaxDamage));
    if (result.maxDurability > 0)
        result.damage = std::clamp(static_cast<int>(stack.getDamageValue()), 0, result.maxDurability);
    if (stack.isEnchanted()) {
        static constexpr std::array ids{
            "protection", "fire_protection", "feather_falling", "blast_protection", "projectile_protection",
            "thorns", "respiration", "depth_strider", "aqua_affinity", "sharpness", "smite", "bane_of_arthropods",
            "knockback", "fire_aspect", "looting", "efficiency", "silk_touch", "unbreaking", "fortune",
            "power", "punch", "flame", "infinity", "luck_of_the_sea", "lure", "frost_walker", "mending",
            "binding_curse", "vanishing_curse", "impaling", "riptide", "loyalty", "channeling", "multishot",
            "piercing", "quick_charge", "soul_speed", "swift_sneak", "wind_burst", "density", "breach", "lunge"};
        static_assert(ids.size() == static_cast<std::size_t>(Enchant::Type::NumEnchantments));
        auto enchants = stack.constructItemEnchantsFromUserData();
        for (auto const& entries : *enchants.mItemEnchants) for (auto const& entry : entries) {
            auto type = static_cast<std::size_t>(entry.mEnchantType);
            if (type < ids.size() && entry.mLevel > 0) {
                auto& level = result.enchantments[std::string("minecraft:") + ids[type]];
                level = std::max(level, entry.mLevel);
            }
        }
    }
    for (auto const& tag : *item.mTags) result.tags.push_back(tag.getString());
    std::array<std::pair<char const*, char const*>, 9> categories{{
        {"minecraft:is_sword", "sword"}, {"minecraft:is_axe", "axe"}, {"minecraft:is_pickaxe", "pickaxe"},
        {"minecraft:is_shovel", "shovel"}, {"minecraft:is_hoe", "hoe"}, {"minecraft:is_shield", "shield"},
        {"minecraft:is_crossbow", "crossbow"}, {"minecraft:is_bow", "bow"}, {"minecraft:is_trident", "spear"}}};
    for (auto const& [tag, category] : categories)
        if (std::find(result.tags.begin(), result.tags.end(), tag) != result.tags.end()) { result.category = category; break; }
    // Several built-in Bedrock items do not expose the Java classifier tags.
    if (result.id.starts_with("minecraft:") && result.category.empty()) {
        auto id = result.id.substr(10);
        if (id.ends_with("_sword")) result.category = "sword";
        else if (id.ends_with("_pickaxe")) result.category = "pickaxe";
        else if (id.ends_with("_axe")) result.category = "axe";
        else if (id.ends_with("_shovel")) result.category = "shovel";
        else if (id.ends_with("_hoe")) result.category = "hoe";
        else if (id == "shield" || id == "crossbow" || id == "bow" || id == "fishing_rod") result.category = id;
        else if (id == "trident") result.category = "spear";
        else if (id == "splash_potion" || id == "lingering_potion") result.category = "throwable_potion";
    }
    switch (item.mUseAnim) {
    case SharedTypes::Legacy::UseAnimation::Eat: result.useAction = "eat"; break;
    case SharedTypes::Legacy::UseAnimation::Drink: result.useAction = "drink"; break;
    case SharedTypes::Legacy::UseAnimation::Block: result.useAction = "block"; break;
    case SharedTypes::Legacy::UseAnimation::Bow: result.useAction = "bow"; break;
    case SharedTypes::Legacy::UseAnimation::Spear: result.useAction = "spear"; break;
    case SharedTypes::Legacy::UseAnimation::Crossbow: result.useAction = "crossbow"; break;
    case SharedTypes::Legacy::UseAnimation::Spyglass: result.useAction = "spyglass"; break;
    case SharedTypes::Legacy::UseAnimation::GoatHorn: result.useAction = "toot_horn"; break;
    case SharedTypes::Legacy::UseAnimation::Brush: result.useAction = "brush"; break;
    default: break;
    }
    adaptBedrockHandItem(result);
    result.charged = result.category == "crossbow" && stack.mChargedItem && !stack.mChargedItem->isNull();
    return result;
}
static HandInput handInput(Player const& player, bool enabled) {
    HandInput result;
    result.enabled = enabled;
    result.sleeping = player.isSleeping();
    auto mount = [](Actor const* actor) -> std::optional<HandInput::Mount> {
        if (!actor) return {};
        HandInput::Mount result;
        result.id = actor->getTypeName();
        appendJavaEntityTags(result.id, result.tags);
        result.alive = actor->isAlive();
        if (result.id == "minecraft:pig") result.legacyAnimation = "ride_pig";
        else if (result.id == "minecraft:chest_boat"
            || std::ranges::find(result.tags, "minecraft:boat") != result.tags.end()) result.legacyAnimation = "boat";
        else if (result.id == "minecraft:horse" || result.id == "minecraft:donkey" || result.id == "minecraft:mule"
            || result.id == "minecraft:skeleton_horse" || result.id == "minecraft:zombie_horse"
            || result.id == "minecraft:camel" || result.id == "minecraft:strider") result.legacyAnimation = "ride";
        return result;
    };
    result.vehicle = mount(player.getVehicle());
    result.passenger = mount(player.getFirstPassenger());
    result.items[0] = handItem(player.getSelectedItem(), player.getSelectedItemSlot());
    result.items[1] = handItem(player.getOffhandSlot(), -1);
    result.items[0].fishing = player.getLevel().fetchEntity(*player.mFishingHookID, false) != nullptr;
    for (auto const& [name, slot] : std::array<std::pair<char const*, SharedTypes::Legacy::ArmorSlot>, 4>{{
        {"head", SharedTypes::Legacy::ArmorSlot::Head}, {"chest", SharedTypes::Legacy::ArmorSlot::Torso},
        {"legs", SharedTypes::Legacy::ArmorSlot::Legs}, {"feet", SharedTypes::Legacy::ArmorSlot::Feet}}})
        result.armor[name] = handItem(player.getArmor(slot), -2 - static_cast<int>(slot));
    // Remote players can advertise use without populating the local active stack.
    bool nativeUsing = player.getStatusFlag(ActorFlags::Usingitem);
    if ((nativeUsing || player.getItemUseDuration() > 0) && !player.mItemInUse->mItem->isNull())
        result.usingHand = player.mItemInUse->mSlot->mContainerId == ContainerID::Offhand ? 1 : 0;
    else if (nativeUsing) {
        for (int hand = 0; hand < 2; ++hand) {
            auto const& item = result.items[hand];
            if (item.category == "bow" || item.category == "crossbow") {
                result.usingHand = hand;
                break;
            }
        }
    }
    if (player.isBlocking()) {
        if (result.items[0].category == "shield") result.usingHand = 0;
        else if (result.items[1].category == "shield") result.usingHand = 1;
    }
    if (*player.mSwingingHand && (player.mSwingTime > 0 || player.mAttackAnim > 0)) {
        result.swingingHand = **player.mSwingingHand == HandSlot::Offhand ? 1 : 0;
        result.swingProgress = player.mSwingTime;
    }
    return result;
}

static EntityQueryInput entityInput(Player const& player, float frameAlpha,
    HandInput const& hands, double modelTime) {
    EntityQueryInput input;
    auto feet = player.getFeetPos();
    input.position = {feet.x, feet.y, feet.z};
    auto delta = player.getPosDelta();
    input.velocity = {delta.x, delta.y, delta.z};
    if (auto move = player.getEntityContext().tryGetComponent<LocalMoveVelocityComponent>()) {
        input.moveInput = {move->mValue->x, move->mValue->y, move->mValue->z};
        input.nativeMoveInputAvailable = true;
    }
    // The game's completed-frame statistic is independent of how many body,
    // hand, UI or shadow pose callbacks happen during a display frame.
    input.framesPerSecond = ProfilerLite::gProfilerLiteInstance().mFrameData->mFPS;
    auto rotation = player.getRotation();
    input.yaw = rotation.y;
    auto partialTick = std::isfinite(frameAlpha) ? std::clamp(static_cast<double>(frameAlpha), 0.0, 1.0) : 0;
    input.renderTicks = modelTime * 20;
    auto nativePosition = player.getPosition(), interpolated = player.getInterpolatedPosition(static_cast<float>(partialTick));
    input.renderPosition = {feet.x + (interpolated.x - nativePosition.x), feet.y + (interpolated.y - nativePosition.y),
        feet.z + (interpolated.z - nativePosition.z)};
    auto previousYaw = player.mBuiltInComponents->mActorRotationComponent->mRotPrev->y;
    input.viewYaw = previousYaw + (rotation.y - previousYaw) * partialTick;
    auto previousPitch = player.mBuiltInComponents->mActorRotationComponent->mRotPrev->x;
    // ActorRenderData is absent in some real pose passes. Use the player's
    // interpolated head/body angles, matching Java's relative-yaw convention.
    input.setHeadAngles(previousPitch + (rotation.x - previousPitch) * partialTick,
        player.getInterpolatedBodyRot(static_cast<float>(partialTick)),
        player.getInterpolatedHeadRot(static_cast<float>(partialTick)));
    input.tick = player.mTickCount;
    input.uuidLeast = std::bit_cast<std::int64_t>(player.getUuid().b);
    if (auto value = player.getAttribute(SharedAttributes::HEALTH()); value.mPtr) {
        input.health = value.mPtr->mCurrentValue;
        input.maxHealth = value.mPtr->mCurrentMaxValue;
    }
    if (auto value = player.getAttribute(Player::HUNGER()); value.mPtr) input.foodLevel = value.mPtr->mCurrentValue;
    if (auto value = player.getAttribute(Player::LEVEL()); value.mPtr) {
        auto level = static_cast<double>(value.mPtr->mCurrentValue);
        if (std::isfinite(level)) input.playerLevel = static_cast<int>(std::clamp(level, 0.0, double(std::numeric_limits<int>::max())));
    }
    input.equipmentCount = static_cast<int>(std::ranges::count_if(hands.armor,
        [](auto const& entry) { return !entry.second.id.empty(); }));
    input.usingItem = hands.usingHand >= 0;
    if (input.usingItem) {
        auto const& active = *player.mItemInUse->mItem;
        auto const& item = !active.isNull() ? active
            : hands.usingHand == 1 ? player.getOffhandSlot() : player.getSelectedItem();
        if (!item.isNull() && item.mItem) {
            input.itemMaxUseTicks = item.mItem->getMaxUseDuration(&item);
            input.itemUseTicks = !active.isNull() ? player.mItemInUse->getUsedDuration(player.getEntityContext())
                : player.getItemUseDuration();
        }
    }
    auto& level = player.getLevel();
    input.worldTime = level.getTime();
    input.onGround = player.isOnGround();
    input.crouching = player.isSneaking();
    input.passenger = hands.vehicle && hands.vehicle->alive;
    input.onClimbable = player.onClimbableBlock();
    input.hurt = player.mBuiltInMobComponents->mMobHurtTimeComponent->mValue > 0;
    if (auto spin = player.getEntityContext().tryGetComponent<RiptideTridentSpinAttackComponent>()) input.riptide = spin->mValue > 0;
    auto const& walk = *player.mBuiltInMobComponents->mWalkAnimationComponent;
    input.walkAnimationSpeed = walk.mWalkAnimSpeedO + (walk.mWalkAnimSpeed - walk.mWalkAnimSpeedO) * partialTick;
    input.inWater = player.isInWater();
    input.inRain = !input.inWater && player.isInRain();
    input.swimming = player.isSwimming();
    input.flying = player.isFlying();
    input.gliding = player.isGliding();
    input.sprinting = player.isSprinting();
    input.crawling = player.isCrawling();
    input.sleeping = player.isSleeping();
    input.alive = player.isAlive();
    {
        std::scoped_lock lock(arrowHitMutex);
        if (auto found = arrowHits.find(player.getOrCreateUniqueID().rawID); found != arrowHits.end()) {
            input.arrowCount = found->second.arrows.sample(input.tick, input.alive);
            if (!input.arrowCount) arrowHits.erase(found);
        }
    }
    input.eating = hands.usingHand >= 0 && hands.items[hands.usingHand].useAction == "eat";
    input.fishing = hands.items[0].fishing;
    return input;
}

static std::size_t registerVisualGeometry(AnimationState& state, std::shared_ptr<DataDrivenGeometry> const& geometry,
    std::span<RenderController const* const> controllers, bool validateNames) {
    auto& cached = state.visualGeometryCaches[geometry.get()];
    auto const& parts = *geometry->mModelParts;
    bool sameParts = cached.geometry.lock() == geometry && cached.parts == parts.data() && cached.partCount == parts.size();
    if (!sameParts) {
        cached = {};
        cached.geometry = geometry;
        cached.parts = parts.data(); cached.partCount = parts.size();
    }
    if (validateNames && !cached.namesValidated) {
        if (parts.size() != state.poseBindings.size() || !std::all_of(parts.begin(), parts.end(), [&](auto const& part) {
            return state.poseBinding(part.mName->getString()) != nullptr;
        })) return 0;
        cached.namesValidated = true;
    }
    std::size_t count = 0;
    bool sameMeshes = sameParts;
    for (auto const& group : *geometry->mSkinnedMeshGroups) for (auto const& mesh : *group.mSkinnedMeshes) {
        if (count >= cached.meshes.size() || cached.meshes[count].lock() != mesh) sameMeshes = false;
        ++count;
    }
    sameMeshes = sameMeshes && count == cached.meshes.size();
    bool sameControllers = std::all_of(controllers.begin(), controllers.end(), [&](auto* controller) {
        return cached.controllers.contains(controller);
    });
    if (sameMeshes && sameControllers) { ++state.visualGeometryCacheHits; return count; }
    // Resource identities are verified through weak owners and every mesh
    // slot. Skin/texture replacements refresh immediately, including same-size
    // vectors and reused addresses; stable frames reuse the binding table.
    if (!sameMeshes) {
        cached.meshes.clear(); cached.controllers.clear();
        std::erase_if(state.visualMeshes, [](auto const& entry) { return entry.second.mesh.expired(); });
    }
    for (auto const& group : *geometry->mSkinnedMeshGroups) for (auto const& mesh : *group.mSkinnedMeshes) {
        if (!sameMeshes) cached.meshes.push_back(mesh);
        for (auto* controller : controllers) {
            auto& entry = state.visualMeshes[{mesh.get(), controller}];
            if (!sameParts || entry.mesh.lock() != mesh) entry.parts.clear();
            entry.mesh = mesh;
        }
    }
    cached.controllers.insert(controllers.begin(), controllers.end());
    ++state.visualGeometryRefreshes;
    return count;
}
class NumericAnimationPlayer final : public ActorAnimationPlayer {
    std::weak_ptr<AnimationState> state;
    Player* player;
    NativePoseLayout<AnimationState::PoseBinding> poseLayout;
    HashedString rawName{"animation.ysm_bedrock.numeric"};
public:
    NumericAnimationPlayer(std::shared_ptr<AnimationState> const& source, Player& target) : state(source), player(&target) {
        mFriendlyName = rawName;
        mBlendExpression->mImpl->emplace<float>(1.0f);
        mExpanded = true;
        mAnimationFinished = false;
    }
    void applyToPose(ApplyAnimationContext const&, RenderParams& params,
        std::unordered_map<SkeletalHierarchyIndex, std::vector<BoneOrientation>>& skeletons, float) override {
        auto data = state.lock();
        if (!data || !data->active || params.mActor != player) return;
        std::scoped_lock lock(data->frameMutex);
        if (!data->active) return;
        auto prepareStarted = std::chrono::steady_clock::now();
        auto bindLayout = [&](auto const& entry) {
            auto const& bones = entry.second;
            return bones.size() == data->poseBindings.size() && poseLayout.bind(bones.size(),
                [&](std::size_t i) -> decltype(auto) { return bones[i].mName->getString(); },
                [&](std::string const& name) { return data->poseBinding(name); });
        };
        auto firstMatching = std::find_if(skeletons.begin(), skeletons.end(), bindLayout);
        if (firstMatching == skeletons.end()) return;
        if (data->calls < 4) data->startupEvent("pose", {{"first_person_scope", FirstPersonRenderScope::isActive()},
            {"component_initialized", params.mAnimationComponent && params.mAnimationComponent->mAnimationComponentInitialized},
            {"pose_calls", data->calls}});
        auto* renderData = static_cast<ActorRenderData*>(params.mActorRenderData);
        bool firstPerson = data->bindings.bind(params, data->texture, data->geometry.armParts);
        bool inventory = (UiRenderContextTracker::isActive() && !data->inGame && !data->remote) || (renderData && renderData->mIsInUI);
        if (inventory) firstPerson = false;
        if (!data->nativeRenderStacks.contains("initial")) data->nativeRenderStacks["initial"] = nativeRenderStack();
        if (!inventory && data->calls >= 2000 && !data->nativeRenderStacks.contains("world"))
            data->nativeRenderStacks["world"] = nativeRenderStack();
        if (data->visualMaterials) if (auto model = params.mModel.lock()) {
            // Skin/texture preparation can replace meshes after getGeometry.
            // Rebind from this exact player's current model while its native
            // animation pass owns the render inputs, including untagged draws.
            std::vector<RenderController const*> controllers{nullptr};
            for (auto const& controller : *model->mRenderControllers) controllers.push_back(controller.first.get());
            if (params.mAnimationComponent) {
                if (auto client = params.mAnimationComponent->tryGetClient())
                    for (auto const& controller : *client->mRenderControllersToProcess) controllers.push_back(controller.mRenderController.get());
            }
            for (auto const& geometry : *model->mGeometries) {
                if (geometry) registerVisualGeometry(*data, geometry, controllers, true);
            }
        }
        data->prepareCpu.record(prepareStarted);
        auto now = std::chrono::steady_clock::now();
        bool firstState = false;
        std::string current = data->state;
        double seconds = data->stateTime;
        Pose observedPose;
        if (inventory) {
            ++data->inventoryCalls;
            auto context = data->evaluation.expressions.forObservation();
            auto input = data->entitySnapshot;
            input.renderingInInventory = true;
            auto binder = data->entityQueries;
            binder.bind(context, input);
            NativeBlockQueryScope blocks(context, *player);
            context.keyboardEnabled = false;
            context.pressedKeys.clear();
            observedPose = data->controllers.observePlayer(data->sampler, context);
            data->inventoryShoulders = {shoulderLocator(context, true), shoulderLocator(context, false)};
            if (!data->diagnostics.empty()) {
                data->lastObservation = {{"inventory", context.read("ysm.rendering_in_inventory").number()},
                    {"ui_context", UiRenderContextTracker::isActive()}, {"native_ui_flag", renderData && renderData->mIsInUI},
                    {"life_time", context.read("query.life_time").number()}, {"physics_time", context.physics ? context.physics->time() : 0},
                    {"main_updates", data->mainUpdates}, {"owner_frame", data->evaluation.expressions.frame},
                    {"failures", context.failures - data->evaluation.expressions.failures},
                    {"variables", diagnosticVariables(context)},
                    {"diagnostics", context.diagnostics},
                    {"head", observedPose.contains("head") ? Json(observedPose.at("head").rotation) : Json()},
                    {"root", observedPose.contains("root") ? AnimationSampler::report({{"root", observedPose.at("root")}}) : Json()}};
            }
        } else {
        if (firstPerson) ++data->handCalls; else ++data->bodyCalls;
        auto hands = handInput(*player, data->preview.empty());
        auto frameAlpha = params.mBaseActorRenderContext ? params.mBaseActorRenderContext->mFrameAlpha : 0.0f;
        auto animationNow = data->paused ? data->pauseStarted : now;
        double parallelTime = std::chrono::duration<double>(animationNow - data->created).count();
        auto nextInput = entityInput(*player, frameAlpha, hands, parallelTime);
        if (auto render = params.mBaseActorRenderContext; render && !data->remote)
            nextInput.viewPerspective = std::clamp(render->mClientInstance.getOptions().getPlayerViewPerspective(), 0, 2);
        if (!data->remote) {
            std::scoped_lock lock(elytraInputMutex);
            nextInput.elytraRotationZ = nativeElytraInput.sample(nativeInputFrame.load(),
                !firstPerson && !data->paused && data->hasElytraLocator, hands.equipment("chest").id == "minecraft:elytra",
                nextInput.gliding, nextInput.crouching, nextInput.velocity);
        }
        if (data->peerFlight) {
            nextInput.flying = (*data->peerFlight & 1u) != 0;
            nextInput.gliding = (*data->peerFlight & 2u) != 0;
        }
        current = nextInput.mainAnimation();
        if (!data->preview.empty()) current = data->preview;
        bool changed = current != data->state;
        if (data->sampledFrame != data->renderFrames && (changed || data->cachedPose.empty()
            || (!data->paused && now - data->lastSample >= std::chrono::duration<double>(1.0 / 120)))) {
            firstState = !data->diagnostics.empty() && data->observedStates.insert(current).second;
            if (changed) { data->state = current; data->epoch = animationNow; }
            seconds = std::chrono::duration<double>(animationNow - data->epoch).count();
            data->stateTime = seconds;
            data->sampledFrame = data->renderFrames;
            ++data->mainUpdates;
            auto logicStarted = std::chrono::steady_clock::now();
            auto& context = data->evaluation.expressions;
            context.localSync = !data->preview.empty();
            context.pressedKeys = data->pressedKeys;
            context.keyboardEnabled = data->inGame;
            context.modVersions = data->modVersions;
            auto& queries = context.queries;
            queries["ysm.texture_name"] = data->textureName;
            hands.armorValue = player->getArmorValue();
            static HashedString const shieldId("minecraft:shield");
            if (auto shield = player->getLevel().getItemRegistry().lookupByNameNoAlias(shieldId); shield)
                hands.shieldBlockCooldown = player->isItemOnCooldown(shield->getCooldownCategory());
            data->handInput = std::move(hands);
            data->logicalTime = parallelTime;
            data->weatherSnapshot = sampleNativeSceneQueries(nextInput, *player, params.mBaseActorRenderContext);
            bool inputChanged = data->motionChanges.empty() || nextInput.moveInput != data->entitySnapshot.moveInput
                || nextInput.nativeMoveInputAvailable != data->entitySnapshot.nativeMoveInputAvailable;
            data->entitySnapshot = nextInput;
            data->entityQueries.bind(context, data->entitySnapshot);
            NativeBlockQueryScope blocks(context, *player);
            if (inputChanged) {
                if (data->motionChanges.size() >= 32) data->motionChanges.erase(data->motionChanges.begin());
                data->motionChanges.push_back({{"tick", nextInput.tick}, {"time", parallelTime},
                    {"available", nextInput.nativeMoveInputAvailable}, {"move_input", nextInput.moveInput},
                    {"velocity", nextInput.velocity}, {"yya", context.read("ysm.yya").number()}});
            }
            data->lastSample = now;
            data->handLayers = data->hands.update(parallelTime, data->handInput);
            for (auto const& layer : data->handLayers) data->handAnimationsSeen.insert(layer.name);
            double extraTime = data->extra.name.empty() ? 0 : std::chrono::duration<double>(animationNow - data->extraEpoch).count();
            // Preview changes the selected base/input, not the lifetime of
            // parallel players. A separate raw sampler would start new event
            // owners at the accumulated world clock and replay old intervals.
            auto sampledPose = data->controllers.samplePlayer(data->sampler, current, seconds, parallelTime,
                data->evaluation, data->handInput, data->handLayers, data->extra.name, extraTime);
            if (!data->diagnostics.empty()) {
                // A single swing can begin and end between regular status writes.
                auto const& hand = data->handInput;
                bool maceSwing = hand.swingingHand == 0 && hand.items[0].id == "minecraft:mace";
                bool startedSwing = maceSwing && (!data->samplingMaceSwing || hand.swingProgress < data->lastMaceProgress);
                if (startedSwing) data->lastMaceSwing = Json::array();
                if ((maceSwing && data->lastMaceSwing.size() < 12) || (!maceSwing && data->samplingMaceSwing)) {
                    Json layers = Json::array(), visibility = Json::object();
                    for (auto const& layer : data->handLayers) layers.push_back(layer.name);
                    for (auto const& [name, bone] : sampledPose) {
                        auto old = data->cachedPose.find(name);
                        bool hidden = bone.scale == Triple{};
                        bool wasHidden = old != data->cachedPose.end() && old->second.scale == Triple{};
                        if (hidden != wasHidden && visibility.size() < 64) visibility[name] = !hidden;
                    }
                    for (auto const& [name, bone] : data->cachedPose)
                        if (bone.scale == Triple{} && !sampledPose.contains(name) && visibility.size() < 64) visibility[name] = true;
                    data->lastMaceSwing.push_back({{"time", parallelTime}, {"tick", nextInput.tick},
                        {"swinging", maceSwing}, {"item", hand.items[0].id}, {"category", hand.items[0].category},
                        {"layers", layers}, {"swing_states", data->controllers.swingStates()},
                        {"grips_before", data->geometry.visibleHandMasks(data->cachedPose)},
                        {"grips_after", data->geometry.visibleHandMasks(sampledPose)}, {"local_visibility_changes", visibility}});
                }
                data->samplingMaceSwing = maceSwing;
                data->lastMaceProgress = hand.swingProgress;
            }
            data->cachedPose = std::move(sampledPose);
            data->awaitingFormPose = false;
            // Both installations are idle under frameMutex. A failed logical
            // round retains dirty state for a later successful normal update.
            if (context.failures == context.frameFailures)
                if (auto delta = projectRoamingArm(*context.roaming, *data->armEvaluation.expressions.roaming)) {
                    // Roaming is latest-value state, not an event queue. Keep
                    // the complete bounded delta for main-thread adoption.
                    for (auto const& [name, value] : *delta) data->pendingRoaming[name] = value;
                    ++data->roamingProjections;
                }
            data->logicCpu.record(logicStarted);
        }
        if (firstPerson && data->armTime != data->logicalTime
            && (data->geometry.armCarriers.contains("rightarm") || data->geometry.armCarriers.contains("leftarm"))) {
            auto const& context = data->evaluation.expressions;
            data->armEvaluation.expressions.queries = context.queries;
            data->armEvaluation.expressions.entityPosition = context.entityPosition;
            data->armEvaluation.expressions.positionDelta = context.positionDelta;
            data->armEvaluation.expressions.particleOrigin = context.particleOrigin;
            data->armEvaluation.expressions.pressedKeys = context.pressedKeys;
            data->armEvaluation.expressions.keyboardEnabled = context.keyboardEnabled;
            data->armEvaluation.expressions.modVersions = context.modVersions;
            if (data->armEpoch < 0) data->armEpoch = data->logicalTime;
            NativeBlockQueryScope blocks(data->armEvaluation.expressions, *player);
            data->cachedArmPose = data->armControllers.sampleFirstPerson(data->armSampler, data->logicalTime - data->armEpoch,
                data->armEvaluation, data->handInput);
            data->armTime = data->logicalTime;
            ++data->armUpdates;
        }
        }
        current = data->state;
        seconds = data->stateTime;
        auto const& pose = inventory ? observedPose : data->cachedPose;
        if (!firstPerson) {
            auto masks = data->geometry.visibleHandMasks(pose);
            data->visibleHands.update(inventory, masks);
        }
        std::size_t applied = 0;
        std::size_t transparentParts = 0;
        Json nativeArms = Json::object();
        Json nativeBodyRoots = Json::object();
        bool reportDue = !data->diagnostics.empty() && (firstState || now - data->lastReport > std::chrono::seconds(1));
        bool nativeSpear = !firstPerson && !inventory && data->hands.usesNativeSpearPose();
        if (nativeSpear) {
            auto const& input = data->handInput;
            auto stage = input.usingHand == 0 ? "player.use" : input.swingingHand == 0 ? "player.swing" : "player.hold_mainhand";
            nativeSpear = !data->controllers.hasActiveStageAnimation(stage);
        }
        bool nativeSpearApplied = false;
        if (!firstPerson && !inventory) {
            auto const& input = data->handInput;
            data->ysmLanceCharge = input.enabled && !input.sleeping && input.usingHand == 0
                && input.items[0].isLance() && !nativeSpear;
        }
        double sourceYOffset = sourcePlayerRenderYOffset(player->isSneaking(), !firstPerson && !inventory);
        std::optional<glm::mat4> seat;
        if (!firstPerson && !inventory) if (auto* vehicle = player->getVehicle()) {
            if (auto resources = data->seatSource.lock()) {
                auto alpha = params.mBaseActorRenderContext ? params.mBaseActorRenderContext->mFrameAlpha : 1.f;
                if (auto world = resources->passengerTransform(*player, *vehicle, alpha, true)) {
                    auto yaw = glm::rotate(glm::mat4(1), glm::radians(180.f - player->getInterpolatedBodyYaw(alpha)), glm::vec3(0, 1, 0));
                    seat = glm::transpose(yaw) * *world * yaw;
                }
            }
        }
        auto poseStarted = std::chrono::steady_clock::now();
        for (auto hierarchy = firstMatching; hierarchy != skeletons.end(); ++hierarchy) {
            if (hierarchy != firstMatching && !bindLayout(*hierarchy)) continue;
            auto& bones = hierarchy->second;
            std::optional<Vec3> spearShoulder;
            if (nativeSpear) {
                auto arm = std::find_if(bones.begin(), bones.end(), [](auto const& bone) { return bone.mName->getString() == "rightarm"; });
                if (arm != bones.end()) spearShoulder = *arm->mTransform->mData[1];
            }
            Pose nativeItemPoses;
            if (firstPerson) for (auto const& [target, item] : data->geometry.firstPersonItems) {
                auto found = std::find_if(bones.begin(), bones.end(), [&](auto const& bone) { return bone.mName->getString() == item; });
                if (found == bones.end()) continue;
                auto const& value = found->mTransform->mData;
                auto const& baseline = found->mDefaultTransform->mData;
                auto& native = nativeItemPoses[target];
                auto triple = [](Vec3 const& v) { return Triple{v.x, v.y, v.z}; };
                auto position = triple(*value[0]), basePosition = triple(*baseline[0]);
                auto rotation = triple(*value[1]), baseRotation = triple(*baseline[1]);
                auto scale = triple(*value[2]), baseScale = triple(*baseline[2]);
                for (int axis = 0; axis < 3; ++axis) {
                    native.position[axis] = position[axis] - basePosition[axis];
                    native.rotation[axis] = rotation[axis] - baseRotation[axis];
                    native.scale[axis] = baseScale[axis] != 0 ? scale[axis] / baseScale[axis] : 1;
                }
            }
            for (std::size_t index = 0; index < bones.size(); ++index) {
                auto& bone = bones[index];
                auto const& name = bone.mName->getString();
                auto const& binding = poseLayout[index];
                auto& value = bone.mTransform->mData;
                auto const& baseline = bone.mDefaultTransform->mData;
                // The native spear controller has already evaluated its attack
                // and kinetic charge on rightarm/rightitem during this pass.
                // Keep the grip on the native item and transfer the shoulder
                // rotation to the model's corresponding third-person arms.
                bool nativeSpearGrip = nativeSpear && name == "rightitem";
                if (binding.carrier && reportDue)
                    nativeArms[name] = Json::array({Json::array({value[0]->x,value[0]->y,value[0]->z}),
                        Json::array({value[1]->x,value[1]->y,value[1]->z}),Json::array({value[2]->x,value[2]->y,value[2]->z})});
                if (!binding.carrier || !firstPerson) {
                    if (!nativeSpearGrip) for (int i = 0; i < 2; ++i) *value[i] = *baseline[i];
                    *value[2] = *baseline[2];
                }
                if (binding.carrier) *value[2] = *baseline[2];
                auto const& sourcePose = binding.item ? nativeItemPoses : binding.arm ? data->cachedArmPose : pose;
                static std::string const emptyName;
                auto const& logical = binding.source ? *binding.source : emptyName;
                if (auto found = sourcePose.find(logical); found != sourcePose.end() && !binding.staticBackground) {
                    auto const& p = found->second;
                    auto position = binding.item ? p.position : nativeAnimationPosition(p.position);
                    if (!nativeSpearGrip) {
                        value[0]->x += static_cast<float>(position[0]);
                        value[0]->y += static_cast<float>(position[1]);
                        value[0]->z += static_cast<float>(position[2]);
                        value[1]->x += static_cast<float>(p.rotation[0]);
                        value[1]->y += static_cast<float>(p.rotation[1]);
                        value[1]->z += static_cast<float>(p.rotation[2]);
                    }
                    value[2]->x *= static_cast<float>(p.scale[0]);
                    value[2]->y *= static_cast<float>(p.scale[1]);
                    value[2]->z *= static_cast<float>(p.scale[2]);
                }
                if (spearShoulder && binding.meleeSpearArm) {
                    value[1]->x = baseline[1]->x + spearShoulder->x;
                    value[1]->y = baseline[1]->y + spearShoulder->y;
                    value[1]->z = baseline[1]->z + spearShoulder->z;
                    nativeSpearApplied = true;
                }
                if (binding.scaleRoot) {
                    // Translate the private carrier before its own scale, as
                    // the source renderer translates the entity pose stack.
                    value[0]->y += static_cast<float>(sourceYOffset * 16.0);
                    if (seat) {
                        // This private, zero-pivot carrier precedes every author root.
                        // The body pass can bypass the individual actor dispatcher.
                        auto const& matrix = *seat;
                        value[0]->x -= 16.f * matrix[3].x;
                        value[0]->y += 16.f * matrix[3].y;
                        value[0]->z += 16.f * matrix[3].z;
                        glm::mat3 axes(matrix);
                        glm::vec3 scale(glm::length(axes[0]), glm::length(axes[1]), glm::length(axes[2]));
                        if (glm::determinant(axes) < 0.f) scale.x = -scale.x;
                        if (std::abs(scale.x) > 1.e-6f && scale.y > 1.e-6f && scale.z > 1.e-6f) {
                            for (int i = 0; i < 3; ++i) axes[i] /= scale[i];
                            float y = std::asin(std::clamp(-axes[0].z, -1.f, 1.f));
                            bool regular = std::abs(std::cos(y)) > 1.e-5f;
                            float x = regular ? std::atan2(axes[1].z, axes[2].z) : 0.f;
                            float z = regular ? std::atan2(axes[0].y, axes[0].x) : std::atan2(-axes[1].x, axes[1].y);
                            value[1]->x -= glm::degrees(x);
                            value[1]->y -= glm::degrees(y);
                            value[1]->z += glm::degrees(z);
                        }
                        value[2]->x *= scale.x;
                        value[2]->y *= scale.y;
                        value[2]->z *= scale.z;
                    }
                    value[2]->x *= static_cast<float>(data->width);
                    value[2]->y *= static_cast<float>(data->height);
                    value[2]->z *= static_cast<float>(data->width);
                }
                if (binding.authoredRoot && reportDue)
                    nativeBodyRoots[logical] = {{"baseline_position", {baseline[0]->x, baseline[0]->y, baseline[0]->z}},
                        {"position", {value[0]->x, value[0]->y, value[0]->z}},
                        {"baseline_rotation", {baseline[1]->x, baseline[1]->y, baseline[1]->z}},
                        {"rotation", {value[1]->x, value[1]->y, value[1]->z}},
                        {"previous_matrix_translation", {(*bone.mMatrix->_m)[3][0],
                            (*bone.mMatrix->_m)[3][1], (*bone.mMatrix->_m)[3][2]}}};
                if (binding.carrier) {
                    value[2]->x *= firstPerson ? static_cast<float>(data->width) : 0.0f;
                    value[2]->y *= firstPerson ? static_cast<float>(data->height) : 0.0f;
                    value[2]->z *= firstPerson ? static_cast<float>(data->width) : 0.0f;
                }
                if (binding.opacitySurface && binding.visual && binding.visual->alpha == 0) {
                    value[2]->x = value[2]->y = value[2]->z = 0;
                    ++transparentParts;
                }
                ++applied;
            }
        }
        data->poseCpu.record(poseStarted);
        if (applied && !inventory && !firstPerson) data->worldBodyPosed.store(true, std::memory_order_relaxed);
        if (params.mAnimationComponent) params.mAnimationComponent->mNeedToUpdateQueryableBoneOrientations = true;
        ++data->calls;
        if (reportDue) {
            data->lastReport = now;
            Json nativeOrigin;
            if (auto render = params.mActorRenderData) {
                auto feet = player->getFeetPos();
                auto position = player->getPosition();
                nativeOrigin = {{"sneaking", player->isSneaking()}, {"in_ui", static_cast<bool>(render->mIsInUI)},
                    {"source_render_y_offset", sourceYOffset},
                    {"feet", {feet.x, feet.y, feet.z}}, {"actor_position", {position.x, position.y, position.z}},
                    {"render_position", {render->position->x, render->position->y, render->position->z}}};
                if (auto offset = player->getEntityContext().tryGetComponent<OffsetsComponent>())
                    nativeOrigin["height_offset"] = static_cast<float>(offset->mHeightOffset);
                if (params.mBaseActorRenderContext && params.mBaseActorRenderContext->mImpl) {
                    auto const& camera = *params.mBaseActorRenderContext->mImpl->mCameraPosition;
                    nativeOrigin["camera"] = {camera.x, camera.y, camera.z};
                }
            }
            Json renderResources = data->bindings.report(params);
            if (auto component = player->getAnimationComponent().tryGetClient()) {
                renderResources["controllers"] = Json::array();
                for (auto const& entry : *component->mRenderControllersToProcess) {
                    if (auto const& controller = entry.mRenderController) {
                        Json visibility = Json::array();
                        for (auto const& [part, condition] : *controller->mPartVisibility)
                            visibility.push_back({part, condition.getExpressionString()});
                        renderResources["controllers"].push_back(Json{{"name", controller->mName->getString()}, {"parts", visibility}});
                    }
                }
            }
            auto handLayers = Json::array();
            for (auto const& entry : data->handLayers)
                handLayers.push_back({{"name", entry.name}, {"time", entry.time}, {"owner", entry.timelineId}});
            auto entityQueries = Json::object();
            for (auto const* key : {"query.health", "query.max_health", "ysm.food_level", "query.time_stamp",
                "query.ground_speed", "query.vertical_speed", "query.yaw_speed", "query.body_y_rotation",
                "query.eye_target_y_rotation", "query.cardinal_facing_2d", "query.head_x_rotation", "query.head_y_rotation",
                "query.is_jumping", "query.is_sneaking", "query.is_eating", "ysm.is_fishing", "ysm.is_open_air",
                "ysm.weather", "ysm.is_close_eyes", "ysm.fps", "ysm.yya", "ysm.rendering_in_inventory", "ysm.elytra_rot_z",
                "ysm.ground_speed2", "ysm.input_horizontal", "ysm.input_vertical", "ysm.has_mainhand", "ysm.has_offhand",
                "ysm.has_helmet", "ysm.has_elytra", "ysm.armor_value", "ysm.in_shield_block_cooldown", "ctrl.playing_extra_animation",
                "query.player_level", "query.distance_from_camera", "query.item_in_use_duration", "ysm.hit_target_id", "ysm.hit_target_type",
                "query.is_riding", "query.is_in_water_or_rain"})
                entityQueries[key] = data->evaluation.expressions.read(key).number();
            auto frameAlpha = params.mBaseActorRenderContext ? params.mBaseActorRenderContext->mFrameAlpha : 0.0f;
            auto nativeAngles = Json{{"entity", {player->getRotation().x, player->getRotation().y}},
                {"body_yaw", player->getInterpolatedBodyRot(frameAlpha)},
                {"head_yaw", player->getInterpolatedHeadRot(frameAlpha)}};
            if (renderData) {
                nativeAngles["render_rotation"] = {renderData->rotation->x, renderData->rotation->y};
                nativeAngles["render_head"] = {renderData->mHeadRot->x, renderData->mHeadRot->y};
            }
            auto const& snapshot = data->entitySnapshot;
            auto const& weather = data->weatherSnapshot;
            auto nativeWeather = Json{{"source", weatherOriginName(weather.origin)},
                {"normalized_rain", snapshot.rainLevel}, {"normalized_lightning", snapshot.thunderLevel},
                {"rain", weather.rain}, {"lightning", weather.lightning}, {"events", weather.events},
                {"last_event", weather.lastEvent}, {"last_data", weather.lastData}};
            auto cpuCosts = Json{{"prepare", data->prepareCpu.report()}, {"logical_animation", data->logicCpu.report()},
                {"apply_pose", data->poseCpu.report()}};
            auto drawCosts = boneVisualRenderReport();
            if (!inventory && !data->performanceReport.empty()) {
                Json drawSinceStart = Json::object();
                for (auto const& [key, value] : drawCosts.items()) {
                    auto before = data->initialDrawCounters.find(key);
                    if (!value.is_number() || before == data->initialDrawCounters.end() || !before->is_number()) continue;
                    drawSinceStart[key] = value.get<double>() - before->get<double>();
                }
                auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                std::ofstream(data->performanceReport) << Json{{"model", data->modelId}, {"timestamp", stamp},
                    {"fps", data->evaluation.expressions.read("ysm.fps").number()}, {"first_person", firstPerson},
                    {"bones", data->poseBindings.size()}, {"elapsed_seconds", std::chrono::duration<double>(now - data->created).count()},
                    {"render_frames", data->renderFrames}, {"body_calls", data->bodyCalls}, {"cpu_costs", cpuCosts},
                    {"last_mace_swing", data->lastMaceSwing}, {"surface_timing_sample_interval", 64}, {"draw_since_installation", drawSinceStart}}.dump(2);
            }
            std::ofstream(data->diagnostics) << Json{{"calls", data->calls}, {"bones_applied", applied},
                {"cpu_costs", cpuCosts},
                {"bone_visual_render", drawCosts}, {"transparent_parts", transparentParts},
                {"bone_visual_bindings", {{"registered", data->visualMeshes.size()},
                    {"geometry_cache_hits", data->visualGeometryCacheHits}, {"geometry_refreshes", data->visualGeometryRefreshes},
                    {"matches", data->visualDrawMatches}, {"owner_palette_matches", data->visualDrawRecovered}, {"misses", data->visualDrawMisses},
                    {"samples", data->visualMissSamples}}},
                {"native_render_stacks", data->nativeRenderStacks},
                {"state", current}, {"time", seconds}, {"states_seen", data->observedStates},
                {"mode", data->preview.empty() ? "automatic" : "preview"},
                {"texture_name", data->textureName},
                {"entity_queries", entityQueries},
                {"native_motion", {{"move_input", data->entitySnapshot.moveInput},
                    {"walk_animation_speed", data->entitySnapshot.walkAnimationSpeed.value_or(0)},
                    {"hurt", data->entitySnapshot.hurt}, {"riptide", data->entitySnapshot.riptide},
                    {"vehicle", data->handInput.vehicle ? Json(data->handInput.vehicle->id) : Json()},
                    {"vehicle_tags", data->handInput.vehicle ? Json(data->handInput.vehicle->tags) : Json::array()},
                    {"passenger", data->handInput.passenger ? Json(data->handInput.passenger->id) : Json()},
                    {"passenger_tags", data->handInput.passenger ? Json(data->handInput.passenger->tags) : Json::array()},
                    {"available", data->entitySnapshot.nativeMoveInputAvailable},
                    {"velocity", data->entitySnapshot.velocity}, {"fps_raw", data->entitySnapshot.framesPerSecond},
                    {"render_position", data->evaluation.expressions.entityPosition},
                    {"position_delta", data->evaluation.expressions.positionDelta},
                    {"input_changes", data->motionChanges}}},
                {"native_angles", nativeAngles},
                {"native_weather", nativeWeather},
                {"native_sky", {{"visible", snapshot.skyVisible}, {"feet_y", snapshot.blockY}, {"blocking_y", snapshot.blockingHeight}}},
                {"life_time", data->evaluation.expressions.read("query.life_time").number()},
                {"physics", {{"states", data->evaluation.expressions.physics->size()},
                    {"time", data->evaluation.expressions.physics->time()}}},
                {"extra", data->extra.name},
                {"controllers", data->controllers.report()}, {"arm_controllers", data->armControllers.report()},
                {"arm_updates", data->armUpdates}, {"arm_time", data->armEvaluation.expressions.read("query.life_time").number()},
                {"arm_last_body_time", data->armTime},
                {"arm_physics", {{"states", data->armEvaluation.expressions.physics->size()},
                    {"time", data->armEvaluation.expressions.physics->time()}}},
                {"arm_variables", diagnosticVariables(data->armEvaluation.expressions)},
                {"roaming", {{"projections", data->roamingProjections},
                    {"pending", data->pendingRoaming},
                    {"body_fields", data->evaluation.expressions.roaming->fields()},
                    {"arm_fields", data->armEvaluation.expressions.roaming->fields()},
                    {"body_values", data->evaluation.expressions.roaming->namedValues()},
                    {"arm_values", data->armEvaluation.expressions.roaming->namedValues()},
                    {"body_dirty", data->evaluation.expressions.roaming->hasDelta()}}},
                {"arm_unsupported_channels", data->armSampler.unsupportedChannels()},
                {"arm_compile_diagnostics", data->armSampler.compileDiagnostics()},
                {"arm_evaluation_diagnostics", data->armEvaluation.expressions.diagnostics},
                {"arm_expression_failures", data->armEvaluation.expressions.failures},
                {"hands", {{"main_item", data->handInput.items[0].id}, {"offhand_item", data->handInput.items[1].id},
                    {"main_tags", data->handInput.items[0].tags}, {"offhand_tags", data->handInput.items[1].tags},
                    {"main_category", data->handInput.items[0].category}, {"main_use_action", data->handInput.items[0].useAction},
                    {"main_charged", data->handInput.items[0].charged}, {"offhand_charged", data->handInput.items[1].charged},
                    {"native_melee_spear_pose", nativeSpearApplied},
                    {"third_person_locator_masks", {data->visibleHands.get(inventory, false), data->visibleHands.get(inventory, true)}},
                    {"using_hand", data->handInput.usingHand}, {"swinging_hand", data->handInput.swingingHand},
                    {"layers", handLayers}, {"animations_seen", data->handAnimationsSeen}}},
                {"first_person", firstPerson}, {"native_arm_transforms", nativeArms},
                {"native_body_roots", nativeBodyRoots}, {"native_origin", nativeOrigin},
                {"native_glide_rotation", {{"suppressed", data->suppressedGlideRotations.load()},
                    {"last_native_xy", {data->nativeGlideX.load(), data->nativeGlideY.load()}}}},
                {"source_root_pose", data->geometry.bodyBones.contains(data->geometry.bodyRoot)
                    && pose.contains(data->geometry.bodyBones.at(data->geometry.bodyRoot))
                    ? AnimationSampler::report({{data->geometry.bodyBones.at(data->geometry.bodyRoot),
                        pose.at(data->geometry.bodyBones.at(data->geometry.bodyRoot))}}) : Json()},
                {"geometry_preparation_calls", data->preparationCalls}, {"hand_calls", data->handCalls}, {"body_calls", data->bodyCalls},
                {"inventory", inventory}, {"inventory_calls", data->inventoryCalls},
                {"main_updates", data->mainUpdates}, {"render_frames", data->renderFrames},
                {"ui_observation", data->lastObservation},
                {"keyboard", {{"in_game", data->inGame.load()}, {"keys", data->pressedKeys}}},
                {"mod_versions", data->modVersions},
                {"variables", diagnosticVariables(data->evaluation.expressions)},
                {"render_resources", renderResources},
                {"paused", data->paused},
                {"head_input", {data->evaluation.expressions.read("ysm.head_pitch").number(), data->evaluation.expressions.read("ysm.head_yaw").number()}},
                {"head_rotation", data->cachedPose.contains("head") ? Json(data->cachedPose.at("head").rotation) : Json()},
                {"unsupported_channels", data->sampler.unsupportedChannels()},
                {"compile_diagnostics", data->sampler.compileDiagnostics()},
                {"evaluation_diagnostics", data->evaluation.expressions.diagnostics},
                {"local_sync", data->evaluation.expressions.localSync},
                {"sync_dispatches", data->evaluation.expressions.syncDispatches},
                {"body_expression_failures", data->evaluation.expressions.failures},
                {"expression_failures", data->evaluation.expressions.failures + data->armEvaluation.expressions.failures}}.dump(2);
        }
    }
    void resetAnimation() override {}
    void buildBoneToPartMapping(AnimationComponent&) override {}
    void bindParticleEffects(std::unordered_map<HashedString, HashedString> const&) override {}
    void bindSoundEffects(std::unordered_map<HashedString, std::string> const&) override {}
    bool hasAnimationFinished() const override { return false; }
    std::shared_ptr<ActorAnimationPlayer> findAnimation(HashedString const&) override { return {}; }
    ActorAnimationType getAnimationType() const override { return ActorAnimationType::Empty; }
    HashedString const& getRawName() const override { return rawName; }
    void visit(AnimationVisitor&& visitor) override { visitor.mBase.get()(*this); }
};

struct AnimationRuntime::Impl {
    std::shared_ptr<AnimationState> state;
    std::shared_ptr<ActorAnimationPlayer> player;
    Player* target = nullptr;
};
AnimationRuntime::AnimationRuntime() : impl(std::make_unique<Impl>()) {}
AnimationRuntime::~AnimationRuntime() { reset(); }
AnimationRuntime::Prepared AnimationRuntime::prepare(ModelPack const& pack, std::filesystem::path diagnostics) {
    return std::make_shared<AnimationState>(pack, std::move(diagnostics));
}
std::shared_ptr<AnimationSampler const> AnimationRuntime::preparedBodySampler(Prepared const& prepared) {
    return prepared ? prepared->bodySampler : nullptr;
}
std::string const* AnimationRuntime::skinGeometry() const {
    return impl->state ? &impl->state->skinGeometryJson : nullptr;
}
std::vector<std::string> AnimationRuntime::preparedDiagnostics(Prepared const& prepared) {
    return prepared ? prepared->controllers.report().at("diagnostics").get<std::vector<std::string>>() : std::vector<std::string>{};
}
AnimationRuntime::RoamingPreparation AnimationRuntime::roamingPreparation(Prepared const& prepared) {
    if (!prepared || prepared->calls || prepared->attachments) throw std::runtime_error("Roaming candidate has already been used");
    return {prepared->roamingInstallation, prepared->evaluation.expressions.roaming->fields()};
}
void AnimationRuntime::restoreRoaming(Prepared const& prepared, RoamingPayload const& values) {
    if (!prepared || prepared->calls || prepared->attachments || prepared->roamingInitialized)
        throw std::runtime_error("Roaming candidate has already been initialized");
    prepared->evaluation.expressions.roaming->initializeProducer(values);
    connectRoamingArm(*prepared->evaluation.expressions.roaming, *prepared->armEvaluation.expressions.roaming);
    prepared->roamingInitialized = true;
}
AnimationRuntime::RoamingChanges AnimationRuntime::takeRoamingChanges(bool retire) {
    if (!impl->state) return {};
    auto& state = *impl->state;
    if (retire) state.active = false;
    // Joins the complete render callback before main-thread adoption. A retired
    // worker checks active again under this same lock and cannot append later.
    std::scoped_lock lock(state.frameMutex);
    RoamingChanges result{state.roamingInstallation, {}};
    result.delta.swap(state.pendingRoaming);
    return result;
}
RoamingPayload AnimationRuntime::roamingSnapshot() const {
    if (!impl->state) return {};
    auto& state = *impl->state;
    std::scoped_lock lock(state.frameMutex);
    return state.evaluation.expressions.roaming->snapshot();
}
void AnimationRuntime::start(ModelPack const& pack, std::filesystem::path diagnostics) {
    start(prepare(pack, std::move(diagnostics)));
}
std::vector<SoundCue> AnimationRuntime::takeSoundCues() {
    if (!impl->state) return {};
    auto& state = *impl->state;
    std::scoped_lock lock(state.frameMutex);
    std::vector<SoundCue> result;
    result.swap(state.evaluation.expressions.soundCues);
    for (auto& cue : state.armEvaluation.expressions.soundCues) {
        cue.firstPerson = true;
        result.push_back(std::move(cue));
    }
    state.armEvaluation.expressions.soundCues.clear();
    return result;
}
std::vector<ParticleCue> AnimationRuntime::takeParticleCues() {
    if (!impl->state) return {};
    auto& state = *impl->state;
    std::scoped_lock lock(state.frameMutex);
    auto result = state.evaluation.expressions.takeParticleCues();
    for (auto& cue : state.armEvaluation.expressions.takeParticleCues()) {
        cue.firstPerson = true;
        result.push_back(std::move(cue));
    }
    return result;
}
std::shared_ptr<PreviewPose const> AnimationRuntime::previewPose() const {
    auto state = impl->state;
    if (!state || !state->active) return {};
    std::scoped_lock lock(state->frameMutex);
    if (state->awaitingFormPose || state->sampledFrame == std::numeric_limits<std::uint64_t>::max()) return {};
    return std::make_shared<PreviewPose const>(PreviewPose{state->cachedPose, state->evaluation.expressions.boneVisuals});
}
std::vector<std::string> AnimationRuntime::debugLines() const {
    auto state = impl->state;
    if (!state || !state->active) return {};
    std::scoped_lock lock(state->frameMutex);
    std::vector<std::string> lines{"YSM 动画调试",
        "当前动作  " + (state->state.empty() ? std::string("等待输入") : state->state),
        "等级 " + std::to_string(state->entitySnapshot.playerLevel) + "   脚本错误累计 "
            + std::to_string(state->evaluation.expressions.failures)};
    if (!state->extra.name.empty()) lines.push_back("额外动作  " + state->extra.name);
    return lines;
}
void AnimationRuntime::start(Prepared next) {
    if (!next || next->calls || next->attachments) throw std::runtime_error("Animation candidate has already been used");
    if (!next->roamingInitialized) restoreRoaming(next, {});
    // CPU preparation may happen long before installation. Its animation clock
    // starts at publication, without replaying time spent loading in the worker.
    next->epoch = next->created = std::chrono::steady_clock::now();
    if (!next->performanceReport.empty()) {
        std::error_code error;
        std::filesystem::create_directories(next->performanceReport.parent_path(), error);
        next->initialDrawCounters = boneVisualRenderReport();
    }
    reset();
    impl->state = std::move(next);
    impl->state->startupEvent("started", {{"roaming_initial_values", impl->state->evaluation.expressions.roaming->namedValues()}});
    if (!impl->state->diagnostics.empty())
        std::ofstream(impl->state->diagnostics) << Json{{"stage", "waiting_for_animation_component"}}.dump(2);
}
void AnimationRuntime::swap(AnimationRuntime& other) noexcept { impl.swap(other.impl); }
void AnimationRuntime::attach(Player& player) {
    if (!impl->state) return;
    impl->state->active = true;
    impl->target = &player;
    auto& component = player.getAnimationComponent();
    if (!component.mAnimationComponentInitialized) return;
    if (!impl->player) impl->player = std::make_shared<NumericAnimationPlayer>(impl->state, player);
    auto& players = *component.mComponentAnimationPlayers;
    if (std::find(players.begin(), players.end(), impl->player) == players.end()) {
        players.push_back(impl->player);
        std::scoped_lock lock(impl->state->frameMutex);
        if (impl->state->attachments < 8) impl->state->startupEvent("attached", {{"players", players.size()},
            {"pose_calls", impl->state->calls}, {"geometry_preparations", impl->state->preparationCalls}});
        if (++impl->state->attachments == 1 && !impl->state->diagnostics.empty())
            std::ofstream(impl->state->diagnostics) << Json{{"stage", "attached"}, {"players", players.size()}}.dump(2);
    }
}
void AnimationRuntime::suspend(Player& player) {
    if (impl->state) {
        impl->state->active = false;
        std::scoped_lock lock(impl->state->frameMutex);
        impl->state->bindings.reset();
    }
    if (impl->player) {
        auto& players = *player.getAnimationComponent().mComponentAnimationPlayers;
        std::erase(players, impl->player);
    }
    impl->target = nullptr;
}
void AnimationRuntime::detach(Player& player) {
    suspend(player);
    reset();
}
void AnimationRuntime::preview(std::string state) {
    if (!impl->state) return;
    std::scoped_lock lock(impl->state->frameMutex);
    impl->state->preview = std::move(state);
    impl->state->epoch = std::chrono::steady_clock::now();
    impl->state->evaluation.activeBase.clear();
    impl->state->cachedPose.clear();
}
bool AnimationRuntime::playExtra(std::string state) {
    if (!impl->state) return false;
    std::scoped_lock lock(impl->state->frameMutex);
    if (!impl->state->sampler.hasClip(state)) return false;
    impl->state->extra.play(std::move(state));
    impl->state->evaluation.timelineCursors.erase(impl->state->extra.name);
    impl->state->extraEpoch = std::chrono::steady_clock::now();
    impl->state->cachedPose.clear();
    return true;
}
void AnimationRuntime::stopExtra() {
    if (!impl->state) return;
    std::scoped_lock lock(impl->state->frameMutex);
    if (impl->state->extra.stop()) impl->state->cachedPose.clear();
}
void AnimationRuntime::updateExtraMovement(bool locked, float strafe, float forward, bool jump, bool sneak) {
    if (!impl->state) return;
    auto& state=*impl->state;
    std::scoped_lock lock(state.frameMutex);
    if (state.extra.stopOnMovement(locked,strafe,forward,jump,sneak)) state.cachedPose.clear();
}
void AnimationRuntime::relayEnabled(bool enabled) {
    if (!impl->state) return;
    auto& data = *impl->state;
    std::scoped_lock lock(data.frameMutex);
    auto& context = data.evaluation.expressions;
    context.networkSync = enabled && !data.remote;
    if (!context.networkSync) context.syncOut.clear();
}
RelayPose AnimationRuntime::outgoingPose(Player const& owner, std::uint64_t& eventSequence, bool takeEvents) {
    RelayPose result;
    if (!impl->state || !impl->state->active) return result;
    auto& data = *impl->state;
    std::scoped_lock lock(data.frameMutex);
    auto& context = data.evaluation.expressions;
    result.texture = data.textureName;
    result.roaming = context.roaming->snapshot();
    // Local ability flags may not be replicated by Bedrock to observers.
    // Sample on the game tick even if the owner never renders their body.
    result.flight = (owner.isFlying() ? 1u : 0u) | (owner.isGliding() ? 2u : 0u);
    result.formValues = data.uiBindings.snapshotScalars(context);
    result.action = data.extra.name;
    result.actionSerial = data.extra.serial;
    if (!data.extra.name.empty()) {
        auto now = data.paused ? data.pauseStarted : std::chrono::steady_clock::now();
        result.actionTime = std::clamp(std::chrono::duration<double>(now - data.extraEpoch).count(), 0.0, 86400.0);
    }
    if (takeEvents) {
        auto count = std::min<std::size_t>(64, context.syncOut.size());
        for (std::size_t i = 0; i < count; ++i)
            result.events.push_back({++eventSequence, std::move(context.syncOut[i])});
        context.syncOut.erase(context.syncOut.begin(), context.syncOut.begin() + count);
    }
    return result;
}
void AnimationRuntime::applyPeerPose(RelayPose const& peer, std::uint64_t& eventCursor, bool replaceAction, Player const& actor) {
    if (!impl->state || !impl->state->active) return;
    auto& data = *impl->state;
    std::scoped_lock lock(data.frameMutex);
    data.remote = true;
    data.peerFlight = peer.flight;
    auto& context = data.evaluation.expressions;
    context.networkSync = false;
    auto const& feet = actor.getFeetPos();
    context.particleOrigin = {{feet.x, feet.y, feet.z}, actor.getRotation().y, actor.getInterpolatedBodyRot(1.0f), true};
    auto& roaming = *context.roaming;
    RoamingPayload authorized;
    for (auto const& [name, value] : peer.roaming) if (roaming.fields().contains(name)) authorized[name] = value;
    if (roaming.namedValues() != authorized) {
        roaming.initializeProducer(authorized);
        connectRoamingArm(roaming, *data.armEvaluation.expressions.roaming);
        data.cachedPose.clear();
    }
    if (data.uiBindings.applyScalars(context, data.receivedFormValues, peer.formValues)) data.cachedPose.clear();
    if (data.receivedFormValues != peer.formValues) data.receivedFormValues = peer.formValues;
    if (replaceAction) {
        data.extra.name = data.sampler.hasClip(peer.action) ? peer.action : std::string();
        data.extraEpoch = std::chrono::steady_clock::now()
            - std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(peer.actionTime));
        data.evaluation.timelineCursors.erase(data.extra.name);
        data.cachedPose.clear();
    }
    for (auto const& event : peer.events) if (event.sequence > eventCursor) {
        eventCursor = event.sequence;
        std::vector<MolangValue> args;
        for (auto value : event.arguments) args.emplace_back(static_cast<double>(value));
        context.dispatchSync(args);
    }
}
bool AnimationRuntime::hasWorldBodyPose() const {
    return impl->state && impl->state->worldBodyPosed.load(std::memory_order_relaxed);
}
void AnimationRuntime::remoteFrame() {
    if (!impl->state) return;
    auto& data = *impl->state;
    std::scoped_lock lock(data.frameMutex);
    ++data.renderFrames;
    // Remote actions are dispatched once from the main tick.
    data.evaluation.expressions.syncOut.clear();
}
Json AnimationRuntime::modelUiValues() const {
    if (!impl->state || !impl->state->active) return Json::object();
    std::scoped_lock lock(impl->state->frameMutex);
    return impl->state->uiBindings.observe(impl->state->evaluation.expressions);
}
bool AnimationRuntime::modelUiAction(std::string const& route, double value, int label) {
    if (!impl->state || !impl->state->active) return false;
    auto& state = *impl->state;
    std::scoped_lock lock(state.frameMutex);
    auto& context = state.evaluation.expressions;
    if (!state.uiBindings.act(context, route, value, label)) return false;
    if (auto delta = projectRoamingArm(*context.roaming, *state.armEvaluation.expressions.roaming))
        for (auto const& [name, item] : *delta) state.pendingRoaming[name] = item;
    state.cachedPose.clear(); state.cachedArmPose.clear();
    state.awaitingFormPose=true;
    return true;
}
void AnimationRuntime::setPaused(bool paused) {
    if (!impl->state) return;
    std::scoped_lock lock(impl->state->frameMutex);
    auto& data = *impl->state;
    if (data.paused == paused) return;
    auto now = std::chrono::steady_clock::now();
    if (paused) data.pauseStarted = now;
    else {
        auto duration = now - data.pauseStarted;
        data.epoch += duration;
        data.created += duration;
        data.extraEpoch += duration;
        data.lastSample = now;
    }
    data.paused = paused;
}
void AnimationRuntime::reset(bool entityInputs) {
    if (impl->state) {
        impl->state->active = false;
        std::scoped_lock lock(impl->state->frameMutex);
        impl->state->bindings.reset();
    }
    impl->player.reset();
    impl->target = nullptr;
    impl->state.reset();
    if (entityInputs) {
        std::scoped_lock lock(elytraInputMutex);
        nativeElytraInput = {};
        std::scoped_lock arrowLock(arrowHitMutex);
        arrowHits.clear();
    }
}
void AnimationRuntime::setRenderTexture(NativeTextureSet texture, std::string const& name) {
    if (!impl->state) return;
    std::scoped_lock lock(impl->state->frameMutex);
    impl->state->texture = std::move(texture);
    impl->state->textureName = name;
}
void AnimationRuntime::setHostInputs(std::set<int> keys, bool inGame,
    std::map<std::string, std::string> const& versions, bool newFrame) {
    if (newFrame) ++nativeInputFrame;
    if (!impl->state) return;
    std::scoped_lock lock(impl->state->frameMutex);
    impl->state->pressedKeys = std::move(keys);
    impl->state->inGame = inGame;
    impl->state->modVersions = versions;
    if (newFrame) ++impl->state->renderFrames;
}
void AnimationRuntime::trackSeats(std::weak_ptr<NativeReplacedResources> source) {
    if (!impl->state) return;
    std::scoped_lock lock(impl->state->frameMutex);
    impl->state->seatSource = std::move(source);
}
void AnimationRuntime::prepareForRender(Player& player, AnimationComponent& component) {
    if (!impl->state || !impl->state->active || &component != &player.getAnimationComponent()) return;
    // Native skin initialization needs its model/render-parameter preparation.
    // Reconcile after that preparation and before animation-player iteration.
    // Doing this in getGeometry could invalidate an iteration already in flight.
    {
        std::scoped_lock lock(impl->state->frameMutex);
        if (impl->state->startupRenderPreparations++ < 8)
            impl->state->startupEvent("render_component_prepared", {
                {"component_initialized", static_cast<bool>(component.mAnimationComponentInitialized)},
                {"players", component.mComponentAnimationPlayers->size()}, {"pose_calls", impl->state->calls}});
    }
    attach(player);
}
void AnimationRuntime::reportFirstPersonPass(bool rendered) {
    if (!impl->state || !impl->state->active || impl->state->diagnostics.empty()) return;
    std::scoped_lock lock(impl->state->frameMutex);
    if (impl->state->startupFirstPersonPasses++ >= 4) return;
    impl->state->startupEvent("first_person_pass_end", {{"rendered", rendered}, {"pose_calls", impl->state->calls},
        {"hand_calls", impl->state->handCalls}, {"body_calls", impl->state->bodyCalls},
        {"geometry_preparations", impl->state->preparationCalls}});
}
void AnimationRuntime::prepareGeometry(RenderController& controller, std::shared_ptr<DataDrivenGeometry> const& geometry, RenderParams& params) {
    if (!impl->state || !impl->state->active || params.mActor != impl->target) return;
    auto const& info = geometry->mSourceGeometry->mGeometryInfoPtr;
    if (!info || !info->mName->getString().starts_with("geometry.ysm_bedrock.")) return;
    std::scoped_lock lock(impl->state->frameMutex);
    bool modelDataBefore = geometry->mUseModelData;
    std::size_t visualMeshCount = 0;
    if (impl->state->visualMaterials) {
        RenderController const* requested[]{&controller};
        visualMeshCount = registerVisualGeometry(*impl->state, geometry, requested, false);
    }
    // Retain the native geometry so initialization and held-item parent mapping
    // can finish. Suppress visibility only until its animation component is ready.
    bool drawEnabled = !(params.mActor == impl->target && params.mAnimationComponent
        && !params.mAnimationComponent->mAnimationComponentInitialized);
    auto guardedParts = impl->state->bindings.prepare(controller, geometry, impl->state->geometry.armParts, drawEnabled);
    if (!drawEnabled) {
        if (impl->state->geometryDeferrals++ < 8)
            impl->state->startupEvent("geometry_visibility_deferred", {{"controller", controller.mName->getString()},
                {"pose_calls", impl->state->calls}, {"guarded_parts", guardedParts}, {"total_parts", geometry->mModelParts->size()}});
    }
    if (!impl->state->diagnostics.empty() && impl->state->preparationCalls < 8) {
        std::size_t body = 0, bodyVisible = 0, bodyConditions = 0, bodyConstantHidden = 0;
        for (auto const& part : *geometry->mModelParts) {
            if (!impl->state->geometry.bodyBones.contains(part.mName->getString())) continue;
            ++body;
            if (part.mVisible) ++bodyVisible;
            auto found = part.mIsVisibleMap->find(&controller);
            if (found != part.mIsVisibleMap->end()) {
                ++bodyConditions;
                if (auto constant = std::get_if<float>(&*(*found).second.mImpl); constant && *constant == 0) ++bodyConstantHidden;
            }
        }
        bool attached = params.mAnimationComponent && impl->player
            && std::find(params.mAnimationComponent->mComponentAnimationPlayers->begin(), params.mAnimationComponent->mComponentAnimationPlayers->end(), impl->player)
                != params.mAnimationComponent->mComponentAnimationPlayers->end();
        impl->state->startupEvent("geometry", {{"controller", controller.mName->getString()},
            {"model_data_before", modelDataBefore}, {"model_data_after", static_cast<bool>(geometry->mUseModelData)},
            {"native_visual_meshes", visualMeshCount}, {"native_visual_groups", geometry->mSkinnedMeshGroups->size()},
            {"first_person_scope", FirstPersonRenderScope::isActive()}, {"numeric_attached", attached},
            {"draw_enabled", drawEnabled}, {"guarded_parts", guardedParts},
            {"pose_calls", impl->state->calls}, {"body_parts", body}, {"body_static_visible", bodyVisible},
            {"body_visibility_conditions", bodyConditions}, {"body_constant_hidden", bodyConstantHidden}});
    }
    ++impl->state->preparationCalls;
}
std::span<BoneVisual const> AnimationRuntime::boneVisuals(SkinnedMesh const& mesh, RenderController const* controller,
    std::span<BoneVisual> local, std::vector<BoneVisual>& overflow, Actor const* owner) {
    auto state = impl->state;
    if (!state || !state->active || !state->visualMaterials) return {};
    std::scoped_lock lock(state->frameMutex);
    auto prepared = state->visualMeshes.find({&mesh, controller});
    if (prepared == state->visualMeshes.end()) prepared = state->visualMeshes.find({&mesh, nullptr});
    if (prepared == state->visualMeshes.end() || prepared->second.mesh.lock().get() != &mesh) {
        // Use transient palette lookup only in this peer's own native draw.
        // Unattributed draws must never infer the owner from shared bone names.
        if (owner && owner == impl->target) {
            auto const& parts = *mesh.mSkinnedParts;
            if (parts.size() > local.size()) { overflow.resize(parts.size()); local = overflow; }
            if (resolveOwnedPalette(parts, local, [&](std::string const& name, BoneVisual& output) {
                auto binding = state->poseBinding(name);
                if (!binding) return false;
                output = binding->visual ? *binding->visual : BoneVisual{};
                output.cutout = state->forceCutout || binding->oneSided;
                return true;
            })) {
                ++state->visualDrawRecovered;
                return local.first(parts.size());
            }
        }
        ++state->visualDrawMisses;
        if (state->visualMissSamples.size() < 4 && std::any_of(mesh.mSkinnedParts->begin(), mesh.mSkinnedParts->end(), [](auto const* part) {
            return part && part->mName->getString().starts_with("__ysm_");
        })) {
            Json names = Json::array();
            for (auto const* part : *mesh.mSkinnedParts) if (part) {
                auto name = part->mName->getString();
                names.push_back(name);
            }
            auto sample = Json{{"parts", names}, {"controller", controller ? controller->mName->getString() : ""},
                {"registered", prepared != state->visualMeshes.end()}};
            if (std::find(state->visualMissSamples.begin(), state->visualMissSamples.end(), sample) == state->visualMissSamples.end())
                state->visualMissSamples.push_back(std::move(sample));
        }
        return {};
    }
    ++state->visualDrawMatches;
    auto const& parts = *mesh.mSkinnedParts;
    if (parts.size() > local.size()) {
        overflow.resize(parts.size());
        local = overflow;
    }
    auto result = local.first(parts.size());
    auto& cached = prepared->second.parts;
    cached.resize(parts.size());
    for (std::size_t i = 0; i < parts.size(); ++i) {
        // A mesh can replace or reorder its palette without changing its size.
        // Only the binding is cached; the actor's visual value is read on every draw.
        auto hash = parts[i] ? parts[i]->mName->getHash() : 0;
        if (cached[i].part != parts[i] || cached[i].nameHash != hash) {
            cached[i] = {parts[i], parts[i] ? state->poseBinding(parts[i]->mName->getString()) : nullptr, static_cast<uint64_t>(hash)};
        }
        auto binding = cached[i].binding;
        result[i] = binding && binding->visual ? *binding->visual : BoneVisual{};
        result[i].cutout = state->forceCutout || (binding && binding->oneSided);
    }
    return result;
}
bool AnimationRuntime::needsIndividualSurfaces(Actor const& actor) const {
    auto state = impl->state;
    return state && state->active && state->visualMaterials && impl->target == &actor;
}
bool AnimationRuntime::rendersPlayer(Actor const& actor) const {
    auto state = impl->state;
    return state && state->active && impl->target == &actor;
}
unsigned AnimationRuntime::handDrawMask(Actor const& actor, bool offhand, bool nativeUi) const {
    auto state = impl->state;
    return state && state->active && impl->target == &actor ? state->visibleHands.get(nativeUi || (UiRenderContextTracker::isActive() && !state->inGame), offhand) : 1u;
}
bool AnimationRuntime::drivesLanceCharge(Actor const& actor) const {
    return rendersPlayer(actor) && impl->state->ysmLanceCharge.load();
}
std::optional<glm::mat4> AnimationRuntime::shoulderTransform(bool left, bool inventory) const {
    auto state = impl->state;
    if (!state) return {};
    std::scoped_lock lock(state->frameMutex);
    // The HUD paper doll can also use an orthographic camera while gameplay
    // remains active. In that pass the player's own pose is the world pose.
    auto locator = inventory && !state->inGame ? state->inventoryShoulders[left ? 0 : 1]
                                               : shoulderLocator(state->evaluation.expressions, left);
    if (locator) {
        auto size = glm::vec3(state->width, state->height, state->width);
        *locator = glm::scale(glm::mat4(1), size) * *locator;
    }
    return locator;
}
bool AnimationRuntime::suppressNativeGlideRotation(Actor const& actor, bool firstPerson,
    std::array<float, 2> const& rotation) {
    auto state = impl->state;
    if (!state || !state->active || impl->target != &actor || firstPerson || !actor.isGliding()) return false;
    // YSM's GeoReplacedEntityRenderer extends LivingEntityRenderer, whose
    // model transform has no PlayerRenderer flight pitch. Authored Root bones
    // already provide that tilt. Keep the Bedrock player state unchanged and
    // suppress only this replacement's additional renderer-space rotation.
    state->nativeGlideX = rotation[0];
    state->nativeGlideY = rotation[1];
    ++state->suppressedGlideRotations;
    return true;
}
}
