#pragma once
#include "AnimationSampler.h"
#include <memory>

namespace ysm {
enum class ControllerPolicy { Strict, SourcePackage };
struct HandInput;
struct ReplacedAnimationInput {
    bool present = true, inWater = false, onGround = false, onFire = false;
    bool inGround = false, hasPassengers = false;
    Triple velocity{};
    int onGroundTicks = 0;
    bool biting = false, spectralArrow = false;
    std::string shootItemId, throwableItem, hookedEntity;
    MolangValue projectileOwner;
};
// Definitions are immutable. Each owner has its own state, child controllers,
// playback clocks and event cursors; drawing the same time only observes them.
class AnimationControllers {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    explicit AnimationControllers(Json const& document, ControllerPolicy policy = ControllerPolicy::Strict);
    ~AnimationControllers();
    bool contains(std::string const& name) const;
    std::vector<std::string> matching(std::string const& prefix) const;
    std::set<std::string> roamingFields(AnimationSampler const& sampler, bool firstPerson = false,
        RoamingLimit limit = RoamingLimit::WireCompatible) const;
    bool apply(std::string const& name, AnimationSampler const& sampler, double time,
        Pose& pose, Pose const& previous, AnimationEvaluation& evaluation);
    Pose samplePlayer(AnimationSampler const& sampler, std::string const& base, double baseTime, double time,
        AnimationEvaluation& evaluation, HandInput const& input, std::span<AnimationLayer const> handLayers,
        std::string const& extra, double extraTime, bool guiPreview = false);
    Pose sampleFirstPerson(AnimationSampler const& sampler, double time, AnimationEvaluation& evaluation,
        HandInput const& input);
    Pose sampleReplaced(AnimationSampler const& sampler, ReplacedModelKind kind, double time,
        AnimationEvaluation& evaluation, ReplacedAnimationInput const& input);
    // Re-evaluate continuous channels at the last accepted playback clocks.
    // No controller transitions, instructions, hooks or owner state advances.
    Pose observePlayer(AnimationSampler const& sampler, AnimationEvaluation& observation) const;
    bool hasActiveStageAnimation(std::string const& name) const;
    Json swingStates() const;
    Json report() const;
    void reset();
};
}
