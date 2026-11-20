#include "Molang.h"
#include "MolangPhysics.h"
#include "JavaVanillaInputs.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace ysm {
static bool isBoneVector(std::string_view name) {
    return name == "ysm.bone_rot" || name == "ysm.bone_scale" || name == "ysm.bone_pos"
        || name == "ysm.bone_pivot_abs";
}

static std::array<double, 3> bonePivot(MolangContext const& context, std::string const& name) {
    if (!context.boneRig) return {};
    auto const& rig = *context.boneRig;
    auto node = rig.find(name);
    if (node == rig.end()) return {};
    // The renderer mirrors authored X. Use the same rotation order and return
    // the transformed pivot in model coordinates (in pixels).
    auto pointInRenderSpace = [](auto const& value) { return glm::dvec3(-value[0], value[1], value[2]); };
    glm::dvec3 point = pointInRenderSpace(node->second.pivot);
    for (std::size_t traversed = 0; node != rig.end(); ++traversed) {
        if (traversed == rig.size()) return {};
        if (auto value = context.boneChannels.find(node->first); value != context.boneChannels.end()) {
            auto const& channels = value->second;
            auto pivot = pointInRenderSpace(node->second.pivot);
            auto relative = (point - pivot) * glm::dvec3(channels.scale[0], channels.scale[1], channels.scale[2]);
            if (channels.rotation != std::array<double, 3>{}) {
                auto matrix = glm::rotate(glm::dmat4(1), glm::radians(channels.rotation[2]), glm::dvec3(0, 0, 1));
                matrix = glm::rotate(matrix, -glm::radians(channels.rotation[1]), glm::dvec3(0, 1, 0));
                matrix = glm::rotate(matrix, -glm::radians(channels.rotation[0]), glm::dvec3(1, 0, 0));
                relative = glm::dvec3(matrix * glm::dvec4(relative, 0));
            }
            point = pivot + pointInRenderSpace(channels.position) + relative;
        }
        if (node->second.parent.empty()) break;
        node = rig.find(node->second.parent);
    }
    return {-point.x, point.y, point.z};
}

double MolangValue::number() const {
    if (auto number = std::get_if<double>(&value)) return *number;
    if (auto text = std::get_if<std::string>(&value)) return text->empty() ? 0 : 1;
    if (isArray()) return array().empty() ? 0 : 1;
    if (isRecord()) return 1;
    return 0;
}
MolangValue::MolangValue(std::string schema, std::map<std::string, MolangValue> fields) {
    if (schema.empty() || fields.size() > 256) throw std::runtime_error("Invalid Molang record schema");
    for (auto const& [name, field] : fields) {
        if (name.empty() || field.isArray() || field.isRecord()) throw std::runtime_error("Molang records must be flat");
        if (!field.isString() && !std::isfinite(field.number())) throw std::runtime_error("Non-finite Molang record field");
    }
    value = std::make_shared<MolangRecord const>(MolangRecord{std::move(schema), std::move(fields)});
}
MolangValue MolangValue::vector(std::array<double, 3> const& values) {
    Vector coordinates{static_cast<float>(values[0]), static_cast<float>(values[1]), static_cast<float>(values[2])};
    for (float coordinate : coordinates)
        if (!std::isfinite(coordinate)) throw std::runtime_error("Non-finite Molang record field");
    MolangValue result;
    result.value = coordinates;
    return result;
}
static int coordinateIndex(std::string const& name) {
    if (name.size() != 1) return -1;
    char axis = name.front();
    if (axis >= 'x' && axis <= 'z') return axis - 'x';
    return axis == 'w' ? 3 : -1;
}
MolangRecord MolangValue::record() const {
    if (auto coords = std::get_if<Coordinates>(&value)) {
        MolangRecord result{"ysm.legacy_struct", {}, true};
        constexpr char const* names[]{"x", "y", "z", "w"};
        for (int i = 0; i < 4; ++i)
            if (coords->defined & (1u << i)) result.fields.emplace(names[i], static_cast<double>(coords->axes[i]));
        return result;
    }
    if (auto coordinates = std::get_if<Vector>(&value)) return {"ysm.vector", {
        {"x", static_cast<double>((*coordinates)[0])}, {"y", static_cast<double>((*coordinates)[1])},
        {"z", static_cast<double>((*coordinates)[2])}}};
    if (auto entry = std::get_if<std::shared_ptr<MolangRecord const>>(&value)) return **entry;
    throw std::runtime_error("Molang member receiver is not a record");
}
MolangValue MolangValue::field(std::string const& name) const {
    if (!present()) return {};
    if (auto coords = std::get_if<Coordinates>(&value)) {
        int index = coordinateIndex(name);
        if (index < 0 || !(coords->defined & (1u << index))) return {};
        return static_cast<double>(coords->axes[index]);
    }
    if (auto coordinates = std::get_if<Vector>(&value)) {
        if (name.size() == 1 && name[0] >= 'x' && name[0] <= 'z')
            return static_cast<double>((*coordinates)[name[0] - 'x']);
        throw std::runtime_error("Unknown Molang record field: " + name);
    }
    auto entry = std::get_if<std::shared_ptr<MolangRecord const>>(&value);
    if (!entry) throw std::runtime_error("Molang member receiver is not a record");
    auto const& fields = (*entry)->fields;
    auto found = fields.find(name);
    if (found == fields.end()) {
        if ((*entry)->extensible) return {};
        throw std::runtime_error("Unknown Molang record field: " + name);
    }
    return found->second;
}
double MolangValue::numericField(std::string const& name) const {
    // Reading an inline axis into arithmetic needs no intermediate variant.
    int axis = coordinateIndex(name);
    if (auto coords = std::get_if<Coordinates>(&value)) {
        return axis >= 0 && (coords->defined & (1u << axis)) ? coords->axes[axis] : 0;
    }
    if (auto vec = std::get_if<Vector>(&value); vec && axis >= 0 && axis < 3) return (*vec)[axis];
    auto result = field(name);
    if (!result.isFinite()) throw std::runtime_error("Non-finite Molang result");
    return result.number();
}
MolangValue MolangValue::withField(std::string const& name, MolangValue const& next) const {
    if (!present()) throw std::runtime_error("Cannot write a missing Molang record");
    if (auto coordinates = std::get_if<Vector>(&value)) {
        if (name.size() != 1 || name[0] < 'x' || name[0] > 'z') throw std::runtime_error("Unknown Molang record field: " + name);
        if (next.isArray() || next.isRecord()) throw std::runtime_error("Incompatible Molang record field: " + name);
        float scalar = static_cast<float>(next.number());
        if (!std::isfinite(scalar)) throw std::runtime_error("Non-finite Molang record field");
        MolangValue result = *this;
        std::get<Vector>(result.value)[name[0] - 'x'] = scalar;
        return result;
    }
    if (auto coords = std::get_if<Coordinates>(&value)) {
        int index = coordinateIndex(name);
        if (auto number = std::get_if<double>(&next.value); number && index >= 0) {
            float scalar = static_cast<float>(*number);
            if (!std::isfinite(scalar)) throw std::runtime_error("Non-finite Molang record field");
            auto updated = *coords;
            updated.defined |= static_cast<unsigned char>(1u << index);
            updated.axes[index] = scalar;
            MolangValue result;
            result.value = updated;
            return result;
        }
    }
    auto [schema, fields, extensible] = record();
    if (extensible) {
        if (fields.size() >= 256 && !fields.contains(name)) throw std::runtime_error("Molang record field limit exceeded");
        fields[name] = next;
        MolangValue result;
        result.value = std::make_shared<MolangRecord const>(MolangRecord{std::move(schema), std::move(fields), true});
        return result;
    }
    auto found = fields.find(name);
    if (found == fields.end()) throw std::runtime_error("Unknown Molang record field: " + name);
    if (next.isArray() || next.isRecord() || (found->second.isString() && next.present() && !next.isString()))
        throw std::runtime_error("Incompatible Molang record field: " + name);
    found->second = found->second.isString() ? (next.present() ? next : MolangValue(std::string()))
        : MolangValue(static_cast<double>(static_cast<float>(next.number())));
    return MolangValue(std::move(schema), std::move(fields));
}
std::span<MolangValue const> MolangValue::array() const {
    if (auto values = std::get_if<std::shared_ptr<std::vector<MolangValue> const>>(&value)) return **values;
    return {};
}
std::string MolangValue::string() const {
    if (auto text = std::get_if<std::string>(&value)) return *text;
    return std::to_string(number());
}
bool MolangValue::operator==(MolangValue const& other) const {
    if (isString() && other.isString()) return string() == other.string();
    if (isArray() && other.isArray()) return std::equal(array().begin(), array().end(), other.array().begin(), other.array().end());
    if (isRecord() && other.isRecord()) {
        if (auto coordinates = std::get_if<Vector>(&value))
            if (auto rhs = std::get_if<Vector>(&other.value)) return *coordinates == *rhs;
        auto left = record(), right = other.record();
        return left.schema == right.schema && left.fields == right.fields;
    }
    if ((isRecord() || isArray() || isString()) && (other.isRecord() || other.isArray() || other.isString())) return false;
    return number() == other.number();
}
std::string MolangContext::canonical(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.starts_with("v.")) name.replace(0, 2, "variable.");
    else if (name.starts_with("t.")) name.replace(0, 2, "temp.");
    else if (name.starts_with("q.")) name.replace(0, 2, "query.");
    else if (name.starts_with("c.")) name.replace(0, 2, "context.");
    else if (name.starts_with("r.")) name.replace(0, 2, roamingPrefix);
    else if (name.starts_with("roaming.")) name.replace(0, 8, roamingPrefix);
    return name;
}
static bool localVariable(std::string const& name) {
    return name.starts_with("temp.") || name == "args";
}
void MolangContext::beginFrame(double delta, std::optional<double> time) {
    if (programDepth || commitDepth) throw std::runtime_error("Molang round cannot reenter an active evaluation");
    if (!std::isfinite(delta)) throw std::runtime_error("Invalid Molang frame delta");
    deltaTime = std::max(0.0, delta);
    physicsTime = time.value_or(physicsTime + deltaTime);
    if (!physics) physics = std::make_shared<MolangPhysics>();
    physics->advance(physicsTime, legacyMolang);
    ++frame;
    remainingSteps = DefaultExecutionSteps;
    frameFailures = failures;
    effects.clear();
    deferred.clear();
    deferredCaptures = 0;
    deferredOwner = nullptr;
    roundOpen = true;
    syncRounds = 0;
    syncLimitReported = false;
    allowActions = false;
    std::erase_if(localVariables, [](auto const& item) { return item.first != "args"; });
}
bool MolangContext::isAction(std::string const& name) {
    return name == "ctrl.set_animation" || name == "ctrl.set_beginning_transition_length"
        || name == "ctrl.indicate_reload" || name == "ctrl.reset" || name == "ysm.defer" || name == "ysm.sync"
        || name == "ysm.bone_color" || name == "ysm.bone_transparency" || name == "ysm.bone_glow"
        || name == "ysm.play_sound" || name == "ysm.stop_sound" || name == "ysm.stop_all_sounds"
        || name == "ysm.particle" || name == "ysm.abs_particle";
}
void MolangContext::emitParticle(ParticleCue cue) {
    if (observation || cue.name.empty()) return;
    auto emit = [this, cue = std::move(cue)]() mutable {
        if (particleCues.size() >= 4096) { diagnose("Particle command budget exceeded"); return; }
        try {
            // Upstream consumes the animation processor's shared random
            // source when the prepared effect commits, before the next round.
            cue.spawns = expandParticle(cue, random, 4096 - particleSpawnCount);
            particleSpawnCount += cue.spawns.size();
        } catch (std::exception const& failure) {
            diagnose(std::string("Particle dispatch: ") + failure.what());
        }
        particleCues.push_back(std::move(cue));
    };
    if (roundOpen) {
        if (effects.size() >= 4096) throw std::runtime_error("Molang effect budget exceeded");
        effects.push_back(std::move(emit));
    } else emit();
}
std::vector<ParticleCue> MolangContext::takeParticleCues() {
    std::vector<ParticleCue> result;
    result.swap(particleCues);
    particleSpawnCount = 0;
    return result;
}
void MolangContext::emitSound(SoundCue cue) {
    if (observation) return;
    auto emit = [this, cue = std::move(cue)] {
        if (soundCues.size() < 4096) soundCues.push_back(cue);
        else diagnose("Sound command budget exceeded");
    };
    if (roundOpen) {
        if (effects.size() >= 4096) throw std::runtime_error("Molang effect budget exceeded");
        effects.push_back(std::move(emit));
    } else emit();
}
void MolangContext::submit(std::function<void()> effect) {
    if (!allowActions) return;
    if (!roundOpen) throw std::runtime_error("Molang effect requires an open round");
    if (effects.size() >= 4096) throw std::runtime_error("Molang effect budget exceeded");
    effects.push_back(std::move(effect));
}
void MolangContext::finishFrame() {
    if (!roundOpen) return;
    if (programDepth) throw std::runtime_error("Molang round cannot close inside a program");
    drainDeferred();
    auto pending = std::move(effects);
    effects.clear();
    roundOpen = false;
    if (failures != frameFailures) return;
    struct Commit {
        std::size_t& depth;
        explicit Commit(std::size_t& value) : depth(value) { ++depth; }
        ~Commit() { --depth; }
    } commit(commitDepth);
    for (auto& effect : pending) effect();
}
static MolangValue eventArguments(std::span<MolangValue const> args, std::size_t minimum, std::size_t maximum,
    char const* name) {
    if (args.size() < minimum || args.size() > maximum)
        throw std::runtime_error(std::string("Invalid ") + name + " argument count (" + std::to_string(minimum)
            + ".." + std::to_string(maximum) + ")");
    std::vector<MolangValue> values;
    values.reserve(args.size());
    for (auto const& value : args) {
        auto number = static_cast<float>(value.number());
        if (!std::isfinite(number))
            throw std::runtime_error(std::string(name) + " arguments must be finite floats");
        values.emplace_back(static_cast<double>(number));
    }
    return MolangValue(std::move(values));
}
void MolangContext::dispatchSync(std::span<MolangValue const> args) {
    if (roundOpen || programDepth) throw std::runtime_error("Molang sync ingress cannot reenter an active round");
    if (!library) throw std::runtime_error("Molang sync ingress has no current function library");
    auto packed = eventArguments(args, 0, 16, "ysm.sync");
    if (!commitDepth && !syncDepth) { syncRounds = 0; syncLimitReported = false; }
    // New local ACTION rounds cannot rely on the source program's call budget.
    // Bound both depth and total fan-out across one immediate delivery graph.
    if (syncDepth >= 64 || syncRounds >= 256) {
        if (!syncLimitReported) {
            ++failures;
            diagnose(syncDepth >= 64 ? "Molang sync nesting budget exceeded (64)" : "Molang sync delivery budget exceeded (256)");
            syncLimitReported = true;
        }
        return;
    }
    ++syncDepth;
    ++syncRounds;
    struct Round {
        MolangContext& context;
        std::size_t failures, steps, captures;
        void const* owner;
        bool permission;
        std::array<std::pair<std::string, std::optional<MolangValue>>, 4> clocks;
        explicit Round(MolangContext& value)
            : context(value), failures(value.frameFailures), steps(value.remainingSteps), captures(value.deferredCaptures),
              owner(value.deferredOwner), permission(value.allowActions) {
            std::array<char const*, 4> names{"query.anim_time", "query.state_time", "query.any_animation_finished", "query.all_animations_finished"};
            for (std::size_t i = 0; i < names.size(); ++i) {
                auto found = context.queries.find(names[i]);
                clocks[i] = {names[i], found == context.queries.end() ? std::optional<MolangValue>{} : found->second};
                context.queries[names[i]] = 0.0;
            }
            context.frameFailures = context.failures;
            context.remainingSteps = DefaultExecutionSteps;
            context.deferredCaptures = 0;
            context.deferredOwner = nullptr;
            context.allowActions = false;
            context.roundOpen = true;
        }
        ~Round() {
            context.effects.clear();
            context.deferred.clear();
            context.roundOpen = false;
            context.frameFailures = failures;
            context.remainingSteps = steps;
            context.deferredCaptures = captures;
            context.deferredOwner = owner;
            context.allowActions = permission;
            for (auto const& [name, value] : clocks) {
                if (value) context.queries[name] = *value;
                else context.queries.erase(name);
            }
            --context.syncDepth;
        }
    } round(*this);
    MolangSoundScope soundScope(*this, "global");
    ++syncDispatches;
    for (auto const& handler : library->events("sync")) {
        remainingSteps = DefaultExecutionSteps;
        MolangActionScope permission(*this, true);
        handler.evaluate(*this, packed);
    }
    finishFrame();
}
void MolangContext::drainDeferred() noexcept {
    auto previousPermission = allowActions;
    auto previousBudget = remainingSteps;
    try {
        while (!deferred.empty() && failures == frameFailures) {
            auto capture = std::move(deferred.back());
            deferred.pop_back();
            if (!library || capture.library != library) continue;
            MolangSoundScope soundScope(*this, capture.soundOwner);
            for (auto const& handler : capture.library->events("defer")) {
                // Each handler is a fresh top-level action entry. Its ordinary
                // effects still belong to the source frame's single batch.
                remainingSteps = DefaultExecutionSteps;
                allowActions = true;
                handler.evaluate(*this, capture.arguments);
            }
        }
    } catch (std::exception const& error) {
        recordFailure(error);
    }
    if (failures != frameFailures) deferred.clear();
    remainingSteps = previousBudget;
    allowActions = previousPermission;
}
MolangDeferredScope::MolangDeferredScope(MolangContext& value, void const* owner)
    : context(value), previous(value.deferredOwner), changed(previous != owner), exceptions(std::uncaught_exceptions()) {
    if (changed) {
        context.drainDeferred();
        context.deferredOwner = owner;
    }
}
MolangDeferredScope::~MolangDeferredScope() {
    if (!changed) return;
    if (std::uncaught_exceptions() > exceptions) context.deferred.clear();
    else context.drainDeferred();
    context.deferredOwner = previous;
}
void MolangContext::recordFailure(std::exception const& error) {
    ++failures;
    // A particle rejected by the 2.6.5 selector parser stops this program,
    // but effects issued before that call are already observable in game.
    if (dynamic_cast<ParticleSelectorError const*>(&error)) ++frameFailures;
    diagnose(error.what());
}
void MolangContext::diagnose(std::string message) {
    if (diagnostics.size() < 64) diagnostics.insert(std::move(message));
}
MolangContext MolangContext::forObservation() const {
    if (programDepth || roundOpen || commitDepth) throw std::runtime_error("Cannot observe an active Molang round");
    auto result = *this;
    if (physics) result.physics = std::make_shared<MolangPhysics>(*physics);
    result.observation = true;
    result.allowActions = false;
    result.deltaTime = 0;
    result.remainingSteps = DefaultExecutionSteps;
    result.effects.clear();
    result.networkSync = false;
    result.syncOut.clear();
    result.soundCues.clear();
    result.particleCues.clear();
    result.particleSpawnCount = 0;
    result.soundOwner = "global";
    result.deferred.clear();
    result.deferredOwner = nullptr;
    result.deferredCaptures = 0;
    return result;
}
MolangValue MolangContext::read(std::string const& name) const {
    if (name == "math.pi") return legacyMolang ? std::numbers::pi_v<float> : std::numbers::pi;
    if (name == "true") return 1.0;
    if (name == "false") return 0.0;
    if (name == "null") return {};
    if (name == "ctrl.state_continue") return 2.0;
    if (name == "ctrl.state_stop") return 3.0;
    if (name == "ctrl.state_bypass") return 5.0;
    if (roaming && name.starts_with(roamingPrefix)) return static_cast<double>(roaming->read(name.substr(roamingPrefix.size())));
    auto const& slots = localVariable(name) ? localVariables : variables;
    if (auto it = slots.find(name); it != slots.end()) {
        if (name.starts_with("variable.roaming.")) return static_cast<double>(static_cast<float>(it->second.number()));
        return it->second;
    }
    if (name.starts_with("variable.roaming.")) return 0.0;
    if (auto it = queries.find(name); it != queries.end()) return it->second;
    if (name == "ysm.shoot_item_id") return std::string{};
    return {};
}
void MolangContext::writeVariable(std::string const& name, MolangValue value) {
    if (name.starts_with(roamingPrefix)) {
        auto scalar = static_cast<float>(value.number());
        if (!std::isfinite(scalar)) throw std::runtime_error("Non-finite Molang roaming assignment");
        if (roaming) {
            roaming->write(name.substr(roamingPrefix.size()), scalar);
            return;
        }
        value = static_cast<double>(scalar);
    }
    auto& slots = localVariable(name) ? localVariables : variables;
    slots[name] = std::move(value);
}
static MolangCallSignature const* builtinSignature(std::string const& name) {
    // Resolve before visiting arguments, so missing/arity recovery discards
    // their entire subtree. Keep arity facts independent of provider effects.
    static auto const equipmentSelection = [] {
        MolangCallSignature signature{2, 255};
        signature.stringParameters.set();
        return signature;
    }();
    static auto const blockSelection = [](std::size_t maximum) {
        MolangCallSignature signature{4, maximum};
        signature.stringParameters.set().reset(0).reset(1).reset(2);
        return signature;
    };
    static auto const biomeSelection = [] {
        MolangCallSignature signature{1, 255};
        signature.stringParameters.set();
        return signature;
    }();
    static std::map<std::string, MolangCallSignature> const builtins{
        {"ysm.defer", {1, 255}}, {"ysm.sync", {0, 16}}, {"ysm.keyboard", {1, 255}},
        {"ysm.mod_version", {1, 1, {0}}}, {"ysm.first_order", {2, 255, {0}}},
        {"ysm.second_order", {2, 255, {0}}}, {"ysm.bone_rot", {1, 1, {0}}}, {"ysm.bone_scale", {1, 1, {0}}},
        {"ysm.bone_pivot_abs", {1, 1, {0}}}, {"ysm.bone_pos", {1, 1, {0}}},
        {"query.position", {1, 1}}, {"query.position_delta", {1, 1}},
        {"query.rotation_to_camera", {1, 1}},
        {"query.biome_has_any_tag", biomeSelection}, {"query.biome_has_all_tags", biomeSelection},
        {"ysm.shoot_item_id", {0, 0}}, {"ysm.relative_block_name", {3, 3}},
        {"query.relative_block_has_any_tag", blockSelection(255)}, {"query.relative_block_has_all_tags", blockSelection(8)},
        {"ysm.relative_block_name_any", blockSelection(255)},
        {"query.is_item_name_any", equipmentSelection}, {"query.equipped_item_any_tag", equipmentSelection},
        {"query.equipped_item_all_tags", equipmentSelection}, {"ysm.equipped_enchantment_level", equipmentSelection},
        {"query.max_durability", {1, 1, {0}}}, {"query.remaining_durability", {1, 1, {0}}},
        {"ysm.bone_color", {4, 4, {0}}}, {"ysm.bone_transparency", {2, 2, {0}}}, {"ysm.bone_glow", {2, 2, {0}}},
        {"ysm.play_sound", {2, 5, {1}}}, {"ysm.stop_sound", {1, 2}}, {"ysm.stop_all_sounds", {0, 1}},
        {"ysm.particle", {1, 255, {0}}}, {"ysm.abs_particle", {1, 255, {0}}},
        {"query.any_animation_finished", {0, 0}}, {"query.all_animations_finished", {0, 0}},
        {"ctrl.set_animation", {1, 2, {0}}}, {"ctrl.set_beginning_transition_length", {1, 1}},
        {"ctrl.indicate_reload", {0, 0}}, {"ctrl.reset", {0, 0}},
        {"ctrl.hold", {2, 3, {0, 1}}}, {"ctrl.use", {2, 3, {0, 1}}},
        {"ctrl.swing", {2, 3, {0, 1}}}, {"ctrl.armor", {2, 3, {0, 1}}},
        {"math.abs", {1, 1}}, {"math.clamp", {3, 3}}, {"math.cos", {1, 1}}, {"math.exp", {1, 1}},
        {"math.floor", {1, 1}}, {"math.lerp", {3, 3}}, {"math.max", {2, 2}}, {"math.min", {2, 2}},
        {"math.mod", {2, 2}}, {"math.pow", {2, 2}}, {"math.round", {1, 1}}, {"math.sin", {1, 1}},
        {"math.sqrt", {1, 1}}, {"math.min_angle", {1, 1}}, {"math.pi", {0, 0}}, {"math.tan", {1, 1}},
        {"math.asin", {1, 1}}, {"math.acos", {1, 1}}, {"math.atan", {1, 1}}, {"math.atan2", {2, 2}},
        {"math.ceil", {1, 1}}, {"math.trunc", {1, 1}}, {"math.ln", {1, 1}}, {"math.lerprotate", {3, 3}},
        {"math.lerp_rotate", {3, 3}}, {"math.hermite_blend", {1, 1}}, {"math.random", {2, 2}}, {"math.random_integer", {2, 2}}};
    auto found = builtins.find(name);
    return found != builtins.end() ? &found->second : nullptr;
}
std::optional<MolangCallSignature> MolangContext::signature(std::string const& name) const {
    if (auto builtin = builtinSignature(name)) return *builtin;
    if (name.starts_with("math.")) return {};
    if (name.starts_with("fn.")) return library && library->contains(name.substr(3))
        ? std::optional<MolangCallSignature>(MolangCallSignature{0, 255}) : std::nullopt;
    if (functions.contains(name)) return MolangCallSignature{0, name.starts_with("ctrl.") ? 0u : 255u};
    auto const& slots = localVariable(name) ? localVariables : variables;
    if (slots.contains(name) || queries.contains(name) || name.starts_with("ctrl.state_")) return MolangCallSignature{0, 0};
    return {};
}
MolangValue MolangContext::call(std::string const& name, std::span<MolangValue const> args) {
    // Nested author calls must not retain the stack frame used for native
    // particle, sound, and other host actions at every function level.
    if (name.starts_with("fn.") && library) return library->call(name.substr(3), *this, args);
    return callBuiltin(name, args);
}
// YSM 2.6.5 uses the Minecraft client's float-indexed sine table. Cosine
// applies its quarter-turn before truncation, which matters for negative angles.
static float legacyTableTrig(float degrees, bool cosine) {
    static const auto sines = [] {
        std::vector<float> table(65536);
        for (std::size_t i = 0; i < table.size(); ++i)
            table[i] = static_cast<float>(std::sin(static_cast<double>(i) * 2.0 * std::numbers::pi / 65536.0));
        return table;
    }();
    float radians = degrees / 180.0f * std::numbers::pi_v<float>;
    float position = radians * 10430.378f;
    if (cosine) position += 16384.0f;
    // JVM f2i saturates instead of overflowing. INT_MIN and NaN both index 0
    // after applying the client's low-16-bit mask.
    if (!(position > static_cast<float>(std::numeric_limits<std::int32_t>::min()))) return sines.front();
    if (position >= static_cast<float>(std::numeric_limits<std::int32_t>::max())) return sines.back();
    return sines[static_cast<std::uint32_t>(static_cast<std::int32_t>(position)) & 65535u];
}

MolangValue MolangContext::callBuiltin(std::string const& name, std::span<MolangValue const> args) {
    if (isAction(name) && !allowActions) return {};
    if (name == "query.rotation_to_camera") {
        if (args.size() != 1) return 0.0;
        double axis = args[0].number();
        return axis == 0 ? cameraRotation[0] : axis == 1 ? cameraRotation[1] : 0.0;
    }
    if (name == "query.biome_has_any_tag" || name == "query.biome_has_all_tags") {
        if (biomeId.empty() || args.empty()) return 0.0;
        auto matches = [&](MolangValue const& value) {
            if (!value.isString()) return false;
            auto text = value.string();
            auto tag = std::string_view(text);
            if (tag.starts_with('#')) tag.remove_prefix(1);
            return hasJavaBiomeTag(biomeId, canonicalRegistryId(tag));
        };
        return (name == "query.biome_has_any_tag" ? std::ranges::any_of(args, matches) : std::ranges::all_of(args, matches)) ? 1.0 : 0.0;
    }
    if (name == "ysm.particle" || name == "ysm.abs_particle") {
        if (args.empty() || args.size() > 255 || !args[0].isString())
            throw std::runtime_error("Invalid particle arguments");
        std::vector<float> values;
        values.reserve(args.size() - 1);
        for (std::size_t i = 1; i < args.size(); ++i) values.push_back(static_cast<float>(args[i].number()));
        auto cue = prepareParticle(args[0].string(), values, name == "ysm.abs_particle");
        cue.origin = particleOrigin;
        submit([this, cue = std::move(cue)] { emitParticle(cue); });
        return {};
    }
    if (name == "ysm.play_sound" || name == "ysm.stop_sound" || name == "ysm.stop_all_sounds") {
        bool play = name == "ysm.play_sound", all = name == "ysm.stop_all_sounds";
        if (args.size() < (play ? 2u : all ? 0u : 1u) || args.size() > (play ? 5u : all ? 1u : 2u))
            throw std::runtime_error("Invalid sound arguments");
        auto scalar = [&](std::size_t at, float fallback) {
            if (at >= args.size()) return fallback;
            auto value = static_cast<float>(args[at].number());
            if (!std::isfinite(value)) throw std::runtime_error("Non-finite sound argument");
            return value;
        };
        auto integer = [](float value) {
            return static_cast<int>(std::clamp(static_cast<double>(value), -2147483648.0, 2147483647.0));
        };
        SoundCue cue;
        bool valid = true;
        if (!all) {
            if (args[0].isString()) cue.id = "s:" + args[0].string();
            else {
                auto id = integer(scalar(0, 0));
                valid = id >= 0;
                if (id != 0) cue.id = "n:" + std::to_string(id);
            }
        }
        if (play) {
            if (!args[1].isString()) throw std::runtime_error("Invalid sound selector");
            cue.name = args[1].string();
            auto mode = integer(scalar(2, 0));
            valid = valid && mode >= 0 && mode <= 7 && cue.name.find_first_not_of(" \t\r\n") != std::string::npos;
            cue.owner = (mode & 2) ? "global" : soundOwner;
            cue.force = (mode & 1) != 0;
            cue.looping = (mode & 4) != 0;
            cue.volume = std::clamp(scalar(3, 1), .001f, 1000.f);
            cue.pitch = std::clamp(scalar(4, 1), .001f, 1000.f);
        } else {
            cue.owner = scalar(all ? 0 : 1, 0) != 0 ? "global" : soundOwner;
            cue.stopId = !all;
        }
        submit([this, cue = std::move(cue), valid] { if (valid) emitSound(cue); });
        return {};
    }
    if (name == "ysm.bone_color" || name == "ysm.bone_transparency" || name == "ysm.bone_glow") {
        if (args.size() != (name == "ysm.bone_color" ? 4u : 2u) || !args[0].isString())
            throw std::runtime_error("Invalid bone visual arguments");
        auto rounded = [&](std::size_t i, int minimum, int maximum) {
            auto value = static_cast<float>(args[i].number());
            if (!std::isfinite(value)) throw std::runtime_error("Non-finite bone visual argument");
            // Math.round(float), then clamp; negative halves round toward +inf.
            return static_cast<int>(std::clamp(std::floor(static_cast<double>(value) + .5),
                static_cast<double>(minimum), static_cast<double>(maximum)));
        };
        auto key = args[0].string();
        std::array<int, 3> color{};
        int value = 0;
        if (name == "ysm.bone_color") for (std::size_t i = 0; i < 3; ++i) color[i] = rounded(i + 1, 0, 255);
        else value = rounded(1, name == "ysm.bone_glow" ? -1 : 0, name == "ysm.bone_glow" ? 15 : 255);
        // Capture the exact declared target and payload while this owner is
        // borrowed. Missing targets remain an ordinary submitted no-op.
        auto found = boneVisuals.find(key);
        auto* target = found == boneVisuals.end() ? nullptr : &found->second;
        submit([target, name, color, value] {
            if (!target) return;
            if (name == "ysm.bone_color") target->color = color;
            else if (name == "ysm.bone_glow") target->glow = value;
            else target->alpha = value;
        });
        return {};
    }
    if (name == "ysm.defer") {
        auto packed = eventArguments(args, 1, 255, "ysm.defer");
        if (deferredCaptures++ >= 64) throw std::runtime_error("Molang deferred capture budget exceeded (64)");
        // args[0] remains the authored first float. Upstream dispatches at
        // context exit; it does not retain a timer or queue across frames.
        deferred.push_back({std::move(packed), library, soundOwner});
        return {};
    }
    if (name == "ysm.sync") {
        auto packed = eventArguments(args, 0, 16, "ysm.sync");
        auto source = library;
        if (localSync) submit([this, source, packed] {
            if (library == source) dispatchSync(packed.array());
        });
        else if (networkSync) submit([this, source, packed] {
            if (library != source) return;
            if (syncOut.size() >= 4096) {
                diagnose("Molang network event backlog exceeded (4096)");
                return;
            }
            auto& message = syncOut.emplace_back();
            for (auto const& value : packed.array()) message.push_back(static_cast<float>(value.number()));
        });
        else submit([] {});
        return {};
    }
    if (auto it = functions.find(name); it != functions.end()) return it->second(args);
    if (isAction(name)) return {};
    auto arity = [&](std::size_t count) {
        if (args.size() != count) throw std::runtime_error("Wrong argument count: " + name);
    };
    auto n = [&](std::size_t i) { return i < args.size() ? args[i].number() : 0.0; };
    if (name == "ysm.keyboard") {
        if (args.empty() || args.size() > 255) throw std::runtime_error("ysm.keyboard requires 1..255 keys");
        if (!keyboardEnabled) return 0.0;
        for (auto const& argument : args) {
            auto key = static_cast<float>(argument.number());
            if (std::isfinite(key) && key >= 32 && key < 349 && pressedKeys.contains(static_cast<int>(key))) return 1.0;
        }
        return 0.0;
    }
    if (name == "ysm.mod_version") {
        arity(1);
        if (!args[0].isString()) return {};
        auto found = modVersions.find(args[0].string());
        return found == modVersions.end() ? std::string() : found->second;
    }
    if (isBoneVector(name)) {
        arity(1);
        if (!args[0].isString()) return {};
        auto boneName = args[0].string();
        if (name == "ysm.bone_pivot_abs") {
            if (auto cached = bonePivots.find(boneName); cached != bonePivots.end()) return cached->second;
            auto value = MolangValue::vector(bonePivot(*this, boneName));
            if (boneRig && boneRig->contains(boneName)) bonePivots.emplace(std::move(boneName), value);
            return value;
        }
        auto found = boneChannels.find(boneName);
        if (found == boneChannels.end()) return MolangValue::vector({});
        if (name == "ysm.bone_scale") return MolangValue::vector(found->second.scale);
        if (name == "ysm.bone_pos") return MolangValue::vector(found->second.position);
        return MolangValue::vector(found->second.rotation);
    }
    if (name == "query.any_animation_finished" || name == "query.all_animations_finished") {
        arity(0);
        return read(name);
    }
    auto angle = [](double a) { return a - std::floor((a + 180) / 360) * 360; };
    if (name == "query.position" || name == "query.position_delta") {
        arity(1);
        auto axis = std::trunc(n(0));
        if (axis >= 0 && axis < 3) {
            auto const& values = name == "query.position" ? entityPosition : positionDelta;
            return values[static_cast<std::size_t>(axis)];
        }
        return name == "query.position" ? MolangValue{} : MolangValue{0.0};
    }
    if (name == "query.is_item_name_any" || name == "query.equipped_item_any_tag" || name == "query.equipped_item_all_tags"
        || name == "query.max_durability" || name == "query.remaining_durability" || name == "ysm.equipped_enchantment_level")
        return {}; // No living equipment provider for this owner.
    if (name == "query.relative_block_has_any_tag" || name == "query.relative_block_has_all_tags"
        || name == "ysm.relative_block_name" || name == "ysm.relative_block_name_any") {
        if (!blockLookup || args.size() < 3) return {};
        std::array<int, 3> position{};
        for (std::size_t i = 0; i < position.size(); ++i) {
            auto offset = static_cast<float>(n(i));
            if (!std::isfinite(offset) || std::abs(offset) > 5) return {};
            // The source probes world axes from the entity's current feet.
            auto block = std::floor(particleOrigin.position[i] + offset);
            if (!std::isfinite(block) || block < std::numeric_limits<int>::min() || block > std::numeric_limits<int>::max())
                return {};
            position[i] = static_cast<int>(block);
        }
        if (name != "ysm.relative_block_name_any")
            return blockLookup(position, args.subspan(3), name == "query.relative_block_has_all_tags");
        auto actual = blockLookup(position, {}, false);
        if (!actual.isString()) return {};
        for (auto const& argument : args.subspan(3)) {
            if (!argument.isString()) return {};
            auto id = canonicalRegistryId(argument.string());
            if (id.empty()) return {};
            if (id == actual.string()) return 1.0;
        }
        return 0.0;
    }
    if (name == "ysm.first_order" || name == "ysm.second_order") {
        if (!physics) physics = std::make_shared<MolangPhysics>();
        return physics->evaluate(name == "ysm.first_order", args);
    }
    if (!name.starts_with("math.")) {
        if (args.empty()) return read(name);
        diagnose("Unbound function: " + name);
        return {};
    }
    auto fn = name.substr(5);
    // Pinned source MathBuiltins uses Java float parameters and results.
    // Keep those boundaries separate from the additional Bedrock math helpers.
    static std::map<std::string, std::size_t> const sourceMath{{"abs", 1}, {"clamp", 3}, {"cos", 1},
        {"exp", 1}, {"floor", 1}, {"lerp", 3}, {"max", 2}, {"min", 2}, {"mod", 2},
        {"pow", 2}, {"round", 1}, {"sin", 1}, {"sqrt", 1}, {"min_angle", 1}};
    if (auto signature = sourceMath.find(fn); signature != sourceMath.end()) {
        arity(signature->second);
        std::array<float, 3> values{};
        for (std::size_t i = 0; i < args.size(); ++i) {
            values[i] = static_cast<float>(args[i].number());
            if (!std::isfinite(values[i])) throw std::runtime_error("Non-finite math argument: " + name);
        }
        auto a = values[0], b = values[1], c = values[2];
        auto minimum = [](float x, float y) {
            if (x == 0 && y == 0) return std::signbit(x) || std::signbit(y) ? -0.0f : 0.0f;
            return std::min(x, y);
        };
        auto maximum = [](float x, float y) {
            if (x == 0 && y == 0) return std::signbit(x) && std::signbit(y) ? -0.0f : 0.0f;
            return std::max(x, y);
        };
        float result = 0;
        if (legacyMolang && fn == "sqrt" && a < 0) return MolangValue::legacyNaN();
        if (fn == "abs") result = std::abs(a);
        else if (fn == "clamp") result = minimum(maximum(a, b), c);
        else if (fn == "cos") result = legacyMolang ? legacyTableTrig(a, true)
            : static_cast<float>(std::cos(static_cast<double>(a) / 180.0 * std::numbers::pi));
        else if (fn == "exp") result = static_cast<float>(std::exp(static_cast<double>(a)));
        else if (fn == "floor") result = std::floor(a);
        else if (fn == "lerp") { float delta = b - a; float product = delta * c; result = a + product; }
        else if (fn == "max") result = maximum(a, b);
        else if (fn == "min") result = minimum(a, b);
        else if (fn == "mod") result = std::fmod(a, b);
        else if (fn == "pow") result = static_cast<float>(std::pow(static_cast<double>(a), static_cast<double>(b)));
        else if (fn == "round") result = static_cast<float>(std::clamp(std::floor(static_cast<double>(a) + 0.5),
            static_cast<double>(std::numeric_limits<int>::min()), static_cast<double>(std::numeric_limits<int>::max())));
        else if (fn == "sin") result = legacyMolang ? legacyTableTrig(a, false)
            : static_cast<float>(std::sin(static_cast<double>(a) / 180.0 * std::numbers::pi));
        else if (fn == "sqrt") result = a < 0 ? 0.0f : static_cast<float>(std::sqrt(static_cast<double>(a)));
        else {
            result = std::fmod(a, 360.0f);
            if (result >= 180) result -= 360;
            else if (result < -180) result += 360;
        }
        if (!std::isfinite(result)) throw std::runtime_error("Non-finite math result: " + name);
        return static_cast<double>(result);
    }
    constexpr double Degrees = std::numbers::pi / 180;
    if (fn == "pi") { arity(0); return std::numbers::pi; }
    if (fn == "tan") { arity(1); return std::tan(n(0) * Degrees); }
    // Release 2.6.5 authors convert inverse trig results to degrees themselves.
    // Packs compiled against the typed runtime keep its established angular units.
    double inverseUnit = legacyMolang ? 1.0 : Degrees;
    if (fn == "asin" || fn == "acos") {
        arity(1);
        auto input = n(0);
        if (legacyMolang && std::abs(input) > 1) return MolangValue::legacyNaN();
        return (fn == "asin" ? std::asin(input) : std::acos(input)) / inverseUnit;
    }
    if (fn == "atan") { arity(1); return std::atan(n(0)) / inverseUnit; }
    if (fn == "atan2") { arity(2); return std::atan2(n(0), n(1)) / inverseUnit; }
    if (fn == "ceil") { arity(1); return std::ceil(n(0)); }
    if (fn == "trunc") { arity(1); return std::trunc(n(0)); }
    if (fn == "ln") { arity(1); return std::log(n(0)); }
    if (fn == "lerprotate" || fn == "lerp_rotate") { arity(3); return n(0) + angle(n(1) - n(0)) * std::clamp(n(2), 0.0, 1.0); }
    if (fn == "hermite_blend") { arity(1); double t = std::clamp(n(0), 0.0, 1.0); return t * t * (3 - 2 * t); }
    if (fn == "random") { arity(2); return std::uniform_real_distribution<double>(std::min(n(0), n(1)), std::max(n(0), n(1)))(random); }
    if (fn == "random_integer") {
        arity(2);
        auto integer = [](double value) { return static_cast<int>(std::clamp(value, static_cast<double>(std::numeric_limits<int>::min()), static_cast<double>(std::numeric_limits<int>::max()))); };
        return static_cast<double>(std::uniform_int_distribution<int>(integer(std::min(n(0), n(1))), integer(std::max(n(0), n(1))))(random));
    }
    throw std::runtime_error("Unknown math function: " + name);
}

enum class MolangKind { Number, String, Variable, Unary, Binary, Assign, Conditional, Call, Index, Member, Sequence, Return, Break, Continue, ReciprocalExp };
enum class Arithmetic { None, Add, Subtract, Multiply, Divide, Remainder };
enum class SlotDomain { Other, Temporary, Persistent };
struct LegacyMolangSlot {
    std::size_t root;
    std::string name;
};
struct MolangNode {
    MolangKind kind;
    double value = 0;
    std::string text;
    std::vector<std::size_t> children;
    std::unique_ptr<LegacyMolangSlot> legacySlot{};
    MolangCallSignature const* builtin = nullptr;
    // Nonzero only if access validation on this subtree cannot depend on a
    // binding, value, or declaration. Depth preserves the validator's limit.
    unsigned char plainAccessDepth = 0;
    Arithmetic arithmetic = Arithmetic::None;
    SlotDomain domain = SlotDomain::Other;
};
struct MolangProgram::Impl {
    std::vector<MolangNode> nodes;
    std::size_t root = 0;
    bool complex = false;
};
class MolangParser {
    std::string source, token;
    std::size_t cursor = 0, depth = 0;
    enum class TokenKind { End, Symbol, Number, String, Name } kind;
    double numeric = 0;
public:
    std::vector<MolangNode> nodes;
    bool complex = false;
    explicit MolangParser(std::string text) : source(std::move(text)) { next(); }
    [[noreturn]] void fail(std::string const& message) { throw std::runtime_error("Molang at " + std::to_string(cursor) + ": " + message); }
    void next() {
        while (true) {
            while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor]))) ++cursor;
            if (cursor == 0 && source.starts_with("\xEF\xBB\xBF")) { cursor = 3; continue; }
            if (source.compare(cursor, 2, "//") == 0) {
                auto end = source.find('\n', cursor + 2);
                cursor = end == std::string::npos ? source.size() : end + 1;
                continue;
            }
            if (source.compare(cursor, 2, "/*") == 0) {
                auto end = source.find("*/", cursor + 2);
                if (end == std::string::npos) fail("Unterminated comment");
                cursor = end + 2;
                continue;
            }
            break;
        }
        if (cursor == source.size()) { kind = TokenKind::End; token.clear(); return; }
        char c = source[cursor];
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && cursor + 1 < source.size() && std::isdigit(static_cast<unsigned char>(source[cursor + 1])))) {
            char* end;
            numeric = std::strtod(source.c_str() + cursor, &end);
            cursor = static_cast<std::size_t>(end - source.c_str());
            if (!std::isfinite(numeric)) fail("Non-finite number");
            kind = TokenKind::Number; return;
        }
        if (c == '\'' || c == '"') {
            ++cursor; token.clear();
            while (cursor < source.size() && source[cursor] != c) {
                char value = source[cursor++];
                if (value == '\\' && cursor < source.size()) value = source[cursor++];
                token += value;
            }
            if (cursor == source.size()) fail("Unterminated string");
            ++cursor; kind = TokenKind::String; return;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            auto start = cursor++;
            while (cursor < source.size()) {
                char ch = source[cursor];
                if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '.') break;
                ++cursor;
            }
            token = MolangContext::canonical(source.substr(start, cursor - start)); kind = TokenKind::Name; return;
        }
        kind = TokenKind::Symbol;
        token = source.substr(cursor++, 1);
        if (cursor < source.size()) {
            auto pair = token + source[cursor];
            if (pair == "&&" || pair == "||" || pair == "==" || pair == "!=" || pair == "<=" || pair == ">=" || pair == "??" || pair == "+=" || pair == "-=" || pair == "*=" || pair == "/=" || pair == "->") { token = pair; ++cursor; }
        }
    }
    bool take(char const* symbol) {
        if (kind == TokenKind::Symbol && token == symbol) { next(); return true; }
        return false;
    }
    void require(char const* symbol) { if (!take(symbol)) fail(std::string("Expected ") + symbol); }
    std::size_t add(MolangNode node) {
        if (nodes.size() >= 65536) fail("Node budget exceeded");
        if (node.kind == MolangKind::Variable) {
            if (node.text.starts_with("temp.")) node.domain = SlotDomain::Temporary;
            else if (node.text.starts_with("variable.") && !node.text.starts_with(roamingPrefix))
                node.domain = SlotDomain::Persistent;
        }
        if (node.kind == MolangKind::Call || (node.kind == MolangKind::Variable &&
            !node.text.starts_with("variable.") && !node.text.starts_with("temp.") && !node.text.starts_with("context.")))
            node.builtin = builtinSignature(node.text);

        bool plain = node.kind == MolangKind::String || node.kind == MolangKind::Number || node.kind == MolangKind::Variable
            || node.kind == MolangKind::Binary || node.kind == MolangKind::Unary || node.kind == MolangKind::Index;
        if (node.kind == MolangKind::Member) {
            auto const& receiver = nodes[node.children.front()];
            plain = receiver.kind == MolangKind::Variable && receiver.text != "true"
                && receiver.text != "false" && receiver.text != "math.pi";
        }
        if (plain) {
            unsigned char depth = 1;
            for (auto child : node.children) {
                auto prior = nodes[child].plainAccessDepth;
                if (!prior || prior >= 128) { depth = 0; break; }
                depth = std::max(depth, static_cast<unsigned char>(prior + 1));
            }
            node.plainAccessDepth = depth;
        }
        nodes.push_back(std::move(node)); return nodes.size() - 1;
    }
    std::size_t member(std::size_t base, std::string field) {
        auto const& parent = nodes[base];
        MolangNode node{MolangKind::Member, 0, std::move(field), {base}};
        if (parent.legacySlot) {
            node.legacySlot = std::make_unique<LegacyMolangSlot>(
                LegacyMolangSlot{parent.legacySlot->root, parent.legacySlot->name + '.' + node.text});
        } else if (parent.kind == MolangKind::Variable
            && (parent.text.starts_with("variable.") || parent.text.starts_with("temp."))
            && !parent.text.starts_with(roamingPrefix)) {
            node.legacySlot = std::make_unique<LegacyMolangSlot>(LegacyMolangSlot{base, parent.text + '.' + node.text});
        }
        return add(std::move(node));
    }
    std::size_t sequence(bool block) {
        std::vector<std::size_t> statements;
        while (kind != TokenKind::End && !(kind == TokenKind::Symbol && token == "}")) {
            statements.push_back(expression());
            if (!take(";")) break;
            if (!block) complex = true;
        }
        if (block) require("}");
        else if (kind != TokenKind::End) fail("Unexpected token " + token);
        // YSM's frontend produces the same root for a value expression with
        // or without a trailing delimiter. Legacy model keyframes commonly
        // use "(condition ? angle) + ...;"; that is still the angle, not zero.
        // Keep statement/assignment programs' explicit-return convention.
        if (!block && statements.size() == 1 && std::none_of(nodes.begin(), nodes.end(), [](auto const& node) {
                return node.kind == MolangKind::Assign || node.kind == MolangKind::Return
                    || node.kind == MolangKind::Break || node.kind == MolangKind::Continue;
            }))
            complex = false;
        return add({MolangKind::Sequence, 0, "", std::move(statements)});
    }
    int precedence() const {
        if (kind != TokenKind::Symbol) return 0;
        if (token == "=" || token == "+=" || token == "-=" || token == "*=" || token == "/=") return 1;
        if (token == "?") return 2;
        if (token == "??") return 3;
        if (token == "||") return 4;
        if (token == "&&") return 5;
        if (token == "==" || token == "!=") return 6;
        if (token == "<" || token == ">" || token == "<=" || token == ">=") return 7;
        if (token == "+" || token == "-") return 8;
        if (token == "*" || token == "/" || token == "%") return 9;
        if (token == "->") return 10;
        return 0;
    }
    std::size_t binary(std::string op, std::size_t left, std::size_t right) {
        auto isOne = [&](std::size_t index) { return nodes[index].kind == MolangKind::Number && nodes[index].value == 1; };
        auto const& sum = nodes[right];
        if (op == "/" && isOne(left) && sum.kind == MolangKind::Binary && sum.text == "+") {
            for (int side = 0; side < 2; ++side) {
                auto const& call = nodes[sum.children[side]];
                if (isOne(sum.children[1 - side]) && call.kind == MolangKind::Call
                    && call.text == "math.exp" && call.children.size() == 1)
                    return add({MolangKind::ReciprocalExp, 0, {}, call.children});
            }
        }
        MolangNode node{MolangKind::Binary, 0, std::move(op), {left, right}};
        if (node.text.size() == 1) switch (node.text.front()) {
        case '+': node.arithmetic = Arithmetic::Add; break;
        case '-': node.arithmetic = Arithmetic::Subtract; break;
        case '*': node.arithmetic = Arithmetic::Multiply; break;
        case '/': node.arithmetic = Arithmetic::Divide; break;
        case '%': node.arithmetic = Arithmetic::Remainder; break;
        }
        return add(std::move(node));
    }
    std::size_t expression(int minimum = 1) {
        if (++depth > 128) fail("Nesting budget exceeded");
        auto left = primary();
        for (int priority; (priority = precedence()) >= minimum && priority > 0;) {
            auto op = token; next();
            if (op == "?") {
                auto yes = expression();
                auto no = add({MolangKind::Number, 0, "", {}});
                if (take(":")) no = expression(priority);
                left = add({MolangKind::Conditional, 0, "", {left, yes, no}});
            } else {
                auto right = expression(priority == 1 ? priority : priority + 1);
                if (priority == 1 && nodes[left].kind != MolangKind::Variable && nodes[left].kind != MolangKind::Member)
                    fail("Assignment needs a writable location");
                left = priority == 1 ? add({MolangKind::Assign, 0, op, {left, right}}) : binary(std::move(op), left, right);
            }
        }
        --depth;
        return left;
    }
    std::size_t primary() {
        auto value = atom();
        while (true) {
            if (take("[")) {
                auto index = expression();
                require("]");
                value = add({MolangKind::Index, 0, "", {value, index}});
            } else if (take(".")) {
                if (kind != TokenKind::Name) fail("Expected member name");
                auto fields = token; next();
                std::size_t offset = 0;
                do {
                    auto end = fields.find('.', offset);
                    auto field = fields.substr(offset, end == std::string::npos ? end : end - offset);
                    if (field.empty()) fail("Empty member name");
                    value = member(value, std::move(field));
                    if (end == std::string::npos) break;
                    offset = end + 1;
                } while (true);
            } else break;
        }
        return value;
    }
    std::size_t atom() {
        if (kind == TokenKind::Number) { double value = numeric; next(); return add({MolangKind::Number, value, "", {}}); }
        if (kind == TokenKind::String) { auto text = token; next(); return add({MolangKind::String, 0, std::move(text), {}}); }
        if (take("(")) { auto result = expression(); require(")"); return result; }
        if (take("{")) return sequence(true);
        if (kind == TokenKind::Symbol && (token == "!" || token == "-" || token == "+")) {
            auto op = token; next(); return add({MolangKind::Unary, 0, op, {expression(10)}});
        }
        if (kind != TokenKind::Name) fail("Expected expression");
        auto name = token; next();
        if (name == "return") {
            bool empty = kind == TokenKind::End || (kind == TokenKind::Symbol && (token == ";" || token == "}"));
            auto value = empty ? add({MolangKind::Number, 0, "", {}}) : expression();
            return add({MolangKind::Return, 0, "", {value}});
        }
        if (name == "break") return add({MolangKind::Break, 0, "", {}});
        if (name == "continue") return add({MolangKind::Continue, 0, "", {}});
        if (name == "nan" || name == "infinity") fail("Non-finite literal");
        if (!take("(")) {
            auto first = name.find('.');
            auto end = first == std::string::npos ? first : name.find('.', first + 1);
            // Roaming is a declared scalar storage namespace, not a record
            // stored in variable.roaming. A following field still uses normal
            // member semantics and cannot turn that scalar into a nested map.
            if (name.starts_with("variable.roaming.")) end = name.find('.', std::string("variable.roaming.").size());
            auto value = add({MolangKind::Variable, 0, name.substr(0, end), {}});
            while (end != std::string::npos) {
                auto offset = end + 1;
                end = name.find('.', offset);
                auto field = name.substr(offset, end == std::string::npos ? end : end - offset);
                if (field.empty()) fail("Empty member name");
                value = member(value, std::move(field));
            }
            return value;
        }
        std::vector<std::size_t> args;
        if (!take(")")) {
            do { args.push_back(expression()); } while (take(","));
            require(")");
        }
        return add({MolangKind::Call, 0, name, std::move(args)});
    }
};
enum class MolangFlow { Normal, Return, Break, Continue };
struct MolangEvaluator {
    std::vector<MolangNode> const& nodes;
    MolangContext& context;
    MolangFlow flow = MolangFlow::Normal;
    std::size_t depth = 0;
    // Only facts proved by source nodes are used here. Cross-entry slot and
    // user-function specialization still require a complete static closure.
    enum class Shape { Unknown, Scalar, String, Record, Missing };
    bool clearLiteral(std::size_t index) const {
        auto const& node = nodes[index];
        return (node.kind == MolangKind::Number && node.value == 0)
            || (node.kind == MolangKind::Variable && node.text == "false");
    }
    Shape shape(std::size_t index, std::size_t nesting = 0) const {
        if (nesting >= 128) return Shape::Unknown;
        auto const& node = nodes[index];
        auto child = [&](std::size_t i) { return shape(node.children[i], nesting + 1); };
        switch (node.kind) {
        case MolangKind::Number: case MolangKind::Unary: return Shape::Scalar;
        case MolangKind::String: return Shape::String;
        case MolangKind::Variable:
            if (node.text == "null") return Shape::Missing;
            if (node.text == "true" || node.text == "false" || node.text == "math.pi") return Shape::Scalar;
            return Shape::Unknown;
        case MolangKind::Binary:
            if (node.text != "??") return Shape::Scalar;
            return child(0) == child(1) ? child(0) : Shape::Unknown;
        case MolangKind::Assign: return node.text == "=" ? child(1) : Shape::Scalar;
        case MolangKind::Conditional: return child(1) == child(2) ? child(1) : Shape::Unknown;
        case MolangKind::Return: return child(0);
        case MolangKind::Call:
            if (node.text.starts_with("math.") || node.text == "ysm.keyboard" || node.text == "ysm.first_order"
                || node.text == "ysm.second_order") return Shape::Scalar;
            if (isBoneVector(node.text)) return Shape::Record;
            if (node.text == "ysm.mod_version" || node.text == "ysm.shoot_item_id" || node.text == "ysm.relative_block_name")
                return Shape::String;
            return Shape::Unknown;
        default: return Shape::Unknown;
        }
    }
    MolangCallSignature const* signatureFor(MolangNode const& node, std::optional<MolangCallSignature>& dynamic) const {
        if (node.builtin) return node.builtin;
        dynamic = context.signature(node.text);
        return dynamic ? &*dynamic : nullptr;
    }
    bool accepts(std::string const& name, std::span<std::size_t const> arguments, MolangCallSignature const& signature) {
        if (arguments.size() < signature.minimum || arguments.size() > signature.maximum) {
            context.diagnose("Recovered Molang argument count: " + name + " (" + std::to_string(signature.minimum)
                + ".." + std::to_string(signature.maximum) + ")");
            return false;
        }
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            validateAccess(arguments[i]);
            if (!signature.stringParameters[i]) continue;
            auto type = shape(arguments[i]);
            if ((type == Shape::Scalar && !clearLiteral(arguments[i])) || type == Shape::Record) {
                return false;
            }
        }
        return true;
    }
    void validateAccess(std::size_t index, std::size_t nesting = 0) {
        if (nesting >= 128) throw std::runtime_error("Molang access analysis budget exceeded");
        auto const& node = nodes[index];
        if (node.plainAccessDepth && nesting + node.plainAccessDepth <= 128) return;
        if (node.kind == MolangKind::Call && node.text != "loop") {
            std::optional<MolangCallSignature> dynamic;
            if (auto signature = signatureFor(node, dynamic)) accepts(node.text, node.children, *signature);
            return;
        }
        if (node.kind == MolangKind::Assign) {
            auto const& address = nodes[node.children[0]];
            auto const& base = address.kind == MolangKind::Member ? nodes[address.children[0]] : address;
            if (base.kind == MolangKind::Variable && !base.text.starts_with("variable.") && !base.text.starts_with("temp."))
                throw std::runtime_error("Read-only Molang name: " + base.text);
            if (address.kind == MolangKind::Member && base.kind == MolangKind::Index)
                throw std::runtime_error("Read-only Molang array element");
        }
        if (node.kind == MolangKind::Member) {
            auto receiver = shape(node.children[0]);
            if (receiver == Shape::Scalar || receiver == Shape::String)
                throw std::runtime_error("Molang member receiver is not a record");
            auto const& base = nodes[node.children[0]];
            if (base.kind == MolangKind::Call && isBoneVector(base.text)
                && node.text != "x" && node.text != "y" && node.text != "z")
                throw std::runtime_error("Unknown Molang record field: " + node.text);
        }
        for (auto child : node.children) validateAccess(child, nesting + 1);
    }
    void enterNode() {
        if (context.remainingSteps == 0 || ++depth > 256) throw std::runtime_error("Molang execution budget exceeded");
        --context.remainingSteps;
    }
    // Borrow ordinary stored values only for the current access. Nested calls
    // can rebind variables or swap the entire invocation-local environment.
    MolangValue const* stored(MolangNode const& node) const {
        if (node.domain == SlotDomain::Other) return nullptr;
        auto const& slots = node.domain == SlotDomain::Temporary ? context.localVariables : context.variables;
        auto found = slots.find(node.text);
        return found == slots.end() ? nullptr : &found->second;
    }
    double arithmetic(MolangNode const& node) {
        double left = number(node.children[0]);
        double right = number(node.children[1]);
        if (context.legacyMolang) return calculate(node.arithmetic, static_cast<float>(left), static_cast<float>(right));
        return calculate(node.arithmetic, left, right);
    }
    template<class T> static T calculate(Arithmetic op, T left, T right) {
        switch (op) {
        case Arithmetic::Add: return left + right;
        case Arithmetic::Subtract: return left - right;
        case Arithmetic::Multiply: return left * right;
        case Arithmetic::Divide: return right == 0 ? 0 : left / right;
        case Arithmetic::Remainder: return right == 0 ? 0 : std::fmod(left, right);
        default: throw std::logic_error("Invalid arithmetic instruction");
        }
    }
    double number(std::size_t index) {
        auto const& node = nodes[index];
        if (node.kind == MolangKind::Variable) {
            if (auto slot = stored(node)) {
                enterNode();
                --depth;
                if (!slot->isFinite()) throw std::runtime_error("Non-finite Molang result");
                return slot->number();
            }
        } else if (node.kind == MolangKind::Member) {
            if (auto slot = stored(nodes[node.children[0]]); slot && slot->isRecord()) {
                enterNode();
                enterNode();
                depth -= 2;
                return slot->numericField(node.text);
            }
        }
        if (node.kind != MolangKind::Number && node.arithmetic == Arithmetic::None)
            return eval(index).number();
        enterNode();
        double result = node.kind == MolangKind::Number ? node.value : arithmetic(node);
        --depth;
        if (!std::isfinite(result)) throw std::runtime_error("Non-finite Molang result");
        return result;
    }
    MolangValue eval(std::size_t index) {
        enterNode();
        auto result = execute(nodes[index]);
        --depth;
        if (!result.isFinite()) throw std::runtime_error("Non-finite Molang result");
        return result;
    }
    std::string const* legacyVariablePath(MolangNode const& address) const {
        if (!context.legacyMolang || !address.legacySlot) return nullptr;
        auto const& slot = *address.legacySlot;
        // The caller already read the direct receiver. Deeper paths still
        // need to distinguish a missing variable root from a typed record.
        if (slot.root != address.children[0] && context.read(nodes[slot.root].text).present()) return nullptr;
        return &slot.name;
    }
    MolangValue execute(MolangNode const& node) {
        auto child = [&](std::size_t index) { return eval(node.children.at(index)); };
        switch (node.kind) {
        case MolangKind::Number: return node.value;
        case MolangKind::String: return node.text;
        case MolangKind::Variable:
            if (auto value = stored(node)) return *value;
            if (node.text != "math.pi" && !node.text.starts_with("fn.") && !node.text.starts_with("variable.")
                && !node.text.starts_with("temp.") && !node.text.starts_with("context.")) {
                std::optional<MolangCallSignature> dynamic;
                if (auto signature = signatureFor(node, dynamic)) {
                    if (MolangContext::isAction(node.text) && !context.allowActions) return {};
                    return accepts(node.text, {}, *signature) ? context.call(node.text, {}) : MolangValue{};
                }
            }
            return context.read(node.text);
        case MolangKind::Member: {
            if (auto receiver = stored(nodes[node.children[0]]); receiver && receiver->present()) {
                enterNode();
                --depth;
                if (!receiver->isFinite()) throw std::runtime_error("Non-finite Molang result");
                if (context.legacyMolang && node.legacySlot && !receiver->isRecord()) return {};
                return receiver->field(node.text);
            }
            auto receiver = child(0);
            if (!receiver.present()) if (auto path = legacyVariablePath(node)) return context.read(*path);
            if (context.legacyMolang && node.legacySlot && !receiver.isRecord()) return {};
            return receiver.field(node.text);
        }
        case MolangKind::Index: {
            auto source = child(0);
            double index = child(1).number();
            if (!source.isArray() || index < 0 || index >= source.array().size()) return {};
            return source.array()[static_cast<std::size_t>(index)];
        }
        case MolangKind::Unary: { double n = child(0).number(); return node.text == "!" ? (n == 0 ? 1.0 : 0.0) : node.text == "-" ? -n : n; }
        case MolangKind::Conditional: return child(0).number() != 0 ? child(1) : child(2);
        case MolangKind::ReciprocalExp: {
            auto argument = child(0);
            if (!context.legacyMolang)
                return 1.0 / (1.0 + context.call("math.exp", std::span(&argument, 1)).number());
            // Java 2.6.5 evaluates exp in double precision. Evaluate the
            // reciprocal as a unit so an overflowing denominator returns
            // zero without passing a non-finite value to the host.
            return 1.0 / (1.0 + std::exp(argument.number()));
        }
        case MolangKind::Assign: {
            auto const& address = nodes[node.children[0]];
            bool member = address.kind == MolangKind::Member;
            auto const& base = member ? nodes[address.children[0]] : address;
            bool slot = base.kind == MolangKind::Variable;
            bool temporary = member && !slot;
            if (temporary && base.kind == MolangKind::Index) throw std::runtime_error("Read-only Molang array element");
            if (slot && !base.text.starts_with("variable.") && !base.text.starts_with("temp."))
                throw std::runtime_error("Read-only Molang name: " + base.text);
            // Capture the location and old immutable receiver before RHS. A
            // RHS alias/rebind must not observe or redirect a partial write.
            bool compound = node.text != "=";
            auto receiver = member ? eval(address.children[0]) : compound ? context.read(base.text) : MolangValue{};
            // 2.6.5 promotes an untyped slot to a struct on a direct field
            // assignment, including when it previously held a scalar. Passing
            // or returning the slot then copies the entire value.
            if (member && slot && context.legacyMolang && address.legacySlot && !receiver.isRecord())
                receiver = MolangValue::emptyLegacyStruct();
            auto legacyPath = member && !receiver.present() ? legacyVariablePath(address) : nullptr;
            auto const& target = legacyPath ? *legacyPath : base.text;
            if (legacyPath) {
                if (compound) receiver = context.read(target);
                member = false;
            }
            auto previous = member ? receiver.field(address.text) : receiver;
            // Render observations own a copied variable map. Legacy model
            // channels may calculate helper variables there without writing
            // them back to the logical owner or gaining ACTION permission.
            auto value = child(1);
            if (compound) {
                double left = previous.number(), right = value.number();
                value = node.text == "+=" ? left + right : node.text == "-=" ? left - right : node.text == "*=" ? left * right : right == 0 ? 0 : left / right;
            }
            if (!value.isFinite()) throw std::runtime_error("Non-finite assignment");
            if (member) {
                auto updated = receiver.withField(address.text, value);
                value = updated.field(address.text);
                if (!temporary) context.writeVariable(target, std::move(updated));
            } else {
                context.writeVariable(target, value);
                if (target.starts_with(roamingPrefix)) value = context.read(target);
            }
            return value;
        }
        case MolangKind::Binary: {
            if (node.arithmetic != Arithmetic::None) return arithmetic(node);
            auto left = child(0);
            if (node.text == "->") {
                auto const& query = nodes[node.children[1]];
                if (!left.isRecord() || (query.kind != MolangKind::Variable
                    && (query.kind != MolangKind::Call || !query.children.empty()))) return {};
                auto owner = left.record();
                if (owner.schema != "projectile_owner") return {};
                auto found = owner.fields.find(query.text);
                return found == owner.fields.end() ? MolangValue{} : found->second;
            }
            if (node.text == "??") return left.present() ? left : child(1);
            if (node.text == "&&") return left.number() != 0 && child(1).number() != 0 ? 1.0 : 0.0;
            if (node.text == "||") return left.number() != 0 || child(1).number() != 0 ? 1.0 : 0.0;
            auto right = child(1);
            double a = left.number(), b = right.number();
            if (node.text == "==") return left == right ? 1.0 : 0.0;
            if (node.text == "!=") return left == right ? 0.0 : 1.0;
            if (node.text == "<") return a < b ? 1.0 : 0.0;
            if (node.text == ">") return a > b ? 1.0 : 0.0;
            if (node.text == "<=") return a <= b ? 1.0 : 0.0;
            return a >= b ? 1.0 : 0.0;
        }
        case MolangKind::Sequence: {
            MolangValue result;
            for (auto index : node.children) { result = eval(index); if (flow != MolangFlow::Normal) break; }
            return result;
        }
        case MolangKind::Return: { auto value = child(0); flow = MolangFlow::Return; return value; }
        case MolangKind::Break: flow = MolangFlow::Break; return 0.0;
        case MolangKind::Continue: flow = MolangFlow::Continue; return 0.0;
        case MolangKind::Call: {
            if (node.text == "loop") {
                if (node.children.size() != 2) throw std::runtime_error("loop requires count and body");
                double count = std::max(0.0, std::floor(child(0).number()));
                if (count > 1024) throw std::runtime_error("Molang loop budget exceeded");
                MolangValue value;
                for (int i = 0; i < static_cast<int>(count); ++i) {
                    value = child(1);
                    if (flow == MolangFlow::Return) return value;
                    bool stop = flow == MolangFlow::Break;
                    flow = MolangFlow::Normal;
                    if (stop) break;
                }
                return 0.0;
            }
            // Reject a disallowed action before evaluating its arguments.
            if (MolangContext::isAction(node.text) && !context.allowActions) return {};
            if (node.text.starts_with("variable.") || node.text.starts_with("temp.") || node.text.starts_with("context."))
                throw std::runtime_error("Molang variable is not callable: " + node.text);
            std::optional<MolangCallSignature> dynamic;
            auto signature = signatureFor(node, dynamic);
            if (!signature || !accepts(node.text, node.children, *signature)) return {};
            // Math, bone and physics queries fit without a per-call heap allocation.
            std::array<MolangValue, 6> localArguments;
            std::vector<MolangValue> largeArguments;
            auto arguments = std::span<MolangValue>(localArguments);
            if (node.children.size() > arguments.size()) {
                largeArguments.resize(node.children.size());
                arguments = largeArguments;
            }
            arguments = arguments.first(node.children.size());
            for (std::size_t i = 0; i < node.children.size(); ++i) {
                arguments[i] = eval(node.children[i]);
                if (signature->stringParameters[i] && !arguments[i].isString()) {
                    if (!arguments[i].present() || clearLiteral(node.children[i])) arguments[i] = std::string();
                    else return {};
                }
            }
            return context.call(node.text, arguments);
        }
        }
        return 0.0;
    }
};
MolangProgram::MolangProgram(double value) : literalValue(value) {}
MolangProgram::MolangProgram(std::string const& source) {
    if (source.size() > 1024 * 1024) throw std::runtime_error("Molang source budget exceeded");
    MolangParser parser(source);
    auto root = parser.sequence(false);
    if (!parser.complex && parser.nodes.size() == 1 && parser.nodes.front().kind == MolangKind::Number) {
        literalValue = parser.nodes.front().value;
        return;
    }
    auto program = std::make_shared<Impl>();
    program->nodes = std::move(parser.nodes);
    program->root = root;
    program->complex = parser.complex;
    impl = std::move(program);
}
MolangValue MolangProgram::invoke(MolangContext& context, std::optional<MolangValue> arguments) const {
    if (context.programDepth >= 128) throw std::runtime_error("Molang function nesting budget exceeded");
    // YSM FloatExpression.Literal bypasses its program executor. Preserve this
    // host's scope/budget behavior: use the direct path only when there is no
    // caller, args slot or temporary namespace to save or clear.
    auto readLiteral = [&] {
        if (context.remainingSteps == 0) throw std::runtime_error("Molang execution budget exceeded");
        --context.remainingSteps;
        if (!std::isfinite(literalValue)) throw std::runtime_error("Non-finite Molang result");
        return MolangValue(literalValue);
    };
    if (!impl && !arguments && context.programDepth == 0 && context.localVariables.empty())
        return readLiteral();
    struct Locals {
        MolangContext& context;
        std::optional<MolangContext::Variables> saved;
        explicit Locals(MolangContext& value) : context(value) {
            if (value.programDepth++ > 0) saved.emplace().swap(context.localVariables);
            else if (!context.localVariables.empty()) context.localVariables.clear();
        }
        ~Locals() {
            --context.programDepth;
            if (saved) context.localVariables.swap(*saved);
            else if (!context.localVariables.empty()) context.localVariables.erase("args");
        }
    } locals(context);
    if (arguments) context.localVariables["args"] = *arguments;
    if (!impl) return readLiteral();
    MolangEvaluator evaluator{impl->nodes, context};
    auto result = evaluator.eval(impl->root);
    if (impl->complex && evaluator.flow != MolangFlow::Return) return 0.0;
    return result;
}
MolangValue MolangProgram::evaluate(MolangContext& context) const {
    try {
        return invoke(context);
    } catch (std::exception const& error) {
        context.recordFailure(error);
        return 0.0;
    }
}
MolangValue MolangProgram::evaluate(MolangContext& context, MolangValue const& arguments) const {
    try { return invoke(context, arguments); }
    catch (std::exception const& error) { context.recordFailure(error); return 0.0; }
}
bool MolangProgram::hasControllerActions() const {
    return impl && std::any_of(impl->nodes.begin(), impl->nodes.end(), [](auto const& node) {
        return node.kind == MolangKind::Call && MolangContext::isAction(node.text);
    });
}
bool MolangProgram::hasBoneVisualActions() const {
    return impl && std::any_of(impl->nodes.begin(), impl->nodes.end(), [](auto const& node) {
        return node.kind == MolangKind::Call && (node.text == "ysm.bone_color"
            || node.text == "ysm.bone_transparency" || node.text == "ysm.bone_glow");
    });
}
bool MolangProgram::hasBoneTransparency() const {
    return impl && std::any_of(impl->nodes.begin(), impl->nodes.end(), [](auto const& node) {
        return node.kind == MolangKind::Call && node.text == "ysm.bone_transparency";
    });
}
std::set<std::string> MolangProgram::roamingFields() const {
    std::set<std::string> fields;
    if (!impl) return fields;
    for (auto const& node : impl->nodes)
        if (node.kind == MolangKind::Variable && node.text.starts_with(roamingPrefix))
            fields.insert(node.text.substr(roamingPrefix.size()));
    return fields;
}
MolangFunctionLibrary::MolangFunctionLibrary(std::vector<std::pair<std::string, std::string>> const& sources) {
    if (sources.size() > 4096) throw std::runtime_error("Too many Molang function files");
    std::size_t bytes = 0;
    for (auto const& [rawName, source] : sources) {
        bytes += source.size();
        if (bytes > 16 * 1024 * 1024) throw std::runtime_error("Molang function library budget exceeded");
        auto name = rawName;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto separator = name.find('@');
        MolangProgram program;
        try { program = MolangProgram(source); }
        catch (std::exception const& error) { diagnostics.push_back("Function " + rawName + ": " + error.what()); }
        if (separator != 0) {
            auto callable = name.substr(0, separator);
            if (!callables.emplace(callable, program).second) throw std::runtime_error("Duplicate Molang function: " + callable);
        }
        if (separator != std::string::npos) {
            auto event = name.substr(separator + 1);
            bool coded = event.find("_ctrl_") != std::string::npos && event.find("_ctrl_") > 0
                && !event.ends_with("_ctrl_") && std::all_of(event.begin(), event.end(), [](unsigned char c) {
                    return std::islower(c) || std::isdigit(c) || c == '_'; });
            if (event == "player_init" || event == "player_update" || event == "sync" || event == "defer" || coded)
                handlers[event].push_back(program);
            else diagnostics.push_back("Unknown Molang event: " + event);
        }
    }
}
MolangValue MolangFunctionLibrary::call(std::string const& name, MolangContext& context, std::span<MolangValue const> arguments) const {
    auto found = callables.find(name);
    if (found == callables.end()) return {};
    return found->second.invoke(context, MolangValue(std::vector<MolangValue>(arguments.begin(), arguments.end())));
}
std::span<MolangProgram const> MolangFunctionLibrary::events(std::string const& name) const {
    auto found = handlers.find(name);
    return found == handlers.end() ? std::span<MolangProgram const>{} : std::span<MolangProgram const>(found->second);
}
std::vector<std::string> MolangFunctionLibrary::eventNames() const {
    std::vector<std::string> names;
    for (auto const& [name, programs] : handlers) names.push_back(name);
    return names;
}
std::set<std::string> MolangFunctionLibrary::roamingFields(bool firstPerson) const {
    std::set<std::string> fields;
    auto include = [&](MolangProgram const& program) {
        auto names = program.roamingFields();
        fields.insert(names.begin(), names.end());
    };
    for (auto const& [name, program] : callables) include(program);
    for (auto const& [name, programs] : handlers) {
        if (firstPerson && name != "defer" && name.find("_ctrl_") == std::string::npos) continue;
        for (auto const& program : programs) include(program);
    }
    return fields;
}
}
