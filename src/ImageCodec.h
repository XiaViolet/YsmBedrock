#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ysm {
struct TextureImage {
    std::string path;
    std::uint32_t width = 0, height = 0;
    std::vector<unsigned char> encoded, rgba;
    std::string format;
};
TextureImage decodeTexture(std::string path, std::vector<unsigned char> bytes, std::size_t decodedBudget);
}
