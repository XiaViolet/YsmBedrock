#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ysm {
enum class PbrEncoding { OldPbr, LabPbr13 };
enum class PbrLayer { Normal, Material };
using PbrTexel = std::array<std::uint8_t, 4>;

inline PbrTexel convertPbrTexel(unsigned char const* pixel, PbrEncoding encoding, PbrLayer layer) {
    auto [r, g, b, a] = PbrTexel{pixel[0], pixel[1], pixel[2], pixel[3]};
    if (layer == PbrLayer::Normal) {
        if (encoding == PbrEncoding::LabPbr13) {
            double x = r / 127.5 - 1, y = g / 127.5 - 1;
            double z = std::sqrt(std::max(0.0, 1.0 - x*x - y*y));
            b = static_cast<std::uint8_t>(std::lround((z + 1) * 127.5));
        }
        return {r, g, b, 255};
    }
    auto roughness = static_cast<std::uint8_t>(255 - r);
    if (encoding == PbrEncoding::OldPbr) return {g, b, roughness, 0};
    bool metal = g >= 230;
    auto emission = static_cast<std::uint8_t>(a == 255 ? 0 : (a * 255 + 127) / 254);
    auto subsurface = static_cast<std::uint8_t>(metal || b <= 65 ? 0 : ((b - 65) * 255 + 95) / 190);
    return {static_cast<std::uint8_t>(metal ? 255 : 0), emission, roughness, subsurface};
}
}
