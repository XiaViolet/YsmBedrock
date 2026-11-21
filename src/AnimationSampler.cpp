#include "AnimationSampler.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <tuple>
#include <limits>
#include <unordered_map>
#include <utility>

namespace ysm {
namespace {
Triple& poseChannel(BonePose& pose, int index) {
    return index == 0 ? pose.position : index == 1 ? pose.rotation : pose.scale;
}

struct InstructionBudget {
    MolangContext& context;
    std::size_t& frameSteps;
    std::size_t entrySteps;
    std::size_t previousSteps;

    explicit InstructionBudget(AnimationEvaluation& evaluation)
        : context(evaluation.expressions), frameSteps(evaluation.remainingInstructionSteps),
          entrySteps(std::min(frameSteps, MolangContext::DefaultExecutionSteps)),
          previousSteps(std::exchange(context.remainingSteps, entrySteps)) {}

    ~InstructionBudget() {
        frameSteps -= entrySteps - context.remainingSteps;
        context.remainingSteps = previousSteps;
    }
};
}
Triple sourceRotationLerp(Triple const& origin, Triple const& destination, Triple const& initial, double progress) {
    constexpr double radians = 3.14159265358979323846 / 180;
    auto quaternion = [&](Triple const& offset) {
        // YSM's authored X/Y rotations are negated when entering Java's
        // right-handed ZYX rotation space. Preserve the model's base rotation.
        double x = -(offset[0] + initial[0]) * radians / 2;
        double y = -(offset[1] + initial[1]) * radians / 2;
        double z = (offset[2] + initial[2]) * radians / 2;
        double sx = std::sin(x), cx = std::cos(x), sy = std::sin(y), cy = std::cos(y), sz = std::sin(z), cz = std::cos(z);
        return std::array<double, 4>{sx*cy*cz-cx*sy*sz, cx*sy*cz+sx*cy*sz,
            cx*cy*sz-sx*sy*cz, cx*cy*cz+sx*sy*sz};
    };
    auto start = quaternion(origin), end = quaternion(destination);
    double dot = 0;
    for (int i = 0; i < 4; ++i) dot += start[i] * end[i];
    double squared = 0;
    for (int i = 0; i < 4; ++i) {
        start[i] = start[i] * (1 - progress) + end[i] * (dot < 0 ? -progress : progress);
        squared += start[i] * start[i];
    }
    double length = std::sqrt(squared);
    for (auto& value : start) value /= length;
    auto [x, y, z, w] = start;
    return Triple{-std::atan2(y*z+w*x, 0.5-x*x-y*y) / radians - initial[0],
        -std::asin(std::clamp(-2*(x*z-w*y), -1.0, 1.0)) / radians - initial[1],
        std::atan2(x*y+w*z, 0.5-y*y-z*z) / radians - initial[2]};
}
void BonePoseHistory::apply(Pose& pose, double time, std::map<std::string, Triple> const& initialRotations,
    double resetSeconds) {
    if (!std::isfinite(time) || time < 0 || !std::isfinite(resetSeconds) || resetSeconds < 0)
        throw std::runtime_error("Invalid bone reset time");
    if (time < lastTime) reset();
    bool advanced = time != lastTime;
    lastTime = time;
    auto nextSnapshot = snapshots.begin();
    for (auto const& [name, bone] : pose) {
        if (std::none_of(bone.driven.begin(), bone.driven.end(), [](bool driven) { return driven; })) continue;
        while (nextSnapshot != snapshots.end() && nextSnapshot->first < name) ++nextSnapshot;
        if (nextSnapshot == snapshots.end() || nextSnapshot->first != name)
            nextSnapshot = snapshots.try_emplace(nextSnapshot, name);
        auto& snapshot = nextSnapshot++->second;
        for (int c = 0; c < 3; ++c) if (bone.driven[c]) {
            poseChannel(snapshot.origin, c) = c == 0 ? bone.position : c == 1 ? bone.rotation : bone.scale;
            snapshot.updated[c] = time;
            snapshot.active[c] = true;
        }
    }
    for (auto it = snapshots.begin(); it != snapshots.end();) {
        auto const& name = it->first;
        auto& snapshot = it->second;
        auto incoming = pose.end();
        for (int c = 0; c < 3; ++c) if (snapshot.active[c]) {
            if (advanced && snapshot.updated[c] == time) continue;
            // On a second sample of the same time the incoming channel may
            // have been removed, even though the previous stamp is equal.
            if (!advanced && incoming == pose.end()) incoming = pose.find(name);
            if (incoming != pose.end() && incoming->second.driven[c]) continue;
            double progress = resetSeconds == 0 ? 1 : (time - snapshot.updated[c]) / resetSeconds;
            if (progress >= 1) { snapshot.active[c] = false; continue; }
            auto const& origin = poseChannel(snapshot.origin, c);
            if (incoming == pose.end()) incoming = pose.try_emplace(name).first;
            auto& target = poseChannel(incoming->second, c);
            if (c == 1) {
                auto base = initialRotations.find(name);
                target = sourceRotationLerp(origin, Triple{}, base == initialRotations.end() ? Triple{} : base->second, progress);
            } else for (int axis = 0; axis < 3; ++axis)
                target[axis] = origin[axis] + ((c == 2 ? 1.0 : 0.0) - origin[axis]) * progress;
            // Reset output is visible, but it is not an authored channel and
            // must not renew its own last-driven time on subsequent frames.
        }
        if (std::none_of(snapshot.active.begin(), snapshot.active.end(), [](bool active) { return active; }))
            it = snapshots.erase(it);
        else ++it;
    }
}
std::string AnimationSampler::normalized(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
}
static double sourceAnimationTime(double seconds) {
    // Animation.Adapter and BoneKeyFrameList parse F32 seconds, then multiply
    // by 20 in F32. Keep that tick coordinate even though this host uses seconds.
    float ticks=static_cast<float>(seconds)*20.f;
    if(!std::isfinite(ticks)||seconds<0)throw std::runtime_error("Invalid animation time");
    return static_cast<double>(ticks)/20.0;
}
static double timestamp(std::string const& text) {
    char* end = nullptr;
    double result = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(result) || result < 0)
        throw std::runtime_error("Invalid animation timestamp");
    return sourceAnimationTime(result);
}
namespace {
enum class TimelineAnnotation { None, Open, Close, Note };

TimelineAnnotation timelineAnnotation(std::string_view line) {
    auto start = line.find_first_not_of(" \t\r\n");
    if (start == line.npos) return TimelineAnnotation::None;
    line = line.substr(start, line.find_last_not_of(" \t\r\n") - start + 1);
    if (line == "[;") return TimelineAnnotation::Open;
    if (line == "];") return TimelineAnnotation::Close;
    if (!line.ends_with("';") || line.find(';') != line.size() - 1) return TimelineAnnotation::None;
    // Legacy editors leave marked annotation lines in the timeline arrays.
    // A title has no executable text after its quoted string.
    if (line.starts_with("['") && line.find('\'', 2) == line.size() - 2) return TimelineAnnotation::Open;
    if (line.starts_with("'注释：") || line.starts_with("'注释:")) return TimelineAnnotation::Note;
    return TimelineAnnotation::None;
}
}

AnimationSampler::AnimationSampler(Json const& document, Json const& geometry) {
    legacyMolang = document.value("ysm_legacy_molang", document.value("ysm_legacy_variable_paths", false));
    auto rig = std::make_shared<MolangContext::BoneRig>();
    std::vector<std::pair<std::string, std::string>> sources;
    auto sourceFunctions = document.value("ysm_functions", Json::object());
    if (!sourceFunctions.is_object()) throw std::runtime_error("Invalid Molang function library");
    for (auto const& [name, body] : sourceFunctions.items()) sources.emplace_back(name, body.get<std::string>());
    functions = std::make_shared<MolangFunctionLibrary const>(sources);
    controllerActions = !functions->empty();
    diagnostics = functions->compileDiagnostics();
    if (!geometry.is_null()) for (auto const& bone : geometry.at("minecraft:geometry").at(0).at("bones")) {
        auto name = bone.at("name").get<std::string>();
        auto rotation = bone.value("rotation", Triple{});
        if (!std::all_of(rotation.begin(), rotation.end(), [](double value) { return std::isfinite(value); }))
            throw std::runtime_error("Invalid initial bone rotation");
        initialRotations[name] = rotation;
        auto pivot = bone.value("pivot", Triple{});
        rig->emplace(name, MolangContext::BoneBasis{pivot, bone.value("parent", "")});
        initialVisuals[name].glow = name.starts_with("ysmGlow") ? 15 : -1;
        // YSM's Head locator also admits numeric suffixes, with case preserved.
        auto end = name.find_last_not_of("0123456789");
        if (end != std::string::npos && name.substr(0, end + 1) == "Head") headBones.insert(name);
    }
    boneRig = std::move(rig);
    // Compiled programs are immutable. Mirrored model parts often repeat the
    // exact same author expression across hundreds of tracks and keyframes.
    std::unordered_map<std::string, MolangProgram> channelPrograms;
    auto compile = [&](Json const& value, double fallback, std::string const& location) {
        try {
            if (value.is_number()) return MolangProgram(value.get<double>());
            if (value.is_string()) {
                auto const& source = value.get_ref<std::string const&>();
                auto found = channelPrograms.find(source);
                if (found != channelPrograms.end()) return found->second;
                auto program = MolangProgram(source);
                controllerActions = controllerActions || program.hasControllerActions();
                auto names = program.roamingFields();
                roamingNames.insert(names.begin(), names.end());
                channelPrograms.emplace(source, program);
                return program;
            }
            throw std::runtime_error("Expected a number or expression");
        } catch (std::exception const& error) {
            ++unsupported;
            if (diagnostics.size() < 64) diagnostics.push_back(location + ": " + error.what());
            return MolangProgram(fallback);
        }
    };
    auto vector = [&](Json const& value, double fallback, std::string const& location) {
        VectorExpression result;
        if (!value.is_array()) {
            result.values[0] = compile(value, fallback, location);
            result.uniform = true;
        } else {
            if (value.size() != 3) throw std::runtime_error("Invalid animation vector");
            for (int axis = 0; axis < 3; ++axis) result.values[axis] = compile(value[axis], fallback, location);
        }
        result.finiteLiterals = std::all_of(result.values.begin(), result.values.end(), [](auto const& program) {
            auto value = program.numericLiteral();
            return value && std::isfinite(*value);
        });
        return result;
    };
    bool mergeInstructions = document.value("ysm_merge_multiline_expr", false);
    auto fallbackClips = document.value("ysm_fallback_animations", std::vector<std::string>{});
    std::set<std::string> fallbackNames(fallbackClips.begin(), fallbackClips.end());
    Json const emptyBones = Json::object();
    for (auto const& [name, source] : document.at("animations").items()) {
        conditions.add(name);
        auto& clip = clips[name];
        bool explicitLength = source.contains("animation_length");
        clip.length = source.value("animation_length", 0.0);
        if (!std::isfinite(clip.length) || clip.length < 0) throw std::runtime_error("Invalid animation length");
        clip.length=sourceAnimationTime(clip.length);
        auto loop = source.value("loop", Json(false));
        clip.loop = loop.is_boolean() && loop.get<bool>();
        clip.hold = loop.is_string() && loop.get<std::string>() == "hold_on_last_frame";
        if (source.contains("blend_weight")) clip.weight = compile(source["blend_weight"], 1, name + " blend_weight");
        if (source.contains("timeline")) for (auto const& [stamp, commands] : source["timeline"].items()) {
            Event event{timestamp(stamp), {}};
            auto location = name + " timeline " + stamp;
            if (commands.is_array()) {
                std::vector<TimelineAnnotation> annotations(commands.size());
                bool balanced = true;
                int depth = 0;
                if (legacyMolang) for (std::size_t i = 0; i < commands.size(); ++i) {
                    auto const& command = commands[i];
                    if (!command.is_string()) continue;
                    auto marker = timelineAnnotation(command.get_ref<std::string const&>());
                    annotations[i] = marker;
                    if (marker == TimelineAnnotation::Open) ++depth;
                    if (marker == TimelineAnnotation::Close && --depth < 0) balanced = false;
                }
                auto isInstruction = [&](std::size_t i) {
                    auto marker = annotations[i];
                    return commands[i].is_string() && marker != TimelineAnnotation::Note
                        && (marker == TimelineAnnotation::None || !balanced || depth != 0);
                };
                if (mergeInstructions && !fallbackNames.contains(name)) {
                    std::string merged;
                    for (std::size_t i = 0; i < commands.size(); ++i) if (isInstruction(i)) {
                        merged += commands[i].get_ref<std::string const&>();
                        merged += '\n';
                    }
                    if (!merged.empty()) event.programs.push_back(compile(merged, 0, location));
                } else {
                    for (std::size_t i = 0; i < commands.size(); ++i) if (isInstruction(i))
                        event.programs.push_back(compile(commands[i], 0, location));
                }
            } else if (commands.is_string()) event.programs.push_back(compile(commands, 0, location));
            if (!event.programs.empty()) clip.events.push_back(std::move(event));
        }
        if (source.contains("sound_effects") && !source["sound_effects"].is_null()) {
            auto const& effects = source.at("sound_effects");
            if (!effects.is_object()) throw std::runtime_error("Invalid sound_effects in " + name);
            for (auto const& [stamp, group] : effects.items()) {
                auto time = timestamp(stamp);
                auto append = [&](Json const& effect) {
                    if (!effect.is_object()) throw std::runtime_error("Invalid sound keyframe in " + name);
                    auto value = effect.find("effect");
                    if (value != effect.end() && value->is_string() && !value->get_ref<std::string const&>().empty())
                        clip.sounds.emplace_back(time, value->get<std::string>());
                };
                if (group.is_array()) for (auto const& effect : group) append(effect);
                else append(group);
            }
            std::stable_sort(clip.sounds.begin(), clip.sounds.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        }
        std::stable_sort(clip.events.begin(), clip.events.end(), [](auto const& a, auto const& b) { return a.time < b.time; });
        auto const& bones = source.contains("bones") ? source.at("bones") : emptyBones;
        for (auto const& [bone, channels] : bones.items()) {
            auto& target = clip.bones[bone];
            std::array<char const*, 3> names{"position", "rotation", "scale"};
            for (int channel = 0; channel < 3; ++channel) {
                if (!channels.contains(names[channel])) continue;
                auto const& data = channels.at(names[channel]);
                double fallback = channel == 2 ? 1 : 0;
                Track track;
                auto append = [&](double time, Json const& value) {
                    bool split = value.is_object();
                    bool legacy = split && value.contains("vector") && !value["vector"].is_null();
                    auto has = [&](char const* key) { return split && value.contains(key) && !value[key].is_null(); };
                    auto postSource = legacy ? value["vector"] : has("post") ? value["post"] : has("pre") ? value["pre"] : split ? Json() : value;
                    auto preSource = legacy ? postSource : has("pre") ? value["pre"] : postSource;
                    auto post = vector(postSource, fallback, name + " " + bone);
                    auto pre = preSource == postSource ? post : vector(preSource, fallback, name + " " + bone);
                    auto easing = split ? value.value(legacy ? "easing" : "lerp_mode", Json()) : Json();
                    bool curved = easing.is_string() && normalized(easing.get<std::string>()) == "catmullrom";
                    track.keys.push_back({time, std::move(pre), std::move(post), curved});
                };
                if (data.is_object()) {
                    if (data.contains("pre") || data.contains("post")) append(0, data);
                    else for (auto const& [stamp, value] : data.items()) append(timestamp(stamp), value);
                } else append(0, data);
                if (track.keys.empty()) continue;
                std::sort(track.keys.begin(), track.keys.end(), [](auto const& a, auto const& b) { return a.time < b.time; });
                if (!explicitLength) clip.length = std::max(clip.length, track.keys.back().time);
                target.values[channel] = std::move(track);
            }
        }
        // Release YSM treats an omitted length without a positive bone key
        // as unbounded. Event keys do not determine the clip length.
        if (!explicitLength && clip.length == 0)
            clip.length = std::numeric_limits<double>::infinity();
    }
}
std::set<std::string> AnimationSampler::roamingFields(bool firstPerson) const {
    auto fields = functions->roamingFields(firstPerson);
    fields.insert(roamingNames.begin(), roamingNames.end());
    return fields;
}
Triple AnimationSampler::VectorExpression::sample(MolangContext& context, MolangValue& input, Triple const& current, double fallback) const {
    Triple result;
    int axes = uniform ? 1 : 3;
    // Literal channels still consume one instruction per axis and expose the last
    // input through 'this'. Keep the normal evaluator for scoped or partial runs.
    if (finiteLiterals && context.programDepth == 0 && context.localVariables.empty() && context.remainingSteps >= axes) {
        context.remainingSteps -= axes;
        input = current[axes - 1];
        if (uniform) result.fill(*values[0].numericLiteral());
        else for (int axis = 0; axis < 3; ++axis) result[axis] = *values[axis].numericLiteral();
        return result;
    }
    for (int axis = 0; axis < axes; ++axis) {
        input = current[axis];
        auto failures = context.failures;
        double value = values[axis].evaluate(context).number();
        result[axis] = context.failures == failures ? value : fallback;
    }
    if (uniform) result[1] = result[2] = result[0];
    return result;
}
Triple AnimationSampler::Track::sample(double time, MolangContext& context, MolangValue& input, Triple const& current, double fallback) const {
    if (time < keys.front().time) return keys.front().pre.sample(context, input, current, fallback);
    if (time >= keys.back().time) return keys.back().post.sample(context, input, current, fallback);
    auto upper = std::upper_bound(keys.begin(), keys.end(), time, [](double t, Key const& key) { return t < key.time; });
    auto const& right = *upper;
    auto const& left = *(upper - 1);
    double alpha = (time - left.time) / (right.time - left.time);
    // Source keyframes do not evaluate the unused endpoint near a boundary.
    // This also preserves variable/physics evaluation order in authored curves.
    if (alpha < 0.00001f) return left.post.sample(context, input, current, fallback);
    if (alpha > 0.99999f) return right.post.sample(context, input, current, fallback);
    Triple result;
    if (left.curved || right.curved) {
        auto const& previousKey = upper - 1 == keys.begin() ? left : *(upper - 2);
        auto const& nextKey = upper + 1 == keys.end() ? right : *(upper + 1);
        auto previous = previousKey.post.sample(context, input, current, fallback);
        auto a = left.post.sample(context, input, current, fallback);
        auto b = right.pre.sample(context, input, current, fallback);
        auto next = nextKey.pre.sample(context, input, current, fallback);
        for (int axis = 0; axis < 3; ++axis)
            result[axis] = 0.5 * ((2*a[axis]) + (-previous[axis]+b[axis])*alpha
                + (2*previous[axis]-5*a[axis]+4*b[axis]-next[axis])*alpha*alpha
                + (-previous[axis]+3*a[axis]-3*b[axis]+next[axis])*alpha*alpha*alpha);
    } else {
        auto a = left.post.sample(context, input, current, fallback), b = right.pre.sample(context, input, current, fallback);
        for (int axis = 0; axis < 3; ++axis) result[axis] = a[axis] + (b[axis]-a[axis])*alpha;
    }
    return result;
}
void AnimationSampler::layer(Pose& pose, std::string const& name, double time, MolangContext& context, bool additiveRotation, AnimationLoop loop) const {
    auto info = clipInfo(name, loop);
    if (!info) return;
    auto const& clip = *info;
    if (clip.loop && clip.length > 0) time = std::fmod(std::max(0.0, time), clip.length);
    else if (clip.length > 0) {
        if (clip.hold) time = std::min(time, clip.length);
        else if (time > clip.length) return;
    }
    double weight = blendWeight(name, time, context);
    ClipPose sampled;
    sampleRawClip(sampled, pose, name, time, context);
    for (auto const& [bone, value] : sampled) {
        auto& target = pose[*bone];
        for (int channel = 0; channel < 3; ++channel) if (value.driven[channel]) {
            auto const& source = channel == 0 ? value.position : channel == 1 ? value.rotation : value.scale;
            auto& output = channel == 0 ? target.position : channel == 1 ? target.rotation : target.scale;
            target.driven[channel] = true;
            for (int axis = 0; axis < 3; ++axis) {
                double weighted = channel == 2 ? 1 + (source[axis] - 1) * weight : source[axis] * weight;
                if (channel == 1 && additiveRotation) output[axis] += weighted;
                else output[axis] = weighted;
            }
        }
    }
}
void AnimationSampler::sampleRawClip(ClipPose& sampled, Pose const& lower,
    std::string const& name, double time, MolangContext& context) const {
    sampled.clear();
    MolangActionScope permission(context, false);
    auto it = clips.find(name);
    if (it == clips.end()) return;
    if (!std::isfinite(time) || time < 0) throw std::runtime_error("Invalid clip time");
    context.queries["query.anim_time"] = time;
    auto const& clip = it->second;
    sampled.reserve(clip.bones.size());
    MolangValue* input = nullptr;
    auto previous = lower.begin();
    bool dense = clip.bones.size() >= lower.size() / 4;
    for (auto const& [bone, channels] : clip.bones) {
        if (context.remainingSteps == 0) { context.diagnose("Animation expression budget exceeded"); break; }
        BonePose target;
        if (dense) {
            while (previous != lower.end() && previous->first < bone) ++previous;
        } else previous = lower.find(bone);
        if (previous != lower.end() && previous->first == bone) {
            target = previous->second;
            target.driven = {};
        }
        for (int channel = 0; channel < 3; ++channel) {
            if (!channels.values[channel]) continue;
            if (!input) input = &context.queries["this"];
            auto& output = poseChannel(target, channel);
            output = channels.values[channel]->sample(time, context, *input, output, channel == 2 ? 1 : 0);
            target.driven[channel] = true;
        }
        if (std::any_of(target.driven.begin(), target.driven.end(), [](bool driven) { return driven; }))
            sampled.emplace_back(&bone, target);
    }
}
double AnimationSampler::blendWeight(std::string const& name, double time, MolangContext& context) const {
    MolangActionScope permission(context, false);
    auto found = clips.find(name);
    if (found == clips.end()) return 0;
    context.queries["query.anim_time"] = time;
    double weight = found->second.weight ? found->second.weight->evaluate(context).number() : 1.0;
    return weight > 0 ? weight : 0;
}
Pose AnimationSampler::sample(std::string const& state, double time) const {
    AnimationEvaluation evaluation;
    return sample(state, time, time, evaluation);
}
Pose AnimationSampler::sample(std::string const& state, double stateTime, double parallelTime, AnimationEvaluation& evaluation,
    std::span<AnimationLayer const> overlays) const {
    if (!std::isfinite(stateTime) || stateTime < 0)
        throw std::runtime_error("Invalid animation time");
    beginFrame(parallelTime, evaluation);
    bindFunctions(evaluation);
    std::vector<AnimationLayer> active;
    for (int i = 0; i < 8; ++i) active.push_back({"pre_parallel" + std::to_string(i), parallelTime, false, {}, false});
    auto const& base = state;
    if (base != evaluation.activeBase) {
        evaluation.retireTimelines([&](auto const& cursor) {
            return cursor.first == evaluation.activeBase || cursor.first == base;
        });
        evaluation.activeBase = base;
    }
    active.push_back({base, stateTime, false, {}, false});
    for (auto const& overlay : overlays) {
        if (!std::isfinite(overlay.time) || overlay.time < 0) throw std::runtime_error("Invalid overlay time");
        active.push_back(overlay);
    }
    for (int i = 0; i < 8; ++i) active.push_back({"parallel" + std::to_string(i), parallelTime, true, {}, false});
    Pose result;
    applyLayers(result, active, parallelTime, evaluation);
    resetUndriven(result, parallelTime, evaluation.poseHistory);
    finishPose(result, evaluation.expressions);
    evaluation.expressions.finishFrame();
    return result;
}
void AnimationSampler::bindFunctions(AnimationEvaluation& evaluation) const {
    if (evaluation.expressions.library != functions) {
        evaluation.expressions.library = functions;
        evaluation.expressions.legacyMolang = legacyMolang;
        evaluation.expressions.boneRig = boneRig;
        auto& channels = evaluation.expressions.boneChannels;
        channels.clear();
        evaluation.expressions.invalidateBonePivots();
        for (auto const& [name, rotation] : initialRotations)
            channels.emplace_hint(channels.end(), name, MolangContext::BoneChannels{rotation});
        evaluation.expressions.boneVisuals = initialVisuals;
        evaluation.functionsInitialized = false;
    }
}
void AnimationSampler::beginFrame(double time, AnimationEvaluation& evaluation) {
    if (!std::isfinite(time) || time < 0) throw std::runtime_error("Invalid animation time");
    double delta = evaluation.lastTime < 0 ? 0 : std::max(0.0, time - evaluation.lastTime);
    evaluation.expressions.beginFrame(delta, time);
    evaluation.remainingTimelineEvents = 4096;
    evaluation.remainingInstructionSteps = AnimationEvaluation::InstructionStepsPerFrame;
    evaluation.lastTime = time;
    evaluation.expressions.queries["query.life_time"] = time;
    evaluation.expressions.queries["query.delta_time"] = delta;
    evaluation.expressions.queries["ysm.time_delta"] = delta;
}
std::uint64_t AnimationSampler::advanceCycle(std::uint64_t current, double loops, MolangContext& context) {
    long double target = static_cast<long double>(current) + std::floor(static_cast<long double>(loops));
    if (!std::isfinite(target) || target >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        context.diagnose("Animation cycle count exceeded");
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(std::max(0.0L, target));
}
void AnimationSampler::applyLayers(Pose& pose, std::span<AnimationLayer const> active, double parallelTime,
    AnimationEvaluation& evaluation) const {
    for (auto const& selected : active) {
        MolangDeferredScope deferred(evaluation.expressions, &selected);
        advanceEvents(std::span<AnimationLayer const>(&selected, 1), parallelTime, evaluation);
        sampleClip(pose, selected, evaluation.expressions);
    }
}
void AnimationSampler::advanceEvents(std::span<AnimationLayer const> active, double parallelTime,
    AnimationEvaluation& evaluation) const {
    auto& context = evaluation.expressions;
    struct Scheduled { double time, local; std::size_t layer; std::uint64_t cycle; Event const* event; };
    std::vector<Scheduled> scheduled;
    for (std::size_t order = 0; order < active.size(); ++order) {
        auto const& selected = active[order];
        if (!std::isfinite(selected.time) || selected.time < 0) throw std::runtime_error("Invalid overlay time");
        auto found = clips.find(selected.name);
        if (found == clips.end() || (found->second.events.empty() && found->second.sounds.empty())) continue;
        auto const& clip = found->second;
        auto playback = *clipInfo(selected.name, selected.loop);
        auto const& owner = selected.timelineId.empty() ? selected.name : selected.timelineId;
        bool restarted = selected.restart && evaluation.timelineCursors.contains(owner);
        if (restarted) evaluation.retireTimelines([&](auto const& entry) { return entry.first == owner; });
        AnimationEventPosition position{0, playback.hold ? std::min(selected.time, clip.length) : selected.time};
        if (selected.eventPosition) position = *selected.eventPosition;
        else if (playback.loop && clip.length > 0) {
            position.clock = std::fmod(selected.time, clip.length);
            position.cycle = advanceCycle(0, std::round((selected.time - position.clock) / clip.length), context);
        }
        if (!std::isfinite(position.clock) || position.clock < 0) throw std::runtime_error("Invalid event clock");
        if (selected.finishEvents) position.clock = clip.length;
        auto instructionCycle = position.instructionCycle.value_or(position.cycle);
        auto& cursor = evaluation.timelineCursors[owner];
        if (cursor.animation != selected.name || cursor.loop != selected.loop || selected.time < cursor.time
            || instructionCycle < cursor.cycle) {
            if (cursor.ownsSounds && evaluation.expressions.soundCues.size() < 4096) context.emitSound({owner, {}});
            cursor = AnimationTimelineCursor{selected.name, selected.loop};
        }
        cursor.time = selected.time;
        cursor.ownsSounds = true; // Instructions may submit scoped sounds even without sound keyframes.
        if (selected.finishEvents || cursor.soundCycle != position.cycle) {
                if (!selected.finishEvents) context.emitSound({owner, {}});
                cursor.nextSound = 0;
                cursor.soundCycle = position.cycle;
        }
        if (!clip.sounds.empty()) {
            // Java advances muted sound keys. A loop reset drops unheard keys from the old cycle.
            while (cursor.nextSound < clip.sounds.size() && clip.sounds[cursor.nextSound].first <= position.clock) {
                auto const& sound = clip.sounds[cursor.nextSound++].second;
                if (selected.emitEvents && !selected.finishEvents && evaluation.expressions.soundCues.size() < 4096)
                    context.emitSound({owner, sound});
            }
        }
        if (!selected.emitEvents || clip.events.empty()) continue;
        bool exhausted = false;
        while (true) {
            bool completedCycle = cursor.cycle < instructionCycle;
            while (cursor.next < clip.events.size()) {
                auto const& event = clip.events[cursor.next];
                if (!completedCycle && !selected.finishEvents && event.time > position.clock) break;
                if (evaluation.remainingTimelineEvents == 0) { exhausted = true; break; }
                --evaluation.remainingTimelineEvents;
                // Keep the final instruction and the next cycle's zero-time
                // instruction distinct, even when their absolute times match.
                double local = completedCycle || selected.finishEvents ? std::min(event.time, clip.length) : event.time;
                double absolute = cursor.cycle == 0 || !playback.loop
                    ? local : static_cast<double>(cursor.cycle) * clip.length + local;
                double contextTime = completedCycle || selected.finishEvents ? clip.length : position.clock;
                scheduled.push_back({absolute + parallelTime - selected.time, contextTime, order, cursor.cycle, &event});
                ++cursor.next;
            }
            if (exhausted || !completedCycle) break;
            ++cursor.cycle;
            cursor.next = 0;
        }
        if (exhausted) {
            context.diagnose("Timeline catch-up budget exceeded: " + selected.name);
            // The rejected interval is retired once. A later draw must not
            // retry it forever or wrap a huge cycle count back to the start.
            cursor.cycle = instructionCycle;
            cursor.next = selected.finishEvents ? clip.events.size() : static_cast<std::size_t>(std::upper_bound(
                clip.events.begin(), clip.events.end(), position.clock,
                [](double time, Event const& event) { return time < event.time; }) - clip.events.begin());
        }
    }
    std::stable_sort(scheduled.begin(), scheduled.end(), [](auto const& a, auto const& b) {
        return std::tie(a.time, a.layer, a.cycle) < std::tie(b.time, b.layer, b.cycle);
    });
    for (auto const& event : scheduled) {
        auto const& selected = active[event.layer];
        MolangSoundScope sounds(context, selected.timelineId.empty() ? selected.name : selected.timelineId);
        MolangActionScope permission(context, true);
        context.queries["query.anim_time"] = event.local;
        for (auto const& program : event.event->programs) {
            if (evaluation.remainingInstructionSteps == 0) {
                ++context.failures;
                context.diagnose("Animation instruction budget exceeded: " + selected.name);
                break;
            }
            // Java executes each authored instruction as a separate entry.
            // Keep an aggregate frame guard for instructions without borrowing
            // the capacity reserved for controller conditions and bone poses.
            InstructionBudget budget(evaluation);
            program.evaluate(context);
        }
    }
    // Java flushes final instructions before retiring their sound manager.
    // A loop started by the last instruction must retire in this update too.
    for (auto const& selected : active) if (selected.finishEvents) {
        auto const& owner = selected.timelineId.empty() ? selected.name : selected.timelineId;
        if (auto found = evaluation.timelineCursors.find(owner);
            found != evaluation.timelineCursors.end() && found->second.ownsSounds)
            context.emitSound({owner, {}});
    }
}
void AnimationSampler::sampleClip(Pose& pose, AnimationLayer const& selected, MolangContext& context) const {
    if (selected.apply) layer(pose, selected.name, selected.time, context, selected.additiveRotation, selected.loop);
}
std::optional<AnimationClipInfo> AnimationSampler::clipInfo(std::string const& name, AnimationLoop loop) const {
    auto found = clips.find(name);
    if (found == clips.end()) return {};
    return AnimationClipInfo{found->second.length, loop == AnimationLoop::Loop || (loop == AnimationLoop::Authored && found->second.loop),
        loop == AnimationLoop::Hold || (loop == AnimationLoop::Authored && found->second.hold)};
}
bool AnimationSampler::hasNonemptyClip(std::string const& name) const {
    auto found = clips.find(name);
    return found != clips.end() && (!found->second.bones.empty() || !found->second.events.empty() || !found->second.sounds.empty());
}
void AnimationSampler::recoverHeadPose(Pose const& authored, MolangContext& context) const {
    // CustomHumanoidEntity.recoverLastCodedAnimation restores X/Y before
    // update events, controller processing, and projected render evaluations.
    // Z has no camera contribution and remains the last committed value.
    if (!headBones.empty()) context.invalidateBonePivots();
    for (auto const& name : headBones) {
        auto initial = initialRotations.find(name);
        Triple rotation = initial == initialRotations.end() ? Triple{} : initial->second;
        if (auto found = authored.find(name); found != authored.end())
            for (std::size_t axis = 0; axis < rotation.size(); ++axis) rotation[axis] += found->second.rotation[axis];
        auto& completed = context.boneChannels.try_emplace(name, MolangContext::BoneChannels{rotation}).first->second;
        completed.rotation[0] = rotation[0];
        completed.rotation[1] = rotation[1];
    }
}
void AnimationSampler::finishPose(Pose& result, MolangContext& context, bool codedHead) const {
    // Java's coded head pass follows authored animation. Convert its YSM
    // angles back to Bedrock geometry angles without altering source channels.
    double pitch = -context.read("ysm.head_pitch").number();
    double yaw = -context.read("ysm.head_yaw").number();
    if (codedHead) for (auto const& name : headBones) {
        result[name].rotation[0] += pitch;
        result[name].rotation[1] += yaw;
        result[name].driven[1] = true;
    }
    // Body and arm instances keep separate completed-pose snapshots. Update
    // events inspect the preceding authored pose after recoverHeadPose removes
    // the previous camera contribution; post-render readers see this draw.
    if (!result.empty()) context.invalidateBonePivots();
    auto base = initialRotations.begin();
    auto output = context.boneChannels.begin();
    bool dense = result.size() >= initialRotations.size() / 4
        && result.size() >= context.boneChannels.size() / 4;
    for (auto const& [name, pose] : result) {
        auto rotation = pose.rotation;
        if (dense) {
            while (base != initialRotations.end() && base->first < name) ++base;
        } else base = initialRotations.find(name);
        if (base != initialRotations.end() && base->first == name)
            for (std::size_t axis = 0; axis < rotation.size(); ++axis) rotation[axis] += base->second[axis];
        if (dense) {
            while (output != context.boneChannels.end() && output->first < name) ++output;
            if (output == context.boneChannels.end() || output->first != name)
                output = context.boneChannels.try_emplace(output, name);
            (output++)->second = {rotation, pose.scale, pose.position};
        } else context.boneChannels[name] = {rotation, pose.scale, pose.position};
    }
}
void AnimationSampler::resetUndriven(Pose& pose, double time, BonePoseHistory& history, double resetSeconds) const {
    history.apply(pose, time, initialRotations, resetSeconds);
}
Json AnimationSampler::report(Pose const& pose) {
    Json result = Json::object();
    for (auto const& [bone, value] : pose) result[bone] = Json{{"position", value.position}, {"rotation", value.rotation}, {"scale", value.scale}};
    return result;
}
}
