#pragma once
#include "AnimationSampler.h"

namespace ysm {
enum class CodedPlayState { Continue, Stop, Pause };

// One coded controller's playback, never shared with another owner or pass.
// Requests use the entity clock; authored keyframes and event cursors use the
// player's own clock, including its beginning and ending transitions.
class CodedAnimationPlayer {
    enum class Phase { Idle, Beginning, Running, Ending };
    std::string owner, name;
    AnimationLoop loop = AnimationLoop::Authored;
    Phase phase = Phase::Idle;
    double entered = 0, beginningLength = 0, endingAt = -1, endClock = 0;
    double lastTime = -1, lastWeight = 1, loopOffset = 0;
    double sampledTime = 0;
    std::size_t generation = 0;
    std::uint64_t eventCycle = 0, soundCycle = 0;
    bool finished = true, reload = false;
    bool sampledAdditive = false;
    Pose from;
    ClipPose raw, output;
    std::map<double, double> blendPoints;
    CodedPlayState lastCommand = CodedPlayState::Stop;
    void beginEnding(double time, double clock);
public:
    explicit CodedAnimationPlayer(std::string name = {}) : owner(std::move(name)) {}
    void process(AnimationSampler const& sampler, AnimationLayer const* request, double time,
        Pose& pose, Pose const& previous, AnimationEvaluation& evaluation, double transition,
        CodedPlayState command = CodedPlayState::Continue, std::map<double, double> const& points = {},
        MolangValue* activeStatus = nullptr);
    void reset(AnimationEvaluation& evaluation);
    void observe(AnimationSampler const& sampler, Pose& pose, MolangContext& context) const;
    bool currentAnimationFinished() const { return finished; }
    bool active() const { return phase != Phase::Idle; }
    double animationTime() const { return sampledTime; }
    Json report() const;
};
}
