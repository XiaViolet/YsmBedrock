#include "NativeMenuPreview.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include <atomic>
#include <stdexcept>

namespace ysm {
void NativeMenuPreview::reset() {
    layers = {};
    group.reset();
    lastFrame = 0;
}
void NativeMenuPreview::draw(MinecraftUIRenderContext& context, UiRect rect, MenuPreviewFrame const& image, std::uint64_t frame) {
    auto textures = context.mClient.getTextureGroup();
    if (!textures) return;
    if (group.lock() != textures) { reset(); group = textures; }
    float side = std::min(rect.w, rect.h);
    float x = rect.x + (rect.w-side)*.5f, y = rect.y + (rect.h-side)*.5f;
    static std::atomic_uint64_t uploadId{};
    auto create = [&](std::shared_ptr<TextureImage const> const& bitmap) {
        ModelTexture texture;
        texture.uv = bitmap;
        return NativeTextureUpload::create(textures, texture, {}, "menu_" + std::to_string(++uploadId));
    };
    if (!layers[0].uploaded || frame != lastFrame) {
        if (!layers[0].uploaded || !layers[0].uploaded->updateColor(image.bitmap)) {
            layers[0].uploaded = create(std::make_shared<TextureImage const>(image.bitmap));
        }
        lastFrame = frame;
    }
    auto updateLayer = [&](Layer& layer, std::shared_ptr<TextureImage const> const& source) {
        if (layer.source == source) return;
        auto uploaded = source ? create(source) : nullptr;
        layer = {source, std::move(uploaded)};
    };
    updateLayer(layers[1], image.background);
    updateLayer(layers[2], image.foreground);
    auto drawLayer = [&](Layer const& layer, float scale) {
        if (!layer.uploaded) return;
        auto size = side * scale;
        context.drawImage(*layer.uploaded->textures().color->mClientTexture, {x+(side-size)/2,y+(side-size)/2}, {size,size}, {0,0}, {1,1}, false);
        context.flushImages(mce::Color(255,255,255), 1.f, HashedString("ui_textured_and_glcolor"));
    };
    drawLayer(layers[1], 1.f);
    drawLayer(layers[0], image.native ? 1.f : static_cast<float>(image.bitmap.width) / ModelPreview::BodyFramePixels);
    drawLayer(layers[2], 1.f);
}
}
