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
// uploadTexture(ImageBuffer) describes each upload as a color image. Publish the
// actual layer kinds on our color texture before native consumers inspect its set.
void describePbrLayers(BedrockTexture& base, NativeTextureSet const& source) {
    using Layer = cg::TextureSetLayerType;
    auto imageDescription = [](std::shared_ptr<BedrockTextureData> const& map) -> cg::ImageDescription const* {
        if (map) for (auto const& entry : *map->mTextureSetDescription->mLayerInfo) {
            if (entry.mLayerType == Layer::Color) return std::get_if<cg::ImageDescription>(&*entry.mData);
        }
        return nullptr;
    };
    auto color = imageDescription(base.mBedrockTextureData);
    auto normal = imageDescription(source.normal);
    auto material = imageDescription(source.material);
    if (!color || (!normal && !material)) return;
    std::vector<cg::TextureSetImageDescription::LayerInfoVar> layers(1u + (normal != nullptr) + (material != nullptr));
    auto next = layers.begin();
    auto append = [&](cg::ImageDescription const& image, Layer kind) {
        next->mLayerType = kind;
        next->mData->emplace<cg::ImageDescription>(image);
        ++next;
    };
    append(*color, Layer::Color);
    if (normal) append(*normal, Layer::Normal);
    if (material) append(*material, source.hasSubsurfaceChannel ? Layer::Mers : Layer::Mer);
    base.mBedrockTextureData->mTextureSetDescription->mLayerInfo->swap(layers);
}

void bindPbrLayers(BedrockTexture& destination, NativeTextureSet const& source) {
    destination.mBedrockNormalTextureData = source.normal;
    destination.mBedrockMERSTextureData = source.material;
}

struct NativeTextureUpload::Impl {
    std::weak_ptr<mce::TextureGroup> group;
    std::vector<std::unique_ptr<ResourceLocation>> locations;
    NativeTextureSet textures;
    ~Impl() {
        if (auto owner = group.lock())
            for (auto const& location : locations) owner->unloadTexture(*location, false);
    }
    BedrockTexture& upload(mce::TextureGroup& owner, TextureImage const& source,
        std::string const& path, PbrEncoding encoding, std::optional<PbrLayer> layer = {}) {
        if (!source.width || !source.height || source.rgba.size() != std::size_t(source.width) * source.height * 4)
            throw std::runtime_error("Invalid native texture image");
        mce::Image image(source.width, source.height, 1, mce::ImageFormat::RGBA8Unorm,
            layer ? mce::ImageUsage::Data : mce::ImageUsage::SRGB);
        image.mAlphaUsage = layer ? mce::AlphaUsage::Data : mce::AlphaUsage::Transparent;
        image.resizeImageBytesToFitImageDescription();
        if (layer) {
            auto usedSubsurface = convertPbrImage(source.rgba, {image.mImageBytes.data(), image.mImageBytes.size()}, encoding, *layer);
            if (*layer == PbrLayer::Material) textures.hasSubsurfaceChannel = usedSubsurface;
        } else std::memcpy(image.mImageBytes.data(), source.rgba.data(), source.rgba.size());
        auto& location = locations.emplace_back(std::make_unique<ResourceLocation>(Core::PathView(path)));
        auto& uploaded = owner.uploadTexture(*location, cg::ImageBuffer(std::move(image)));
        auto const& data = uploaded.mBedrockTextureData;
        if (!data || data->mIsMissingTexture != IsMissingTexture::No
            || data->mTextureDescription->mWidth != source.width || data->mTextureDescription->mHeight != source.height
            || !(static_cast<unsigned char>(data->mTextureLoadState) & static_cast<unsigned char>(TextureLoadState::LoadedBit)))
            throw std::runtime_error("Native texture upload failed: " + source.path);
        return uploaded;
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
    if (texture.normal) layers.normal = result->impl->upload(*group, *texture.normal, path + "_n", encoding, PbrLayer::Normal).mBedrockTextureData;
    if (texture.specular) layers.material = result->impl->upload(*group, *texture.specular, path + "_mers", encoding, PbrLayer::Material).mBedrockTextureData;
    auto& base = result->impl->upload(*group, *texture.uv, path, encoding);
    describePbrLayers(base, layers);
    bindPbrLayers(base, layers);
    layers.color = base.mBedrockTextureData;
    return result;
}
NativeTextureSet NativeTextureUpload::textures() const { return impl->textures; }
}
