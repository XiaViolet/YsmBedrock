#pragma once
#include "ModelPack.h"
#include "NativeTextureSet.h"
#include <atomic>
#include <memory>
#include <functional>
class IClientInstance;
class Player;
class SerializedSkinRef;
struct ClientSkin;
class ClientSkinInfoData;
struct BedrockTextureData;
class ResourceLocation;
class ActorResourceDefinition;
namespace mce { class TextureGroup; struct Image; }
namespace ysm {
class SkinRuntime {
    struct TextureBindingState;
    struct PbrUploads;
    std::unique_ptr<PbrUploads> pbrUploads;
    std::unique_ptr<SerializedSkinRef> original;
    std::unique_ptr<SerializedSkinRef> applied;
    std::unique_ptr<mce::Image> renderImage;
    std::unique_ptr<ResourceLocation> textureLocation;
    std::weak_ptr<mce::TextureGroup> textureGroup;
    std::string renderId;
    std::string renderName;
    NativeTextureSet uploadedTexture;
    std::atomic<std::shared_ptr<TextureBindingState>> textureBinding;
    bool active = false;
public:
    SkinRuntime();
    ~SkinRuntime();
    void apply(IClientInstance& client, Player& player, ModelPack const& pack, std::string_view owner = {},
        std::string const* preparedGeometry = nullptr);
    void updateRenderTexture(IClientInstance& client, ClientSkinInfoData& skin, SerializedSkinRef const& serialized);
    void restore(IClientInstance& client, Player& player);
    bool isActive() const { return active; }
    NativeTextureSet renderTexture() const { return uploadedTexture; }
    std::string const& textureName() const { return renderName; }
    void bindResourceTexture(SerializedSkinRef const& skin, std::shared_ptr<ActorResourceDefinition> const& definition);
    void reset();
    void vanillaHands(Player& player, std::function<void()> const& draw);
};
}
