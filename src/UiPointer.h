#pragma once
#include <cmath>
#include <optional>

namespace ysm {
struct UiPoint { float x, y; };
// Physical client pixels and native GUI units are separate coordinate spaces.
// A button event need not carry a position: retain the last motion instead of
// replacing it with its often-zero x/y fields.
class UiPointer {
    std::optional<UiPoint> pixels;
public:
    void reset() { pixels.reset(); }
    std::optional<UiPoint> feed(int button, float eventX, float eventY,
        std::optional<UiPoint> windowPosition, float clientWidth, float clientHeight,
        float guiWidth, float guiHeight) {
        if(windowPosition) pixels=windowPosition;
        else if(button==0 && std::isfinite(eventX) && std::isfinite(eventY)) pixels=UiPoint{eventX,eventY};
        if(!pixels || clientWidth<=0 || clientHeight<=0 || guiWidth<=0 || guiHeight<=0)return {};
        return UiPoint{pixels->x*guiWidth/clientWidth,pixels->y*guiHeight/clientHeight};
    }
};
}
