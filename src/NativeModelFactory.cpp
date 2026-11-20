#include "NativeModelFactory.h"
#include "mc/client/model/models/DataDrivenModel.h"
#include "mc/client/model/models/DataDrivenGeometry.h"
#include "mc/client/model/GeometryPtr.h"
#include "mc/client/model/GeometryInfo.h"
#include "mc/client/model/Geometry.h"
#include "mc/client/model/geom/ModelPart.h"
#include "mc/client/renderer/actor/ActorResourceDefinition.h"
#include "mc/client/renderer/actor/DataDrivenRenderer.h"
#include "mc/client/renderer/controller/RenderController.h"
#include "mc/util/NamedMolangScript.h"
#include "mc/world/actor/animation/ActorSkeletalAnimationPtr.h"
#include "mc/world/actor/animation/ActorAnimationControllerPtr.h"
#include <algorithm>
#include <Windows.h>
#include <cstring>
#include <stdexcept>

namespace ysm {
namespace {
constexpr std::uintptr_t modelConstructor = 0x58aef00;
constexpr std::uintptr_t definitionCopy = 0x59fbd30;
constexpr std::uintptr_t definitionDestroy = 0x5962430;
constexpr std::uintptr_t rendererConstructor = 0x59835a0;
constexpr unsigned char rendererSignature[] = {
    0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x48,
    0x01,0x00,0x00,0x48,0x8d,0xac,0x24,0x80,0x00,0x00,0x00,0x48,0xc7,0x85,0xc0,0x00,
    0x00,0x00,0xfe,0xff,0xff,0xff,0x44,0x89,0xce,0x4c,0x89,0x45,0x30,0x49,0x89,0xcd,
    0x48,0x89,0x55,0x50,0x48,0x8b,0x0a,0x48,0x85,0xc9,0x48,0x89,0x8d,0xb0,0x00,0x00};
constexpr unsigned char modelSignature[] = {
    0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0xc8,
    0x05,0x00,0x00,0x48,0x8d,0xac,0x24,0x80,0x00,0x00,0x00,0x44,0x0f,0x29,0x85,0x30,
    0x05,0x00,0x00,0x0f,0x29,0xbd,0x20,0x05,0x00,0x00,0x0f,0x29,0xb5,0x10,0x05,0x00,
    0x00,0x48,0xc7,0x85,0x08,0x05,0x00,0x00,0xfe,0xff,0xff,0xff,0x48,0x89,0x95,0xa8,
    0x03,0x00,0x00,0x48,0x89,0xce,0xe8,0x85,0xe8,0x2c,0x01,0x90,0x48,0x8d,0x05,0x8d,
    0x9b,0x9b,0x09,0x48,0x89,0x06};
constexpr unsigned char copySignature[] = {
    0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x18,
    0x01,0x00,0x00,0x48,0x8d,0xac,0x24,0x80,0x00,0x00,0x00,0x0f,0x29,0xbd,0x80,0x00,
    0x00,0x00,0x0f,0x29,0x75,0x70,0x48,0xc7,0x45,0x68,0xfe,0xff,0xff,0xff,0x48,0x89,
    0xd7,0x48,0x89,0xce,0x8b,0x02,0x89,0x01,0x0f,0x57,0xf6,0x0f,0x11,0x71,0x08,0x48};
constexpr unsigned char destroySignature[] = {
    0x55,0x56,0x57,0x53,0x48,0x83,0xec,0x38,0x48,0x8d,0x6c,0x24,0x30,0x48,0xc7,0x45,
    0x00,0xfe,0xff,0xff,0xff,0x48,0x89,0xce,0x48,0x8b,0x81,0x38,0x03,0x00,0x00};
static_assert(sizeof(DataDrivenModel) == 0x1e0 && sizeof(ActorResourceDefinition) == 0x370);
std::uintptr_t imageBase() { return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"Minecraft.Windows.exe")); }
template<std::size_t N> bool matches(std::uintptr_t base, std::uintptr_t offset, unsigned char const (&bytes)[N]) {
    return std::memcmp(reinterpret_cast<void const*>(base + offset), bytes, N) == 0;
}
}
bool NativeModelFactory::supports(Model const& original) {
    auto base = imageBase();
    if (!base) return false;
    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS64 const*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.TimeDateStamp == 0x6ad79ee4
        && nt->OptionalHeader.SizeOfImage == 0x13847000
        && *reinterpret_cast<std::uintptr_t const*>(&original) == base + 0xf268ae0
        && matches(base, modelConstructor, modelSignature) && matches(base, definitionCopy, copySignature)
        && matches(base, definitionDestroy, destroySignature);
}
NativeModelInstance NativeModelFactory::createTarget(DataDrivenModel const& original,
    std::shared_ptr<GeometryInfo> const& geometry, std::set<std::string> const& bones, std::string const& identity) {
    if (!supports(original)) throw std::runtime_error("Native model factory is unavailable for this client layout");
    auto source = original.mResourceDefinition.lock();
    if (!source || !geometry || !geometry->mPtr || bones.empty() || identity.empty())
        throw std::runtime_error("Incomplete native target model input");
    auto base = imageBase();
    // These exact functions were traced from the native resource shared-owner
    // creation/destruction paths, and guarded against the supported PE above.
    // All containers and reference counts are created by the engine's copy.
    using Copy = ActorResourceDefinition* (*)(void*, ActorResourceDefinition const&);
    void* storage = ::operator new(sizeof(ActorResourceDefinition));
    ActorResourceDefinition* created;
    try { created = reinterpret_cast<Copy>(base + definitionCopy)(storage, *source); }
    catch (...) { ::operator delete(storage); throw; }
    NativeModelInstance result;
    result.definition = std::shared_ptr<ActorResourceDefinition>(created, [base](ActorResourceDefinition* value) {
        reinterpret_cast<void (*)(ActorResourceDefinition*)>(base + definitionDestroy)(value);
        ::operator delete(value);
    });
    auto& definition = *result.definition;
    if (definition.mGeometryMap->empty()) throw std::runtime_error("Native source model has no geometry aliases");
    for (auto& [name, slot] : *definition.mGeometryMap) slot.mGeometryInfoPtr = geometry;
    definition.mName = HashedString(identity);
    // Actor scale/visibility scripts from the player prototype use variables
    // which a boat or projectile never initializes. Targets own these policies.
    definition.mActorScripts->clear();
    // The player supplies surface bindings only. The replacement's own
    // TargetNumericPlayer evaluates its bones and controller transitions.
    definition.mActorAnimateScriptArray->clear();
    for (auto& [name, controller] : *definition.mActorAnimationControllerMap)
        controller.mActorAnimationControllerInfoPtr.reset();
    for (auto& [name, animation] : *definition.mActorAnimationMap)
        animation.mActorAnimationInfoPtr.reset();
    // Native entity scale is applied about the entity origin. Scaling each
    // authored root about its pivot instead would displace multi-root models.
    definition.mActorScripts->try_emplace(HashedString("scale")).first->second.mImpl->emplace<float>(0.7f);
    definition.mEnableAttachables = false;
    definition.mHideArmor = true;
    definition.mQueryableGeometryBoneNames->clear();
    using Construct = DataDrivenModel* (*)(void*, std::weak_ptr<ActorResourceDefinition>);
    storage = ::operator new(sizeof(DataDrivenModel));
    DataDrivenModel* model;
    try { model = reinterpret_cast<Construct>(base + modelConstructor)(storage, std::weak_ptr<ActorResourceDefinition>(result.definition)); }
    catch (...) { ::operator delete(storage); throw; }
    result.model = std::shared_ptr<DataDrivenModel>(model, [owner = result.definition](DataDrivenModel* value) {
        auto table = *reinterpret_cast<std::uintptr_t const* const*>(value);
        reinterpret_cast<void (*)(void*, unsigned)>(table[0])(value, 0);
        ::operator delete(value);
    });
    if (model->mGeometries->empty()) throw std::runtime_error("Native target model did not bake a geometry");
    for (auto const& data : *model->mGeometries) {
        std::set<std::string> names;
        if (data) for (auto const& part : *data->mModelParts) names.insert(part.mName->getString());
        if (names != bones) throw std::runtime_error("Native target model baked a different bone set");
    }
    // Native bone-query indices follow the baked geometry's order. A sorted
    // source-name set is a validation input, not an index table.
    definition.mQueryableGeometryBoneNames = model->mGeometries->front()->getQueryableBoneOrientations();
    for (auto const& candidate : *model->mRenderControllers)
        for (auto const& current : *original.mRenderControllers)
            if (candidate.first && candidate.first == current.first)
                throw std::runtime_error("Native target model reused an original render controller");
    auto selected = std::find_if(model->mRenderControllers->begin(), model->mRenderControllers->end(), [](auto const& entry) {
        return entry.first && entry.first->mName->getString() == "controller.render.player.third_person";
    });
    if (selected == model->mRenderControllers->end())
        throw std::runtime_error("Native target prototype lacks the selected surface controller");
    auto controller = selected->first;
    model->mRenderControllers->clear();
    model->mRenderControllers->emplace_back(controller, ExpressionNode{});
    model->mRenderControllers->back().second.mImpl->emplace<float>(1.0f);
    controller->mPartVisibility->clear();
    controller->mPartVisibility->emplace_back("*", ExpressionNode{});
    controller->mPartVisibility->back().second.mImpl->emplace<float>(1.0f);
    controller->mShouldRebuildAnimationMatrices = true;
    model->mEnableAttachables = false;
    return result;
}
std::shared_ptr<ActorRenderer> NativeModelFactory::createRenderer(NativeModelInstance const& instance,
    std::shared_ptr<mce::TextureGroup> const& textures) {
    static_assert(sizeof(DataDrivenRenderer) == 0x210);
    auto base = imageBase();
    if (!instance.model || !instance.definition || !textures || !supports(*instance.model)
        || !matches(base, rendererConstructor, rendererSignature))
        throw std::runtime_error("Native independent renderer is unavailable for this client layout");
    // Traced from the native DataDrivenModel constructor's renderer caller:
    // this path constructs the base, model, materials and independent lifecycle.
    using Construct = DataDrivenRenderer* (*)(void*, std::shared_ptr<ActorResourceDefinition>,
        std::shared_ptr<mce::TextureGroup>, DataDrivenRendererLifecycle);
    auto storage = ::operator new(sizeof(DataDrivenRenderer));
    DataDrivenRenderer* renderer;
    try { renderer = reinterpret_cast<Construct>(base + rendererConstructor)(storage,
        instance.definition, textures, DataDrivenRendererLifecycle::Independent); }
    catch (...) { ::operator delete(storage); throw; }
    std::shared_ptr<ActorRenderer> result(renderer, [owner = instance.definition](ActorRenderer* value) {
        auto table = *reinterpret_cast<std::uintptr_t const* const*>(value);
        reinterpret_cast<void (*)(void*, unsigned)>(table[0])(value, 0);
        ::operator delete(value);
    });
    if (*reinterpret_cast<std::uintptr_t const*>(renderer) != base + 0xf26a7f0)
        throw std::runtime_error("Native independent renderer has an unexpected type");
    renderer->mModel = instance.model;
    return result;
}
}
