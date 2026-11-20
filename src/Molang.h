#pragma once
#include <array>
#include <bitset>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include "MolangRoaming.h"
#include "BoneVisual.h"
#include "ModelAudio.h"
#include "ModelParticles.h"

namespace ysm {
class MolangFunctionLibrary;
class MolangPhysics;
struct MolangRecord;
class MolangValue {
    using Vector = std::array<float, 3>;
    // Most authored structs contain only scalar x/y/z/w fields. Keep those
    // inline, and promote to an immutable field map when another shape appears.
    struct LegacyNaN {};
    struct Coordinates {
        std::array<float, 4> axes{};
        unsigned char defined = 0;
    };
    std::variant<std::monostate, double, std::string, std::shared_ptr<std::vector<MolangValue> const>,
        std::shared_ptr<MolangRecord const>, Vector, Coordinates, LegacyNaN> value;
public:
    MolangValue() = default;
    MolangValue(double number) : value(number) {}
    MolangValue(std::string text) : value(std::move(text)) {}
    explicit MolangValue(std::vector<MolangValue> values) : value(std::make_shared<std::vector<MolangValue> const>(std::move(values))) {}
    MolangValue(std::string schema, std::map<std::string, MolangValue> fields);
    static MolangValue emptyLegacyStruct() { MolangValue result; result.value = Coordinates{}; return result; }
    // The release evaluator retains a domain error for ?? and reads it numerically as zero.
    static MolangValue legacyNaN() { MolangValue result; result.value = LegacyNaN{}; return result; }
    static MolangValue vector(std::array<double, 3> const& values);
    double number() const;
    bool present() const { return value.index() != 0; }
    bool isString() const { return value.index() == 2; }
    bool isArray() const { return value.index() == 3; }
    bool isRecord() const { return value.index() >= 4 && value.index() <= 6; }
    bool isFinite() const {
        auto scalar = std::get_if<double>(&value);
        return !scalar || std::isfinite(*scalar);
    }
    std::span<MolangValue const> array() const;
    MolangRecord record() const;
    MolangValue field(std::string const& name) const;
    double numericField(std::string const& name) const;
    MolangValue withField(std::string const& name, MolangValue const& next) const;
    std::string string() const;
    bool operator==(MolangValue const& other) const;
};
struct MolangRecord {
    std::string schema;
    std::map<std::string, MolangValue> fields;
    bool extensible = false;
};
struct MolangCallSignature {
    std::size_t minimum = 0, maximum = 0;
    std::bitset<256> stringParameters;
    MolangCallSignature(std::size_t lower = 0, std::size_t upper = 0, std::initializer_list<std::size_t> strings = {})
        : minimum(lower), maximum(upper) {
        for (auto index : strings) stringParameters.set(index);
    }
};
struct MolangContext {
    static constexpr std::size_t DefaultExecutionSteps = 200000;
    using Function = std::function<MolangValue(std::span<MolangValue const>)>;
    using BlockLookup = std::function<MolangValue(std::array<int, 3> const&, std::span<MolangValue const>, bool)>;
    struct BoneChannels {
        std::array<double, 3> rotation{};
        std::array<double, 3> scale{1, 1, 1};
        std::array<double, 3> position{};
    };
    struct BoneBasis {
        std::array<double, 3> pivot{};
        std::string parent;
    };
    using BoneRig = std::map<std::string, BoneBasis, std::less<>>;
    using Variables = std::unordered_map<std::string, MolangValue>;
    Variables variables, localVariables;
    std::map<std::string, MolangValue> queries;
    std::map<std::string, Function> functions;
    BlockLookup blockLookup;
    std::shared_ptr<MolangFunctionLibrary const> library;
    std::shared_ptr<MolangPhysics> physics;
    std::optional<MolangRoamingState> roaming;
    std::set<std::string> diagnostics;
    std::array<double, 3> entityPosition{}, positionDelta{};
    std::array<double, 2> cameraRotation{};
    std::string biomeId;
    bool legacyMolang = false;
    std::set<int> pressedKeys;
    std::map<std::string, std::string> modVersions;
    std::map<std::string, BoneChannels> boneChannels;
    std::shared_ptr<BoneRig const> boneRig;
    void invalidateBonePivots() { bonePivots.clear(); }
    BoneVisuals boneVisuals;
    bool keyboardEnabled = false;
    std::mt19937 random{0x59534d};
    double deltaTime = 0;
    std::size_t frame = 0, remainingSteps = DefaultExecutionSteps, failures = 0;
    std::size_t programDepth = 0, frameFailures = 0;
    bool allowActions = false;
    bool observation = false;
    // Host-selected preview delivery. Ordinary players without an authenticated
    // relay keep this disabled, matching upstream's non-preview no-op route.
    bool localSync = false;
    bool networkSync = false;
    std::vector<std::vector<float>> syncOut;
    std::size_t syncDispatches = 0;
    std::vector<std::function<void()>> effects;
    std::vector<SoundCue> soundCues;
    std::vector<ParticleCue> particleCues;
    ParticleOrigin particleOrigin;
    std::size_t particleSpawnCount = 0;
    std::string soundOwner = "global";
    void emitSound(SoundCue cue);
    void emitParticle(ParticleCue cue);
    std::vector<ParticleCue> takeParticleCues();
    void beginFrame(double delta, std::optional<double> physicsTime = {});
    MolangValue read(std::string const& name) const;
    void writeVariable(std::string const& name, MolangValue value);
    MolangValue call(std::string const& name, std::span<MolangValue const> arguments);
    std::optional<MolangCallSignature> signature(std::string const& name) const;
    void diagnose(std::string message);
    void recordFailure(std::exception const& error);
    void submit(std::function<void()> effect);
    void drainDeferred() noexcept;
    void finishFrame();
    void dispatchSync(std::span<MolangValue const> arguments);
    MolangContext forObservation() const;
    static bool isAction(std::string const& name);
    static std::string canonical(std::string name);
private:
    MolangValue callBuiltin(std::string const& name, std::span<MolangValue const> arguments);
    std::unordered_map<std::string, MolangValue> bonePivots;
    double physicsTime = 0;
    struct Deferred {
        MolangValue arguments;
        std::shared_ptr<MolangFunctionLibrary const> library;
        std::string soundOwner;
    };
    std::vector<Deferred> deferred;
    std::size_t deferredCaptures = 0;
    void const* deferredOwner = nullptr;
    bool roundOpen = false, syncLimitReported = false;
    std::size_t commitDepth = 0, syncDepth = 0, syncRounds = 0;
    friend class MolangDeferredScope;
};
class MolangSoundScope {
    MolangContext& context;
    std::string previous;
public:
    MolangSoundScope(MolangContext& value, std::string owner)
        : context(value), previous(std::move(value.soundOwner)) { context.soundOwner = std::move(owner); }
    ~MolangSoundScope() { context.soundOwner = std::move(previous); }
};
// Complete each animation/controller context before its borrowed bindings leave
// scope. Nested calls for the same player share one deferred callback boundary.
class MolangDeferredScope {
    MolangContext& context;
    void const* previous;
    bool changed;
    int exceptions;
public:
    MolangDeferredScope(MolangContext& value, void const* owner);
    ~MolangDeferredScope();
    MolangDeferredScope(MolangDeferredScope const&) = delete;
    MolangDeferredScope& operator=(MolangDeferredScope const&) = delete;
};
class MolangActionScope {
    MolangContext& context;
    bool previous;
public:
    MolangActionScope(MolangContext& value, bool allowed) : context(value), previous(value.allowActions) {
        context.allowActions = allowed && !context.observation;
    }
    ~MolangActionScope() { context.allowActions = previous; }
};
class MolangProgram {
    struct Impl;
    std::shared_ptr<Impl const> impl;
    double literalValue = 0;
public:
    explicit MolangProgram(double value = 0);
    explicit MolangProgram(std::string const& source);
    double const* numericLiteral() const { return impl ? nullptr : &literalValue; }
    MolangValue evaluate(MolangContext& context) const;
    MolangValue evaluate(MolangContext& context, MolangValue const& arguments) const;
    MolangValue invoke(MolangContext& context, std::optional<MolangValue> arguments = {}) const;
    bool hasControllerActions() const;
    bool hasBoneVisualActions() const;
    bool hasBoneTransparency() const;
    std::set<std::string> roamingFields() const;
};
// Shared compiled source; variables, locals, budgets and effects stay in the
// consuming entity's context. Event order follows source declaration order.
class MolangFunctionLibrary {
    std::map<std::string, MolangProgram> callables;
    std::map<std::string, std::vector<MolangProgram>> handlers;
    std::vector<std::string> diagnostics;
public:
    explicit MolangFunctionLibrary(std::vector<std::pair<std::string, std::string>> const& sources = {});
    MolangValue call(std::string const& name, MolangContext& context, std::span<MolangValue const> arguments) const;
    bool contains(std::string const& name) const { return callables.contains(name); }
    std::span<MolangProgram const> events(std::string const& name) const;
    std::vector<std::string> eventNames() const;
    std::set<std::string> roamingFields(bool firstPerson = false) const;
    bool empty() const { return callables.empty() && handlers.empty(); }
    std::vector<std::string> const& compileDiagnostics() const { return diagnostics; }
};
}
