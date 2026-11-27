#pragma once
#include "ModelPreview.h"
#include <chrono>

class IClientInstance;
class Level;
namespace ysm {
class NativePortraitParrots {
    using Clock = std::chrono::steady_clock;
    Level const* level = nullptr;
    std::shared_ptr<Json const> geometry;
    std::shared_ptr<Pose const> pose;
    std::array<std::shared_ptr<TextureImage>, 5> textures{};
    std::array<Clock::time_point, 5> retrySkin{};
    Clock::time_point retryModel{};
    std::array<int, 2> current{-1, -1};
public:
    Json report() const;
    struct Snapshot {
        std::shared_ptr<Json const> geometry;
        std::shared_ptr<Pose const> pose;
        std::array<std::shared_ptr<TextureImage const>, 2> skins{};
        PreviewShoulders view() const { return {geometry.get(), pose.get(), {skins[0].get(), skins[1].get()}}; }
    };
    Snapshot get(IClientInstance& client, std::array<int, 2> variants);
};
}

