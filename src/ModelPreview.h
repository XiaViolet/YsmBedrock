#pragma once
#include "AnimationSampler.h"
#include "AnimationControllers.h"
#include "EntityQueries.h"
#include <memory>
namespace ysm {
struct PreviewCamera {
    float yaw = 15, pitch = -7, zoom = 1, panX = 0, panY = 0;
    bool operator==(PreviewCamera const&) const = default;
};
struct PreviewShoulders {
    static Json readJson(std::string_view bytes);
    static TextureImage readSkin(std::string path, std::string_view bytes);
    Json const* model = nullptr;
    Pose const* pose = nullptr;
    std::array<TextureImage const*, 2> skins{};
};
struct PreviewPose {
    Pose bones;
    BoneVisuals visuals;
};

// Borrowed until this preview's next sample. Each preview has one evaluating thread.
struct PreviewPoseView {
    Pose const& bones;
    BoneVisuals const& visuals;
};

struct PreviewMeshVertex {
    float x, y, z, u, v;
    std::uint32_t color;
};
struct PreviewMesh {
    std::vector<PreviewMeshVertex> vertices;
    std::shared_ptr<TextureImage const> skin, background, foreground;
};

// Independent GUI evaluator. Script variables and authored visibility stay in
// this private owner; no audio, particles or state are delivered to the world.
class ModelPreview {
    Json geometry;
    std::vector<ModelTexture> textures;
    std::shared_ptr<TextureImage const> background, foreground;
    bool noLighting = false;
    bool forceCutout = false;
    std::shared_ptr<AnimationSampler const> sampler;
    mutable AnimationControllers controllers;
    mutable AnimationEvaluation evaluation;
    mutable EntityQueries queries;
    mutable Pose lastPose;
    struct GeometryFrame;
    GeometryFrame collect(std::string const& clip, double time, PreviewCamera const& view,
        std::size_t texture, PreviewPose const* live, PreviewShoulders shoulders) const;
public:
    static constexpr int BodyFramePixels = 224;
    explicit ModelPreview(ModelPack const& source, std::shared_ptr<AnimationSampler const> bodySampler = {});
    PreviewPoseView samplePose(std::string const& clip, double time, std::size_t texture) const;
    PreviewMesh mesh(std::string const& clip, double time, PreviewCamera const& view, std::size_t texture, PreviewPose const* live = nullptr) const;
    TextureImage render(std::string clip, double time, PreviewCamera const& view, std::size_t texture,
        bool decorations = true, PreviewPose const* live = nullptr, PreviewShoulders shoulders = {}) const;
    Json report() const;
};
}
