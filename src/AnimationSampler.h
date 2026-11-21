#pragma once
#include "ModelPack.h"
#include "ConditionalClips.h"
#include "Molang.h"
#include <array>
#include <map>
#include <optional>
#include <cstdint>

namespace ysm {
using Triple = std::array<double, 3>;
Triple sourceRotationLerp(Triple const& begin, Triple const& end, Triple const& initial, double progress);
struct BonePose {
    Triple position{};
    Triple rotation{};
    Triple scale{1, 1, 1};
    std::array<bool, 3> driven{};
};
using Pose = std::map<std::string, BonePose>;
// Samples borrow canonical names from the sampler that owns their clip.
// Controller playback and its sample buffers share that sampler's lifetime.
using ClipPose = std::vector<std::pair<std::string const*, BonePose>>;
class BonePoseHistory {
    struct Snapshot {
        BonePose origin;
        std::array<double, 3> updated{-1, -1, -1};
        std::array<bool, 3> active{};
    };
    std::map<std::string, Snapshot> snapshots;
    double lastTime = -1;
public:
    void apply(Pose& pose, double time, std::map<std::string, Triple> const& initialRotations,
        double resetSeconds = 3.0 / 20.0);
    void reset() { snapshots.clear(); lastTime = -1; }
};
enum class AnimationLoop { Authored, Loop, Once, Hold };
struct AnimationEventPosition {
    std::uint64_t cycle = 0;
    double clock = 0;
    // Java resets sound keys on muted loop updates, but keeps instruction keys.
    std::optional<std::uint64_t> instructionCycle = {};
};
struct AnimationLayer {
    std::string name;
    double time = 0;
    bool additiveRotation = false;
    std::string timelineId;
    bool restart = false;
    bool apply = true;
    bool emitEvents = true;
    bool finishEvents = false;
    AnimationLoop loop = AnimationLoop::Authored;
    std::optional<AnimationEventPosition> eventPosition = {};
};
struct AnimationClipInfo { double length; bool loop, hold; };
struct AnimationTimelineCursor {
    std::string animation;
    AnimationLoop loop = AnimationLoop::Authored;
    double time = -1;
    std::uint64_t cycle = 0;
    std::size_t next = 0;
    std::uint64_t soundCycle = 0;
    std::size_t nextSound = 0;
    bool ownsSounds = false;
};
struct AnimationEvaluation {
    static constexpr std::size_t InstructionStepsPerFrame = 600000;
    MolangContext expressions;
    std::map<std::string, AnimationTimelineCursor> timelineCursors;
    template<class Match> void retireTimelines(Match match) {
        std::erase_if(timelineCursors, [&](auto const& entry) {
            if (!match(entry)) return false;
            if (entry.second.ownsSounds) expressions.emitSound({entry.first, {}});
            return true;
        });
    }
    std::size_t remainingTimelineEvents = 4096;
    std::size_t remainingInstructionSteps = InstructionStepsPerFrame;
    std::string activeBase;
    double lastTime = -1;
    BonePoseHistory poseHistory;
    bool functionsInitialized = false;
};
class AnimationSampler {
    bool legacyMolang = false;
    struct VectorExpression {
        std::array<MolangProgram, 3> values;
        bool uniform = false;
        bool finiteLiterals = false;
        Triple sample(MolangContext& context, MolangValue& input, Triple const& current, double fallback) const;
    };
    struct Key { double time; VectorExpression pre, post; bool curved; };
    struct Track {
        std::vector<Key> keys;
        Triple sample(double time, MolangContext& context, MolangValue& input, Triple const& current, double fallback) const;
    };
    struct Channels { std::array<std::optional<Track>, 3> values; };
    struct Event { double time; std::vector<MolangProgram> programs; };
    struct Clip {
        double length = 0;
        bool loop = false, hold = false;
        std::map<std::string, Channels> bones;
        std::vector<Event> events;
        std::vector<std::pair<double, std::string>> sounds;
        std::optional<MolangProgram> weight;
    };
    std::map<std::string, Clip> clips;
    ConditionalClips conditions;
    std::set<std::string> headBones;
    std::map<std::string, Triple> initialRotations;
    std::shared_ptr<MolangContext::BoneRig const> boneRig;
    BoneVisuals initialVisuals;
    std::size_t unsupported = 0;
    std::vector<std::string> diagnostics;
    std::shared_ptr<MolangFunctionLibrary const> functions;
    bool controllerActions = false;
    std::set<std::string> roamingNames;
    void layer(Pose& pose, std::string const& name, double time, MolangContext& context, bool additiveRotation, AnimationLoop loop) const;
public:
    explicit AnimationSampler(Json const& document, Json const& geometry = Json());
    Pose sample(std::string const& state, double time) const;
    Pose sample(std::string const& state, double stateTime, double parallelTime, AnimationEvaluation& evaluation,
        std::span<AnimationLayer const> overlays = {}) const;
    static void beginFrame(double time, AnimationEvaluation& evaluation);
    void bindFunctions(AnimationEvaluation& evaluation) const;
    std::shared_ptr<MolangFunctionLibrary const> functionLibrary() const { return functions; }
    bool needsControllerActions() const { return controllerActions; }
    BoneVisuals const& declaredVisuals() const { return initialVisuals; }
    std::set<std::string> roamingFields(bool firstPerson = false) const;
    static std::uint64_t advanceCycle(std::uint64_t current, double loops, MolangContext& context);
    void applyLayers(Pose& pose, std::span<AnimationLayer const> layers, double time, AnimationEvaluation& evaluation) const;
    void advanceEvents(std::span<AnimationLayer const> layers, double time, AnimationEvaluation& evaluation) const;
    void sampleClip(Pose& pose, AnimationLayer const& selected, MolangContext& context) const;
    void sampleRawClip(ClipPose& sampled, Pose const& lower, std::string const& name,
        double time, MolangContext& context) const;
    double blendWeight(std::string const& name, double time, MolangContext& context) const;
    Triple blendRotation(std::string const& bone, Triple const& begin, Triple const& end, double progress) const {
        auto found = initialRotations.find(bone);
        return sourceRotationLerp(begin, end, found == initialRotations.end() ? Triple{} : found->second, progress);
    }
    void finishPose(Pose& pose, MolangContext& context, bool codedHead = true) const;
    void recoverHeadPose(Pose const& authored, MolangContext& context) const;
    ConditionalClips const& conditionalClips() const { return conditions; }
    void resetUndriven(Pose& pose, double time, BonePoseHistory& history, double resetSeconds = 3.0 / 20.0) const;
    std::optional<AnimationClipInfo> clipInfo(std::string const& name, AnimationLoop loop = AnimationLoop::Authored) const;
    bool hasClip(std::string const& name) const { return clips.contains(name); }
    bool hasNonemptyClip(std::string const& name) const;
    std::size_t unsupportedChannels() const { return unsupported; }
    std::vector<std::string> const& compileDiagnostics() const { return diagnostics; }
    static std::string normalized(std::string name);
    static Json report(Pose const& pose);
};
}
