#pragma once
#include "PreviewDepthRenderer.h"
#include "NativeTextureUpload.h"
#include "YsmUi.h"

class MinecraftUIRenderContext;
namespace ysm {
class NativeMenuPreview {
    std::weak_ptr<mce::TextureGroup> group;
    std::unique_ptr<PreviewDepthRenderer> renderer;
    std::uint64_t lastFrame = 0;
    unsigned lastSize = 0;
    struct Layer {
        std::shared_ptr<TextureImage const> source;
        std::shared_ptr<NativeTextureUpload> uploaded;
    };
    std::array<Layer, 3> layers{};
public:
    void reset();
    void draw(MinecraftUIRenderContext& context, UiRect rect, PreviewMesh const& mesh, std::uint64_t frame, float pixelDensity);
};
}
