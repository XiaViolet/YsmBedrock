#include "NativeTextureUpload.h"
#include "mc/client/renderer/TextureGroup.h"
#include "mc/deps/core/image/Image.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core_graphics/ImageBuffer.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTexture.h"
#include <cstring>
#include <stdexcept>

namespace ysm {
struct NativeTextureUpload::Impl {
    std::weak_ptr<mce::TextureGroup> group;
    std::vector<std::unique_ptr<ResourceLocation>> locations;
    NativeTextureSet textures;
    ~Impl() {
        if (auto owner = group.lock())
            for (auto const& location : locations) owner->unloadTexture(*location, false);
    }
    std::shared_ptr<BedrockTextureData> upload(mce::TextureGroup& owner, TextureImage const& source,
        std::string const& path, PbrEncoding encoding, std::optional<PbrLayer> layer = {}) {
        if (!source.width || !source.height || source.rgba.size() != std::size_t(source.width) * source.height * 4)
            throw std::runtime_error("Invalid native texture image");
        mce::Image image(source.width, source.height, 1, mce::ImageFormat::RGBA8Unorm,
            layer ? mce::ImageUsage::Data : mce::ImageUsage::SRGB);
        image.mAlphaUsage = mce::AlphaUsage::Transparent;
        image.resizeImageBytesToFitImageDescription();
        if (layer) for (std::size_t i = 0; i < source.rgba.size(); i += 4) {
            auto pixel = convertPbrTexel(source.rgba.data() + i, encoding, *layer);
            std::memcpy(image.mImageBytes.data() + i, pixel.data(), 4);
        } else std::memcpy(image.mImageBytes.data(), source.rgba.data(), source.rgba.size());
        auto& location = locations.emplace_back(std::make_unique<ResourceLocation>(Core::PathView(path)));
        auto data = owner.uploadTexture(*location, cg::ImageBuffer(std::move(image))).mBedrockTextureData;
        if (!data || data->mIsMissingTexture != IsMissingTexture::No
            || data->mTextureDescription->mWidth != source.width || data->mTextureDescription->mHeight != source.height
            || !(static_cast<unsigned char>(data->mTextureLoadState) & static_cast<unsigned char>(TextureLoadState::LoadedBit)))
            throw std::runtime_error("Native texture upload failed: " + source.path);
        return data;
    }
};
NativeTextureUpload::NativeTextureUpload() : impl(std::make_unique<Impl>()) {}
NativeTextureUpload::~NativeTextureUpload() = default;
std::shared_ptr<NativeTextureUpload> NativeTextureUpload::create(std::shared_ptr<mce::TextureGroup> const& group,
    ModelTexture const& texture, PbrEncoding encoding, std::string const& identity) {
    if (!group || !texture.uv || identity.empty()) throw std::runtime_error("Incomplete native texture upload");
    auto result = std::shared_ptr<NativeTextureUpload>(new NativeTextureUpload);
    result->impl->group = group;
    auto path = "textures/ysm_bedrock/targets/" + identity;
    auto& layers = result->impl->textures;
    layers.color = result->impl->upload(*group, *texture.uv, path, encoding);
    if (texture.normal) layers.normal = result->impl->upload(*group, *texture.normal, path + "_n", encoding, PbrLayer::Normal);
    if (texture.specular) layers.material = result->impl->upload(*group, *texture.specular, path + "_mers", encoding, PbrLayer::Material);
    return result;
}
NativeTextureSet NativeTextureUpload::textures() const { return impl->textures; }
}
