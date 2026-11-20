#include "NativeMenuPreview.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include <cmath>
#include <atomic>
#include <stdexcept>

namespace ysm {
void NativeMenuPreview::reset() {
    layers = {};
    group.reset();
    lastSize = 0;
    // Releasing the last D3D11 device reference here can wait inside the display
    // driver while Minecraft's UI render callback is still on the stack.
    if (renderer) renderer->clearFrame();
}

void NativeMenuPreview::draw(MinecraftUIRenderContext& context, UiRect rect, PreviewMesh const& mesh,
                            std::uint64_t frame, float pixelDensity) {
    auto textures = context.mClient.getTextureGroup();
    if (!textures) throw std::runtime_error("Native menu texture group unavailable");
    if (group.lock() != textures) { reset(); group = textures; }
    if (!renderer) renderer = std::make_unique<PreviewDepthRenderer>();
    float side = std::min(rect.w, rect.h);
    float x = rect.x + (rect.w-side)*.5f, y = rect.y + (rect.h-side)*.5f;
    unsigned pixels = static_cast<unsigned>(std::clamp(std::ceil(side * pixelDensity), 224.f, 1024.f));
    static std::atomic_uint64_t uploadId{};
    auto upload = [&](Layer& layer, std::shared_ptr<TextureImage const> const& image) {
        Layer next{image, {}};
        if (image) {
            ModelTexture texture;
            texture.uv = image;
            next.uploaded = NativeTextureUpload::create(textures, texture, {}, "menu_" + std::to_string(++uploadId));
        }
        layer = std::move(next);
    };
    if (lastSize != pixels || frame != lastFrame) {
        upload(layers[0], std::make_shared<TextureImage>(renderer->render(mesh, pixels)));
        lastSize = pixels;
        lastFrame = frame;
    }
    if (layers[1].source != mesh.background) upload(layers[1], mesh.background);
    if (layers[2].source != mesh.foreground) upload(layers[2], mesh.foreground);
    auto drawLayer = [&](Layer const& layer) {
        if (!layer.uploaded) return;
        context.drawImage(*layer.uploaded->textures().color->mClientTexture, {x,y}, {side,side}, {0,0}, {1,1}, false);
        context.flushImages(mce::Color(255,255,255), 1.f, HashedString("ui_textured_and_glcolor"));
    };
    drawLayer(layers[1]);
    drawLayer(layers[0]);
    drawLayer(layers[2]);
}
}
