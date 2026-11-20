#pragma once
#include <memory>

struct BedrockTextureData;
namespace ysm {
struct NativeTextureSet {
    std::shared_ptr<BedrockTextureData const> color;
    std::shared_ptr<BedrockTextureData> normal, material;
};
}
