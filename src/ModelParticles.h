#pragma once
#include <array>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ysm {
struct ParticleSelectorError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
enum class ParticleCoverage { Corresponding, Approximate, Missing };
enum class ParticleParameters { Simple, Block, Item, Dust, DustTransition, Roll, Delay, Destination };
struct ParticleMapping {
    std::string_view id;
    ParticleCoverage coverage;
    std::string_view kind{}, emitter{};
    ParticleParameters parameters = ParticleParameters::Simple;
};
std::span<ParticleMapping const> javaParticleMappings();
std::string_view particleCoverageName(ParticleCoverage coverage);

struct ParticleOrigin {
    std::array<double, 3> position{};
    double yaw = 0, bodyYaw = 0;
    bool player = true;
};
struct ParticleSpawn {
    std::array<double, 3> position{}, velocity{};
    int lifetime = 20;
};
struct ParticleOptions {
    std::string resource;
    std::optional<std::array<float, 3>> color, endColor;
    float scale = 1;
    float roll = 0;
    int delay = 0;
    std::optional<std::array<double, 3>> blockDestination;
};
struct ParticleCue {
    std::string name, nativeType;
    ParticleMapping const* mapping = nullptr;
    ParticleOptions options;
    std::array<float, 3> offset{}, delta{};
    float speed = 0;
    int count = 0, lifetime = 20;
    bool absolute = false, firstPerson = false;
    ParticleOrigin origin;
    std::vector<ParticleSpawn> spawns;
    ParticleCoverage coverage() const;
};
std::array<double, 3> blockVibrationVelocity(ParticleSpawn const& spawn, std::array<double, 3> const& center);
void batchCampfireSmoke(std::vector<ParticleCue>& cues);
// Empty/blank names are admitted no-ops. All numeric arguments are validated,
// including the reserved arguments after the nine source-defined values.
ParticleCue prepareParticle(std::string selector, std::span<float const> values, bool absolute);
std::vector<ParticleSpawn> expandParticle(ParticleCue const& cue, std::mt19937& random, std::size_t budget);
inline constexpr std::size_t sweepArcSize = 13;
std::array<ParticleSpawn, sweepArcSize> sweepArc(ParticleSpawn const& center, double yaw);
}
