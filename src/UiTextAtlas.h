#pragma once
#include "YsmUi.h"
#include <span>
#include <map>
#include <tuple>
namespace ysm {
struct UiTextQuad {UiRect rect;std::array<float,4> uv;};
struct UiTextAtlas {
    TextureImage image;
    std::vector<UiTextQuad> quads;
    float pixelDensity=1;
};
struct UiTextMaskCache {
    using Key = std::tuple<std::string,int,int,int,int>;
    struct Mask { int width = 0, height = 0; std::vector<unsigned char> alpha; };
    std::map<Key,Mask> masks;
    std::size_t bytes = 0;
    void clear() { masks.clear(); bytes = 0; }
};
std::uint64_t uiTextKey(std::span<UiPaint const> paint,float pixelDensity);
UiTextAtlas rasterizeUiText(std::span<UiPaint const> paint,float pixelDensity,UiTextMaskCache* cache = nullptr);
}
