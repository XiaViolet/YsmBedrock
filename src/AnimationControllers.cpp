#include "AnimationControllers.h"
#include "HandAnimationController.h"
#include "JavaVanillaInputs.h"
#include "CodedAnimationPlayer.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ysm {
namespace {
template<class Compile> MolangProgram expression(Json const& value, Compile const& compile) {
    if (value.is_string()) return compile(value.get<std::string>());
    if (value.is_boolean()) return MolangProgram(value.get<bool>() ? 1.0 : 0.0);
    if (value.is_number() && std::isfinite(value.get<double>())) return MolangProgram(value.get<double>());
    throw std::runtime_error("Invalid controller expression");
}
template<class Compile> std::optional<MolangProgram> statements(Json const& value, Compile const& compile) {
    if (value.is_null() || (value.is_array() && value.empty())) return {};
    auto parts = value.is_array() ? value : Json::array({value});
    std::string source;
    for (auto const& item : parts) {
        if (!item.is_string()) throw std::runtime_error("Invalid controller instruction");
        auto text = item.get<std::string>();
        auto end = text.find_last_not_of(" \t\r\n");
        if (end == std::string::npos) continue;
        source += text;
        if (text[end] != ';') source += ';';
        source += '\n';
    }
    return source.empty() ? std::optional<MolangProgram>{} : compile(source);
}
double finiteNumber(Json const& value, char const* label) {
    if (!value.is_number() || !std::isfinite(value.get<double>())) throw std::runtime_error(label);
    return value.get<double>();
}
Triple& channel(BonePose& bone, int index) {
    return index == 0 ? bone.position : index == 1 ? bone.rotation : bone.scale;
}
BonePose& outputBone(Pose& target, Pose::iterator& cursor, std::string const& name, bool dense) {
    if (!dense) return target[name];
    while (cursor != target.end() && cursor->first < name) ++cursor;
    if (cursor == target.end() || cursor->first != name) cursor = target.try_emplace(cursor, name);
    return (cursor++)->second;
}
void assign(Pose& target, Pose const& source) {
    auto cursor = target.begin();
    bool dense = source.size() >= target.size() / 4;
    for (auto const& [name, bone] : source) {
        if (!bone.driven[0] && !bone.driven[1] && !bone.driven[2]) continue;
        auto& out = outputBone(target, cursor, name, dense);
        for (int c = 0; c < 3; ++c) if (bone.driven[c]) {
            channel(out, c) = c == 0 ? bone.position : c == 1 ? bone.rotation : bone.scale;
            out.driven[c] = true;
        }
    }
}
struct Blend {
    double length = 0;
    std::map<double, double> points;
    double progress(double time) const {
        if (length <= 0 || time >= length) return 1;
        if (points.empty()) return time / length;
        auto right = points.upper_bound(time);
        if (right == points.begin()) return 1 - right->second;
        if (right == points.end()) return 1 - points.rbegin()->second;
        auto left = std::prev(right);
        double alpha = (time - left->first) / (right->first - left->first);
        return 1 - (left->second + (right->second - left->second) * alpha);
    }
};
Blend blend(Json const& input) {
    Blend result;
    if (input.is_number()) {
        result.length = finiteNumber(input, "Invalid controller blend length");
        if (result.length < 0) throw std::runtime_error("Negative controller blend length");
    } else if (input.is_object()) {
        for (auto const& [key, value] : input.items()) {
            char* end = nullptr;
            double time = std::strtod(key.c_str(), &end);
            if (end == key.c_str() || *end != '\0' || !std::isfinite(time) || time < 0)
                throw std::runtime_error("Invalid controller blend point");
            if (!result.points.emplace(time, finiteNumber(value, "Invalid controller blend value")).second)
                throw std::runtime_error("Duplicate controller blend point");
        }
        if (!result.points.empty()) result.length = result.points.rbegin()->first;
    } else if (!input.is_null()) throw std::runtime_error("Invalid controller blend");
    return result;
}
struct Choice { std::string name; std::optional<MolangProgram> condition; };
struct State {
    std::vector<Choice> animations, transitions;
    std::vector<std::string> sounds;
    std::optional<MolangProgram> entry, exit;
    Blend blend;
    bool builtin = false;
    std::string child;
};
struct Definition { std::string initial; std::map<std::string, State> states; };
struct Playback {
    ClipPose lastRaw;
    double endingAt = -1;
    double loopOffset = 0;
    std::uint64_t eventCycle = 0, soundCycle = 0;
    bool running = false, retired = false;
};
constexpr double endingLength = 3.0 / 20.0;
using EndingProgress = std::map<std::string, Triple>;
bool needsEndingProgress(std::vector<Playback> const& players, double time) {
    // A player which starts ending in this update still has zero fade.
    return std::any_of(players.begin(), players.end(), [time](auto const& player) {
        return !player.retired && player.endingAt >= 0 && time > player.endingAt && time - player.endingAt < endingLength;
    });
}
void accumulate(Pose& output, ClipPose const& sampled, AnimationSampler const& sampler, std::string const& clip,
    double weight, double elapsedFade, EndingProgress* endings) {
    auto cursor = output.begin();
    bool dense = sampled.size() >= output.size() / 4;
    double fade = std::clamp(elapsedFade / endingLength, 0.0, 1.0);
    for (auto const& [name, bone] : sampled) {
        if (!bone.driven[0] && !bone.driven[1] && !bone.driven[2]) continue;
        auto& out = outputBone(output, cursor, *name, dense);
        double progress = elapsedFade > 0 && sampler.hasScaleTrack(clip, *name) ? 1 : fade;
        double effectiveWeight = weight * (1 - progress);
        auto* minimum = endings ? &endings->try_emplace(*name, Triple{1, 1, 1}).first->second : nullptr;
        for (int c = 0; c < 3; ++c) if (bone.driven[c]) {
            if (minimum) (*minimum)[c] = std::min((*minimum)[c], progress);
            auto const& value = c == 0 ? bone.position : c == 1 ? bone.rotation : bone.scale;
            auto& destination = channel(out, c);
            for (int axis = 0; axis < 3; ++axis) {
                if (c == 2) destination[axis] *= 1 + (value[axis] - 1) * effectiveWeight;
                else destination[axis] += value[axis] * effectiveWeight;
            }
            out.driven[c] = true;
        }
    }
}
struct Instance {
    std::string name, state;
    double entered = 0, lastTime = -1;
    std::size_t generation = 0;
    Pose from, output;
    std::vector<bool> applied, finished;
    std::vector<Playback> playback;
    std::unique_ptr<Instance> child;
    bool handled = true;
    explicit Instance(std::string value = {}) : name(std::move(value)) {}
};
struct FunctionBindings {
    MolangContext& context;
    std::map<std::string, std::optional<MolangContext::Function>> previous;
    explicit FunctionBindings(MolangContext& value) : context(value) {}
    void bind(std::string const& name, MolangContext::Function function) {
        auto old = context.functions.find(name);
        previous[name] = old == context.functions.end() ? std::optional<MolangContext::Function>{} : old->second;
        context.functions[name] = std::move(function);
    }
    ~FunctionBindings() {
        for (auto& [name, function] : previous) {
            if (function) context.functions[name] = std::move(*function);
            else context.functions.erase(name);
        }
    }
};
std::string controllerEvent(std::string name) {
    std::size_t offset = 0;
    while ((offset = name.find('.', offset)) != std::string::npos) { name.replace(offset, 1, "_ctrl_"); offset += 6; }
    return name;
}
std::string eventController(std::string name) {
    std::size_t offset = 0;
    while ((offset = name.find("_ctrl_", offset)) != std::string::npos) { name.replace(offset, 6, "."); ++offset; }
    return name;
}
constexpr std::array<char const*, 6> EquipmentSlots{"mainhand", "offhand", "feet", "legs", "chest", "head"};
void bindEquipmentQueries(MolangContext& context, HandInput const& input, bool firstPerson = false) {
    static constexpr std::array<char const*, 6> presence{
        "ysm.has_mainhand", "ysm.has_offhand", "ysm.has_boots", "ysm.has_leggings", "ysm.has_chest_plate", "ysm.has_helmet"};
    for (std::size_t i = 0; i < EquipmentSlots.size(); ++i) {
        auto const& id = input.equipment(EquipmentSlots[i]).id;
        context.queries[presence[i]] = id.empty() || id == "minecraft:air" ? 0.0 : 1.0;
    }
    context.queries["ysm.has_elytra"] = input.equipment("chest").id == "minecraft:elytra" ? 1.0 : 0.0;
    context.queries["ysm.armor_value"] = static_cast<double>(input.armorValue);
    context.queries["ysm.in_shield_block_cooldown"] = !firstPerson && input.shieldBlockCooldown ? 1.0 : 0.0;
    context.queries["ysm.mainhand_charged_crossbow"] = input.items[0].charged ? 1.0 : 0.0;
    context.queries["ysm.offhand_charged_crossbow"] = input.items[1].charged ? 1.0 : 0.0;
    bool swinging = !input.sleeping && (input.swingingHand == 0 || input.swingingHand == 1);
    context.queries["ysm.swinging"] = swinging ? 1.0 : 0.0;
    context.queries["ysm.swinging_arm"] = swinging ? static_cast<double>(input.swingingHand) : 0.0;
    context.queries["ysm.swing_time"] = swinging ? input.swingProgress : 0.0;
    context.queries["query.is_using_item"] = input.usingHand >= 0 && !input.sleeping ? 1.0 : 0.0;
    if (firstPerson) {
        context.queries["query.is_first_person"] = 1.0;
        context.queries["ysm.person_view"] = 0.0;
    }
    auto equipment = std::make_shared<HandInput const>(input);
    auto select = [equipment](std::span<MolangValue const> args) -> HandItem const* {
        if (args.empty() || !args.front().isString()) return nullptr;
        auto slot = AnimationSampler::normalized(args.front().string());
        if (std::ranges::find(EquipmentSlots, slot) == EquipmentSlots.end()) return nullptr;
        return &equipment->equipment(slot);
    };
    context.functions["query.is_item_name_any"] = [select](std::span<MolangValue const> args) -> MolangValue {
        auto item = select(args);
        if (!item) return {};
        if (item->id.empty() || item->id == "minecraft:air") return 0.0;
        for (auto const& argument : args.subspan(1)) {
            if (!argument.isString()) return {};
            auto expected = canonicalRegistryId(argument.string());
            if (expected.empty()) return {};
            if (expected == item->id) return 1.0;
        }
        return 0.0;
    };
    for (bool all : {false, true}) {
        context.functions[all ? "query.equipped_item_all_tags" : "query.equipped_item_any_tag"]
            = [select, all](std::span<MolangValue const> args) -> MolangValue {
            auto item = select(args);
            if (!item) return {};
            if (item->id.empty() || item->id == "minecraft:air") return 0.0;
            for (auto const& argument : args.subspan(1)) {
                if (!argument.isString()) return {};
                auto tag = canonicalRegistryId(argument.string());
                if (tag.empty()) return {};
                if ((std::ranges::find(item->tags, tag) != item->tags.end()) != all) return all ? 0.0 : 1.0;
            }
            return all ? 1.0 : 0.0;
        };
    }
    context.functions["ysm.equipped_enchantment_level"] = [select](std::span<MolangValue const> args) -> MolangValue {
        auto item = select(args);
        if (!item) return {};
        double total = 0;
        for (auto const& argument : args.subspan(1)) {
            if (!argument.isString()) return {};
            auto id = canonicalRegistryId(argument.string());
            if (id.empty()) return {};
            if (auto found = item->enchantments.find(id); found != item->enchantments.end()) total += found->second;
        }
        return total;
    };
    context.functions["query.max_durability"] = [select](std::span<MolangValue const> args) -> MolangValue {
        if (auto item = select(args)) return static_cast<double>(item->maxDurability);
        return {};
    };
    context.functions["query.remaining_durability"] = [select](std::span<MolangValue const> args) -> MolangValue {
        if (auto item = select(args)) return static_cast<double>(item->maxDurability - item->damage);
        return {};
    };
    for (auto const* operation : {"hold", "use", "swing", "armor"}) {
        context.functions[std::string("ctrl.") + operation] = [equipment, operation](std::span<MolangValue const> args) -> MolangValue {
            auto const& input = *equipment;
            if (args.size() < 2 || args.size() > 3) throw std::runtime_error("Invalid controller item query arguments");
            auto slot = AnimationSampler::normalized(args[0].string());
            bool armor = slot == "feet" || slot == "legs" || slot == "chest" || slot == "head";
            if ((slot != "mainhand" && slot != "offhand" && !armor) || armor != (std::string_view(operation) == "armor")) return {};
            if ((std::string_view(operation) == "use" && (input.usingHand < 0 || input.sleeping))
                || (std::string_view(operation) == "swing" && (input.swingingHand < 0 || input.sleeping))) return 0.0;
            auto const& item = input.equipment(slot);
            auto filter = args[1].string();
            if (filter == "empty") return item.id.empty() ? 1.0 : 0.0;
            if (filter.size() < 2) return 0.0;
            auto value = filter.substr(1);
            if (filter[0] == '$') return (item.id.empty() ? "minecraft:air" : item.id) == value ? 1.0 : 0.0;
            if (filter[0] == '#') return std::find(item.tags.begin(), item.tags.end(), value) != item.tags.end() ? 1.0 : 0.0;
            return !armor && filter[0] == ':' && item.matchesAnimationCategory(value) ? 1.0 : 0.0;
        };
    }
}
bool hasArmorCondition(AnimationSampler const& sampler, std::string const& slot) {
    if (sampler.hasClip(slot + ":default")) return true;
    return std::any_of(sampler.animationNames().begin(), sampler.animationNames().end(), [&](auto const& name) {
        return name.starts_with(slot + "$") || name.starts_with(slot + "#");
    });
}
std::optional<AnimationLayer> armorRequest(AnimationSampler const& sampler, std::string const& slot, HandInput const& input) {
    auto const& item = input.equipment(slot);
    if (item.id.empty()) return {};
    auto name = slot + "$" + item.id;
    if (!sampler.hasClip(name)) {
        name.clear();
        for (auto const& candidate : sampler.animationNames()) if (candidate.starts_with(slot + "#")) {
            auto tag = candidate.substr(slot.size() + 1);
            if (std::find(item.tags.begin(), item.tags.end(), tag) != item.tags.end()) { name = candidate; break; }
        }
        if (name.empty() && sampler.hasClip(slot + ":default")) name = slot + ":default";
    }
    if (name.empty()) return {};
    AnimationLayer request{name, 0};
    request.loop = AnimationLoop::Loop;
    return request;
}
std::optional<AnimationLayer> mountRequest(AnimationSampler const& sampler, HandInput::Mount const* mount,
    std::string const& prefix, bool enabled) {
    if (!enabled || !mount || !mount->alive) return {};
    auto name = prefix + "$" + mount->id;
    if (!sampler.hasClip(name)) {
        name.clear();
        for (auto const& clip : sampler.animationNames()) if (clip.starts_with(prefix + "#")) {
            auto tag = clip.substr(prefix.size() + 1);
            if (std::find(mount->tags.begin(), mount->tags.end(), tag) != mount->tags.end()) { name = clip; break; }
        }
        if (name.empty() && prefix == "vehicle") name = mount->legacyAnimation.empty() ? "sit" : mount->legacyAnimation;
    }
    if (name.empty()) return {};
    AnimationLayer request{name, 0};
    request.loop = AnimationLoop::Loop;
    return request;
}
}
struct AnimationControllers::Impl {
    std::map<std::string, Definition> definitions;
    std::map<std::string, Instance> instances;
    std::set<std::string> diagnostics;
    struct Fallback {
        bool overridden = false, reloadOnSelection = false;
        CodedAnimationPlayer player;
        double lastRequestTime = -1;
        std::optional<AnimationLayer> selected;
        std::optional<double> transitionLength;
    };
    std::map<std::string, Fallback> fallbacks;
    Pose playerPose;
    BonePoseHistory poseHistory;
    double playerTime = -1;
    double lastBaseTime = -1;
    bool firstPerson = false;
    bool humanoid = true;
    std::optional<ReplacedModelKind> replacedKind;
    std::vector<std::string> lastStages;

    void select(Fallback& fallback, AnimationLayer request) {
        request.restart = request.restart || fallback.reloadOnSelection;
        fallback.reloadOnSelection = false;
        fallback.selected = std::move(request);
    }
    void coded(std::string const& name, Fallback& fallback, AnimationSampler const& sampler, double time,
        Pose& pose, AnimationEvaluation& evaluation, std::span<AnimationLayer const> defaults,
        double transitionLength, bool paused = false, std::map<double, double> const& points = {},
        MolangValue* activeStatus = nullptr) {
        auto& context = evaluation.expressions;
        MolangSoundScope sounds(context, "controller-script:" + name);
        FunctionBindings bindings(context);
        MolangDeferredScope deferred(context, &fallback.player);
        if (sampler.needsControllerActions()) {
        bindings.bind("ctrl.set_animation", [this, &fallback, &context, additive = name.starts_with("player.parallel_")
                || name.starts_with("fp.arm.parallel_") || name.starts_with("vehicle.parallel_")
                || name.starts_with("projectile.parallel_")](auto args) -> MolangValue {
            if (args.empty() || args.size() > 2 || !args[0].isString()) throw std::runtime_error("Invalid ctrl.set_animation arguments");
            if (args[0].string().empty()) return {};
            AnimationLayer selected{args[0].string(), 0, false, {}, false};
            selected.additiveRotation = additive;
            auto loop = args.size() == 2 ? args[1].number() : 0;
            if (loop >= 10 && loop < 11) selected.loop = AnimationLoop::Loop;
            else if (loop >= 11 && loop < 12) selected.loop = AnimationLoop::Once;
            else if (loop >= 12 && loop < 13) selected.loop = AnimationLoop::Hold;
            context.submit([this, &fallback, selected] { select(fallback, selected); });
            return {};
        });
        bindings.bind("ctrl.set_beginning_transition_length", [&fallback, &context](auto args) -> MolangValue {
            if (args.size() != 1) throw std::runtime_error("Invalid ctrl.set_beginning_transition_length arguments");
            double length = args[0].number();
            if (length >= 0) context.submit([&fallback, length] { fallback.transitionLength = length; });
            return {};
        });
        bindings.bind("ctrl.indicate_reload", [&fallback, &context](auto args) -> MolangValue {
            if (!args.empty()) throw std::runtime_error("Invalid ctrl.indicate_reload arguments");
            context.submit([&fallback] { fallback.reloadOnSelection = true; });
            return {};
        });
        bindings.bind("ctrl.reset", [&fallback, &context, &evaluation](auto args) -> MolangValue {
            if (!args.empty()) throw std::runtime_error("Invalid ctrl.reset arguments");
            context.submit([&fallback, &evaluation, owner = context.soundOwner] {
                evaluation.expressions.emitSound({owner, {}});
                fallback.player.reset(evaluation);
                fallback.selected.reset();
                fallback.reloadOnSelection = false;
            });
            return {};
        });
        }
        std::optional<CodedPlayState> override;
        auto handlers = sampler.functionLibrary()->events(controllerEvent(name));
        if (!handlers.empty()) {
            context.queries["query.any_animation_finished"] = fallback.player.currentAnimationFinished() ? 1.0 : 0.0;
            context.queries["query.all_animations_finished"] = fallback.player.currentAnimationFinished() ? 1.0 : 0.0;
            context.queries["query.anim_time"] = fallback.player.animationTime();
            auto failures = context.failures;
            MolangActionScope permission(context, true);
            float value = static_cast<float>(handlers.front().evaluate(context).number());
            if (failures == context.failures) {
                if (value == 2) override = CodedPlayState::Continue;
                else if (value == 3) override = CodedPlayState::Stop;
                else if (value == 4) override = CodedPlayState::Pause;
            }
        }
        auto command = override.value_or(paused ? CodedPlayState::Pause : defaults.empty() ? CodedPlayState::Stop : CodedPlayState::Continue);
        if (!override && !defaults.empty()) select(fallback, defaults.front());
        auto* selected = fallback.selected ? &*fallback.selected : nullptr;
        fallback.player.process(sampler, selected, time, pose, playerPose, evaluation,
            fallback.transitionLength.value_or(transitionLength), command,
            fallback.transitionLength ? std::map<double, double>{} : points, activeStatus);
        if (command == CodedPlayState::Continue && fallback.selected) fallback.selected->restart = false;
    }

    void stage(std::string const& name, AnimationSampler const& sampler, double time, Pose& pose,
        AnimationEvaluation& evaluation, std::span<AnimationLayer const> defaults = {}, double transition = 0, bool paused = false) {
        lastStages.push_back(name);
        auto [found, inserted] = fallbacks.try_emplace(name);
        auto& fallback = found->second;
        if (inserted) fallback.player = CodedAnimationPlayer(name);
        if (definitions.contains(name)) {
            auto [active, created] = instances.try_emplace(name);
            if (created) active->second.name = name;
            if (process(active->second, sampler, time, pose, playerPose, evaluation, 1)) {
                if (!fallback.overridden) fallback.player.reset(evaluation);
                fallback.overridden = true;
                return;
            }
        }
        fallback.overridden = false;
        std::map<double, double> const* points = nullptr;
        if (auto active = instances.find(name); active != instances.end() && !active->second.state.empty()) {
            auto const& state = definitions.at(name).states.at(active->second.state);
            transition = state.blend.length;
            points = &state.blend.points;
        }
        coded(name, fallback, sampler, time, pose, evaluation, defaults, transition, paused,
            points ? *points : std::map<double, double>{});
    }

    void armor(std::string const& category, AnimationSampler const& sampler, double time, Pose& pose,
        AnimationEvaluation& evaluation, HandInput const& input) {
        if (!input.enabled) return;
        for (auto const* slot : EquipmentSlots) {
            auto name = category + ".armor_" + slot;
            bool automatic = std::string_view(slot) != "mainhand" && std::string_view(slot) != "offhand"
                && hasArmorCondition(sampler, slot);
            if (!automatic && !definitions.contains(name) && sampler.functionLibrary()->events(controllerEvent(name)).empty()) continue;
            auto request = armorRequest(sampler, slot, input);
            stage(name, sampler, time, pose, evaluation, request ? std::span<AnimationLayer const>(&*request, 1) : std::span<AnimationLayer const>{});
        }
    }

    void exit(Instance& instance, MolangContext& context) {
        MolangSoundScope sounds(context, "controller-script:" + instance.name);
        if (!instance.state.empty()) context.emitSound({context.soundOwner, {}});
        if (instance.child) { exit(*instance.child, context); instance.child.reset(); }
        if (!instance.state.empty()) {
            auto const& state = definitions.at(instance.name).states.at(instance.state);
            if (state.exit) { MolangActionScope permission(context, true); state.exit->evaluate(context); }
        }
    }
    void transition(Instance& instance, std::string const& name, double time, Pose const& previous,
        AnimationEvaluation& evaluation) {
        auto& context = evaluation.expressions;
        exit(instance, context);
        auto prefix = "controller:" + instance.name;
        evaluation.retireTimelines([&](auto const& entry) {
            return entry.first.starts_with(prefix + ":") || entry.first.starts_with(prefix + ".");
        });
        instance.state = name;
        instance.entered = time;
        ++instance.generation;
        instance.from = previous;
        auto const& state = definitions.at(instance.name).states.at(name);
        auto size = state.builtin || !state.child.empty() ? 0 : state.animations.size();
        instance.applied.assign(size, true);
        instance.finished.assign(size, false);
        instance.playback.assign(size, Playback{});
        context.queries["query.state_time"] = 0.0;
        context.queries["query.anim_time"] = 0.0;
        context.queries["query.any_animation_finished"] = 0.0;
        context.queries["query.all_animations_finished"] = state.animations.empty() ? 1.0 : 0.0;
        if (state.entry) {
            MolangSoundScope sounds(context, "controller-script:" + instance.name);
            MolangActionScope permission(context, true); state.entry->evaluate(context);
        }
        for (auto const& sound : state.sounds)
            context.emitSound({"controller-script:" + instance.name, sound});
    }
    bool process(Instance& instance, AnimationSampler const& sampler, double time, Pose& pose,
        Pose const& previous, AnimationEvaluation& evaluation, unsigned depth) {
        if (instance.lastTime == time) { assign(pose, instance.output); return instance.handled; }
        if (instance.lastTime > time) {
            evaluation.expressions.emitSound({"controller-script:" + instance.name, {}});
            auto prefix = "controller:" + instance.name;
            evaluation.retireTimelines([&](auto const& entry) {
                return entry.first.starts_with(prefix + ":") || entry.first.starts_with(prefix + ".");
            });
            instance = Instance(instance.name);
        }
        auto& context = evaluation.expressions;
        MolangSoundScope sounds(context, "controller-script:" + instance.name);
        MolangDeferredScope controllerDeferred(context, &instance);
        auto const& definition = definitions.at(instance.name);
        if (instance.state.empty() && !definition.states.contains(definition.initial)) {
            // Java source controllers may deliberately have no initial state.
            // Their presence suppresses the coded slot without adding a pose.
            instance.output.clear();
            instance.lastTime = time;
            instance.handled = true;
            return true;
        }
        std::set<std::string> visited;
        bool initialized = instance.state.empty();
        if (initialized) {
            transition(instance, definition.initial, time, previous.empty() ? pose : previous, evaluation);
            visited.insert(instance.state);
        }
        while (true) {
            auto const& state = definition.states.at(instance.state);
            // Initialization of a nonempty state does not consume its first
            // transition until the next update, matching Java's player setup.
            if (initialized && !state.animations.empty() && !state.builtin && state.child.empty()) break;
            initialized = false;
            bool any = false, all = true, active = false;
            for (std::size_t i = 0; i < instance.applied.size(); ++i) if (instance.applied[i]) {
                active = true;
                any = any || instance.finished[i];
                all = all && instance.finished[i];
            }
            context.queries["query.any_animation_finished"] = any || !active ? 1.0 : 0.0;
            context.queries["query.all_animations_finished"] = all ? 1.0 : 0.0;
            context.queries["query.state_time"] = time - instance.entered;
            bool moved = false;
            for (auto const& next : state.transitions) {
                if (context.remainingSteps == 0) break;
                auto failures = context.failures;
                bool accepts = next.condition->evaluate(context).number() != 0;
                if (!accepts || context.failures != failures) continue;
                // Java stops this update at the first true transition even if
                // its source destination is absent. No entry/exit hook runs.
                if (!definition.states.contains(next.name)) break;
                if (!visited.insert(next.name).second) {
                    diagnostics.insert("Controller transition cycle: " + instance.name);
                    break;
                }
                transition(instance, next.name, time, previous.empty() ? pose : previous, evaluation);
                moved = true;
                break;
            }
            if (!moved) break;
            auto const& entered = definition.states.at(instance.state);
            if (!entered.animations.empty() && !entered.builtin && entered.child.empty()) break;
        }
        auto const& state = definition.states.at(instance.state);
        instance.output.clear();
        instance.lastTime = time;
        instance.handled = !state.builtin;
        if (!state.child.empty()) {
            auto childName = instance.name + "." + state.child;
            if (depth >= 5) diagnostics.insert("Controller nesting limit: " + childName);
            else if (!definitions.contains(childName)) diagnostics.insert("Missing child controller: " + childName);
            else {
                if (!instance.child) instance.child = std::make_unique<Instance>(childName);
                Pose childPose = pose;
                for (auto& [name, bone] : childPose) bone.driven = {};
                process(*instance.child, sampler, time, childPose, previous, evaluation, depth + 1);
                instance.output = std::move(childPose);
            }
        } else if (!state.builtin) {
            bool blending = time - instance.entered < state.blend.length;
            double local = std::max(0.0, time - instance.entered - state.blend.length);
            EndingProgress endingProgress;
            bool fadeToLower = !blending && needsEndingProgress(instance.playback, time);
            for (std::size_t i = 0; i < state.animations.size(); ++i) {
                auto const& choice = state.animations[i];
                auto failures = context.failures;
                bool applies = !choice.condition || choice.condition->evaluate(context).number() != 0;
                applies = applies && failures == context.failures;
                instance.applied[i] = applies;
                auto info = sampler.clipInfo(choice.name);
                if (!info) {
                    instance.finished[i] = true;
                    diagnostics.insert("Missing controller animation: " + choice.name);
                    continue;
                }
                auto& playback = instance.playback[i];
                MolangDeferredScope animationDeferred(context, &playback);
                if (playback.endingAt >= 0 && time - playback.endingAt >= endingLength)
                    playback.retired = true;
                if (playback.retired) continue;
                AnimationLayer layer{choice.name, local, false,
                    "controller:" + instance.name + ":" + std::to_string(instance.generation) + ":" + std::to_string(i), false, applies, applies};
                // PLAY_ONCE begins its ending transition on the update that
                // observes completion, keeping the last actually drawn raw pose.
                if (!blending && playback.running && playback.endingAt < 0
                    && !info->loop && !info->hold && local >= info->length) {
                    layer.finishEvents = true;
                    sampler.advanceEvents(std::span<AnimationLayer const>(&layer, 1), time, evaluation);
                    playback.endingAt = time;
                    instance.finished[i] = true;
                }
                // Loop/hold completion stays true after the first traversal.
                instance.finished[i] = instance.finished[i] || (!blending && local > info->length);
                bool ending = playback.endingAt >= 0;
                double clipTime = ending ? info->length : std::max(0.0, local - playback.loopOffset);
                if (!ending && clipTime > info->length) {
                    if (info->loop) {
                        double remainder = info->length > 0 ? std::fmod(clipTime, info->length) : 0;
                        // Source AnimationPlayer flushes/reset keys only once
                        // per update, and muted updates retain instruction keys.
                        playback.soundCycle = AnimationSampler::advanceCycle(playback.soundCycle, 1, context);
                        if (applies)
                            playback.eventCycle = AnimationSampler::advanceCycle(playback.eventCycle, 1, context);
                        clipTime = remainder;
                        playback.loopOffset = local - clipTime;
                    }
                    else if (info->hold) clipTime = info->length;
                }
                layer.eventPosition = AnimationEventPosition{playback.soundCycle, clipTime, playback.eventCycle};
                if (!blending && !ending)
                    sampler.advanceEvents(std::span<AnimationLayer const>(&layer, 1), time, evaluation);
                double weight = sampler.blendWeight(choice.name, clipTime, context);
                if (!ending && !blending) playback.running = true;
                if (!applies) continue;
                ClipPose transitionSample;
                auto& sampled = blending && !ending ? transitionSample : playback.lastRaw;
                if (!ending) sampler.sampleRawClip(sampled, pose, choice.name, clipTime, context);
                accumulate(instance.output, sampled, sampler, choice.name, weight,
                    ending ? time - playback.endingAt : 0, fadeToLower ? &endingProgress : nullptr);
            }
            if (blending) {
                double alpha = state.blend.progress(time - instance.entered);
                for (auto& [name, bone] : instance.output) for (int c = 0; c < 3; ++c) if (bone.driven[c]) {
                    BonePose origin;
                    if (auto found = instance.from.find(name); found != instance.from.end()) origin = found->second;
                    auto& output = channel(bone, c);
                    auto const& start = channel(origin, c);
                    if (c == 1) output = sampler.blendRotation(name, start, output, alpha);
                    else for (int axis = 0; axis < 3; ++axis) output[axis] = start[axis] + (output[axis] - start[axis]) * alpha;
                }
            } else if (fadeToLower) for (auto& [name, bone] : instance.output) for (int c = 0; c < 3; ++c) if (bone.driven[c]) {
                // Bedrock controllers attenuate ending contributions before
                // blending the resulting group back into the lower channels.
                double progress = endingProgress.at(name)[c];
                if (progress == 0) continue;
                BonePose lower;
                if (auto found = pose.find(name); found != pose.end()) lower = found->second;
                auto& output = channel(bone, c);
                auto const& target = channel(lower, c);
                if (c == 1) output = sampler.blendRotation(name, output, target, progress);
                else for (int axis = 0; axis < 3; ++axis) output[axis] += (target[axis] - output[axis]) * progress;
            }
        }
        assign(pose, instance.output);
        return instance.handled;
    }
    bool observe(Instance const& instance, AnimationSampler const& sampler, Pose& pose, MolangContext& context) const {
        auto const& definition = definitions.at(instance.name);
        auto foundState = definition.states.find(instance.state);
        if (foundState == definition.states.end()) return instance.handled;
        auto const& state = foundState->second;
        bool any = false, all = true, active = false;
        for (std::size_t i = 0; i < instance.applied.size(); ++i) if (instance.applied[i]) {
            active = true;
            any = any || instance.finished[i];
            all = all && instance.finished[i];
        }
        context.queries["query.any_animation_finished"] = any || !active ? 1.0 : 0.0;
        context.queries["query.all_animations_finished"] = all ? 1.0 : 0.0;
        context.queries["query.state_time"] = instance.lastTime - instance.entered;
        if (!state.child.empty()) {
            if (instance.child) observe(*instance.child, sampler, pose, context);
            return instance.handled;
        }
        if (state.builtin) return false;
        Pose output;
        bool blending = instance.lastTime - instance.entered < state.blend.length;
        double local = std::max(0.0, instance.lastTime - instance.entered - state.blend.length);
        EndingProgress endingProgress;
        bool fadeToLower = !blending && needsEndingProgress(instance.playback, instance.lastTime);
        for (std::size_t i = 0; i < state.animations.size(); ++i) {
            auto const& choice = state.animations[i];
            auto const& playback = instance.playback[i];
            if (playback.retired) continue;
            auto failures = context.failures;
            bool applies = !choice.condition || choice.condition->evaluate(context).number() != 0;
            if (!applies || context.failures != failures) continue;
            auto info = sampler.clipInfo(choice.name);
            if (!info) continue;
            bool ending = playback.endingAt >= 0;
            double clock = ending ? info->length : std::max(0.0, local - playback.loopOffset);
            if (!ending && clock > info->length) {
                if (info->loop) clock = info->length > 0 ? std::fmod(clock, info->length) : 0;
                else if (info->hold) clock = info->length;
            }
            double weight = sampler.blendWeight(choice.name, clock, context);
            ClipPose sampled;
            if (!ending) sampler.sampleRawClip(sampled, pose, choice.name, clock, context);
            accumulate(output, ending ? playback.lastRaw : sampled, sampler, choice.name, weight,
                ending ? instance.lastTime - playback.endingAt : 0, fadeToLower ? &endingProgress : nullptr);
        }
        if (blending) {
            double alpha = state.blend.progress(instance.lastTime - instance.entered);
            for (auto& [name, bone] : output) for (int c = 0; c < 3; ++c) if (bone.driven[c]) {
                BonePose origin;
                if (auto found = instance.from.find(name); found != instance.from.end()) origin = found->second;
                auto& destination = channel(bone, c);
                auto const& start = channel(origin, c);
                if (c == 1) destination = sampler.blendRotation(name, start, destination, alpha);
                else for (int axis = 0; axis < 3; ++axis) destination[axis] = start[axis] + (destination[axis] - start[axis]) * alpha;
            }
        } else if (fadeToLower) for (auto& [name, bone] : output) for (int c = 0; c < 3; ++c) if (bone.driven[c]) {
            double progress = endingProgress.at(name)[c];
            if (progress == 0) continue;
            BonePose lower;
            if (auto found = pose.find(name); found != pose.end()) lower = found->second;
            auto& destination = channel(bone, c);
            auto const& target = channel(lower, c);
            if (c == 1) destination = sampler.blendRotation(name, destination, target, progress);
            else for (int axis = 0; axis < 3; ++axis) destination[axis] += (target[axis] - destination[axis]) * progress;
        }
        assign(pose, output);
        return true;
    }
};
AnimationControllers::AnimationControllers(Json const& document, ControllerPolicy policy) : impl(std::make_unique<Impl>()) {
    auto definitions = document.value("animation_controllers", Json::object());
    if (!definitions.is_object() || definitions.size() > 512) throw std::runtime_error("Invalid controller definitions");
    for (auto const& [name, source] : definitions.items()) {
        if (name.empty() || !source.is_object()) throw std::runtime_error("Invalid controller name");
        Definition definition;
        definition.initial = source.value("initial_state", "default");
        auto states = source.value("states", Json::object());
        if (!states.is_object() || states.size() > 4096 || (states.empty() && policy == ControllerPolicy::Strict))
            throw std::runtime_error("Invalid controller states");
        for (auto const& [stateName, data] : states.items()) {
            if (stateName.empty() || !data.is_object()) throw std::runtime_error("Invalid controller state");
            State state;
            state.builtin = stateName == "ysm-builtin";
            if (stateName.starts_with("ysm-entry-")) state.child = stateName.substr(10);
            auto compile = [&](std::string const& text, std::string const& field) {
                try { return MolangProgram(text); }
                catch (std::runtime_error const& error) {
                    if (policy == ControllerPolicy::Strict) throw;
                    // YSM FrontendPrograms.compile replaces a failed source
                    // program with constant zero, including an entire entry/
                    // exit statement block. Never guess the missing operator.
                    if (impl->diagnostics.size() < 128)
                        impl->diagnostics.insert("Invalid source controller program replaced with 0: "
                            + name + "/" + stateName + "/" + field + ": " + error.what());
                    return MolangProgram(0.0);
                }
            };
            state.entry = statements(data.value("on_entry", Json()), [&](auto const& text) { return compile(text, "on_entry"); });
            state.exit = statements(data.value("on_exit", Json()), [&](auto const& text) { return compile(text, "on_exit"); });
            state.blend = blend(data.value("blend_transition", Json()));
            auto parse = [&](char const* field, std::vector<Choice>& output, bool transitions) {
                auto values = data.value(field, Json::array());
                if (!values.is_array() || values.size() > 4096) throw std::runtime_error("Invalid controller reference list");
                for (auto const& value : values) {
                    if (!transitions && value.is_string()) output.push_back({value.get<std::string>(), {}});
                    else if (value.is_object() && !value.empty()) {
                        for (auto const& [target, condition] : value.items())
                            output.push_back({target, expression(condition, [&](auto const& text) { return compile(text, std::string(field) + "/" + target); })});
                    } else throw std::runtime_error("Invalid controller reference");
                }
            };
            parse("animations", state.animations, false);
            parse("transitions", state.transitions, true);
            auto sounds = data.value("sound_effects", Json::array());
            if (!sounds.is_array() || sounds.size() > 4096) throw std::runtime_error("Invalid controller sound list");
            for (auto const& sound : sounds) {
                if (!sound.is_object()) throw std::runtime_error("Invalid controller sound effect");
                auto effect = sound.value("effect", std::string{});
                // Honor nonblank authored selectors. The upstream controller's
                // negated isNoneBlank check currently drops these selectors.
                if (effect.find_first_not_of(" \t\r\n\f\v") != std::string::npos)
                    state.sounds.push_back(std::move(effect));
            }
            definition.states.emplace(stateName, std::move(state));
        }
        if (!definition.states.contains(definition.initial)) {
            if (policy == ControllerPolicy::Strict) throw std::runtime_error("Missing initial controller state: " + name);
            if (!definition.states.empty() && impl->diagnostics.size() < 128)
                impl->diagnostics.insert("Unresolved initial source state: " + name + "/" + definition.initial);
        }
        for (auto const& [stateName, state] : definition.states) for (auto const& next : state.transitions)
            if (!definition.states.contains(next.name)) {
                if (policy == ControllerPolicy::Strict) throw std::runtime_error("Missing controller transition state: " + next.name);
                if (impl->diagnostics.size() < 128)
                    impl->diagnostics.insert("Unresolved source transition: " + name + "/" + stateName + " -> " + next.name);
            }
        impl->definitions.emplace(name, std::move(definition));
    }
}
AnimationControllers::~AnimationControllers() = default;
std::set<std::string> AnimationControllers::roamingFields(AnimationSampler const& sampler, bool firstPerson, RoamingLimit limit) const {
    auto fields = sampler.roamingFields(firstPerson);
    auto include = [&](MolangProgram const& program) {
        auto names = program.roamingFields();
        fields.insert(names.begin(), names.end());
    };
    // Upstream ExpressionSet.select includes all controller definitions in
    // both sets; only animation files and exclusive event routes are split.
    for (auto const& [name, definition] : impl->definitions) for (auto const& [stateName, state] : definition.states) {
        if (state.entry) include(*state.entry);
        if (state.exit) include(*state.exit);
        for (auto const& choice : state.animations) if (choice.condition) include(*choice.condition);
        for (auto const& choice : state.transitions) if (choice.condition) include(*choice.condition);
    }
    MolangRoamingState::validateLayout(fields, limit);
    return fields;
}
bool AnimationControllers::contains(std::string const& name) const { return impl->definitions.contains(name); }
std::vector<std::string> AnimationControllers::matching(std::string const& prefix) const {
    std::vector<std::string> result;
    for (auto const& [name, definition] : impl->definitions)
        if (name == prefix || name.starts_with(prefix + "_")) result.push_back(name);
    return result;
}
bool AnimationControllers::apply(std::string const& name, AnimationSampler const& sampler, double time,
    Pose& pose, Pose const& previous, AnimationEvaluation& evaluation) {
    if (!std::isfinite(time) || time < 0) throw std::runtime_error("Invalid controller time");
    sampler.bindFunctions(evaluation);
    if (!contains(name)) return false;
    auto [found, inserted] = impl->instances.try_emplace(name);
    if (inserted) found->second.name = name;
    return impl->process(found->second, sampler, time, pose, previous, evaluation, 1);
}
bool AnimationControllers::hasActiveStageAnimation(std::string const& name) const {
    auto stage = impl->fallbacks.find(name);
    return stage != impl->fallbacks.end() && (stage->second.overridden || stage->second.player.active());
}
Json AnimationControllers::swingStates() const {
    Json states = Json::object();
    for (auto const& [name, instance] : impl->instances) {
        if (name == "player.swing" || name.starts_with("player.pre_swing") || name.starts_with("player.post_swing"))
            states[name] = instance.state;
    }
    return states;
}
Json AnimationControllers::report() const {
    Json states = Json::object();
    std::function<void(Instance const&)> append = [&](Instance const& instance) {
        states[instance.name] = {{"state", instance.state}, {"entered", instance.entered}, {"generation", instance.generation}};
        Json playback = Json::array();
        for (std::size_t i = 0; i < instance.playback.size(); ++i) {
            auto const& player = instance.playback[i];
            playback.push_back({{"name", impl->definitions.at(instance.name).states.at(instance.state).animations.at(i).name},
                    {"phase", player.retired ? "idle" : player.endingAt >= 0 ? "ending" : player.running ? "running" : "beginning"},
                {"ending_at", player.endingAt}, {"cycle", player.eventCycle}, {"finished", instance.finished[i]}, {"applied", instance.applied[i]}});
        }
        states[instance.name]["animations"] = std::move(playback);
        if (instance.child) append(*instance.child);
    };
    for (auto const& [name, instance] : impl->instances) append(instance);
    Json coded = Json::object();
    for (auto const& [name, fallback] : impl->fallbacks) if (!fallback.overridden) {
        coded[name] = fallback.player.report();
        coded[name]["selected_animation"] = fallback.selected ? fallback.selected->name : "";
        coded[name]["reload_on_selection"] = fallback.reloadOnSelection;
    }
    return {{"definitions", impl->definitions.size()}, {"states", states}, {"coded", coded},
        {"stages", impl->lastStages}, {"diagnostics", impl->diagnostics}};
}
Pose AnimationControllers::samplePlayer(AnimationSampler const& sampler, std::string const& base,
    double baseTime, double time, AnimationEvaluation& evaluation, HandInput const& input,
    std::span<AnimationLayer const> handLayers, std::string const& extra, double extraTime, bool guiPreview) {
    if (!std::isfinite(time) || !std::isfinite(baseTime) || !std::isfinite(extraTime)
        || time < 0 || baseTime < 0 || extraTime < 0) throw std::runtime_error("Invalid player animation time");
    if (impl->playerTime == time) {
        auto observed = impl->playerPose;
        sampler.finishPose(observed, evaluation.expressions);
        return observed;
    }
    if (impl->playerTime > time) reset();
    impl->firstPerson = false;
    impl->humanoid = true;
    impl->replacedKind.reset();
    AnimationSampler::beginFrame(time, evaluation);
    sampler.bindFunctions(evaluation);
    sampler.recoverHeadPose(impl->playerPose, evaluation.expressions);
    auto& context = evaluation.expressions;
    auto [capEntry, capInserted] = impl->fallbacks.try_emplace("player.cap");
    auto& cap = capEntry->second;
    // Expose the controller's playback phase to earlier stages as well as its
    // own keyframes and subsequent parallel layers.
    if (capInserted) cap.player = CodedAnimationPlayer("player.cap");
    bool extraRequested = !extra.empty() && sampler.hasClip(extra);
    auto& playingExtra = context.queries["ctrl.playing_extra_animation"];
    playingExtra = !guiPreview && extraRequested && cap.player.active() ? 1.0 : 0.0;
    bool mainEnabled = !base.empty() && !(input.enabled && input.vehicle && input.vehicle->alive);
    for (auto const* name : {"idle", "death", "riptide", "sleep", "swim", "climb", "climbing", "ladder_up", "ladder_down", "attacked",
        "ladder_stillness", "fly", "elytra_fly", "swim_stand", "jump", "sneak", "sneaking", "run", "walk"})
        context.queries[std::string("ctrl.") + name] = mainEnabled && base == name ? 1.0 : 0.0;
    bindEquipmentQueries(context, input);
    {
        MolangActionScope permission(context, true);
        context.queries["query.anim_time"] = 0.0;
        context.queries["query.state_time"] = 0.0;
        if (!evaluation.functionsInitialized) {
            evaluation.functionsInitialized = true;
            for (auto const& handler : sampler.functionLibrary()->events("player_init")) handler.evaluate(context);
        }
        for (auto const& handler : sampler.functionLibrary()->events("player_update")) handler.evaluate(context, MolangValue(1.0));
    }
    Pose result;
    impl->lastStages.clear();
    auto stage = [&](std::string const& name, std::span<AnimationLayer const> defaults = {}, bool paused = false) {
        double transition = name == "player.main" || name == "player.hold_offhand" || name == "player.hold_mainhand"
            || name == "player.use" || name == "player.vehicle" || name == "player.passenger" ? 0.1 : 0;
        impl->stage(name, sampler, time, result, evaluation, defaults, transition, paused);
    };
    auto eventControllers = [&] {
        std::vector<std::string> result;
        for (auto event : sampler.functionLibrary()->eventNames()) {
            if (event.find("_ctrl_") == std::string::npos) continue;
            result.push_back(eventController(event));
        }
        return result;
    }();
    auto multi = [&](char const* group) {
        auto prefix = std::string("player.") + group;
        auto definitions = matching(prefix);
        std::set<std::string> roots(definitions.begin(), definitions.end());
        for (auto const& name : eventControllers) if (name == prefix || name.starts_with(prefix + "_")) roots.insert(name);
        for (auto const& name : roots) stage(name);
    };
    auto parallel = [&](char const* group, bool additive) {
        std::map<std::string, std::string> roots;
        auto prefix = std::string("player.") + group;
        for (auto const& name : matching(prefix)) if (name.starts_with(prefix + "_")) roots[name] = "";
        for (auto const& name : eventControllers) if (name.starts_with(prefix + "_")) roots[name] = "";
        for (int i = 0; i < 8; ++i) {
            auto clip = std::string(group) + std::to_string(i);
            if (sampler.hasClip(clip)) roots[prefix + "_" + std::to_string(i)] = clip;
        }
        for (auto const& [name, clip] : roots) {
            AnimationLayer fallback{clip, time, additive, {}, false};
            fallback.loop = AnimationLoop::Loop;
            stage(name, clip.empty() ? std::span<AnimationLayer const>{} : std::span<AnimationLayer const>(&fallback, 1));
        }
    };
    auto hand = [&](char const* name, char const* owner, int slot = -1) {
        std::vector<AnimationLayer> fallback;
        for (auto const& layer : handLayers) if (layer.timelineId == owner) fallback.push_back(layer);
        stage(name, fallback, slot >= 0 && (input.usingHand == slot || input.swingingHand == slot));
    };
    parallel("pre_parallel", false);
    auto vehicle = mountRequest(sampler, input.vehicle ? &*input.vehicle : nullptr, "vehicle", input.enabled);
    stage("player.vehicle", vehicle ? std::span<AnimationLayer const>(&*vehicle, 1) : std::span<AnimationLayer const>{});
    multi("pre_main");
    AnimationLayer main{base, baseTime, false, {}, false};
    main.loop = base == "death" || base == "attacked" ? AnimationLoop::Once : AnimationLoop::Loop;
    main.restart = baseTime < impl->lastBaseTime;
    impl->lastBaseTime = baseTime;
    if (main.name != evaluation.activeBase) {
        evaluation.timelineCursors.erase(main.name);
        evaluation.activeBase = main.name;
    }
    stage("player.main", mainEnabled ? std::span<AnimationLayer const>(&main, 1) : std::span<AnimationLayer const>{});
    multi("post_main");
    multi("pre_hold");
    hand("player.hold_offhand", "hand.hold_offhand", 1);
    hand("player.hold_mainhand", "hand.hold_mainhand", 0);
    multi("post_hold");
    multi("pre_swing");
    hand("player.swing", "hand.swing");
    multi("post_swing");
    multi("pre_use");
    hand("player.use", "hand.use");
    multi("post_use");
    auto passenger = mountRequest(sampler, input.passenger ? &*input.passenger : nullptr, "passenger", input.enabled);
    stage("player.passenger", passenger ? std::span<AnimationLayer const>(&*passenger, 1) : std::span<AnimationLayer const>{});
    AnimationLayer selected{extra, extraTime, false, {}, false};
    if (guiPreview) selected.loop = AnimationLoop::Loop;
    impl->lastStages.push_back("player.cap");
    selected.restart = extraRequested && (cap.lastRequestTime < 0 || extraTime < cap.lastRequestTime);
    cap.lastRequestTime = extraRequested ? extraTime : -1;
    impl->coded("player.cap", cap, sampler, time, result, evaluation,
        extraRequested ? std::span<AnimationLayer const>(&selected, 1) : std::span<AnimationLayer const>{}, 0, false, {},
        extraRequested && !guiPreview ? &playingExtra : nullptr);
    parallel("parallel", true);
    impl->armor("player", sampler, time, result, evaluation, input);
    sampler.resetUndriven(result, time, impl->poseHistory);
    // Keep authored/reset output separate from per-draw head rotation.
    // Beginning transitions must not capture the previous camera angle.
    impl->playerPose = result;
    sampler.finishPose(result, context);
    impl->playerTime = time;
    context.finishFrame();
    return result;
}
Pose AnimationControllers::sampleFirstPerson(AnimationSampler const& sampler, double time,
    AnimationEvaluation& evaluation, HandInput const& input) {
    if (!std::isfinite(time) || time < 0) throw std::runtime_error("Invalid first-person animation time");
    if (impl->playerTime == time) {
        auto result = impl->playerPose;
        sampler.finishPose(result, evaluation.expressions, false);
        return result;
    }
    if (impl->playerTime > time) reset();
    impl->firstPerson = true;
    impl->humanoid = false;
    impl->replacedKind.reset();
    AnimationSampler::beginFrame(time, evaluation);
    sampler.bindFunctions(evaluation);
    auto& context = evaluation.expressions;
    bindEquipmentQueries(context, input, true);
    context.queries["ctrl.playing_extra_animation"] = 0.0;
    // Body-exclusive init/update hooks never run in the first-person owner.
    // Its controller predicates and instruction events keep their own clocks.
    Pose result;
    impl->lastStages.clear();
    auto stage = [&](std::string const& name, std::span<AnimationLayer const> defaults = {}) {
        impl->stage(name, sampler, time, result, evaluation, defaults);
    };
    if (contains("fp.arm.misc") || !sampler.functionLibrary()->events(controllerEvent("fp.arm.misc")).empty()) stage("fp.arm.misc");
    std::map<std::string, std::string> parallel;
    for (auto const& name : matching("fp.arm.parallel")) if (name.starts_with("fp.arm.parallel_")) parallel[name] = "";
    for (auto const& event : sampler.functionLibrary()->eventNames()) {
        auto name = eventController(event);
        if (name.starts_with("fp.arm.parallel_")) parallel[name] = "";
    }
    for (int i = 0; i < 8; ++i) if (sampler.hasClip("parallel" + std::to_string(i)))
        parallel["fp.arm.parallel_" + std::to_string(i)] = "parallel" + std::to_string(i);
    for (auto const& [name, clip] : parallel) {
        AnimationLayer request{clip, time, true};
        request.loop = AnimationLoop::Loop;
        stage(name, clip.empty() ? std::span<AnimationLayer const>{} : std::span<AnimationLayer const>(&request, 1));
    }
    impl->armor("fp.arm", sampler, time, result, evaluation, input);
    sampler.resetUndriven(result, time, impl->poseHistory);
    impl->playerPose = result;
    sampler.finishPose(result, context, false);
    impl->playerTime = time;
    context.finishFrame();
    return result;
}
Pose AnimationControllers::sampleReplaced(AnimationSampler const& sampler, ReplacedModelKind kind, double time,
    AnimationEvaluation& evaluation, ReplacedAnimationInput const& input) {
    if (!std::isfinite(time) || time < 0 || !std::all_of(input.velocity.begin(), input.velocity.end(),
            [](double value) { return std::isfinite(value); })) throw std::runtime_error("Invalid replaced animation input");
    if (impl->playerTime == time && impl->replacedKind == kind) {
        auto result = impl->playerPose;
        sampler.finishPose(result, evaluation.expressions, false);
        return result;
    }
    if (impl->playerTime > time || (impl->playerTime >= 0 && impl->replacedKind != kind)) reset();
    impl->firstPerson = false;
    impl->humanoid = false;
    impl->replacedKind = kind;
    AnimationSampler::beginFrame(time, evaluation);
    sampler.bindFunctions(evaluation);
    auto& context = evaluation.expressions;
    context.queries["query.is_on_fire"] = input.onFire ? 1.0 : 0.0;
    context.queries["query.has_rider"] = input.hasPassengers ? 1.0 : 0.0;
    context.queries["ysm.shoot_item_id"] = kind == ReplacedModelKind::Projectile ? input.shootItemId : std::string{};
    if (kind == ReplacedModelKind::Projectile) {
        context.queries["ysm.in_ground"] = input.inGround ? 1.0 : 0.0;
        context.queries["ysm.on_ground_time"] = input.inGround ? std::max(input.onGroundTicks, 0) : 0;
        context.queries["ysm.delta_movement_length"] = std::hypot(input.velocity[0], input.velocity[1], input.velocity[2]);
        context.queries["ysm.is_spectral_arrow"] = input.spectralArrow ? 1.0 : 0.0;
        context.queries["ysm.throwable_item"] = input.throwableItem;
        context.queries["ysm.hooked_in"] = input.hookedEntity;
        context.queries["ysm.is_biting"] = input.biting ? 1.0 : 0.0;
        context.queries["ysm.projectile_owner"] = input.projectileOwner;
    }
    Pose result;
    impl->lastStages.clear();
    auto category = std::string(replacedModelKindName(kind));
    auto declared = [&](std::string const& name) {
        return contains(name) || !sampler.functionLibrary()->events(controllerEvent(name)).empty();
    };
    auto stage = [&](std::string const& suffix, std::initializer_list<char const*> admission,
                     std::string const& selected, double transition = 0.0) {
        auto name = category + "." + suffix;
        bool accept = declared(name) || std::any_of(admission.begin(), admission.end(),
            [&](auto clip) { return sampler.hasNonemptyClip(clip); });
        if (!accept) return;
        AnimationLayer request{selected, time};
        // Vehicle/projectile predicates retain the animation file's loop type.
        impl->stage(name, sampler, time, result, evaluation,
            input.present && !selected.empty() ? std::span<AnimationLayer const>(&request, 1) : std::span<AnimationLayer const>{}, transition);
    };
    auto parallel = [&](char const* group, bool additive) {
        for (int index = 0; index < 8; ++index) {
            auto suffix = std::string(group) + "_" + std::to_string(index);
            auto name = category + "." + suffix;
            auto clip = std::string(group) + std::to_string(index);
            if (!declared(name) && !sampler.hasNonemptyClip(clip)) continue;
            AnimationLayer request{clip, time, additive};
            // ParallelPredicate explicitly loops, unlike the main/move/ride predicates.
            request.loop = AnimationLoop::Loop;
            impl->stage(name, sampler, time, result, evaluation,
                sampler.hasClip(clip) ? std::span<AnimationLayer const>(&request, 1) : std::span<AnimationLayer const>{});
        }
    };
    if (kind == ReplacedModelKind::Vehicle) parallel("pre_parallel", false);
    stage("pre_main", {}, "");
    if (kind == ReplacedModelKind::Vehicle) {
        stage("main", {"water", "ground", "fly"}, input.inWater ? "water" : input.onGround ? "ground" : "fly", .1);
        auto moving = std::sqrt(input.velocity[0] * input.velocity[0] + input.velocity[2] * input.velocity[2]) > .05;
        stage("move", {"forward", "idle"}, moving ? "forward" : "idle", .1);
        // Vanilla has no VehicleOrigin rotation. The source origin controller
        // only receives rotation from the optional aircraft integrations.
        impl->lastStages.push_back("vehicle.origin");
        stage("ride", {"has_ride", "not_ride"}, input.hasPassengers ? "has_ride" : "not_ride", .1);
    } else {
        auto main = input.inWater ? "water" : input.onFire ? "fire" : input.inGround ? "ground" : "air";
        // The pinned source requests air but advertises fly in ANIM_LIST.
        // Honor authored air; keep existing fly-only source packs usable.
        if (std::string_view(main) == "air" && !sampler.hasClip("air") && sampler.hasNonemptyClip("fly")) {
            main = "fly";
            context.diagnose("Projectile air/fly compatibility: using fly");
        }
        stage("main", {"water", "ground", "fly", "fire", "air"}, main, .1);
    }
    stage("post_main", {}, "");
    parallel("parallel", true);
    sampler.resetUndriven(result, time, impl->poseHistory);
    impl->playerPose = result;
    sampler.finishPose(result, context, false);
    impl->playerTime = time;
    context.finishFrame();
    return result;
}
Pose AnimationControllers::observePlayer(AnimationSampler const& sampler, AnimationEvaluation& observation) const {
    auto& context = observation.expressions;
    if (!context.observation) throw std::runtime_error("Player observation requires an observation context");
    if (impl->playerTime < 0) return {};
    MolangActionScope permission(context, false);
    if (impl->humanoid && !impl->firstPerson) sampler.recoverHeadPose(impl->playerPose, context);
    Pose result;
    for (auto const& name : impl->lastStages) {
        if (auto instance = impl->instances.find(name); instance != impl->instances.end())
            if (impl->observe(instance->second, sampler, result, context)) continue;
        if (auto fallback = impl->fallbacks.find(name); fallback != impl->fallbacks.end())
            fallback->second.player.observe(sampler, result, context);
    }
    auto history = impl->poseHistory;
    sampler.resetUndriven(result, impl->playerTime, history);
    sampler.finishPose(result, context, impl->humanoid && !impl->firstPerson);
    return result;
}
void AnimationControllers::reset() {
    impl->instances.clear();
    impl->fallbacks.clear();
    impl->playerPose.clear();
    impl->poseHistory.reset();
    impl->playerTime = -1;
    impl->lastBaseTime = -1;
    impl->lastStages.clear();
    impl->humanoid = true;
    impl->firstPerson = false;
    impl->replacedKind.reset();
}
}
