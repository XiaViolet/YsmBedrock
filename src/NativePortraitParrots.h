#pragma once
#include "ModelPreview.h"
#include <chrono>

class IClientInstance;
class Level;
namespace ysm {
class NativePortraitParrots {
    using Clock = std::chrono::steady_clock;
    Level const* level = nullptr;
    Json geometry;
    Pose pose;
    std::array<std::shared_ptr<TextureImage>, 5> textures{};
    std::array<Clock::time_point, 5> retrySkin{};
    Clock::time_point retryModel{};
    std::array<int, 2> current{-1, -1};
public:
    Json report() const;
    PreviewShoulders get(IClientInstance& client, std::array<int, 2> variants);
};
}

