#include "SkinRuntime.h"
#include "ClientGeometry.h"
#include "ContentHash.h"
#include "mc/client/components/ClientSkinSystem.h"
#include "mc/client/components/ClientSkin.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/renderer/TextureGroup.h"
#include "mc/deps/core/image/Image.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core_graphics/ImageBuffer.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/client/renderer/actor/ActorResourceDefinition.h"
#include "mc/client/renderer/actor/ActorTextureInfo.h"
#include "mc/client/model/GeometryInfo.h"
#include "mc/client/model/GeometryPtr.h"
#include "mc/deps/json/Reader.h"
#include "mc/resources/MinEngineVersion.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/SerializedSkinRef.h"
#include "mc/world/actor/player/SerializedSkinImpl.h"
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <fstream>
#include <atomic>
#include "ll/api/mod/NativeMod.h"

namespace ysm {
void SkinRuntime::vanillaHands(Player& player, std::function<void()> const& draw) {
    if (!active || !original || !player.mSkin) { draw(); return; }
    SerializedSkinRef saved; saved = *player.mSkin;
    *player.mSkin = *original;
    try { draw(); } catch (...) { *player.mSkin = saved; throw; }
    *player.mSkin = saved;
}
struct SkinRuntime::TextureBindingState {
    std::string id;
    NativeTextureSet textures;
    std::filesystem::path diagnostics;
};
struct SkinRuntime::PbrUploads {
    std::weak_ptr<mce::TextureGroup> group;
    std::vector<std::unique_ptr<ResourceLocation>> locations;
    bool hasSubsurface = false;
    explicit PbrUploads(std::shared_ptr<mce::TextureGroup> const& owner) : group(owner) {}
    ~PbrUploads() {
        if (auto owner = group.lock())
            for (auto const& location : locations) owner->unloadTexture(*location, false);
    }
    std::shared_ptr<BedrockTextureData> upload(mce::TextureGroup& owner, std::string const& path,
        TextureImage const& source, PbrEncoding encoding, PbrLayer layer) {
        mce::Image image(source.width, source.height, 1, mce::ImageFormat::RGBA8Unorm, mce::ImageUsage::Data);
        image.mAlphaUsage = mce::AlphaUsage::Data;
        image.resizeImageBytesToFitImageDescription();
        auto usedSubsurface = convertPbrImage(source.rgba, {image.mImageBytes.data(), image.mImageBytes.size()}, encoding, layer);
        if (layer == PbrLayer::Material) hasSubsurface = usedSubsurface;
        auto& location = locations.emplace_back(std::make_unique<ResourceLocation>(Core::PathView(path)));
        auto data = owner.uploadTexture(*location, cg::ImageBuffer(std::move(image))).mBedrockTextureData;
        if (!data || data->mIsMissingTexture != IsMissingTexture::No
            || data->mTextureDescription->mWidth != source.width || data->mTextureDescription->mHeight != source.height
            || !(static_cast<unsigned char>(data->mTextureLoadState) & static_cast<unsigned char>(TextureLoadState::LoadedBit)))
            throw std::runtime_error("PBR texture upload failed: " + source.path);
        return data;
    }
};
SkinRuntime::SkinRuntime() = default;
SkinRuntime::~SkinRuntime() = default;

void SkinRuntime::apply(IClientInstance& client, Player& player, ModelPack const& pack, std::string_view owner,
    std::string const* preparedGeometry) {
    auto key = pack.renderKey();
    // The same key is used in geometry identifiers and texture paths. Room
    // identities (e.g. "uuid:...") are not Bedrock resource path components.
    if (!owner.empty()) {
        auto bytes = std::span(reinterpret_cast<unsigned char const*>(owner.data()), owner.size());
        key += "_p" + contentSha256(bytes);
    }
    auto skinId = "ysm.client." + key;
    auto reducedId = "ysm.client.reduced." + key;
    if (active && (renderId == skinId || renderId == reducedId)) {
        renderName = pack.currentTexture().name;
        return;
    }
    bool premium = true;
    auto const& source = *pack.currentTexture().uv;
    int width = static_cast<int>(source.width), height = static_cast<int>(source.height);
    mce::Image image(width, height, 1, mce::ImageFormat::RGBA8Unorm, mce::ImageUsage::SRGB);
    image.mAlphaUsage = mce::AlphaUsage::Transparent;
    image.resizeImageBytesToFitImageDescription();
    std::memcpy(image.mImageBytes.data(), source.rgba.data(), source.rgba.size());
    auto transportImage = image.clone();
    transportImage.mAlphaUsage = mce::AlphaUsage::Transparent;

    auto geometryName = "geometry.ysm_bedrock." + key;
    std::string fallbackGeometry;
    if (!preparedGeometry) {
        fallbackGeometry = ClientGeometry::build(pack).document.dump();
        preparedGeometry = &fallbackGeometry;
    }
    auto readGeometry = [&] {
        ::Json::Value geometry;
        ::Json::Reader reader;
        if (!reader.parse(*preparedGeometry, geometry, false)) throw std::runtime_error("Game rejected model JSON");
        geometry["minecraft:geometry"][0]["description"]["identifier"] = ::Json::Value(geometryName);
        return geometry;
    };
    ysm::Json patch = {{"geometry", {{"default", geometryName}}}};
    auto version = MinEngineVersion::fromString("1.21.0");
    mce::Image cape;
    auto next = std::make_unique<SerializedSkinRef>(
        skinId, patch.dump(),
        image, static_cast<ushort>(width), static_cast<ushort>(height),
        cape, static_cast<ushort>(0), static_cast<ushort>(0), readGeometry(), version,
        "", "", true, premium, false, false, false, true);
    next->setFullId(skinId);
    next->setProfileHash(skinId);
    auto& nextData = next->mSkinImpl->mObject;
    nextData.mSkinImage->mDepth = 1;
    nextData.mSkinImage->mAlphaUsage = mce::AlphaUsage::Transparent;
    auto trace = Json{{"incoming_id", nextData.mId}, {"full_id", nextData.mFullId},
        {"geometry", nextData.mDefaultGeometryName}, {"resource_patch", nextData.mResourcePatch},
        {"image_width", nextData.mSkinImage->mWidth}, {"image_height", nextData.mSkinImage->mHeight},
        {"image_depth", nextData.mSkinImage->mDepth}, {"image_bytes", nextData.mSkinImage->mImageBytes.size()},
        {"skin_valid", next->isValid()}};
    auto diagnostics = ll::mod::NativeMod::current()->getModDir() / "skin-diagnostics.json";
    std::ofstream(diagnostics) << trace.dump(2);
    if (!next->isValid()) {
        mce::Image reduced(128, 128, 1, mce::ImageFormat::RGBA8Unorm, mce::ImageUsage::SRGB);
        reduced.mAlphaUsage = mce::AlphaUsage::Transparent;
        reduced.resizeImageBytesToFitImageDescription();
        for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x)
            std::memcpy(reduced.mImageBytes.data() + (y * 128 + x) * 4,
                image.mImageBytes.data() + ((y * height / 128) * width + x * width / 128) * 4, 4);
        auto trial = std::make_unique<SerializedSkinRef>(
            reducedId, patch.dump(),
            reduced, static_cast<ushort>(128), static_cast<ushort>(128), cape,
            static_cast<ushort>(0), static_cast<ushort>(0), readGeometry(), version,
            "", "", true, premium, false, false, false, true);
        trial->setFullId(reducedId);
        trial->setProfileHash(reducedId);
        trace["trial_128_valid"] = trial->isValid();
        if (trial->isValid()) {
            next = std::move(trial);
            transportImage = std::move(reduced);
            trace["skin_transport_texture_size"] = 128;
        }
        std::ofstream(diagnostics) << trace.dump(2);
    }
    if (!next->isValid()) throw std::runtime_error("Game rejected serialized skin; see skin-diagnostics.json");
    auto nextId = *next->mSkinImpl->mObject.mId;
    auto nextName = pack.currentTexture().name;
    auto nextImage = std::make_unique<mce::Image>(std::move(image));
    if (!active) {
        if (!player.mSkin) throw std::runtime_error("Player skin is not ready");
        original = std::make_unique<SerializedSkinRef>();
        *original = *player.mSkin;
    }
    auto nextBinding = std::make_shared<TextureBindingState>();
    nextBinding->id = nextId;
    nextBinding->diagnostics = ll::mod::NativeMod::current()->getModDir() / "early-texture-bindings.json";
    // Publish the full-resolution texture before the native geometry's first bake.
    auto group = client.getTextureGroup();
    if (!group) throw std::runtime_error("Client texture group is not ready");
    auto basePath = "textures/ysm_bedrock/" + nextId;
    auto nextLocation = std::make_unique<ResourceLocation>(Core::PathView(basePath));
    auto nextLayers = std::make_unique<PbrUploads>(group);
    NativeTextureSet nextSet;
    auto const& variant = pack.currentTexture();
    if (variant.normal) nextSet.normal = nextLayers->upload(*group, basePath + "_n", *variant.normal, pack.pbrEncoding, PbrLayer::Normal);
    if (variant.specular) nextSet.material = nextLayers->upload(*group, basePath + "_mers", *variant.specular, pack.pbrEncoding, PbrLayer::Material);
    nextSet.hasSubsurfaceChannel = nextLayers->hasSubsurface;
    auto& uploaded = group->uploadTexture(*nextLocation, cg::ImageBuffer(nextImage->clone()));
    auto nextTexture = uploaded.mBedrockTextureData;
    if (!nextTexture || nextTexture->mIsMissingTexture != IsMissingTexture::No
        || nextTexture->mTextureDescription->mWidth != nextImage->mWidth
        || nextTexture->mTextureDescription->mHeight != nextImage->mHeight
        || (static_cast<unsigned char>(nextTexture->mTextureLoadState) & static_cast<unsigned char>(TextureLoadState::LoadedBit)) == 0) {
        group->unloadTexture(*nextLocation, false);
        throw std::runtime_error("Original-resolution texture upload did not complete");
    }
    nextSet.color = nextTexture;
    nextBinding->textures = nextSet;
    describePbrLayers(uploaded, nextSet);
    bindPbrLayers(uploaded, nextSet);
    auto oldActive = active;
    auto oldImage = std::move(renderImage);
    auto oldLocation = std::move(textureLocation);
    auto oldGroup = textureGroup;
    auto oldId = std::move(renderId);
    auto oldName = std::move(renderName);
    auto oldTexture = uploadedTexture;
    auto oldBinding = textureBinding.load(std::memory_order_acquire);
    active = true;
    renderImage = std::move(nextImage);
    renderId = std::move(nextId);
    renderName = std::move(nextName);
    textureLocation = std::move(nextLocation);
    textureGroup = group;
    auto oldLayers = std::exchange(pbrUploads, std::move(nextLayers));
    uploadedTexture = std::move(nextSet);
    textureBinding.store(std::move(nextBinding), std::memory_order_release);
    try {
        player.updateSkin(*next, static_cast<int>(client.getClientSubId()));
        ClientSkinSystem::applySkinToPlayer(client, player, *next);
        ClientSkinSystem::createOrUpdate(client, player.getAnimationComponent(), *next, &transportImage, &cape, false, [] {});
    } catch (...) {
        group->unloadTexture(*textureLocation, false);
        active = oldActive;
        renderImage = std::move(oldImage);
        textureLocation = std::move(oldLocation);
        textureGroup = oldGroup;
        renderId = std::move(oldId);
        renderName = std::move(oldName);
        uploadedTexture = std::move(oldTexture);
        pbrUploads = std::move(oldLayers);
        textureBinding.store(std::move(oldBinding), std::memory_order_release);
        try {
            auto previous = oldActive ? applied.get() : original.get();
            if (previous) {
                player.updateSkin(*previous, static_cast<int>(client.getClientSubId()));
                ClientSkinSystem::applySkinToPlayer(client, player, *previous);
            }
            if (!oldActive) reset();
        } catch (...) { reset(); }
        throw;
    }
    if (auto previousGroup = oldGroup.lock(); previousGroup && oldLocation)
        previousGroup->unloadTexture(*oldLocation, false);
    if (player.mSkin) trace["player_skin_id"] = player.mSkin->mSkinImpl->mObject.mId;
    trace["skin_initialized"] = ClientSkinSystem::isAnySkinOfNameInitialized(client, *next->mSkinImpl->mObject.mId);
    trace["transport_image_width"] = transportImage.mWidth;
    trace["render_image_width"] = renderImage->mWidth;
    trace["texture_name"] = pack.currentTexture().name;
    trace["texture_set_layers"] = uploadedTexture.color->mTextureSetDescription->mLayerInfo->size();
    trace["material_layout"] = !uploadedTexture.material ? "none" : uploadedTexture.hasSubsurfaceChannel ? "MERS" : "MER";
    trace["pbr_normal_uploaded"] = static_cast<bool>(uploadedTexture.normal);
    trace["pbr_material_uploaded"] = static_cast<bool>(uploadedTexture.material);
    std::ofstream(diagnostics) << trace.dump(2);
    applied = std::move(next);
}

bool SkinRuntime::updateRenderTexture(IClientInstance& client, ClientSkinInfoData& skin, SerializedSkinRef const& serialized) {
    if (!active || !renderImage || *serialized.mSkinImpl->mObject.mId != renderId) return false;
    auto group = client.getTextureGroup();
    if (!group) return false;
    if (!textureLocation) textureLocation = std::make_unique<ResourceLocation>(Core::PathView("textures/ysm_bedrock/" + renderId));
    auto& uploaded = group->uploadTexture(*textureLocation, cg::ImageBuffer(renderImage->clone()));
    auto const& data = uploaded.mBedrockTextureData;
    if (!data || data->mIsMissingTexture != IsMissingTexture::No
        || data->mTextureDescription->mWidth != renderImage->mWidth
        || data->mTextureDescription->mHeight != renderImage->mHeight
        || (static_cast<unsigned char>(data->mTextureLoadState) & static_cast<unsigned char>(TextureLoadState::LoadedBit)) == 0)
        throw std::runtime_error("Original-resolution texture upload did not complete");
    describePbrLayers(uploaded, uploadedTexture);
    bindPbrLayers(uploaded, uploadedTexture);
    auto& target = *skin.mSkinTexture;
    target.mBedrockTextureData = uploaded.mBedrockTextureData;
    bindPbrLayers(target, uploadedTexture);
    textureGroup = group;
    uploadedTexture.color = data;
    auto binding = std::make_shared<TextureBindingState>();
    binding->id = renderId;
    binding->textures = uploadedTexture;
    binding->diagnostics = ll::mod::NativeMod::current()->getModDir() / "early-texture-bindings.json";
    textureBinding.store(std::move(binding), std::memory_order_release);
    std::ofstream(ll::mod::NativeMod::current()->getModDir() / "texture-status.json") << Json{
        {"width", data->mTextureDescription->mWidth}, {"height", data->mTextureDescription->mHeight},
        {"load_state", static_cast<unsigned char>(data->mTextureLoadState)},
        {"missing", data->mIsMissingTexture != IsMissingTexture::No},
        {"serialized_skin_valid", serialized.isValid()}, {"render_id", renderId}}.dump(2);
    return true;
}

void SkinRuntime::restore(IClientInstance& client, Player& player) {
    if (active && original) {
        player.updateSkin(*original, static_cast<int>(client.getClientSubId()));
        ClientSkinSystem::applySkinToPlayer(client, player, *original);
    }
    reset();
}
NativeTextureSet SkinRuntime::publishedTextures() const {
    auto state = textureBinding.load(std::memory_order_acquire);
    return state ? state->textures : NativeTextureSet{};
}
void SkinRuntime::bindResourceTexture(SerializedSkinRef const& skin, std::shared_ptr<ActorResourceDefinition> const& definition) {
    auto binding = textureBinding.load(std::memory_order_acquire);
    if (!binding || *skin.mSkinImpl->mObject.mId != binding->id || !definition) return;
    bool ownGeometry = std::any_of(definition->mGeometryMap->begin(), definition->mGeometryMap->end(), [](auto const& entry) {
        return entry.second.mGeometryInfoPtr && entry.second.mGeometryInfoPtr->mName->getString().starts_with("geometry.ysm_bedrock.");
    });
    auto found = definition->mTextureMap->find("default");
    if (!ownGeometry || found == definition->mTextureMap->end()) return;
    // Apply before native render data is extracted. The Java replacement has
    // no humanoid armor layer; equipment remains available to YSM animations.
    definition->mHideArmor = true;
    auto& target = found->second.mTexturePtrs->mColorTexture->mClientTexture;
    auto before = target ? target->mTextureDescription->mWidth : 0;
    target = binding->textures.color;
    std::ofstream(binding->diagnostics) << Json{
        {"previous_width", before}, {"width", target->mTextureDescription->mWidth},
        {"height", target->mTextureDescription->mHeight}, {"own_geometry", ownGeometry},
        {"enable_attachables", static_cast<bool>(definition->mEnableAttachables)},
        {"hide_armor", static_cast<bool>(definition->mHideArmor)}}.dump(2);
}

void SkinRuntime::reset() {
    textureBinding.store({}, std::memory_order_release);
    active = false;
    applied.reset();
    original.reset();
    renderImage.reset();
    uploadedTexture = {};
    pbrUploads.reset();
    if (auto group = textureGroup.lock(); group && textureLocation) group->unloadTexture(*textureLocation, false);
    textureGroup.reset();
    textureLocation.reset();
    renderId.clear();
    renderName.clear();
}
}
