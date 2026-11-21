#include "NativeRenderBindings.h"
#include "HandSlotBinding.h"
#include "mc/world/actor/RenderParams.h"
#include "mc/client/model/models/DataDrivenModel.h"
#include "mc/client/model/models/DataDrivenGeometry.h"
#include "mc/client/model/GeometryInfo.h"
#include "mc/client/model/geom/ModelPart.h"
#include "mc/client/renderer/actor/ActorResourceDefinition.h"
#include "mc/client/renderer/actor/ActorTextureInfo.h"
#include "mc/client/renderer/controller/RenderController.h"
#include "mc/client/renderer/controller/RenderControllerToProcess.h"
#include "mc/client/world/actor/animation/ClientAnimationComponent.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/world/actor/animation/BoneOrientation.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include "mc/deps/json/Value.h"
#include <algorithm>

ExpressionNode::ExpressionNode(ExpressionNode const& source) : mImpl(std::in_place_type<float>, 0.0f) {
    if (auto constant = std::get_if<float>(&*source.mImpl)) mImpl->emplace<float>(*constant);
    else if (auto const& expression = std::get<std::unique_ptr<IComplexExpression>>(*source.mImpl))
        mImpl->emplace<std::unique_ptr<IComplexExpression>>(expression->clone());
}

namespace ysm {
static thread_local unsigned firstPersonDepth = 0;
FirstPersonRenderScope::FirstPersonRenderScope() { ++firstPersonDepth; }
FirstPersonRenderScope::~FirstPersonRenderScope() { --firstPersonDepth; }
bool FirstPersonRenderScope::isActive() { return firstPersonDepth != 0; }
unsigned FirstPersonRenderScope::depth() { return firstPersonDepth; }
static thread_local std::set<void const*> uiContexts;
void UiRenderContextTracker::enter(void const* context) { uiContexts.insert(context); }
void UiRenderContextTracker::leave(void const* context) { uiContexts.erase(context); }
bool UiRenderContextTracker::isActive() { return !uiContexts.empty(); }
struct HeldItemBoneScope::Impl {
    struct Entry { std::size_t index; glm::mat4 original, replacement; };
    struct BoneEntry { SkeletalHierarchyIndex hierarchy; std::size_t index; glm::mat4 original, replacement; };
    ClientAnimationComponent* component;
    std::vector<Entry> entries;
    std::vector<BoneEntry> bones;
};
// Resolve against the actual live entries. The client can supply a map
// whose iterated hierarchy is present but is not found by our DLL's find().
static auto findHierarchy(AnimationComponent& component, SkeletalHierarchyIndex index) {
    auto& hierarchies = *component.mBoneOrientations;
    return std::find_if(hierarchies.begin(), hierarchies.end(), [index](auto const& h) { return h.first == index; });
}
HeldItemBoneScope::HeldItemBoneScope(RenderParams& params, std::string_view hand, unsigned copy) {
    if (copy == 0 && !FirstPersonRenderScope::isActive()) return;
    auto replacementName = [copy](char const* name) {
        return copy ? std::string("__ysm_") + name + std::to_string(copy) : std::string("__ysm_fp_") + name;
    };
    auto* component = params.mRootAnimationComponent ? params.mRootAnimationComponent : params.mAnimationComponent;
    auto* client = component ? component->tryGetClient() : nullptr;
    if (!client) return;
    auto pending = std::make_unique<Impl>();
    pending->component = client;
    for (auto& [hierarchy, bones] : *component->mBoneOrientations) {
        for (auto const* name : {"rightitem", "leftitem"}) {
            if (copy && hand != name) continue;
            auto destination = replacementName(name);
            auto item = std::find_if(bones.begin(), bones.end(), [&](auto const& bone) { return bone.mName->getString() == name; });
            auto wrist = std::find_if(bones.begin(), bones.end(), [&](auto const& bone) { return bone.mName->getString() == destination; });
            if (item == bones.end() || wrist == bones.end()) continue;
            pending->bones.push_back({hierarchy, static_cast<std::size_t>(item - bones.begin()), *item->mMatrix->_m, *wrist->mMatrix->_m});
            *item->mMatrix->_m = *wrist->mMatrix->_m;
        }
    }
    auto& matrices = *client->mQueryableBoneOrientations;
    for (auto const* name : {"rightitem", "leftitem"}) {
        if (copy && hand != name) continue;
        auto* item = component->getQueryableBoneOrientation(HashedString::computeHash(name));
        auto* wrist = component->getQueryableBoneOrientation(HashedString::computeHash(replacementName(name)));
        if (!item || !wrist) continue;
        auto found = std::find_if(matrices.begin(), matrices.end(), [&](auto const& matrix) { return &matrix == item; });
        if (found == matrices.end()) continue;
        pending->entries.push_back({static_cast<std::size_t>(found - matrices.begin()), *found->_m, *wrist->_m});
        *found->_m = *wrist->_m;
    }
    if (!pending->bones.empty() || !pending->entries.empty()) impl = std::move(pending);
}
bool HeldItemBoneScope::bound() const { return static_cast<bool>(impl); }
HeldItemBoneScope::~HeldItemBoneScope() {
    if (!impl) return;
    auto& matrices = *impl->component->mQueryableBoneOrientations;
    for (auto const& entry : impl->entries)
        if (entry.index < matrices.size() && *matrices[entry.index]._m == entry.replacement)
            *matrices[entry.index]._m = entry.original;
    for (auto const& entry : impl->bones) {
        auto found = findHierarchy(*impl->component, entry.hierarchy);
        if (found != impl->component->mBoneOrientations->end() && entry.index < found->second.size()) {
            auto& bone = found->second[entry.index];
            if (*bone.mMatrix->_m == entry.replacement) *bone.mMatrix->_m = entry.original;
        }
    }
}
namespace {
struct AttachableDraw {
    Actor const* actor;
    AttachableSlotIndex slot;
    unsigned copy;
    bool lanceCharge;
    struct Roll {
        std::shared_ptr<AnimationComponent> child;
        SkeletalHierarchyIndex hierarchy;
        std::size_t index;
        float degrees;
    };
    std::vector<Roll> rolls;
    void suppressRoll(std::shared_ptr<AnimationComponent> const& child, SkeletalHierarchyIndex hierarchy,
                      std::size_t index, float& degrees) {
        if (!degrees) return;
        auto old = std::ranges::find_if(rolls, [&](auto const& r) {
            return r.child == child && r.hierarchy == hierarchy && r.index == index;
        });
        if (old == rolls.end()) rolls.push_back({child, hierarchy, index, degrees});
        else old->degrees = degrees; // Native setup may have re-evaluated the channel.
        degrees = 0;
    }
    ~AttachableDraw() {
        for (auto const& roll : rolls) {
            auto found = findHierarchy(*roll.child, roll.hierarchy);
            if (found == roll.child->mBoneOrientations->end() || roll.index >= found->second.size()) continue;
            auto& bone = found->second[roll.index];
            if (bone.mName->getString() == "spear" && !bone.mTransform->mData[1]->z)
                bone.mTransform->mData[1]->z = roll.degrees;
        }
    }
};
thread_local AttachableDraw* drawingGrip = nullptr;
}
void bindAttachableParent(RenderParams& params, AttachableSlotIndex slot, ItemStack const& item, Actor const& actor) {
    auto* active = drawingGrip && drawingGrip->actor == &actor && drawingGrip->slot == slot ? drawingGrip : nullptr;
    auto copy = active ? active->copy : 0u;
    bool hand = slot == AttachableSlotIndex::CarriedItem || slot == AttachableSlotIndex::OffhandItem;
    bool elytra = (slot == AttachableSlotIndex::Torso || slot == AttachableSlotIndex::Body)
        && !FirstPersonRenderScope::isActive() && item.mItem && item.mItem->mFullName->getString() == "minecraft:elytra";
    if (!hand && !elytra) return;
    auto* component = params.mRootAnimationComponent ? params.mRootAnimationComponent : params.mAnimationComponent;
    auto index = static_cast<std::size_t>(slot);
    if (!component || index >= component->mChildAnimationComponents->size()) return;
    auto child = (*component->mChildAnimationComponents)[index].mChildAnimationComponent;
    if (!child) return;
    bool vanillaLance = false;
    if (active && active->lanceCharge) {
        auto model = child->mRenderParams->mModel.lock();
        vanillaLance = model && std::ranges::any_of(*model->mGeometries, [](auto const& geometry) {
            return geometry && geometry->mSourceGeometry->mGeometryInfoPtr
                && geometry->mSourceGeometry->mGeometryInfoPtr->mName->getString() == "geometry.spear";
        });
    }
    for (auto const& [parentHierarchy, bones] : *component->mBoneOrientations) {
        if (!std::any_of(bones.begin(), bones.end(), [](auto const& bone) {
            return bone.mName->getString().starts_with("__ysm_body_");
        })) continue;
        for (auto& [hierarchy, attached] : *child->mBoneOrientations) for (auto& bone : attached) {
            auto name = bone.mName->getString();
            bool expression = bone.mBoneBindingMethod == BoneBindingMethod::BindingExpression;
            if (hand && expression) {
                if (!usesEquipmentSlotBone(bone.mBoneBindingExpression->getExpressionString())) continue;
                name = slot == AttachableSlotIndex::OffhandItem ? "leftitem" : "rightitem";
            } else {
                if (elytra ? name != "body" : name != "rightitem" && name != "leftitem") continue;
                if (bone.mBoneBindingMethod != BoneBindingMethod::MatchingParentBoneName && !(elytra && expression)) continue;
            }
            auto target = elytra ? "__ysm_elytra" : FirstPersonRenderScope::isActive() ? "__ysm_fp_" + name : name;
            if (copy && name == (slot == AttachableSlotIndex::OffhandItem ? "leftitem" : "rightitem"))
                target = "__ysm_" + name + std::to_string(copy);
            auto found = std::find_if(bones.begin(), bones.end(), [&](auto const& entry) { return entry.mName->getString() == target; });
            if (found == bones.end()) continue;
            // Native attachables bind directly to parent bone matrices; the
            // query cache is independent of this parent skeleton binding.
            auto targetIndex = static_cast<int>(found - bones.begin());
            if (hand && expression) {
                auto const& channels = bone.mTransform->mData;
                auto zero = [](Vec3 const& v) { return v.x == 0 && v.y == 0 && v.z == 0; };
                auto const& pre = bone.mLocalPreTransformMatrix;
                bool plain = !pre || *pre->_m == glm::mat4(1);
                if (zero(*bone.mPivot) && plain && !bone.mOverrideStackMatrix
                    && bone.mRotationRelativeMode == BoneAnimationRelativeMode::Parent) {
                    // A hidden parent has already erased the child's world
                    // axes. The original local transform still survives.
                    auto vector = [](Vec3 const& v) { return glm::vec3(v.x, v.y, v.z); };
                    auto& rotation = *bone.mTransform->mData[1];
                    bool charge = vanillaLance && bone.mName->getString() == "spear";
                    if (usesAnimatedLanceRoll(charge, vector(rotation)))
                        active->suppressRoll(child, hierarchy, static_cast<std::size_t>(&bone - attached.data()), rotation.z);
                    *bone.mMatrix->_m = slotRootAtGrip(*found->mMatrix->_m, vector(*channels[0]),
                        vector(rotation), vector(*channels[2]));
                } else {
                    int oldIndex = bone.mParentBoneIndex;
                    auto old = findHierarchy(*component, bone.mParentSkeletalHierarchyIndex);
                    glm::mat4 const* previous = nullptr;
                    if (old != component->mBoneOrientations->end() && oldIndex >= 0
                        && static_cast<std::size_t>(oldIndex) < old->second.size())
                        previous = &*old->second[oldIndex].mMatrix->_m;
                    bool identity = zero(*channels[0]) && zero(*channels[1]) && channels[2]->x == 1
                        && channels[2]->y == 1 && channels[2]->z == 1 && plain && !bone.mOverrideStackMatrix;
                    auto resolved = retargetSlotBone(*bone.mMatrix->_m, previous, *found->mMatrix->_m,
                        identity, bone.mParentSkeletalHierarchyIndex == parentHierarchy && oldIndex == targetIndex);
                    if (!resolved) continue;
                    *bone.mMatrix->_m = *resolved;
                }
            } else *bone.mMatrix->_m = *found->mMatrix->_m;
            bone.mParentBoneIndex = targetIndex;
            bone.mParentSkeletalHierarchyIndex = parentHierarchy;
            child->mNeedToUpdateQueryableBoneOrientations = true;
        }
    }
}
void drawAtAttachableGrip(RenderParams& params, AttachableSlotIndex slot, ItemStack const& item, Actor const& actor,
    unsigned copy, bool lanceCharge, std::function<void()> const& draw) {
    // Keep the chosen grip and roll through nested native setup as well as
    // mesh submission. Native render passes can rebuild the child matrix.
    struct Restore {
        AttachableDraw* previous;
        ~Restore() { drawingGrip = previous; }
    } restore{drawingGrip};
    AttachableDraw pass{&actor, slot, copy, lanceCharge, {}};
    drawingGrip = &pass;
    bindAttachableParent(params, slot, item, actor);
    draw();
}
static void prepareParts(RenderController& controller, std::map<std::string, std::string> const& armParts) {
    if (controller.mName->getString().find(".first_person") == std::string::npos || armParts.empty()) return;
    auto& visibility = *controller.mPartVisibility;
    if (std::any_of(visibility.begin(), visibility.end(), [](auto const& entry) { return entry.first.starts_with("__ysm_fp_"); })) return;
    std::map<std::string, std::unique_ptr<ExpressionNode>> conditions;
    for (auto const& [name, expression] : visibility)
        if (name == "rightarm" || name == "leftarm") conditions[name] = std::make_unique<ExpressionNode>(expression);
    for (auto const& [bone, carrier] : armParts) {
        if (auto found = conditions.find(carrier); found != conditions.end()) visibility.emplace_back(bone, *found->second);
        else if (carrier == "__ysm_fp_background") {
            ExpressionNode visible;
            visible.mImpl->emplace<float>(1.0f);
            visibility.emplace_back(bone, visible);
        }
    }
    controller.mShouldRebuildAnimationMatrices = true;
}
class ResolvedNativeExpression final : public Molang::details::IComplexExpression {
    MolangScriptArg value;
public:
    ResolvedNativeExpression(MolangScriptArg const& source, NativeTextureSet const& textures = {})
    : value(source) {
        if (!textures.color) return;
        auto& target = std::get<MolangClientTextureSet>(value.mData);
        target.mColorTextureDataWeakPtr = textures.color;
        target.mPBRData.reset();
        if (textures.normal || textures.material) {
            auto extra = std::make_unique<MolangClientTextureSet::ExtraPBRData>();
            extra->mNormalTextureDataWeakPtr = textures.normal;
            extra->mMERSTextureDataWeakPtr = textures.material;
            extra->mNormalTextureMode = NormalTextureMode::NormalMap;
            extra->mMERSTextureMode = textures.hasSubsurfaceChannel ? MERSTextureMode::Mers : MERSTextureMode::Mer;
            target.mPBRData = std::move(extra);
        }
    }
    std::unique_ptr<IComplexExpression> clone() const override { return std::make_unique<ResolvedNativeExpression>(*this); }
    MolangScriptArg const& evalGeneric(RenderParams&) const override { return value; }
    bool isInitialized() const override { return true; }
    ::Json::Value toJson() const override { return {}; }
    std::string getExpressionString() const override { return value.mType == MolangScriptArgType::Float ? std::to_string(value.mPOD.mFloat) : "texture.ysm_bedrock.original_resolution"; }
    MolangVersion getMolangVersion() const override { return MolangVersion::Latest; }
    void replaceArrayVariables(std::unordered_map<HashedString, Molang::details::ExpressionNode>&) override {}
    void validateArrayVariables() const override {}
    bool isValid() const override { return true; }
    bool isConstantValue() const override { return true; }
    bool hasVariableAssignments() const override { return false; }
    ExpressionOp getOp() const override { return value.mType == MolangScriptArgType::Float ? ExpressionOp::Float : ExpressionOp::Texture; }
    bool needsToCompile() const override { return false; }
    std::optional<MolangScriptArg> getValueIfConstant() const override { return value; }
    Molang::details::SourceTree* getSource() override { return nullptr; }
};
struct NativeRenderBindings::Impl {
    struct ArmorPolicyBinding {
        std::weak_ptr<DataDrivenModel> model;
        bool original;
    };
    struct TextureBinding {
        std::weak_ptr<RenderController> controller;
        std::size_t index;
        std::unique_ptr<ExpressionNode> original;
        Molang::details::IComplexExpression* replacement;
    };
    struct ResourceBinding {
        std::weak_ptr<ActorResourceDefinition> definition;
        std::shared_ptr<BedrockTextureData const> original, replacement;
    };
    struct VisibilityBinding {
        std::weak_ptr<DataDrivenGeometry> geometry;
        RenderController const* controller;
        std::size_t index;
        std::unique_ptr<ExpressionNode> original;
        Molang::details::IComplexExpression* replacement;
        bool visible;
    };
    std::vector<TextureBinding> textures;
    std::vector<ArmorPolicyBinding> armorPolicies;
    std::vector<ResourceBinding> resources;
    std::vector<std::weak_ptr<RenderController>> parts;
    std::vector<std::weak_ptr<DataDrivenModel>> models;
    std::vector<VisibilityBinding> visibility;
    std::vector<VisibilityBinding> deferredVisibility;
    Json originalTextureSizes = Json::array();
    NativeTextureSet boundTextures;
    void restoreTextureBindings() {
        for (auto& binding : textures) if (auto controller = binding.controller.lock()) {
            if (binding.index >= controller->mTextures->size()) continue;
            auto& expression = (*controller->mTextures)[binding.index];
            auto current = std::get_if<std::unique_ptr<Molang::details::IComplexExpression>>(&*expression.mImpl);
            if (current && current->get() == binding.replacement) *expression.mImpl = std::move(*binding.original->mImpl);
        }
        for (auto& binding : resources) if (auto definition = binding.definition.lock()) {
            auto found = definition->mTextureMap->find("default");
            if (found != definition->mTextureMap->end()) {
                auto& texture = found->second.mTexturePtrs->mColorTexture->mClientTexture;
                if (texture == binding.replacement) texture = binding.original;
            }
        }
        textures.clear();
        resources.clear();
        models.clear();
        originalTextureSizes.clear();
        boundTextures = {};
    }
    void restoreDeferredVisibility(RenderController const* controller = nullptr, DataDrivenGeometry const* selected = nullptr) {
        std::erase_if(deferredVisibility, [&](auto& binding) {
            auto geometry = binding.geometry.lock();
            if (geometry && controller && (binding.controller != controller || geometry.get() != selected)) return false;
            if (geometry && binding.index < geometry->mModelParts->size()) {
                auto& part = (*geometry->mModelParts)[binding.index];
                auto found = part.mIsVisibleMap->find(binding.controller);
                if (found != part.mIsVisibleMap->end()) {
                    auto& expression = (*found).second;
                    auto current = std::get_if<std::unique_ptr<Molang::details::IComplexExpression>>(&*expression.mImpl);
                    if (current && current->get() == binding.replacement)
                        *expression.mImpl = std::move(*binding.original->mImpl);
                }
            }
            return true;
        });
    }
};
NativeRenderBindings::NativeRenderBindings() : impl(std::make_unique<Impl>()) {}
NativeRenderBindings::~NativeRenderBindings() { reset(); }
std::size_t NativeRenderBindings::prepare(RenderController& controller, std::shared_ptr<DataDrivenGeometry> const& geometry,
    std::map<std::string, std::string> const& armParts, bool drawEnabled) {
    impl->restoreDeferredVisibility(&controller, geometry.get());
    prepareParts(controller, armParts);
    if (controller.mName->getString().find(".first_person") != std::string::npos)
      for (std::size_t index = 0; index < geometry->mModelParts->size(); ++index) {
        auto& part = (*geometry->mModelParts)[index];
        auto name = part.mName->getString();
        auto selected = armParts.find(name);
        if (selected == armParts.end()) continue;
        auto found = part.mIsVisibleMap->find(&controller);
        if (found == part.mIsVisibleMap->end()) continue;
        auto& target = (*found).second;
        auto current = std::get_if<std::unique_ptr<Molang::details::IComplexExpression>>(&*target.mImpl);
        bool prepared = std::any_of(impl->visibility.begin(), impl->visibility.end(), [&](auto const& binding) {
            return binding.geometry.lock() == geometry && binding.index == index && binding.controller == &controller
                && current && current->get() == binding.replacement;
        });
        if (prepared) continue;
        auto source = std::find_if(controller.mPartVisibility->begin(), controller.mPartVisibility->end(),
            [&](auto const& entry) { return entry.first == selected->second; });
        if (source == controller.mPartVisibility->end()) continue;
        std::unique_ptr<Molang::details::IComplexExpression> replacement;
        if (auto constant = std::get_if<float>(&*source->second.mImpl))
            replacement = std::make_unique<ResolvedNativeExpression>(MolangScriptArg(*constant));
        else if (auto const& expression = std::get<std::unique_ptr<Molang::details::IComplexExpression>>(*source->second.mImpl))
            replacement = expression->clone();
        if (!replacement) continue;
        auto original = std::make_unique<ExpressionNode>(target);
        auto identity = replacement.get();
        target.mImpl->emplace<std::unique_ptr<Molang::details::IComplexExpression>>(std::move(replacement));
        impl->visibility.push_back({geometry, &controller, index, std::move(original), identity, part.mVisible});
        part.mVisible = true;
    }
    std::size_t hidden = 0;
    if (!drawEnabled) for (std::size_t index = 0; index < geometry->mModelParts->size(); ++index) {
        auto& part = (*geometry->mModelParts)[index];
        auto found = part.mIsVisibleMap->find(&controller);
        if (found == part.mIsVisibleMap->end()) continue;
        auto& expression = (*found).second;
        auto original = std::make_unique<ExpressionNode>(expression);
        auto replacement = std::make_unique<ResolvedNativeExpression>(MolangScriptArg(0.0f));
        auto identity = replacement.get();
        expression.mImpl->emplace<std::unique_ptr<Molang::details::IComplexExpression>>(std::move(replacement));
        impl->deferredVisibility.push_back({geometry, &controller, index, std::move(original), identity, part.mVisible});
        ++hidden;
    }
    return hidden;
}
bool NativeRenderBindings::bind(RenderParams& params, NativeTextureSet const& textures,
    std::map<std::string, std::string> const& armParts) {
    auto const& texture = textures.color;
    auto model = params.mModel.lock();
    if (!model) return false;
    bool ours = std::any_of(model->mGeometries->begin(), model->mGeometries->end(), [](auto const& geometry) {
        return geometry && geometry->mSourceGeometry->mGeometryInfoPtr
            && geometry->mSourceGeometry->mGeometryInfoPtr->mName->getString().starts_with("geometry.ysm_bedrock.");
    });
    if (!ours) return false;
    if (std::none_of(impl->armorPolicies.begin(), impl->armorPolicies.end(), [&](auto const& binding) { return binding.model.lock() == model; }))
        impl->armorPolicies.push_back({model, static_cast<bool>(model->mHideArmor)});
    model->mHideArmor = true;
    // Skin refreshes can replace GPU resources while Bedrock retains the model.
    if (textures != impl->boundTextures) {
        impl->restoreTextureBindings();
        impl->boundTextures = textures;
    }
    bool prepared = std::any_of(impl->models.begin(), impl->models.end(), [&](auto const& weak) { return weak.lock() == model; });
    std::shared_ptr<BedrockTextureData const> previousDefault;
    if (!prepared && texture) if (auto definition = model->mResourceDefinition.lock()) {
        auto found = definition->mTextureMap->find("default");
        if (found != definition->mTextureMap->end()) {
            auto& target = found->second.mTexturePtrs->mColorTexture->mClientTexture;
            previousDefault = target;
            if (target) impl->originalTextureSizes.push_back({target->mTextureDescription->mWidth, target->mTextureDescription->mHeight});
            if (target != texture) {
                impl->resources.push_back({definition, target, texture});
                target = texture;
            }
        }
    }
    if (!prepared) for (auto const& [controller, condition] : *model->mRenderControllers) {
        if (!controller) continue;
        if (texture) for (std::size_t index = 0; index < controller->mTextures->size(); ++index) {
            auto& expression = (*controller->mTextures)[index];
            auto const& value = expression.evalGeneric(params);
            if (value.mType != MolangScriptArgType::Variant || !std::holds_alternative<MolangClientTextureSet>(value.mData)) continue;
            auto const& clientTexture = std::get<MolangClientTextureSet>(value.mData);
            auto name = clientTexture.mName->getString();
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            auto previous = clientTexture.mColorTextureDataWeakPtr.lock();
            if (name != "default" && name != "texture.default" && (!previousDefault || previous != previousDefault)) continue;
            if (previous == texture && !textures.normal && !textures.material && !clientTexture.mPBRData) continue;
            auto original = std::make_unique<ExpressionNode>(expression);
            auto replacement = std::make_unique<ResolvedNativeExpression>(value, textures);
            auto identity = replacement.get();
            expression.mImpl->emplace<std::unique_ptr<Molang::details::IComplexExpression>>(std::move(replacement));
            impl->textures.push_back({controller, index, std::move(original), identity});
        }
        if (controller->mName->getString().find(".first_person") == std::string::npos || armParts.empty()) continue;
        prepareParts(*controller, armParts);
        if (std::none_of(impl->parts.begin(), impl->parts.end(), [&](auto const& part) { return part.lock() == controller; }))
            impl->parts.push_back(controller);
    }
    if (!prepared && texture) impl->models.push_back(model);
    return firstPersonDepth != 0;
}
Json NativeRenderBindings::report(RenderParams& params) const {
    Json result = {{"hand_visibility_bindings", impl->visibility.size()}, {"original_texture_sizes", impl->originalTextureSizes}};
    auto* root = params.mRootAnimationComponent ? params.mRootAnimationComponent : params.mAnimationComponent;
    result["root_is_current_component"] = root == params.mAnimationComponent;
    auto describeSkeletons = [](AnimationComponent* component) {
        Json result = Json::array();
        if (!component) return result;
        for (auto const& [hierarchy, bones] : *component->mBoneOrientations) {
            Json skeleton = {{"hierarchy", static_cast<int>(hierarchy)}, {"bones", bones.size()}, {"item_bones", Json::array()}};
            for (auto const& bone : bones) {
                auto const& name = bone.mName->getString();
                if (name != "rightitem" && name != "leftitem" && !name.starts_with("__ysm_fp_")) continue;
                if (name.starts_with("__ysm_fp_") && name != "__ysm_fp_rightitem" && name != "__ysm_fp_leftitem") continue;
                auto const& matrix = *bone.mMatrix->_m;
                skeleton["item_bones"].push_back({{"bone", name}, {"parent_index", static_cast<int>(bone.mParentBoneIndex)},
                    {"parent_hierarchy", static_cast<int>(bone.mParentSkeletalHierarchyIndex)},
                    {"translation", {matrix[3][0], matrix[3][1], matrix[3][2]}}});
            }
            result.push_back(std::move(skeleton));
        }
        return result;
    };
    result["root_skeletons"] = describeSkeletons(root);
    result["current_skeletons"] = describeSkeletons(params.mAnimationComponent);
    result["attachable_children"] = Json::array();
    if (root) {
        auto const& children = *root->mChildAnimationComponents;
        for (std::size_t index = 0; index < children.size() && index < 64; ++index) {
            Json entry = {{"index", index}};
            auto child = children[index].mChildAnimationComponent;
            entry["present"] = static_cast<bool>(child);
            if (child) {
                entry["slot"] = static_cast<int>(child->mRenderParams->mSlotIndex);
                if (auto model = child->mRenderParams->mModel.lock()) entry["model"] = model->mName->getString();
                entry["bindings"] = Json::array();
                for (auto const& [hierarchy, bones] : *child->mBoneOrientations) for (auto const& bone : bones) {
                    bool expression = bone.mBoneBindingMethod == BoneBindingMethod::BindingExpression;
                    if (!expression && bone.mBoneBindingMethod != BoneBindingMethod::MatchingParentBoneName) continue;
                    auto parentHierarchy = bone.mParentSkeletalHierarchyIndex;
                    auto parentIndex = static_cast<int>(bone.mParentBoneIndex);
                    Json binding = {{"bone", bone.mName->getString()}, {"hierarchy", static_cast<int>(hierarchy)},
                        {"parent_hierarchy", static_cast<int>(parentHierarchy)}, {"parent_index", parentIndex}};
                    if (expression) binding["expression"] = bone.mBoneBindingExpression->getExpressionString();
                    auto found = findHierarchy(*root, parentHierarchy);
                    if (found != root->mBoneOrientations->end() && parentIndex >= 0 && static_cast<std::size_t>(parentIndex) < found->second.size())
                        binding["parent_name"] = found->second[parentIndex].mName->getString();
                    auto const& matrix = *bone.mMatrix->_m;
                    binding["translation"] = {matrix[3][0], matrix[3][1], matrix[3][2]};
                    if (expression) {
                        binding["axes"] = Json::array();
                        for (int axis = 0; axis < 3; ++axis)
                            binding["axes"].push_back({matrix[axis][0], matrix[axis][1], matrix[axis][2]});
                    }
                    entry["bindings"].push_back(std::move(binding));
                }
            }
            result["attachable_children"].push_back(std::move(entry));
        }
    }
    if (auto model = params.mModel.lock()) {
        result["queryable_geometry_index"] = static_cast<uint64>(model->mQueryableGeometryIndex);
        result["geometry_list"] = Json::array();
        for (auto const& geometry : *model->mGeometries)
            result["geometry_list"].push_back(geometry && geometry->mSourceGeometry->mGeometryInfoPtr
                ? geometry->mSourceGeometry->mGeometryInfoPtr->mName->getString() : "");
        if (params.mAnimationComponent) if (auto client = params.mAnimationComponent->tryGetClient())
            result["query_cache_count"] = client->mQueryableBoneOrientations->size();
        result["enable_attachables"] = static_cast<bool>(model->mEnableAttachables);
        result["hide_armor"] = static_cast<bool>(model->mHideArmor);
        if (auto definition = model->mResourceDefinition.lock()) {
            result["queryable_geometry_name"] = definition->mQueryableGeometryName->getString();
            result["resource_enable_attachables"] = static_cast<bool>(definition->mEnableAttachables);
            result["resource_hide_armor"] = static_cast<bool>(definition->mHideArmor);
            result["textures"] = Json::object();
            for (auto const& [name, info] : *definition->mTextureMap) {
                auto const& texture = info.mTexturePtrs->mColorTexture->mClientTexture;
                if (texture) result["textures"][name] = {texture->mTextureDescription->mWidth, texture->mTextureDescription->mHeight};
                if (name == "default") {
                    result["default_texture_location"] = info.mResourceLocations->mColorLocation->mPath->value;
                    if (auto location = info.mTexturePtrs->mColorTexture->mResourceLocationPtr)
                        result["default_texture_pointer_location"] = location->mPath->value;
                }
            }
        }
        result["geometry_uv_dimensions"] = Json::object();
        for (auto const& geometry : *model->mGeometries) if (geometry)
            for (auto const& part : *geometry->mModelParts) if (!part.mCubes->empty()) {
                result["geometry_uv_dimensions"][part.mName->getString()] = {part.mTexSize->x, part.mTexSize->y};
                if (part.mName->getString().starts_with("__ysm_body_")) {
                    auto faces = Json::array();
                    for (auto const& face : *part.mCubes->front().mFaceData)
                        faces.push_back({{"uv", {face.mUV->x, face.mUV->y}}, {"size", {face.mUVSize->x, face.mUVSize->y}}, {"valid", static_cast<bool>(face.mFaceValid)}});
                    result["first_body_cube"] = {{"bone", part.mName->getString()}, {"faces", faces}};
                }
                break;
            }
        result["controller_textures"] = Json::object();
        result["controller_pbr"] = Json::object();
        for (auto const& [controller, condition] : *model->mRenderControllers) if (controller) {
            Json textures = Json::array();
            for (auto const& expression : *controller->mTextures) {
                auto const& value = expression.evalGeneric(params);
                if (value.mType != MolangScriptArgType::Variant || !std::holds_alternative<MolangClientTextureSet>(value.mData)) continue;
                auto const& set = std::get<MolangClientTextureSet>(value.mData);
                if (auto texture = set.mColorTextureDataWeakPtr.lock())
                    textures.push_back({texture->mTextureDescription->mWidth, texture->mTextureDescription->mHeight});
                if (auto const& extra = set.mPBRData) {
                    auto dimensions = [](auto const& weak) -> Json {
                        if (auto data = weak.lock()) return {data->mTextureDescription->mWidth, data->mTextureDescription->mHeight};
                        return nullptr;
                    };
                    result["controller_pbr"][controller->mName->getString()].push_back({
                        {"normal", dimensions(extra->mNormalTextureDataWeakPtr)}, {"mers", dimensions(extra->mMERSTextureDataWeakPtr)},
                        {"material_layout", extra->mMERSTextureMode == MERSTextureMode::Mers ? "MERS" : "MER"}});
                }
            }
            result["controller_textures"][controller->mName->getString()] = textures;
        }
    }
    return result;
}
void NativeRenderBindings::reset() {
    for (auto const& binding : impl->armorPolicies) if (auto model = binding.model.lock())
        if (model->mHideArmor) model->mHideArmor = binding.original;
    impl->restoreDeferredVisibility();
    for (auto& binding : impl->visibility) if (auto geometry = binding.geometry.lock()) {
        if (binding.index >= geometry->mModelParts->size()) continue;
        auto& part = (*geometry->mModelParts)[binding.index];
        auto found = part.mIsVisibleMap->find(binding.controller);
        if (found == part.mIsVisibleMap->end()) continue;
        auto& expression = (*found).second;
        auto current = std::get_if<std::unique_ptr<Molang::details::IComplexExpression>>(&*expression.mImpl);
        if (current && current->get() == binding.replacement) {
            *expression.mImpl = std::move(*binding.original->mImpl);
            part.mVisible = binding.visible;
        }
    }
    impl->restoreTextureBindings();
    for (auto const& weak : impl->parts) if (auto controller = weak.lock()) {
        auto& entries = *controller->mPartVisibility;
        std::size_t destination = 0;
        for (std::size_t source = 0; source < entries.size(); ++source) {
            if (entries[source].first.starts_with("__ysm_fp_")) continue;
            if (source != destination) {
                entries[destination].first = std::move(entries[source].first);
                *entries[destination].second.mImpl = std::move(*entries[source].second.mImpl);
            }
            ++destination;
        }
        while (entries.size() > destination) entries.pop_back();
    }
    impl->armorPolicies.clear();
    impl->parts.clear();
    impl->visibility.clear();
}
}
