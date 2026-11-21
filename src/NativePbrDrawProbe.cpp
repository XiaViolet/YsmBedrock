#include "NativePbrDrawProbe.h"
#include "ll/api/memory/Hook.h"
#include "mc/deps/core/renderer/RenderMaterialInfo.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/deps/minecraft_renderer/resources/ServerTexture.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/model/geom/ModelPart.h"
#include "mc/client/model/models/SkinnedMesh.h"
#include "mc/client/model/models/SortedMeshDrawList.h"
#include "mc/client/model/models/DrawsByRenderController.h"
#include "mc/client/model/models/DrawsByMaterial.h"
#include "mc/client/model/models/DrawsByMesh.h"
#include "mc/client/model/models/DrawsByTexture.h"
#include "mc/client/model/models/MeshRenderData.h"
#include "mc/client/renderer/controller/RenderController.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialVariants.h"
#include "mc/deps/minecraft_renderer/resources/MERSUniformData.h"
#include <chrono>
#include <fstream>
#include <mutex>
#include <set>

namespace ysm {
namespace {
using Texture = std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture>;
using Slots = gsl::span<mce::ClientTexture const*>;
using MeshSlots = void (mce::Mesh::*)(mce::MeshContext&, mce::MaterialPtr const&, Slots, uint, uint,
    OffscreenCaptureDescription const&, mce::IndexBufferContainer const*) const;
using MeshSingle = void (mce::Mesh::*)(mce::MeshContext&, mce::MaterialPtr const&, Texture const&, uint, uint,
    OffscreenCaptureDescription const&, mce::IndexBufferContainer const*) const;
using MeshMetadata = void (mce::Mesh::*)(mce::MeshContext&, dragon::RenderMetadata const&, mce::MaterialPtr const&,
    Texture const&, uint, uint, mce::IndexBufferContainer const*) const;
struct Probe {
    SkinRuntime const* skin;
    std::filesystem::path output;
    std::mutex mutex;
    std::set<std::string> records;
    Probe(SkinRuntime const& skin, std::filesystem::path file) : skin(&skin), output(std::move(file)) {}
};
std::atomic<std::shared_ptr<Probe>> active;

mce::ClientTexture const* clientTexture(Texture const& arg) {
    if (auto client = std::get_if<mce::ClientTexture>(&arg)) return client;
    if (auto ptr = std::get_if<mce::TexturePtr>(&arg); ptr && ptr->mClientTexture)
        return &*ptr->mClientTexture->mClientTexture;
    return nullptr;
}
auto matchesLayer(mce::ClientTexture const* value, auto const& layer) {
    return value && value->mResourcePointerBlock && layer
        && value->mResourcePointerBlock == layer->mClientTexture->mResourcePointerBlock;
}
std::string_view slotRole(mce::ClientTexture const* value, NativeTextureSet const& set) {
    if (!value) return "empty";
    if (matchesLayer(value, set.color)) return "color";
    if (matchesLayer(value, set.normal)) return "normal";
    if (matchesLayer(value, set.material)) return "material";
    return "other";
}
void record(Probe& probe, Json data) {
    auto line = data.dump();
    std::scoped_lock lock(probe.mutex);
    if (probe.records.size() < 128 && probe.records.insert(line).second)
        std::ofstream(probe.output, std::ios::app) << line << '\n';
}
void observe(std::string_view entry, mce::MaterialPtr const& material, Slots slots) {
    auto probe = active.load(std::memory_order_acquire);
    if (!probe) return;
    auto set = probe->skin->publishedTextures();
    if (!set.color) return;
    if (std::none_of(slots.begin(), slots.end(), [&](auto* slot) { return matchesLayer(slot, set.color); })) return;
    std::vector<std::string_view> roles;
    for (auto* slot : slots) roles.push_back(slotRole(slot, set));
    record(*probe, {{"entry", entry}, {"color_width", set.color->mTextureDescription->mWidth},
        {"color_height", set.color->mTextureDescription->mHeight}, {"uploaded_normal", !!set.normal},
        {"uploaded_material", !!set.material}, {"slots", roles},
        {"draw_material", material.mRenderMaterialInfoPtr ? material.mRenderMaterialInfoPtr->mHashedName->getString() : ""}});
}
// Only inspect queue entries owned by the sampled mesh and controller. Some
// frame builders take the immediate submission without populating this list.
void observeSkinned(SkinnedMesh const& mesh, ScreenContext& screen, RenderController const* controller,
    MaterialVariants const& material, SortedMeshDrawList* queue, bool hasUniforms) {
    using Clock = std::chrono::steady_clock;
    thread_local Clock::time_point nextSample{};
    auto now = Clock::now();
    if (now < nextSample || screen.isDrawingUI || !controller) return;
    if (!controller->mName->getString().starts_with("controller.render.player.") ||
        std::ranges::none_of(*mesh.mSkinnedParts, [](auto* p) {
            return p && p->mName->getString().starts_with("__ysm_");
        })) return;
    nextSample = now + std::chrono::milliseconds(250);
    auto probe = active.load(std::memory_order_acquire);
    if (!probe) return;
    auto set = probe->skin->publishedTextures();
    if (!set.color) return;
    auto info = material.mSkinningMaterialPtr->mRenderMaterialInfoPtr;
    Json roles = Json::array();
    std::vector<std::string_view> path;
    auto scan = [&](auto&& self, DrawsByTexture const& node) -> void {
        path.push_back(slotRole(node.mTexture, set));
        if (std::ranges::any_of(*node.mMeshes, [&](auto const& draw) { return draw.mSkinnedMesh == &mesh; }))
            roles.push_back(path);
        for (auto const& child : *node.mDrawsByTexture) self(self, child);
        path.pop_back();
    };
    if (queue) for (auto const& c : *queue->mDrawsByRenderController) {
        if (c.mRenderController != controller) continue;
        for (auto const& m : *c.mDrawsByMaterial)
            for (auto const& b : *m.mDrawsByMesh)
                for (auto const& t : *b.mDrawsByTexture) scan(scan, t);
    }
    record(*probe, {{"entry", "SkinnedMesh::draw"}, {"controller", controller->mName->getString()},
        {"draw_material", info ? info->mHashedName->getString() : ""}, {"queue_provided", queue != nullptr},
        {"matching_packet_slot_paths", roles}, {"uniform_mers", hasUniforms},
        {"local_color_width", set.color->mTextureDescription->mWidth}, {"local_normal_uploaded", !!set.normal},
        {"local_material_uploaded", !!set.material}});
}
LL_TYPE_INSTANCE_HOOK(PbrSkinned, ll::memory::HookPriority::Low, SkinnedMesh,
    &SkinnedMesh::draw, void, ScreenContext& screen, dragon::RenderMetadata const& meta, RenderController const* controller,
    uint subLayer, MaterialVariants const& material, ClientPBRTextureData const& texture, SortedMeshDrawList* queue,
    std::optional<glm::mat4> const& previous, std::optional<MERSUniformData> const& mers, ShadowContext shadow, bool dithering) {
    origin(screen, meta, controller, subLayer, material, texture, queue, previous, mers, shadow, dithering);
    observeSkinned(*this, screen, controller, material, queue, mers.has_value());
}
LL_TYPE_INSTANCE_HOOK(PbrMeshSlots, ll::memory::HookPriority::Normal, mce::Mesh,
    static_cast<MeshSlots>(&mce::Mesh::renderMesh), void, mce::MeshContext& ctx, mce::MaterialPtr const& mat,
    Slots slots, uint start, uint count, OffscreenCaptureDescription const& capture, mce::IndexBufferContainer const* indices) {
    observe("slots", mat, slots);
    origin(ctx, mat, slots, start, count, capture, indices);
}
LL_TYPE_INSTANCE_HOOK(PbrMeshSingle, ll::memory::HookPriority::Normal, mce::Mesh,
    static_cast<MeshSingle>(&mce::Mesh::renderMesh), void, mce::MeshContext& ctx, mce::MaterialPtr const& mat,
    Texture const& tex, uint start, uint count, OffscreenCaptureDescription const& capture, mce::IndexBufferContainer const* indices) {
    auto* slot = clientTexture(tex);
    observe("single", mat, Slots(&slot,1));
    origin(ctx, mat, tex, start, count, capture, indices);
}
LL_TYPE_INSTANCE_HOOK(PbrMeshMetadata, ll::memory::HookPriority::Normal, mce::Mesh,
    static_cast<MeshMetadata>(&mce::Mesh::renderMesh), void, mce::MeshContext& ctx, dragon::RenderMetadata const& meta,
    mce::MaterialPtr const& mat, Texture const& tex, uint start, uint count, mce::IndexBufferContainer const* indices) {
    auto* slot = clientTexture(tex);
    observe("metadata", mat, Slots(&slot,1));
    origin(ctx, meta, mat, tex, start, count, indices);
}
using Hooks = ll::memory::HookRegistrar<PbrSkinned, PbrMeshSlots, PbrMeshSingle, PbrMeshMetadata>;
std::unique_ptr<Hooks> hooks;
}
void startPbrDrawProbe(std::filesystem::path const& directory, SkinRuntime const& localSkin) {
    if (!std::filesystem::exists(directory / "pbr-draw-probe.enable")) return;
    auto probe = std::make_shared<Probe>(localSkin, directory / "pbr-draw.jsonl");
    std::ofstream(probe->output) << "{\"probe\":\"mesh submission\"}\n";
    active.store(std::move(probe), std::memory_order_release);
    hooks = std::make_unique<Hooks>();
}
void stopPbrDrawProbe() {
    active.store({}, std::memory_order_release);
    hooks.reset();
}
}
