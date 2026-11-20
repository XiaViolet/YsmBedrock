#include "NativePortraitParrots.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/resources/ResourcePackManager.h"

namespace ysm {
namespace {
Json readJson(ResourcePackManager const& packs, char const* name) {
    std::string bytes;
    if (!packs.load(ResourceLocation(Core::PathView(name)), bytes)) return {};
    return PreviewShoulders::readJson(bytes);
}
void loadModel(ResourcePackManager const& packs, Json& geometry, Pose& pose) {
    auto file = readJson(packs, "models/entity/parrot.geo.json");
    auto models = file.find("minecraft:geometry");
    if (models == file.end() || !models->is_array()) return;
    for (auto const& model : *models) {
        if (!model.is_object() || model.value("description", Json::object()).value("identifier", "") != "geometry.parrot") continue;
        auto document = Json{{"minecraft:geometry", Json::array({model})}};
        std::size_t bones = 0, cubes = 0;
        validateGeometry(document, bones, cubes);
        auto animations = readJson(packs, "animations/parrot.animation.json");
        if (!animations.is_object() || !animations.contains("animations")) return;
        AnimationSampler sampler(animations, document);
        AnimationEvaluation eval;
        std::array<AnimationLayer, 2> sitting{};
        sitting[0].name = "animation.parrot.base";
        sitting[1].name = "animation.parrot.sitting";
        Pose perched;
        sampler.applyLayers(perched, sitting, 0, eval);
        pose = std::move(perched);
        geometry = model;
        return;
    }
}
}

Json NativePortraitParrots::report() const {
    std::array<bool, 5> ready{};
    for (std::size_t i = 0; i < ready.size(); ++i) ready[i] = bool(textures[i]);
    return {{"variants", current}, {"geometry", geometry.is_object()}, {"skin_ready", ready}};
}

PreviewShoulders NativePortraitParrots::get(IClientInstance& client, std::array<int, 2> variants) {
    auto* player = client.getLocalPlayer();
    if (!player) return {};
    if (level != &player->getLevel()) {
        *this = {};
        level = &player->getLevel();
    }
    current = variants;
    if (variants[0] < 0 && variants[1] < 0) return {};
    auto& packs = client.getResourcePackManager();
    auto now = Clock::now();
    constexpr auto retry = std::chrono::seconds(2);
    if (!geometry.is_object() && now >= retryModel) {
        retryModel = now + retry;
        try { loadModel(packs, geometry, pose); }
        catch (std::exception const&) { return {}; }
    }
    if (!geometry.is_object()) return {};
    constexpr std::array<char const*, 5> names{"red_blue", "blue", "green", "yellow_blue", "grey"};
    PreviewShoulders result{&geometry, &pose};
    for (std::size_t side = 0; side < variants.size(); ++side) {
        int variant = variants[side];
        if (variant < 0 || variant >= static_cast<int>(names.size())) continue;
        auto& skin = textures[variant];
        if (!skin && now >= retrySkin[variant]) {
            retrySkin[variant] = now + retry;
            auto path = std::string("textures/entity/parrot/parrot_") + names[variant] + ".png";
            std::string encoded;
            try {
                if (packs.load(ResourceLocation(Core::PathView(path)), encoded) && !encoded.empty()) {
                    skin = std::make_shared<TextureImage>(PreviewShoulders::readSkin(path, encoded));
                }
            } catch (std::exception const&) { continue; }
        }
        result.skins[side] = skin.get();
    }
    return result;
}
}

