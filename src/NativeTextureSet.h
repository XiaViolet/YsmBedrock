#pragma once
#include <memory>

class BedrockTexture;
struct BedrockTextureData;
namespace ysm {
struct NativeTextureSet {
    std::shared_ptr<BedrockTextureData const> color;
    std::shared_ptr<BedrockTextureData> normal, material;
    bool hasSubsurfaceChannel = false;
    bool operator==(NativeTextureSet const&) const = default;
};
void describePbrLayers(BedrockTexture& base, NativeTextureSet const& source);
void bindPbrLayers(BedrockTexture& destination, NativeTextureSet const& source);
}
