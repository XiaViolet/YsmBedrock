#include "CodedAnimationPlayer.h"
#include <algorithm>
#include <cmath>

namespace ysm {
namespace {
Triple& channel(BonePose& bone, int c) { return c == 0 ? bone.position : c == 1 ? bone.rotation : bone.scale; }
BonePose const& poseAt(Pose const& pose, Pose::const_iterator& cursor, std::string const& bone, bool dense) {
    static BonePose const empty;
    if (dense) {
        while (cursor != pose.end() && cursor->first < bone) ++cursor;
    } else cursor = pose.find(bone);
    return cursor != pose.end() && cursor->first == bone ? cursor->second : empty;
}
void assign(Pose& target, ClipPose const& source) {
    auto next = target.begin();
    bool dense = source.size() >= target.size() / 4;
    for (auto const& [name, bone] : source) {
        if (dense) {
            while (next != target.end() && next->first < *name) ++next;
            if (next == target.end() || next->first != *name) next = target.try_emplace(next, *name);
        } else next = target.try_emplace(*name).first;
        auto& value = (next++)->second;
        for (int c = 0; c < 3; ++c) if (bone.driven[c]) {
            channel(value, c) = c == 0 ? bone.position : c == 1 ? bone.rotation : bone.scale;
            value.driven[c] = true;
        }
    }
}
}
void CodedAnimationPlayer::reset(AnimationEvaluation& evaluation) {
    auto prefix = "coded:" + owner + ":";
    evaluation.retireTimelines([&](auto const& cursor) { return cursor.first.starts_with(prefix); });
    auto keepOwner = owner;
    *this = CodedAnimationPlayer(std::move(keepOwner));
}
void CodedAnimationPlayer::beginEnding(double time, double clock) {
    endingAt = time;
    endClock = clock;
    phase = Phase::Ending;
    finished = true;
}
void CodedAnimationPlayer::process(AnimationSampler const& sampler, AnimationLayer const* request, double time,
    Pose& pose, Pose const& previous, AnimationEvaluation& evaluation, double transition, CodedPlayState command,
    std::map<double, double> const& points, MolangValue* activeStatus) {
    if (!std::isfinite(time) || time < 0 || !std::isfinite(transition) || transition < 0)
        throw std::runtime_error("Invalid coded player time");
    MolangDeferredScope deferred(evaluation.expressions, this);
    if (lastTime > time) reset(evaluation);
    bool select = command == CodedPlayState::Continue && request && !request->name.empty();
    bool changed = select && (request->name != name || request->loop != loop || request->restart || reload);
    if (!changed && lastTime == time && lastCommand == command) { assign(pose, output); return; }
    if (changed) {
        auto prefix = "coded:" + owner + ":";
        evaluation.retireTimelines([&](auto const& cursor) { return cursor.first.starts_with(prefix); });
        name = request->name;
        loop = request->loop;
        ++generation;
        entered = time;
        beginningLength = transition;
        loopOffset = 0;
        eventCycle = soundCycle = 0;
        blendPoints = points;
        endingAt = -1;
        from = previous.empty() ? pose : previous;
        raw.clear();
        reload = false;
        finished = !sampler.hasClip(name);
        phase = finished ? Phase::Idle : transition > 0 ? Phase::Beginning : Phase::Running;
    }
    lastTime = time;
    lastCommand = command;
    if (activeStatus) *activeStatus = active() ? 1.0 : 0.0;
    auto info = sampler.clipInfo(name, loop);
    if (!info || phase == Phase::Idle) { output.clear(); return; }
    auto& context = evaluation.expressions;
    constexpr double endingLength = 3.0 / 20.0;
    double local = std::max(0.0, time - entered - beginningLength);
    bool beginning = phase == Phase::Beginning && time - entered < beginningLength;
    bool stopped = command == CodedPlayState::Stop && phase != Phase::Ending;
    if (stopped) {
        auto key = "coded:" + owner + ":" + std::to_string(generation);
        if (auto cursor = evaluation.timelineCursors.find(key); cursor != evaluation.timelineCursors.end()
            && cursor->second.ownsSounds && evaluation.expressions.soundCues.size() < 4096) evaluation.expressions.emitSound({key, {}});
        beginEnding(time, phase == Phase::Beginning ? 0 : std::min(std::max(0.0, local - loopOffset), info->length));
        reload = true;
    } else if (phase == Phase::Ending && time - endingAt >= endingLength) {
        phase = Phase::Idle;
        if (activeStatus) *activeStatus = 0.0;
        raw.clear();
        output.clear();
        return;
    } else if (phase == Phase::Running && !changed && !info->loop && !info->hold && local >= info->length) {
        if (command != CodedPlayState::Pause) {
            AnimationLayer ending{name, local, false, "coded:" + owner + ":" + std::to_string(generation)};
            ending.loop = loop;
            ending.finishEvents = true;
            sampler.advanceEvents(std::span<AnimationLayer const>(&ending, 1), time, evaluation);
        }
        beginEnding(time, info->length);
    }
    bool ending = phase == Phase::Ending;
    double clock = ending ? endClock : beginning ? 0 : std::max(0.0, local - loopOffset);
    if (!ending && !beginning) {
        phase = Phase::Running;
        if (clock > info->length) {
            finished = true;
            if (info->loop) {
                double remainder = info->length > 0 ? std::fmod(clock, info->length) : 0;
                // AnimationPlayer.process resets once per observed boundary,
                // even when a render gap spans many physical loop durations.
                soundCycle = AnimationSampler::advanceCycle(soundCycle, 1, context);
                if (command != CodedPlayState::Pause)
                    eventCycle = AnimationSampler::advanceCycle(eventCycle, 1, context);
                clock = remainder;
                loopOffset = local - clock;
            }
            else if (info->hold) clock = info->length;
        }
        {
            AnimationLayer selected{name, local, false, "coded:" + owner + ":" + std::to_string(generation)};
            selected.loop = loop;
            selected.emitEvents = command != CodedPlayState::Pause;
            selected.eventPosition = AnimationEventPosition{soundCycle, clock, eventCycle};
            sampler.advanceEvents(std::span<AnimationLayer const>(&selected, 1), time, evaluation);
        }
    }
    sampledTime = clock;
    double weight = stopped ? lastWeight : sampler.blendWeight(name, clock, context);
    lastWeight = weight;
    if (command == CodedPlayState::Pause) {
        if (!ending) raw.clear();
        output.clear();
        return;
    }
    // STOP starts the ending phase without processing it until the next
    // update. The previous queue remains visible on this first stop frame.
    if (stopped) { assign(pose, output); return; }
    if (!ending) {
        sampler.sampleRawClip(raw, pose, name, clock, context);
    }
    output.clear();
    output.reserve(raw.size());
    bool additive = request && request->additiveRotation && phase == Phase::Running;
    sampledAdditive = additive;
    double alpha = beginning && !ending ? std::clamp((time - entered) / beginningLength, 0.0, 1.0) : 1;
    if (beginning && !ending && !blendPoints.empty()) {
        auto right = blendPoints.upper_bound(time - entered);
        if (right == blendPoints.begin()) alpha = 1 - right->second;
        else if (right == blendPoints.end()) alpha = 1 - blendPoints.rbegin()->second;
        else {
            auto left = std::prev(right);
            double fraction = (time - entered - left->first) / (right->first - left->first);
            alpha = 1 - (left->second + (right->second - left->second) * fraction);
        }
    }
    auto fromCursor = from.cbegin();
    auto lowerCursor = pose.cbegin();
    bool denseFrom = raw.size() >= from.size() / 4;
    bool denseLower = raw.size() >= pose.size() / 4;
    double endProgress = ending ? std::clamp((time - endingAt) / endingLength, 0.0, 1.0) : 0;
    for (auto& [bone, value] : raw) {
        if (std::none_of(value.driven.begin(), value.driven.end(), [](bool driven) { return driven; })) continue;
        auto& out = output.emplace_back(bone, BonePose{}).second;
        BonePose const empty;
        auto const& origin = beginning ? poseAt(from, fromCursor, *bone, denseFrom) : empty;
        auto const& lower = ending || additive ? poseAt(pose, lowerCursor, *bone, denseLower) : empty;
        double progress = endProgress;
        if (ending && time > endingAt && sampler.hasScaleTrack(name, *bone)) progress = 1;
        for (int c = 0; c < 3; ++c) if (value.driven[c]) {
            auto& dst = channel(out, c);
            auto const& source = c == 0 ? value.position : c == 1 ? value.rotation : value.scale;
            auto const& start = c == 0 ? origin.position : c == 1 ? origin.rotation : origin.scale;
            auto const& fallback = c == 0 ? lower.position : c == 1 ? lower.rotation : lower.scale;
            if (c == 1 && (beginning || ending)) {
                Triple weighted = source;
                for (auto& axis : weighted) axis *= weight;
                dst = sampler.blendRotation(*bone, beginning && !ending ? start : weighted,
                    beginning && !ending ? weighted : fallback, beginning && !ending ? alpha : progress);
            } else for (int axis = 0; axis < 3; ++axis) {
                if (beginning && !ending) {
                    // Coded controllers suppress scale blending. Position weights
                    // blend from 1 to the authored weight.
                    double weighted = c == 2 ? 1 + (source[axis] - 1) * weight
                        : c == 1 ? start[axis] + (source[axis] * weight - start[axis]) * alpha
                        : (start[axis] + (source[axis] - start[axis]) * alpha) * (1 + (weight - 1) * alpha);
                    dst[axis] = weighted;
                } else {
                    double weighted = c == 2 ? 1 + (source[axis] - 1) * weight : source[axis] * weight;
                    dst[axis] = ending ? weighted + (fallback[axis] - weighted) * progress
                        : c == 1 && additive ? fallback[axis] + weighted : weighted;
                }
            }
            out.driven[c] = true;
        }
        if (beginning && !ending) {
            if (value.driven[0]) for (int axis = 0; axis < 3; ++axis)
                value.position[axis] = origin.position[axis] + (value.position[axis] - origin.position[axis]) * alpha;
            // The upstream coded rotation path polls the destination directly,
            // so BeginningTransitionPoint has no lastLerpResult to capture.
            value.driven[1] = false;
        }
    }
    assign(pose, output);
}
void CodedAnimationPlayer::observe(AnimationSampler const& sampler, Pose& pose, MolangContext& context) const {
    if (!context.observation) throw std::runtime_error("Coded observation requires an observation context");
    if (phase == Phase::Idle || lastCommand == CodedPlayState::Pause || !sampler.hasClip(name)) return;
    // The first STOP sample keeps the previous queue, including its bindings.
    if (phase == Phase::Ending && endingAt == lastTime && lastCommand == CodedPlayState::Stop) {
        assign(pose, output);
        return;
    }
    MolangActionScope permission(context, false);
    bool ending = phase == Phase::Ending;
    bool beginning = phase == Phase::Beginning && lastTime - entered < beginningLength;
    double weight = sampler.blendWeight(name, sampledTime, context);
    ClipPose sampled;
    if (!ending) sampler.sampleRawClip(sampled, pose, name, sampledTime, context);
    double alpha = beginning ? std::clamp((lastTime - entered) / beginningLength, 0.0, 1.0) : 1;
    if (beginning && !blendPoints.empty()) {
        auto right = blendPoints.upper_bound(lastTime - entered);
        if (right == blendPoints.begin()) alpha = 1 - right->second;
        else if (right == blendPoints.end()) alpha = 1 - blendPoints.rbegin()->second;
        else {
            auto left = std::prev(right);
            double fraction = (lastTime - entered - left->first) / (right->first - left->first);
            alpha = 1 - (left->second + (right->second - left->second) * fraction);
        }
    }
    ClipPose observed;
    auto const& samples = ending ? raw : sampled;
    observed.reserve(samples.size());
    for (auto const& [bone, value] : samples) {
      if (std::none_of(value.driven.begin(), value.driven.end(), [](bool driven) { return driven; })) continue;
      auto& out = observed.emplace_back(bone, BonePose{}).second;
      for (int c = 0; c < 3; ++c) if (value.driven[c]) {
        auto& dst = channel(out, c);
        auto const& source = c == 0 ? value.position : c == 1 ? value.rotation : value.scale;
        BonePose origin, lower;
        if (auto found = from.find(*bone); found != from.end()) origin = found->second;
        if (auto found = pose.find(*bone); found != pose.end()) lower = found->second;
        auto const& start = channel(origin, c);
        auto const& fallback = channel(lower, c);
        double progress = ending ? std::clamp((lastTime - endingAt) / (3.0 / 20.0), 0.0, 1.0) : 0;
        if (ending && lastTime > endingAt && sampler.hasScaleTrack(name, *bone)) progress = 1;
        if (c == 1 && (beginning || ending)) {
            Triple weighted = source;
            for (auto& axis : weighted) axis *= weight;
            dst = sampler.blendRotation(*bone, beginning ? start : weighted,
                beginning ? weighted : fallback, beginning ? alpha : progress);
        } else for (int axis = 0; axis < 3; ++axis) {
            if (beginning) {
                dst[axis] = c == 2 ? 1 + (source[axis] - 1) * weight
                    : c == 1 ? start[axis] + (source[axis] * weight - start[axis]) * alpha
                    : (start[axis] + (source[axis] - start[axis]) * alpha) * (1 + (weight - 1) * alpha);
            } else {
                double weighted = c == 2 ? 1 + (source[axis] - 1) * weight : source[axis] * weight;
                dst[axis] = ending ? weighted + (fallback[axis] - weighted) * progress
                    : c == 1 && sampledAdditive ? fallback[axis] + weighted : weighted;
            }
        }
        out.driven[c] = true;
      }
    }
    assign(pose, observed);
}
Json CodedAnimationPlayer::report() const {
    char const* state = phase == Phase::Idle ? "idle" : phase == Phase::Beginning ? "beginning" : phase == Phase::Running ? "running" : "ending";
    return {{"animation", name}, {"phase", state}, {"entered", entered}, {"generation", generation},
        {"finished", finished}, {"ending_at", endingAt}, {"cycle", eventCycle}, {"paused", lastCommand == CodedPlayState::Pause}};
}
}
