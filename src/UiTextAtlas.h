#pragma once
#include "YsmUi.h"
#include <span>
namespace ysm {
struct UiTextQuad {UiRect rect;std::array<float,4> uv;};
struct UiTextAtlas {
    TextureImage image;
    std::vector<UiTextQuad> quads;
    float pixelDensity=1;
};
std::uint64_t uiTextKey(std::span<UiPaint const> paint,float pixelDensity);
UiTextAtlas rasterizeUiText(std::span<UiPaint const> paint,float pixelDensity);
}
