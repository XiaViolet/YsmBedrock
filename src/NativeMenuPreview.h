#pragma once
#include "NativeTextureUpload.h"
#include "YsmUi.h"

class MinecraftUIRenderContext;
namespace ysm {
struct MenuPreviewFrame {
    TextureImage bitmap;
    std::shared_ptr<TextureImage const> background, foreground;
    std::size_t vertices = 0;
    bool native = false;
};
class NativeMenuPreview {
    std::weak_ptr<mce::TextureGroup> group;
    std::uint64_t lastFrame = 0;
    struct Layer {
        std::shared_ptr<TextureImage const> source;
        std::shared_ptr<NativeTextureUpload> uploaded;
    };
    std::array<Layer, 3> layers{};
public:
    void reset();
    void draw(MinecraftUIRenderContext& context, UiRect rect, MenuPreviewFrame const& image, std::uint64_t frame);
};
}
