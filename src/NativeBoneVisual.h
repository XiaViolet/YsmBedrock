#pragma once
#include "BoneVisual.h"
#include "ModelPack.h"
class SkinnedMesh;
class ScreenContext;
class SortedMeshDrawList;
class RenderController;
class MaterialVariants;
namespace ysm {
bool skipCollapsedYsmMesh(SkinnedMesh const& mesh);
void recordSurfaceCpuCost(double lookupUs, double submitUs);
void drawBoneVisuals(SkinnedMesh& mesh, ScreenContext& screen, SortedMeshDrawList* queue,
    RenderController const* controller, MaterialVariants const& material, std::span<BoneVisual const> visuals,
    bool arkeWorldPass, void const* data, void (*submit)(void const*, MaterialVariants const&));
template<class Draw>
void drawBoneVisuals(SkinnedMesh& mesh, ScreenContext& screen, SortedMeshDrawList* queue,
    RenderController const* controller, MaterialVariants const& material, std::span<BoneVisual const> visuals,
    bool arkeWorldPass, Draw const& draw) {
    // The native submission is synchronous. Borrow the callback for this
    // call instead of allocating an owning std::function per mesh.
    drawBoneVisuals(mesh, screen, queue, controller, material, visuals, arkeWorldPass, &draw,
        [](void const* data, MaterialVariants const& selected) { (*static_cast<Draw const*>(data))(selected); });
}
Json boneVisualRenderReport();
void setSurfaceRouteSupport(bool supported);
void recordSurfaceRoute(bool individual);
}
