#include "NativeModelParticles.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/game/LevelRenderer.h"
#include "mc/client/particle/Particle.h"
#include "mc/client/particle/ParticleEngine.h"
#include "mc/client/particlesystem/particle/ParticleSystemEngine.h"
#include "mc/client/particlesystem/particle/ParticleEffectGroup.h"
#include "mc/client/particlesystem/particle/ParticleEffectInfo.h"
#include "mc/client/particlesystem/particle/ParticleEmitterActual.h"
#include "mc/client/particlesystem/particle/CommonParticle.h"
#include "mc/world/actor/animation/AnimationComponent.h"
#include "mc/util/MolangVariableMap.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/registry/BlockTypeRegistry.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/registry/ItemRegistryRef.h"
#include "ll/api/memory/Memory.h"
#include <algorithm>
#include <bit>
#include <mutex>
#include <cmath>
#include <cstring>
#include <numbers>

namespace ysm {
namespace {
std::mutex reportMutex;
std::uint64_t requested = 0, legacyReturned = 0, modernLifetime = 0, missingLifetime = 0;
std::uint64_t namedEmissions = 0, namedFailures = 0;
std::uint64_t fallbackDispatches = 0, invalidResources = 0;
struct SelectorStats {
    std::uint64_t requested = 0, named = 0, emitters = 0, legacy = 0, unconfirmed = 0, missing = 0, failed = 0, clipped = 0;
};
std::map<std::string, SelectorStats, std::less<>> selectors;
std::map<std::string, std::uint64_t> activeEffects, activeEmitters;
std::map<std::string, std::size_t> sampledKinds;
Json samples = Json::array(), namedAttempts = Json::array();
struct ColorTransition {
    std::uint64_t handle;
    std::string effect;
    std::array<float, 3> start, end;
};
ParticleSystemEngine const* colorTransitionEngine = nullptr;
std::vector<ColorTransition> colorTransitions;
struct BlockVibration {
    std::uint64_t handle;
    Vec3 center;
};
std::vector<BlockVibration> blockVibrations;
std::optional<std::uint64_t> vibrationSampleHandle;
Json vibrationSamples = Json::array();
std::optional<std::uint64_t> smokeSampleHandle;
Json smokeSamples = Json::array();
ParticleType nativeKind(std::string_view name) {
    static constexpr std::pair<std::string_view, ParticleType> kinds[] = {
#define YSM_PARTICLE_KIND(value) {#value, ParticleType::value},
#include "NativeParticleKinds.inc"
#undef YSM_PARTICLE_KIND
    };
    for (auto const& [key, value] : kinds) if (key == name) return value;
    return ParticleType::Undefined;
}

// The SDK declares slot_map_handle as an empty placeholder. The installed
// client returns an eight-byte handle through a hidden output parameter.
// Keep its ABI and slot-map view guarded by verified 1.26.60.05 entry bytes.
using CreateEmitter = void* (*)(ParticleSystemEngine*, std::uint64_t*, HashedString const&, Vec3 const&, MolangVariableMap);
CreateEmitter createEmitterFunction() {
    static auto function = []() -> CreateEmitter {
        static auto const signature = ll::memory::Signature::parse(
            "55 56 57 48 81 EC C0 00 00 00 48 8D AC 24 80 00 00 00 48 C7 45 38 FE FF FF FF 48 89 D6 48 8B 85 80 00 00 00 0F 10 05 51 33 DE 0B 0F 29 45 F0 0F 10 05 56 33 DE 0B 0F 29 45 00 0F 10 05 5B 33 DE 0B 0F 29 45 10 F3 0F 10 05 6B 33 DE 0B F3 0F 11 45 2C F3 41 0F 10 41 08 F2 41 0F 10 09 F2 0F 11");
        auto* address = signature.view().resolve(true);
        constexpr unsigned char expected[] = {0x55,0x56,0x57,0x48,0x81,0xec,0xc0,0,0,0,0x48,0x8d,0xac,0x24,0x80,0,0,0,
            0x48,0xc7,0x45,0x38,0xfe,0xff,0xff,0xff,0x48,0x89,0xd6};
        if (!address || std::memcmp(address, expected, sizeof(expected)) != 0) return nullptr;
        return reinterpret_cast<CreateEmitter>(address);
    }();
    return function;
}
ParticleSystem::ParticleEmitterActual* resolveEmitter(ParticleSystemEngine& engine, std::uint64_t handle, std::string const& name) {
    auto slot = static_cast<std::uint32_t>(handle);
    if (slot == UINT32_MAX) return nullptr;
    // Native accessor: validate sparse index and generation, then dense index.
    // This borrowed pointer is used only synchronously, never across ticks.
    auto* sparse = ll::memory::dAccess<std::uint64_t*>(&engine, 0x38);
    auto* sparseEnd = ll::memory::dAccess<std::uint64_t*>(&engine, 0x40);
    if (!sparse || !sparseEnd || slot >= static_cast<std::size_t>(sparseEnd - sparse)) return nullptr;
    auto entry = sparse[slot];
    if ((entry ^ handle) >> 32) return nullptr;
    auto denseIndex = static_cast<std::uint32_t>(entry);
    auto** dense = ll::memory::dAccess<ParticleSystem::ParticleEmitterActual**>(&engine, 0x18);
    auto denseCount = ll::memory::dAccess<std::uint64_t>(&engine, 0x28);
    if (!dense || denseIndex >= denseCount) return nullptr;
    auto* emitter = dense[denseIndex];
    if (!emitter || emitter->getEffectName().getString() != name) return nullptr;
    return emitter;
}
ParticleSystem::ParticleEmitterActual* createOwnedEmitter(ParticleSystemEngine& engine, std::string const& name,
    Vec3 const& position, MolangVariableMap const& variables, std::uint64_t& handle) {
    auto function = createEmitterFunction();
    if (!function) return nullptr;
    handle = UINT64_MAX;
    function(&engine, &handle, HashedString(name), position, variables);
    return resolveEmitter(engine, handle, name);
}
void updateTrackedParticles(ParticleSystemEngine* engine) {
    if (engine != colorTransitionEngine) {
        colorTransitions.clear();
        smokeSampleHandle.reset();
        blockVibrations.clear();
        colorTransitionEngine = engine;
        vibrationSampleHandle.reset();
        vibrationSamples = Json::array();
    }
    if (!engine) return;
    std::erase_if(blockVibrations, [&](BlockVibration const& signal) {
        auto* emitter = resolveEmitter(*engine, signal.handle, "minecraft:vibration_signal");
        if (!emitter) return true;
        bool arrived = true;
        for (auto& particle : *emitter->mParticles) {
            if (*particle.mParticleAge + std::chrono::milliseconds(50) < *particle.mParticleLifetime) {
                arrived = false;
                continue;
            }
            // The native effect still integrates during its last tick. Java's
            // last finite position is the target, so hold it there until expiry.
            Vec3 world, velocity;
            emitter->getParticleWorldPositionAndVelocity(particle, world, velocity);
            auto correction = signal.center - world;
            auto previousToGoal = correction + (*particle.mPosition - *particle.mPositionPrev);
            particle.mPosition = *particle.mPosition + correction;
            if (previousToGoal.dot(velocity) < 0) particle.mPositionPrev = *particle.mPosition;
            particle.mVelocity = Vec3{};
            particle.mVelocityPrev = Vec3{};
        }
        return arrived;
    });
    if (vibrationSampleHandle) {
        auto* emitter = resolveEmitter(*engine, *vibrationSampleHandle, "minecraft:vibration_signal");
        if (!emitter || emitter->mParticles->empty() || vibrationSamples.size() >= 64) {
            vibrationSampleHandle.reset();
        } else {
            auto& particle = emitter->mParticles->front();
            Vec3 position, velocity;
            emitter->getParticleWorldPositionAndVelocity(particle, position, velocity);
            auto const& origin = *emitter->mEmitterWorldPosition;
            auto age = particle.mParticleAge->count();
            // Sample by simulation age so high rendering rates still capture arrival.
            auto interval = std::max<std::int64_t>(50000000, particle.mParticleLifetime->count() / 62);
            if (vibrationSamples.empty() || vibrationSamples.back().at("age_ns").get<std::int64_t>() / interval != age / interval)
                vibrationSamples.push_back({{"age_ns", age}, {"position", {position.x, position.y, position.z}},
                    {"velocity", {velocity.x, velocity.y, velocity.z}}, {"emitter", {origin.x, origin.y, origin.z}}});
        }
    }
    if (smokeSampleHandle) {
        auto* emitter = resolveEmitter(*engine, *smokeSampleHandle, "ysm:campfire_cosy_smoke");
        if (!emitter || emitter->mParticles->empty() || smokeSamples.size() >= 16) {
            smokeSampleHandle.reset();
        } else {
            auto const& particle = emitter->mParticles->front();
            auto tick = particle.mParticleAge->count() / 50000000;
            if (smokeSamples.empty() || smokeSamples.back().at("tick") != tick)
                smokeSamples.push_back({{"tick", tick}, {"size", {particle.mRenderSize->x, particle.mRenderSize->y}},
                    {"alpha", particle.mColor->a}, {"batch_size", emitter->mParticles->size()}});
        }
    }
    std::erase_if(colorTransitions, [&](ColorTransition const& transition) {
        auto* emitter = resolveEmitter(*engine, transition.handle, transition.effect);
        if (!emitter || emitter->mParticles->empty()) return true;
        for (auto& particle : *emitter->mParticles) {
            auto age = particle.mParticleAge->count(), lifetime = particle.mParticleLifetime->count();
            auto ratio = lifetime <= 0 ? 1.0f : std::clamp(static_cast<float>(age) / static_cast<float>(lifetime), 0.0f, 1.0f);
            auto channel = [&](std::size_t index) { return std::lerp(transition.start[index], transition.end[index], ratio); };
            particle.mColorPrev = *particle.mColor;
            particle.mColor = mce::Color(channel(0), channel(1), channel(2), 1.0f);
        }
        return false;
    });
}
Vec3 vector(std::array<double, 3> const& value, float scale = 1) {
    return Vec3(static_cast<float>(value[0]) * scale, static_cast<float>(value[1]) * scale, static_cast<float>(value[2]) * scale);
}
void setEmitterColor(MolangVariableMap& variables, char const* name, std::array<float, 3> const& rgb) {
    MolangMemberArray members(MolangStruct_RGBA{}, mce::Color(rgb[0], rgb[1], rgb[2], 1.0f));
    variables.setMolangVariable(HashedString(name), MolangScriptArg(members));
}
void noteColor(MolangVariableMap& variables, double pitch) {
    auto angle = static_cast<float>(pitch) * 2 * std::numbers::pi_v<float>;
    auto channel = [&](float phase) { return std::max(0.0f, std::sin(angle + phase) * .65f + .35f); };
    setEmitterColor(variables, "variable.note_color",
        {channel(0), channel(2 * std::numbers::pi_v<float> / 3), channel(4 * std::numbers::pi_v<float> / 3)});
}
std::optional<int> particleData(Level& level, ParticleType type, ParticleCue const& cue) {
    if (cue.options.resource.empty()) return 0;
    if (type == ParticleType::Terrain) {
        auto definition = level.getBlockTypeRegistry()->lookupByName(HashedString(cue.options.resource));
        auto block = definition ? static_cast<Block const*>(definition->mDefaultState) : nullptr;
        if (block && block->mHasRuntimeId) return std::bit_cast<int>(static_cast<std::uint32_t>(block->mNetworkId));
    } else if (type == ParticleType::IconCrack) {
        if (auto item = level.getItemRegistry().lookupByNameNoAlias(HashedString(cue.options.resource)); item) {
            auto itemId = static_cast<std::uint16_t>(item->mId);
            return std::bit_cast<int>(std::uint32_t(itemId) << 16);
        }
    }
    return {};
}
void applyTint(ParticleSystem::CommonParticle& particle, std::optional<std::array<float, 3>> const& color, float scale) {
    if (!color) return;
    auto const& rgb = *color;
    auto tint = mce::Color(rgb[0], rgb[1], rgb[2], 1.0f);
    particle.mColor = tint;
    particle.mColorPrev = tint;
    particle.mRenderSize->x *= scale;
    particle.mRenderSize->y *= scale;
    particle.mRenderSizePrev = *particle.mRenderSize;
}
// The Java particle provider draws its own randomness after the model's
// emitter has positioned each spawn. Keep that stream separate from scripts.
std::mt19937 moteRandom{std::random_device{}()};
float moteUnit() { return std::generate_canonical<float, 24>(moteRandom); }
Vec3 moteVelocity(std::string_view id, bool spore) {
    Vec3 direction(0.8f * moteUnit() - 0.4f, 0.8f * moteUnit() - 0.4f, 0.8f * moteUnit() - 0.4f);
    if (id == "warped_spore") direction.y -= 0.19f * moteUnit() * moteUnit();
    if (id == "crimson_spore") {
        std::normal_distribution<float> gaussian;
        direction.x += gaussian(moteRandom) * 1e-6f;
        direction.y += gaussian(moteRandom) * 1e-4f;
        direction.z += gaussian(moteRandom) * 1e-6f;
    }
    auto length = std::sqrt(direction.dot(direction));
    if (length > 0) direction = direction * ((1 + moteUnit() + moteUnit()) * 1.2f / length);
    direction.y += 2.f; // Java particles start with an upward bias of 0.1 block/tick.
    if (!spore) {
        direction = Vec3(direction.x * 0.1f, direction.y * -0.1f, direction.z * 0.1f);
        if (id == "white_ash") {
            direction.x -= 3.8f * moteUnit() * moteUnit();
            direction.y -= 5.f * moteUnit() * moteUnit();
            direction.z -= 3.8f * moteUnit() * moteUnit();
        }
    }
    return direction;
}
}
void dispatchModelParticles(IClientInstance& client, std::span<ParticleCue const> cues) {
    auto* player = client.getLocalPlayer();
    auto* renderer = client.getLevelRenderer();
    if (!player || !renderer) return;
    auto* engine = renderer->mParticleEngine->get();
    auto* system = renderer->mParticleSystemEngine->get();
    if (!engine) return;
    std::scoped_lock lock(reportMutex);
    updateTrackedParticles(system);
    std::size_t sweepBudget = 4096;
    for (auto const& cue : cues) {
        if (!cue.mapping || cue.spawns.empty()) continue;
        auto id = cue.mapping->id;
        auto& stats = selectors.try_emplace(std::string(id)).first->second;
        stats.requested += cue.spawns.size();
        requested += cue.spawns.size();
        if (cue.coverage() == ParticleCoverage::Missing) {
            stats.missing += cue.spawns.size();
            continue;
        }
        auto type = nativeKind(cue.nativeType);
        auto data = particleData(player->getLevel(), type, cue);
        if (!data) {
            invalidResources += cue.spawns.size();
            stats.failed += cue.spawns.size();
            continue;
        }
        std::vector<ParticleSpawn> dots;
        std::span<ParticleSpawn const> spawns = cue.spawns;
        if (id == "sweep_attack") {
            auto arcs = std::min(spawns.size(), sweepBudget / sweepArcSize);
            stats.clipped += spawns.size() - arcs;
            dots.reserve(arcs * sweepArcSize);
            auto yaw = cue.absolute ? 0.0 : cue.count == 0 && cue.origin.player ? cue.origin.bodyYaw : cue.origin.yaw;
            for (auto const& center : spawns.first(arcs)) {
                auto arc = sweepArc(center, yaw);
                dots.insert(dots.end(), arc.begin(), arc.end());
            }
            sweepBudget -= dots.size();
            spawns = dots;
            if (spawns.empty()) continue;
        }
        auto& samplesForSelector = sampledKinds[std::string(id)];
        auto record = [&](Json sample) {
            if (samples.size() >= 192 || samplesForSelector >= 3) return;
            ++samplesForSelector;
            samples.push_back(std::move(sample));
        };
        auto legacy = [&](ParticleSpawn const& spawn, bool fallback) {
            if (type == ParticleType::Undefined) {
                ++stats.failed;
                return;
            }
            if (fallback) ++fallbackDispatches;
            auto* particle = player->getLevel().addParticle(type, vector(spawn.position), vector(spawn.velocity), *data, nullptr, false);
            if (particle) {
                particle->mLifetime = spawn.lifetime;
                if (cue.options.color) {
                    auto const& rgb = *cue.options.color;
                    particle->mColor = mce::Color(rgb[0], rgb[1], rgb[2], 1.0f);
                    particle->mSize = particle->mSize * cue.options.scale;
                }
                ++legacyReturned;
                ++stats.legacy;
            } else {
                ++missingLifetime;
                ++stats.unconfirmed;
            }
            if (samples.size() < 192 && samplesForSelector < 3)
                record({{"name", cue.name}, {"position", spawn.position}, {"velocity", spawn.velocity}, {"lifetime", spawn.lifetime},
                    {"first_person", cue.firstPerson}, {"legacy_pointer", particle != nullptr}, {"fallback", fallback}});
        };
        std::string effectName(cue.mapping->emitter);
        if (effectName.empty()) {
            auto mapping = engine->mNewParticleSystemJsonLookup->find(type);
            if (mapping != engine->mNewParticleSystemJsonLookup->end()) effectName = mapping->second.getString();
        }
        if (system && !effectName.empty() && cue.options.resource.empty()) {
            auto* source = player->getAnimationComponent().mRenderParams->mVariables;
            if (!source) {
                namedFailures += spawns.size();
                for (auto const& spawn : spawns) legacy(spawn, true);
                continue;
            }
            MolangVariableMap variables(*source);
            variables.clear();
            auto color = cue.options.color;
            bool spore = id == "crimson_spore" || id == "warped_spore";
            bool mote = spore || id == "ash" || id == "white_ash";
            std::optional<Vec3> initialSporeVelocity;
            if (mote) {
                if (id == "ash") {
                    float gray = moteUnit() * .5f;
                    color = std::array{gray, gray, gray};
                }
                setEmitterColor(variables, "variable.color", *color);
                if (spore) {
                    initialSporeVelocity = moteVelocity(id, true);
                    auto speed = std::sqrt(initialSporeVelocity->dot(*initialSporeVelocity));
                    auto direction = speed > 0 ? *initialSporeVelocity * (1.f / speed) : Vec3(0, 1, 0);
                    // creaking_heart_trail reads these inputs while creating the emitter.
                    MolangMemberArray axes(MolangStruct_XYZ{}, direction);
                    variables.setMolangVariable(HashedString("variable.direction"), MolangScriptArg(axes));
                    variables.setMolangVariable(HashedString("variable.particle_initial_speed"), MolangScriptArg(speed));
                    variables.setMolangVariable(HashedString("variable.max_lifetime"), MolangScriptArg(cue.lifetime / 20.f));
                }
            }
            if (id == "note") noteColor(variables, spawns.front().velocity[0]);
            if (id == "sculk_charge")
                variables.setMolangVariable(HashedString("variable.rotation"),
                    MolangScriptArg(cue.options.roll * (180.0f / std::numbers::pi_v<float>)));
            bool javaCampfire = id == "campfire_cosy_smoke";
            static HashedString const smokeName("ysm:campfire_cosy_smoke");
            auto& group = system->mParticleEffectGroup;
            auto info = group.getParticleEffectInfo(javaCampfire ? smokeName : HashedString(effectName));
            auto effect = info ? info->getEffectPtr() : nullptr;
            if (javaCampfire) {
                if (effect) {
                    effectName = smokeName.getString();
                } else {
                    javaCampfire = false;
                    info = group.getParticleEffectInfo(HashedString(effectName));
                    effect = info ? info->getEffectPtr() : nullptr;
                }
            }
            std::uint64_t handle = UINT64_MAX;
            auto* created = effect ? createOwnedEmitter(*system, effectName, vector(spawns.front().position), variables, handle) : nullptr;
            if (namedAttempts.size() < 96 && !std::ranges::any_of(namedAttempts, [&](Json const& attempt) {
                    return attempt.at("name") == effectName && attempt.at("captured") == (created != nullptr);
                }))
                namedAttempts.push_back({{"name", effectName}, {"resource", static_cast<bool>(info)},
                    {"adapter_supported", createEmitterFunction() != nullptr}, {"captured", created != nullptr}});
            if (!created) {
                namedFailures += spawns.size();
                for (auto const& spawn : spawns) legacy(spawn, true);
                continue;
            }
            ++stats.emitters;
            created->setManualParticleEmission(true);
            created->setMaxNumParticles(spawns.size());
            auto initialized = created->mParticles->size();
            for (std::size_t index = 0; index < spawns.size(); ++index) {
                auto const& spawn = spawns[index];
                auto particleIndex = index;
                if (id == "note") noteColor(*created->mMolangVariableMap, spawn.velocity[0]);
                // The source passes velocity into the particle provider, then
                // changes ONLY lifetime. Keep the native effect's initialized
                // motion and shape; do not overwrite them after creation.
                // Reuse any initialized particle. Manual emission takes an
                // offset from this emitter's transform, not a world position.
                if (index >= initialized) {
                    auto previous = created->mParticles->size();
                    std::array<double, 3> offset;
                    for (std::size_t axis = 0; axis < 3; ++axis)
                        offset[axis] = spawn.position[axis] - spawns.front().position[axis];
                    created->emitParticleManually(vector(offset), vector(spawn.velocity, 20), 1);
                    // The manual entry prepares render/Molang context but its
                    // rate component may expose no manual capacity (note and
                    // other instant effects). The native provider still knows
                    // how to initialize one particle from the same resource.
                    if (created->mParticles->size() == previous)
                        created->_emitNewParticle(*effect, vector(offset), vector(spawn.velocity, 20), 1, 0);
                    if (created->mParticles->size() != previous + 1) {
                        ++namedFailures;
                        if (created->mParticles->size() == previous) legacy(spawn, true);
                        else ++stats.failed;
                        continue;
                    }
                    particleIndex = previous;
                }
                auto& particle = created->mParticles->at(particleIndex);
                particle.mParticleLifetime = std::chrono::nanoseconds(static_cast<std::int64_t>(spawn.lifetime) * 50000000);
                if (javaCampfire) {
                    if (!smokeSampleHandle && smokeSamples.empty()) smokeSampleHandle = handle;
                    auto velocity = vector(spawn.velocity, 20);
                    // CosyProvider adds at most 1/500 block/tick to vertical speed.
                    velocity.y += moteUnit() * 0.04f;
                    particle.mVelocity = velocity;
                    particle.mVelocityPrev = velocity;
                }
                if (cue.options.blockDestination) {
                    auto velocity = vector(blockVibrationVelocity(spawn, *cue.options.blockDestination));
                    particle.mVelocity = velocity;
                    particle.mVelocityPrev = velocity;
                    if (!vibrationSampleHandle && vibrationSamples.empty()) vibrationSampleHandle = handle;
                }
                if (mote) {
                    particle.mVelocity = index == 0 && initialSporeVelocity ? *initialSporeVelocity : moteVelocity(id, spore);
                    particle.mVelocityPrev = *particle.mVelocity;
                    if (spore) {
                        particle.mPosition->y -= .125f;
                        particle.mPositionPrev->y -= .125f;
                    }
                }
                applyTint(particle, color, cue.options.scale);
                ++modernLifetime;
                ++namedEmissions;
                ++stats.named;
                if (samples.size() < 192 && samplesForSelector < 3) {
                    Vec3 nativePosition, nativeVelocity;
                    created->getParticleWorldPositionAndVelocity(particle, nativePosition, nativeVelocity);
                    record({{"name", cue.name}, {"position", spawn.position}, {"velocity", spawn.velocity},
                        {"lifetime", spawn.lifetime}, {"first_person", cue.firstPerson}, {"named_emitter", effectName},
                        {"native_position", {nativePosition.x, nativePosition.y, nativePosition.z}},
                        {"native_velocity", {nativeVelocity.x, nativeVelocity.y, nativeVelocity.z}},
                        {"native_color", {particle.mColor->r, particle.mColor->g, particle.mColor->b, particle.mColor->a}},
                        {"initialized_particles", initialized}, {"native_lifetime_ns", particle.mParticleLifetime->count()}});
                }
            }
            // Existing particles finish; no later automatic emission or shared
            // vanilla effect resource is modified.
            if (cue.options.color && cue.options.endColor && colorTransitions.size() < 4096)
                colorTransitions.push_back({handle, effectName, *cue.options.color, *cue.options.endColor});
            if (cue.options.blockDestination && cue.lifetime > 1)
                blockVibrations.push_back({handle, vector(*cue.options.blockDestination)});
            created->expire();
            activeEffects.try_emplace(effectName, 0);
            activeEmitters.try_emplace(effectName, 0);
            continue;
        }
        for (auto const& spawn : spawns) legacy(spawn, false);
    }
    if (system) {
        for (auto& [name, count] : activeEffects) count = system->getEffectParticleCount(HashedString(name));
        for (auto& [name, count] : activeEmitters) count = system->getEffectEmitterCount(HashedString(name));
    }
}
void resetModelParticleTransitions() {
    std::scoped_lock lock(reportMutex);
    colorTransitionEngine = nullptr;
    smokeSampleHandle.reset();
    colorTransitions.clear();
    blockVibrations.clear();
    vibrationSampleHandle.reset();
}
Json particleRenderReport() {
    std::scoped_lock lock(reportMutex);
    Json perSelector = Json::object();
    for (auto const& [name, stats] : selectors)
        perSelector[name] = {{"requested", stats.requested}, {"named_particles", stats.named}, {"named_emitters", stats.emitters},
            {"legacy_particles", stats.legacy}, {"legacy_result_unknown", stats.unconfirmed},
            {"unmapped", stats.missing}, {"failed", stats.failed}, {"sweeps_clipped", stats.clipped}};
    return {{"requested", requested}, {"legacy_lifetime_overrides", legacyReturned},
        {"named_emissions", namedEmissions}, {"named_emission_failures", namedFailures},
        {"fallback_dispatches", fallbackDispatches}, {"unresolved_resources", invalidResources},
        {"modern_lifetime_overrides", modernLifetime}, {"missing_lifetime_overrides", missingLifetime},
        {"campfire_smoke_sample", smokeSamples}, {"active_effect_particles", activeEffects}, {"active_effect_emitters", activeEmitters},
        {"named_attempts", namedAttempts}, {"samples", samples}, {"selectors", perSelector},
        {"first_vibration_path", vibrationSamples}};
}
}
