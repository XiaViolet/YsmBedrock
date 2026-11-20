#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <nlohmann/json.hpp>
#include "ImageCodec.h"
#include "PbrChannels.h"
#include "ModelAudio.h"
#include <map>
#include <span>

namespace ysm {
using Json = nlohmann::ordered_json;
void validateGeometry(Json const& document, std::size_t& boneCount, std::size_t& cubeCount);
std::string modelLocale(std::string locale);
Json const& modelLanguage(Json const& languages, std::string_view locale);
struct ModelTexture {
    std::string name;
    std::shared_ptr<TextureImage const> uv, normal, specular;
};
enum class ReplacedModelKind { Vehicle, Projectile };
std::string_view replacedModelKindName(ReplacedModelKind kind);
struct ReplacedModelTarget {
    std::string id;
    ReplacedModelKind kind = ReplacedModelKind::Projectile;
    std::vector<std::string> matches;
    Json geometry;
    Json animations{{"animations", Json::object()}};
    Json controllers{{"animation_controllers", Json::object()}};
    ModelTexture texture;
    std::size_t boneCount = 0, cubeCount = 0;
    bool individualBoneMaterials = false, boneTransparency = false;
    bool matchesEntity(std::string_view entityId, std::span<std::string const> registryTypeTags = {}, int boatVariant = -1) const;
    Json summary() const;
};
struct ModelPack {
    std::string id;
    std::string librarySource;
    std::string localContentKey;
    Json descriptor;
    Json languages = Json::object();
    Json mainGeometry;
    Json armGeometry;
    Json animations;
    Json controllers{{"animation_controllers", Json::object()}};
    Json functions = Json::object();
    std::vector<ModelTexture> textures;
    std::shared_ptr<TextureImage const> previewBackground, previewForeground;
    std::vector<ReplacedModelTarget> targets;
    std::map<std::string, std::shared_ptr<ModelAudio const>> sounds;
    std::size_t selectedTexture = 0;
    std::vector<std::string> warnings;
    std::size_t boneCount = 0;
    std::size_t cubeCount = 0;
    std::size_t animationCount = 0;
    double widthScale = 0.7, heightScale = 0.7;
    bool individualBoneMaterials = false;
    bool boneTransparency = false;
    PbrEncoding pbrEncoding = PbrEncoding::OldPbr;
    static bool isSourceDirectory(std::filesystem::path const& path);
    static ModelPack load(std::filesystem::path const& source);
    static ModelPack loadRoom(std::filesystem::path const& source,
        std::map<std::string, std::filesystem::path>* directoryMembers = nullptr);
    static Json uiInfo(std::filesystem::path const& source);
    Json catalogLanguages() const;
    Json summary() const;
    Json bodyAnimations() const;
    Json firstPersonAnimations() const;
    Json targetAnimations(ReplacedModelTarget const& target) const;
    ReplacedModelTarget const* findTarget(ReplacedModelKind kind, std::string_view entityId,
        std::span<std::string const> registryTypeTags = {}, int boatVariant = -1) const;
    Json renderGeometry() const;
    std::string renderKey() const;
    ModelTexture const& currentTexture() const { return textures.at(selectedTexture); }
    bool selectTexture(std::string const& name);
    std::optional<std::uint32_t> binaryOriginVersion() const { return legacyOriginVersion; }
private:
    static ModelPack loadImpl(std::filesystem::path const& source, bool roomInput,
        std::map<std::string, std::filesystem::path>* directoryMembers);
    std::optional<std::uint32_t> legacyOriginVersion;
    std::uint64_t geometryHash = 0;
    std::uint64_t hashRenderGeometry() const;
};
}
