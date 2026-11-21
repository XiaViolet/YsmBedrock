#include "NativeBoneVisual.h"
#include "mc/client/model/models/SkinnedMesh.h"
#include "mc/client/model/models/SortedMeshDrawList.h"
#include "mc/client/model/models/DrawsByRenderController.h"
#include "mc/client/model/models/DrawsByMaterial.h"
#include "mc/client/model/models/DrawsByMesh.h"
#include "mc/client/model/models/DrawsByTexture.h"
#include "mc/client/model/models/MeshRenderData.h"
#include "mc/client/model/geom/ModelPart.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/renderer/RenderMaterialGroup.h"
#include "mc/deps/core/renderer/RenderMaterialInfo.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialVariants.h"
#include "mc/deps/minecraft_renderer/constantbuffers/GlobalConstantBuffers.h"
#include "mc/deps/renderer/hal/base/ShaderConstantBase.h"
#include "ll/api/memory/Memory.h"
#include <atomic>
#include <algorithm>
#include <functional>
#include <mutex>
#include <Windows.h>
#include <glm/gtc/type_ptr.hpp>

namespace ysm {
namespace {
std::atomic<std::uint64_t> hookCalls = 0, eligibleDraws = 0, passes = 0, packets = 0, mismatches = 0, immediate = 0, missingBlend = 0;
std::atomic<std::uint64_t> tintedPasses = 0, alphaPasses = 0, glowPasses = 0;
std::atomic<std::uint64_t> lightPasses = 0, missingLight = 0;
std::atomic<std::uint64_t> tintOverrides = 0, missingTint = 0;
std::atomic<std::uint64_t> batchCandidates = 0, individualActors = 0;
std::atomic<std::uint64_t> earlyCollapsedMeshes = 0, uniformSurfaces = 0, nativeSurfaceDraws = 0;
std::atomic<std::uint64_t> collapsedMaterialPasses = 0, unmaskedMixedMeshes = 0;
std::atomic<std::uint64_t> timedSurfaces = 0, lookupCpuNs = 0, submitCpuNs = 0;
std::atomic<bool> surfaceRouteSupported = false;
std::atomic<std::uintptr_t> frameSubmitRva = 0;
std::atomic<std::uint64_t> arkeLayers = 0, arkeLayerMisses = 0;
std::mutex sampleMutex;
Json surfaceSamples = Json::object();
bool collapsed(glm::mat4 const& matrix) {
    return collapsedSkinMatrix(std::span<float const, 16>(glm::value_ptr(matrix), 16));
}
struct ScopedBoneTint {
    mce::ShaderConstantBase* uniform = nullptr;
    glm::vec4* value = nullptr;
    glm::vec4 previous{};
    ScopedBoneTint(ScreenContext& screen, BoneVisual const& visual) {
        if (visual.color == BoneVisual{}.color && visual.alpha == 255) return;
        auto& buffers = static_cast<mce::GlobalConstantBuffers&>(screen.constantBuffers);
        auto pointer = static_cast<mce::ShaderConstantFloat4*>(buffers.entityConstantBuffer->CHANGE_COLOR);
        uniform = reinterpret_cast<mce::ShaderConstantBase*>(pointer);
        if (uniform && *uniform->name == "CHANGE_COLOR")
            value = ll::memory::dAccess<glm::vec4*>(uniform, 0x40);
        if (!value) { ++missingTint; uniform = nullptr; return; }
        previous = *value;
        // CHANGE_COLOR starts at transparent black when the native actor has
        // no dye. It is a color mask value, not an existing white multiplier.
        *value = glm::vec4(visual.color[0] / 255.0f, visual.color[1] / 255.0f,
            visual.color[2] / 255.0f, visual.alpha / 255.0f);
        uniform->dirty = true;
        ++tintOverrides;
    }
    ~ScopedBoneTint() {
        if (!value) return;
        *value = previous;
        uniform->dirty = true;
    }
};
struct ScopedBoneLight {
    mce::ShaderConstantBase* uniform = nullptr;
    glm::vec2* value = nullptr;
    glm::vec2 previous{};
    ScopedBoneLight(ScreenContext& screen, int glow) {
        if (glow < 0) return;
        auto& buffers = static_cast<mce::GlobalConstantBuffers&>(screen.constantBuffers);
        auto pointer = static_cast<mce::ShaderConstantFloat2*>(buffers.entityConstantBuffer->TILE_LIGHT_INTENSITY);
        uniform = reinterpret_cast<mce::ShaderConstantBase*>(pointer);
        // 1.26.60.05's MeshInstanceConstants capture dereferences uniform+0x40.
        // Verify its declared identity before borrowing that versioned data view.
        if (uniform && *uniform->name == "TILE_LIGHT_INTENSITY")
            value = ll::memory::dAccess<glm::vec2*>(uniform, 0x40);
        if (!value) { ++missingLight; uniform = nullptr; return; }
        previous = *value;
        *value = glm::vec2(static_cast<float>(glow) / 15);
        uniform->dirty = true;
        ++lightPasses;
    }
    ~ScopedBoneLight() {
        if (!value) return;
        *value = previous;
        uniform->dirty = true;
    }
};
void visitPackets(SortedMeshDrawList& queue, RenderController const* controller,
    std::function<void(std::string const&, std::vector<MeshRenderData>&)> const& visit) {
    auto textures = [&](auto&& self, std::vector<DrawsByTexture>& values, std::string const& path) -> void {
        for (std::size_t i = 0; i < values.size(); ++i) {
            auto key = path + "/" + std::to_string(i);
            visit(key, *values[i].mMeshes);
            self(self, *values[i].mDrawsByTexture, key);
        }
    };
    for (std::size_t r = 0; r < queue.mDrawsByRenderController->size(); ++r) {
        auto& entry = (*queue.mDrawsByRenderController)[r];
        if (entry.mRenderController != controller) continue;
        for (std::size_t m = 0; m < entry.mDrawsByMaterial->size(); ++m) {
            auto& material = (*entry.mDrawsByMaterial)[m];
            for (std::size_t b = 0; b < material.mDrawsByMesh->size(); ++b)
                textures(textures, *(*material.mDrawsByMesh)[b].mDrawsByTexture,
                    std::to_string(r) + "/" + std::to_string(m) + "/" + std::to_string(b));
        }
    }
}
bool replaceLoadedMaterial(mce::MaterialPtr& target, std::string const& name) {
    for (auto* group : {&mce::RenderMaterialGroup::switchable(), &mce::RenderMaterialGroup::common()}) {
        mce::MaterialPtr candidate(*group, HashedString(name));
        auto info = candidate.mRenderMaterialInfoPtr;
        // getMaterialInfo may return a named placeholder without a renderer.
        // Never replace a live material with that unloaded placeholder.
        if (info && info->mPtr) { target = std::move(candidate); return true; }
    }
    return false;
}
// This Bedrock material tests against depth from the base pass and accepts
// ordinary RGBA textures. The alpha-encoded emissive material has a
// different contract and would reinterpret the author's opacity.
bool selectGlowLayer(MaterialVariants& layer) {
    for (auto* target : {&*layer.mSkinningMaterialPtr, &*layer.mSkinningColorMaterialPtr}) {
        auto info = target->mRenderMaterialInfoPtr;
        if (!info) continue;
        auto const& original = info->mHashedName->getString();
        auto suffix = original.find('.');
        auto name = std::string("entity_emissive_layer_alpha_test") +
            (suffix == original.npos ? "" : original.substr(suffix));
        if (!replaceLoadedMaterial(*target, name) || target->mRenderMaterialInfoPtr->mHashedName->getString() != name)
            return false;
    }
    return layer.mSkinningMaterialPtr->mRenderMaterialInfoPtr != nullptr;
}
MaterialVariants tintMaterial(MaterialVariants const& source, bool translucent) {
    auto result = source;
    for (auto* target : {&*result.mSkinningMaterialPtr, &*result.mSkinningColorMaterialPtr}) {
        auto info = target->mRenderMaterialInfoPtr;
        if (!info) continue;
        auto name = info->mHashedName->getString();
        auto suffix = name.find('.');
        name = std::string(translucent ? "guardian_ghost" : "entity_alphatest_change_color")
            + (suffix == std::string::npos ? std::string{} : name.substr(suffix));
        if (!replaceLoadedMaterial(*target, name)) ++missingBlend;
    }
    return result;
}
MaterialVariants cutoutMaterial(MaterialVariants const& source) {
    auto result = source;
    for (auto* target : {&*result.mSkinningMaterialPtr, &*result.mSkinningColorMaterialPtr}) {
        auto info = target->mRenderMaterialInfoPtr;
        if (!info) continue;
        auto const& original = info->mHashedName->getString();
        auto suffix = original.find('.');
        auto name = std::string("entity_alphatest_one_sided") + (suffix == original.npos ? "" : original.substr(suffix));
        replaceLoadedMaterial(*target, name);
    }
    return result;
}
}
bool skipCollapsedYsmMesh(SkinnedMesh const& mesh) {
    if (!mesh.mUseBones || mesh.mSkinnedParts->empty() || mesh.mSkinnedParts->size() != mesh.mBoneMatrices->size()
        || mesh.mBoneMatrices->size() != mesh.mPreviousBoneMatrices->size()) return false;
    if (!std::all_of(mesh.mBoneMatrices->begin(), mesh.mBoneMatrices->end(), collapsed)
        || !std::all_of(mesh.mPreviousBoneMatrices->begin(), mesh.mPreviousBoneMatrices->end(), collapsed)) return false;
    if (!std::any_of(mesh.mSkinnedParts->begin(), mesh.mSkinnedParts->end(), [](auto const* part) {
        return part && part->mName->getString().starts_with("__ysm_");
    })) return false;
    ++earlyCollapsedMeshes;
    return true;
}
void drawBoneVisuals(SkinnedMesh& mesh, ScreenContext& screen, SortedMeshDrawList* queue,
    RenderController const* controller, MaterialVariants const& material, std::span<BoneVisual const> visuals,
    bool arkeWorldPass, void const* data, void (*submit)(void const*, MaterialVariants const&)) {
    auto draw = [&](MaterialVariants const& selected) { submit(data, selected); };
    ++hookCalls;
    if (!frameSubmitRva) if (auto* builder = screen.frameBuilder->get()) {
        auto target = (*reinterpret_cast<std::uintptr_t**>(builder))[0x378 / sizeof(void*)];
        auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"Minecraft.Windows.exe"));
        if (target > base) frameSubmitRva = target - base;
    }
    if (!visuals.empty()) ++eligibleDraws;
    auto plan = planBoneDraws(visuals);
    if (visuals.empty() || (plan.uniform && *plan.uniform == BoneVisual{})) {
        if (!visuals.empty()) ++nativeSurfaceDraws;
        draw(material);
        return;
    }
    if (visuals.size() != mesh.mSkinnedParts->size() || (!plan.uniform && visuals.size() != mesh.mBoneMatrices->size())) {
        ++mismatches;
        draw(material);
        return;
    }
    if (!plan.uniform && mesh.mUseBones && visuals.size() == mesh.mPreviousBoneMatrices->size()) {
        auto omitted = omitCollapsedMaterialPasses(plan, visuals, [&](std::size_t i) {
            return collapsed((*mesh.mBoneMatrices)[i]) && collapsed((*mesh.mPreviousBoneMatrices)[i]);
        });
        if (omitted) collapsedMaterialPasses.fetch_add(omitted, std::memory_order_relaxed);
        if (plan.uniform) unmaskedMixedMeshes.fetch_add(1, std::memory_order_relaxed);
    }
    bool maskPalette = !plan.uniform;
    auto passCount = plan.passCount();
    if (passCount == 0) return;
    if (!maskPalette) ++uniformSurfaces;
    struct Restore {
        SkinnedMesh& mesh;
        bool maskPalette;
        std::vector<glm::mat4> matrices, previous;
        ShaderColor& shader;
        mce::Color color;
        ~Restore() {
            if (maskPalette) {
                mesh.mBoneMatrices->swap(matrices);
                mesh.mPreviousBoneMatrices->swap(previous);
            }
            *shader.color = color;
            shader.dirty = true;
        }
    } restore{mesh, maskPalette,
        maskPalette ? *mesh.mBoneMatrices : std::vector<glm::mat4>{},
        maskPalette ? *mesh.mPreviousBoneMatrices : std::vector<glm::mat4>{},
        static_cast<ShaderColor&>(screen.currentShaderColor),
        *static_cast<ShaderColor&>(screen.currentShaderColor).color};
    glm::mat4 zeroScale(0.0f);
    zeroScale[3][3] = 1;
    std::span<std::size_t const> previousPass;
    if (maskPalette) {
        std::fill(mesh.mBoneMatrices->begin(), mesh.mBoneMatrices->end(), zeroScale);
        std::fill_n(mesh.mPreviousBoneMatrices->begin(),
            std::min(visuals.size(), mesh.mPreviousBoneMatrices->size()), zeroScale);
    }
    for (std::size_t passIndex = 0; passIndex < passCount; ++passIndex) {
        if (maskPalette) {
            auto const& pass = plan.groups[passIndex];
            for (auto i : previousPass) {
                (*mesh.mBoneMatrices)[i] = zeroScale;
                if (i < mesh.mPreviousBoneMatrices->size()) (*mesh.mPreviousBoneMatrices)[i] = zeroScale;
            }
            for (auto i : pass.bones) {
                (*mesh.mBoneMatrices)[i] = restore.matrices[i];
                if (i < mesh.mPreviousBoneMatrices->size()) (*mesh.mPreviousBoneMatrices)[i] = restore.previous[i];
            }
            previousPass = pass.bones;
        }
        auto const& visual = plan.uniform ? *plan.uniform : plan.groups[passIndex].visual;
        if (visual.color != BoneVisual{}.color) ++tintedPasses;
        if (visual.alpha < 255) ++alphaPasses;
        if (visual.glow >= 0) ++glowPasses;
        auto color = restore.color;
        // Entity tint is supplied through CHANGE_COLOR. Keep the general
        // multiplier neutral so fractional colors are not applied twice.
        color.r = color.g = color.b = 1.0f;
        color.a = (screen.isDrawingUI ? restore.color.a : 1.0f) * static_cast<float>(visual.alpha) / 255;
        std::map<std::string, std::size_t> before;
        if (queue) visitPackets(*queue, controller, [&](auto const& key, auto const& values) { before[key] = values.size(); });
        *restore.shader.color = color;
        restore.shader.dirty = true;
        auto selected = visual.alpha < 255 || visual.color != BoneVisual{}.color
            ? tintMaterial(material, visual.alpha < 255) : material;
        if (visual.cutout && visual.alpha == 255 && visual.color == BoneVisual{}.color) selected = cutoutMaterial(selected);
        ScopedBoneLight light(screen, visual.glow);
        ScopedBoneTint tint(screen, visual);
        // Diagnostic sampling must not allocate strings and lock a global
        // mutex for every surface of a multi-thousand-bone model.
        if (passes < 64 || (passes & 1023) == 0) {
            std::scoped_lock lock(sampleMutex);
            auto key = std::to_string(visual.color[0]) + "," + std::to_string(visual.color[1]) + ","
                + std::to_string(visual.color[2]) + "," + std::to_string(visual.alpha) + "," + std::to_string(visual.glow)
                + (screen.isDrawingUI ? "/ui" : "/world");
            if ((!surfaceSamples.contains(key) && surfaceSamples.size() < 16) ||
                (surfaceSamples.contains(key) && eligibleDraws % 1024 == 0)) {
                auto info = selected.mSkinningMaterialPtr->mRenderMaterialInfoPtr;
                surfaceSamples[key] = {{"shader_color", {color.r, color.g, color.b, color.a}},
                    {"material", info ? info->mHashedName->getString() : ""},
                    {"material_ready", info && static_cast<bool>(info->mPtr)},
                    {"tint_before", tint.value ? Json::array({tint.previous.x, tint.previous.y, tint.previous.z, tint.previous.w}) : Json()},
                    {"tint_override", tint.value ? Json::array({tint.value->x, tint.value->y, tint.value->z, tint.value->w}) : Json()},
                    {"bones", plan.uniform ? visuals.size() : plan.groups[passIndex].bones.size()},
                    {"ui", static_cast<bool>(screen.isDrawingUI)},
                    {"first_person", static_cast<bool>(screen.isDrawingFirstPersonObjects)},
                    {"light_override", light.value ? Json::array({light.value->x, light.value->y}) : Json()}};
            }
        }
        draw(selected);
        ++passes;
        auto sourceInfo = selected.mSkinningMaterialPtr->mRenderMaterialInfoPtr;
        if (arkeWorldPass && sourceInfo && supportsArkeGlowLayer(visual, sourceInfo->mHashedName->getString())) {
            auto layer = selected;
            if (selectGlowLayer(layer)) {
                draw(layer);
                ++arkeLayers;
            } else ++arkeLayerMisses;
        }
        if (!queue) { ++immediate; continue; }
        visitPackets(*queue, controller, [&](auto const& key, auto& values) {
            for (std::size_t i = before[key]; i < values.size(); ++i) {
                auto& value = values[i];
                if (value.mSkinnedMesh != &mesh) continue;
                // The queued draw owns its complete palette and constants.
                // Nothing here mutates a shader global or a later entity.
                *value.mCurrentShaderColor->color = color;
                value.mCurrentShaderColor->dirty = true;
                if (visual.glow >= 0) *value.mMeshInstanceConstants->TILE_LIGHT_INTENSITY = glm::vec2(static_cast<float>(visual.glow) / 15);
                ++packets;
            }
        });
    }
}
void recordSurfaceCpuCost(double lookupUs, double submitUs) {
    lookupCpuNs.fetch_add(static_cast<std::uint64_t>(lookupUs * 1000), std::memory_order_relaxed);
    submitCpuNs.fetch_add(static_cast<std::uint64_t>(submitUs * 1000), std::memory_order_relaxed);
    timedSurfaces.fetch_add(1, std::memory_order_relaxed);
}
Json boneVisualRenderReport() {
    std::scoped_lock lock(sampleMutex);
    return {{"hook_calls", hookCalls.load()}, {"eligible_draws", eligibleDraws.load()},
        {"passes", passes.load()}, {"queued_packets", packets.load()}, {"palette_mismatches", mismatches.load()},
        {"uniform_surface_draws", uniformSurfaces.load()},
        {"early_collapsed_meshes_skipped", earlyCollapsedMeshes.load()}, {"collapsed_material_passes_skipped", collapsedMaterialPasses.load()},
        {"mixed_meshes_without_palette_copy", unmaskedMixedMeshes.load()}, {"native_surface_draws", nativeSurfaceDraws.load()},
        {"timed_surfaces", timedSurfaces.load()}, {"visual_lookup_cpu_us", lookupCpuNs.load() / 1000.0},
        {"surface_submit_cpu_us", submitCpuNs.load() / 1000.0},
        {"tinted_passes", tintedPasses.load()}, {"alpha_passes", alphaPasses.load()}, {"glow_passes", glowPasses.load()},
        {"immediate_passes", immediate.load()}, {"missing_blend_materials", missingBlend.load()},
        {"arke_glow_layers", arkeLayers.load()}, {"arke_glow_layer_misses", arkeLayerMisses.load()},
        {"light_overrides", lightPasses.load()}, {"missing_light_uniforms", missingLight.load()}, {"surface_samples", surfaceSamples},
        {"tint_overrides", tintOverrides.load()}, {"missing_tint_uniforms", missingTint.load()},
        {"surface_route_supported", surfaceRouteSupported.load()}, {"batch_candidates", batchCandidates.load()},
        {"frame_submit_rva", frameSubmitRva.load()},
        {"individual_actor_requests", individualActors.load()}};
}
void setSurfaceRouteSupport(bool supported) { surfaceRouteSupported = supported; }
void recordSurfaceRoute(bool individual) {
    ++batchCandidates;
    if (individual) ++individualActors;
}
}
