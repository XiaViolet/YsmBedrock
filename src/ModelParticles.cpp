#include "ModelParticles.h"
#include "JavaVanillaInputs.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ysm {
namespace {
using enum ParticleCoverage;
using enum ParticleParameters;
constexpr ParticleMapping pinnedParticles[] = {
#include "JavaParticleMappings.inc"
};
// Compatibility with selectors already accepted from newer Java source packs.
constexpr ParticleMapping laterParticles[] = {
    {"gust", Approximate, "WindExplosion"}, {"small_gust", Approximate, "BreezeWindExplosion"},
    {"dust_plume", Corresponding, "DustPlume"}, {"white_smoke", Corresponding, "WhiteSmoke"},
    {"vault_connection", Corresponding, "VaultConnection"}, {"ominous_spawning", Corresponding, "OminousItemSpawner"},
    {"gust_emitter_large", Approximate, "WindExplosion", "minecraft:wind_explosion_emitter"},
    {"gust_emitter_small", Approximate, "BreezeWindExplosion", "minecraft:breeze_wind_explosion_emitter"},
    {"infested", Corresponding, {}, "minecraft:infested_ambient"},
    {"item_cobweb", Approximate, "IconCrack"},
    {"trial_spawner_detection", Corresponding, {}, "minecraft:trial_spawner_detection"},
    {"trial_spawner_detection_ominous", Corresponding, {}, "minecraft:trial_spawner_detection_ominous"},
    {"raid_omen", Corresponding, {}, "minecraft:raid_omen_ambient"},
    {"trial_omen", Corresponding, {}, "minecraft:trial_omen_single"},
    {"pale_oak_leaves", Corresponding, "PaleOakLeaves"}, {"creaking_crumble", Corresponding, "CreakingCrumble"},
    {"open_eyeblossom", Corresponding, "EyeblossomOpen"}, {"close_eyeblossom", Corresponding, "EyeblossomClose"},
    {"dust_pillar", Approximate, "Terrain", {}, Block}, {"block_crumble", Approximate, "Terrain", {}, Block},
};
ParticleMapping const* findMapping(std::string_view id) {
    if (!id.starts_with("minecraft:")) return nullptr;
    id.remove_prefix(10);
    for (auto table : {std::span<ParticleMapping const>(pinnedParticles), std::span<ParticleMapping const>(laterParticles)})
        if (auto found = std::ranges::find(table, id, &ParticleMapping::id); found != table.end()) return &*found;
    return nullptr;
}

using Json = nlohmann::json;
constexpr std::string_view whitespace = " \t\r\n\f\v";
constexpr std::string_view punctuation = "{}[],:";

// Java 1.20.5+ uses an inline SNBT compound for particle options. Translate
// its scalar and container syntax to JSON before reading the supported fields.
Json structuredOptions(std::string_view input) {
    if (input.size() > 4096) throw std::runtime_error("Particle options exceed the input limit");
    constexpr std::string_view tokenEnd = " \t\r\n\f\v{}[],:";
    std::string json;
    int depth = 0;
    while (!input.empty()) {
        auto ch = input[0];
        if (whitespace.find(ch) != whitespace.npos) {
            input.remove_prefix(1);
        } else if (punctuation.find(ch) != punctuation.npos) {
            if (ch == '{' || ch == '[') ++depth;
            if (ch == '}' || ch == ']') --depth;
            if (depth < 0 || depth > 16) throw std::runtime_error("Invalid particle options nesting");
            json += ch;
            input.remove_prefix(1);
            if (ch == '[' && input.size() >= 2 && input[1] == ';'
                && std::string_view("BIL").find(input[0]) != std::string_view::npos)
                input.remove_prefix(2);
        } else if (ch == '\'' || ch == '"') {
            std::string value;
            std::size_t i = 1;
            for (; i < input.size() && input[i] != ch; ++i) {
                if (input[i] == '\\' && i + 1 < input.size()) ++i;
                value += input[i];
            }
            if (i == input.size()) throw std::runtime_error("Unterminated particle option string");
            json += Json(value).dump();
            input.remove_prefix(i + 1);
        } else {
            auto size = input.find_first_of(tokenEnd);
            auto token = input.substr(0, size);
            input.remove_prefix(token.size());
            auto next = input.find_first_not_of(whitespace);
            auto number = token;
            if (number.starts_with('+')) number.remove_prefix(1);
            if (!number.empty() && std::string_view("bBsSlLfFdD").find(number.back()) != number.npos)
                number.remove_suffix(1);
            double value = 0;
            bool numeric = false;
            if (!number.empty() && (next == input.npos || input[next] != ':')) {
                auto end = number.data() + number.size();
                auto parsed = std::from_chars(number.data(), end, value);
                numeric = parsed.ec == std::errc{} && parsed.ptr == end && std::isfinite(value);
            }
            json += numeric ? Json(value).dump() : Json(token).dump();
        }
    }
    auto options = Json::parse(json, nullptr, false);
    if (!options.is_object()) throw std::runtime_error("Invalid structured particle options");
    return options;
}

std::array<float, 3> particleColor(Json const& value) {
    std::array<float, 3> color;
    if (value.is_number()) {
        auto packed = value.get<double>();
        if (std::isfinite(packed) && std::floor(packed) == packed && packed >= -2147483648.0 && packed <= 4294967295.0) {
            auto rgb = static_cast<std::uint32_t>(static_cast<std::int64_t>(packed));
            return {float((rgb >> 16) & 255) / 255, float((rgb >> 8) & 255) / 255, float(rgb & 255) / 255};
        }
    } else if (value.is_array() && (value.size() == 3 || value.size() == 4)) {
        for (std::size_t i = 0; i < color.size(); ++i) {
            auto channel = value.at(i).get<float>();
            if (!std::isfinite(channel)) throw std::runtime_error("Invalid particle color");
            color[i] = std::clamp(channel, 0.0f, 1.0f);
        }
        return color;
    }
    throw std::runtime_error("Invalid particle color");
}

void readDust(ParticleCue& cue, Json const& options, bool transitioning) {
    cue.options.color = particleColor(options.at(transitioning ? "from_color" : "color"));
    auto scale = options.at("scale").get<float>();
    if (!std::isfinite(scale)) throw std::runtime_error("Invalid dust particle scale");
    cue.options.scale = std::clamp(scale, .01f, 4.0f);
    if (transitioning) cue.options.endColor = particleColor(options.at("to_color"));
}

template <typename Number>
Number readNumber(std::istringstream& fields) {
    std::string token;
    fields >> token;
    Number value{};
    auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || !std::isfinite(value))
        throw std::runtime_error("Invalid numeric particle option");
    return value;
}

int readInteger(Json const& input) {
    auto value = input.get<double>();
    if (!std::isfinite(value) || std::floor(value) != value
        || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        throw std::runtime_error("Expected a 32-bit integer particle option");
    return static_cast<int>(value);
}

void readParticleOptions(ParticleCue& cue, std::string_view args) {
    auto parameters = cue.mapping->parameters;
    bool effectColor = cue.mapping->id == "entity_effect" && args.starts_with('{');
    if (parameters == Simple && !effectColor) return;
    bool structured = args.starts_with('{');
    auto options = structured ? structuredOptions(args) : Json::object();
    std::istringstream fields{std::string(args)};
    fields.imbue(std::locale::classic());
    switch (parameters) {
    case Dust:
    case DustTransition: {
        bool transitioning = parameters == DustTransition;
        if (!structured) {
            std::array<float, 3> from, to;
            for (auto& channel : from) channel = readNumber<float>(fields);
            auto scale = readNumber<float>(fields);
            if (transitioning) {
                for (auto& channel : to) channel = readNumber<float>(fields);
                options = {{"from_color", from}, {"to_color", to}, {"scale", scale}};
            } else options = {{"color", from}, {"scale", scale}};
        }
        readDust(cue, options, transitioning);
        break;
    }
    case Block:
    case Item: {
        bool block = parameters == Block;
        std::string resource;
        if (structured) {
            auto const& value = options.at(block ? "block_state" : "item");
            resource = value.is_object() ? value.at(block ? "Name" : "id").get<std::string>() : value.get<std::string>();
        } else {
            fields >> resource;
            // The Bedrock provider uses a runtime block ID or item ID.
            if (auto end = resource.find_first_of(block ? "[{" : "{["); end != resource.npos) resource.resize(end);
        }
        cue.options.resource = canonicalRegistryId(resource);
        if (cue.options.resource.empty()) throw std::runtime_error("Invalid block or item particle resource");
        break;
    }
    case Roll:
        cue.options.roll = structured ? options.at("roll").get<float>() : readNumber<float>(fields);
        if (!std::isfinite(cue.options.roll)) throw std::runtime_error("Invalid sculk charge roll");
        break;
    case Delay:
        cue.options.delay = structured ? readInteger(options.at("delay")) : readNumber<int>(fields);
        break;
    case Destination:
        if (structured) {
            auto const& destination = options.at("destination");
            auto type = canonicalRegistryId(destination.at("type").get<std::string>());
            if (type == "minecraft:block") {
                auto const& pos = destination.at("pos");
                if (!pos.is_array() || pos.size() != 3) throw std::runtime_error("Expected a vibration block position");
                auto& center = cue.options.blockDestination.emplace();
                for (std::size_t axis = 0; axis < center.size(); ++axis)
                    center[axis] = readInteger(pos.at(axis)) + .5;
            } else if (type == "minecraft:entity") {
                // 1.21.1 VibrationParticleOption validates the text codec more
                // strictly than PositionSource itself or the network codec.
                throw std::runtime_error("Entity position sources are not allowed");
            } else throw std::runtime_error("Invalid vibration position source");
            cue.options.delay = readInteger(options.at("arrival_in_ticks"));
        } else {
            for (auto& coordinate : cue.options.blockDestination.emplace())
                coordinate = std::floor(readNumber<double>(fields)) + .5;
            cue.options.delay = readNumber<int>(fields);
        }
        break;
    case Simple:
        if (options.contains("color")) cue.options.color = particleColor(options.at("color"));
        break;
    }
}

void readParticleSelector(std::string const& selector, ParticleCue& cue) {
    auto first = selector.find_first_not_of(whitespace);
    auto last = selector.find_last_not_of(whitespace);
    auto name = selector.substr(first, last - first + 1);
    auto idEnd = name.find_first_of(" \t\r\n\f\v{");
    auto id = canonicalRegistryId(std::string_view(name).substr(0, idEnd));
    cue.mapping = findMapping(id);
    if (!cue.mapping) throw std::runtime_error("Unsupported particle effect: " + selector);
    auto args = idEnd == name.npos ? std::string_view{} : std::string_view(name).substr(idEnd);
    if (auto begin = args.find_first_not_of(whitespace); begin != args.npos) args.remove_prefix(begin);
    readParticleOptions(cue, args);
    cue.nativeType = cue.mapping->kind;
    if (cue.mapping->id == "item_cobweb") cue.options.resource = "minecraft:web";
    if (cue.mapping->id == "ash") cue.options.color = std::array{.25f, .25f, .25f};
    else if (cue.mapping->id == "white_ash") cue.options.color = std::array{186.f / 255, 177.f / 255, 194.f / 255};
    else if (cue.mapping->id == "crimson_spore") cue.options.color = std::array{.9f, .4f, .5f};
    else if (cue.mapping->id == "warped_spore") cue.options.color = std::array{.1f, .1f, .3f};
    else if (cue.mapping->id == "damage_indicator") {
        cue.options.color = std::array{.65f, .17f, .22f};
        cue.options.scale = .75f;
    } else if (cue.mapping->id == "glow" || cue.mapping->id == "scrape") {
        cue.options.color = std::array{.35f, 1.0f, .8f};
    }
    cue.name = name.starts_with("minecraft:") ? std::move(name) : "minecraft:" + name;
}
}

std::span<ParticleMapping const> javaParticleMappings() { return pinnedParticles; }
void batchCampfireSmoke(std::vector<ParticleCue>& cues) {
    // Adjacent smoke calls already contain world-space spawns. One manual
    // emitter can draw nearby particles from this frame in authored order.
    std::size_t written = 0;
    for (std::size_t i = 0; i < cues.size(); ++i) {
        auto& cue = cues[i];
        if (written && cue.mapping && cue.mapping->id == "campfire_cosy_smoke" && !cue.spawns.empty()) {
            auto& batch = cues[written - 1];
            if (batch.mapping == cue.mapping && batch.firstPerson == cue.firstPerson && !batch.spawns.empty()
                && batch.spawns.size() + cue.spawns.size() <= 128) {
                auto const& center = batch.spawns.front().position;
                auto nearby = [&](ParticleSpawn const& spawn) {
                    return std::abs(spawn.position[0] - center[0]) <= 4
                        && std::abs(spawn.position[1] - center[1]) <= 4
                        && std::abs(spawn.position[2] - center[2]) <= 4;
                };
                if (std::ranges::all_of(cue.spawns, nearby)) {
                    batch.spawns.insert(batch.spawns.end(), cue.spawns.begin(), cue.spawns.end());
                    continue;
                }
            }
        }
        if (written != i) cues[written] = std::move(cue);
        ++written;
    }
    cues.resize(written);
}

ParticleCoverage ParticleCue::coverage() const {
    if (!mapping || (mapping->parameters == ParticleParameters::Destination && !options.blockDestination))
        return ParticleCoverage::Missing;
    return mapping->coverage;
}

std::array<double, 3> blockVibrationVelocity(ParticleSpawn const& spawn, std::array<double, 3> const& center) {
    // Java increments age before dividing by the remaining ticks. The last finite
    // sample of a static destination is its block center at age lifetime - 1.
    std::array<double, 3> velocity{};
    if (spawn.lifetime > 1)
        for (std::size_t axis = 0; axis < velocity.size(); ++axis)
            velocity[axis] = (center[axis] - spawn.position[axis]) * (20.0 / (spawn.lifetime - 1));
    return velocity;
}
std::string_view particleCoverageName(ParticleCoverage coverage) {
    switch (coverage) {
    case Corresponding: return "corresponding";
    case Approximate: return "approximate";
    case Missing: return "missing";
    }
    return {};
}
ParticleCue prepareParticle(std::string selector, std::span<float const> values, bool absolute) {
    if (values.size() > 254) throw std::runtime_error("particle accepts at most 254 values");
    for (auto value : values) if (!std::isfinite(value)) throw std::runtime_error("Non-finite particle argument");
    ParticleCue cue;
    cue.absolute = absolute;
    if (selector.find_first_not_of(" \t\r\n\f\v") == std::string::npos) return cue;
    try {
        readParticleSelector(selector, cue);
    } catch (Json::exception const& error) {
        throw ParticleSelectorError(error.what());
    } catch (std::runtime_error const& error) {
        throw ParticleSelectorError(error.what());
    }
    auto number = [&](std::size_t index, float fallback) { return index < values.size() ? values[index] : fallback; };
    auto integer = [](float value) {
        // Java's narrowing float-to-int conversion truncates and saturates.
        return static_cast<int>(std::clamp(static_cast<double>(value),
            static_cast<double>(std::numeric_limits<int>::min()), static_cast<double>(std::numeric_limits<int>::max())));
    };
    for (std::size_t i = 0; i < 3; ++i) { cue.offset[i] = number(i, 0); cue.delta[i] = number(i + 3, 0); }
    cue.speed = number(6, 0);
    cue.count = std::max(integer(number(7, 0)), 0);
    cue.lifetime = std::max(integer(number(8, 20)), 1);
    return cue;
}
std::vector<ParticleSpawn> expandParticle(ParticleCue const& cue, std::mt19937& random, std::size_t budget) {
    if (cue.name.empty()) return {};
    auto count = static_cast<std::size_t>(cue.count == 0 ? 1 : cue.count);
    if (count > budget) throw std::runtime_error("Particle spawn budget exceeded");
    for (auto position : cue.origin.position) if (!std::isfinite(position)) throw std::runtime_error("Non-finite particle origin");
    auto yaw = cue.count == 0 && cue.origin.player ? cue.origin.bodyYaw : cue.origin.yaw;
    if (!std::isfinite(yaw)) throw std::runtime_error("Non-finite particle yaw");
    auto angle = -yaw * std::numbers::pi / 180;
    auto cosine = std::cos(angle), sine = std::sin(angle);
    std::normal_distribution<double> gaussian(0, 1);
    std::vector<ParticleSpawn> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        ParticleSpawn next;
        next.lifetime = cue.lifetime;
        for (std::size_t axis = 0; axis < 3; ++axis)
            next.position[axis] = cue.offset[axis] + (cue.count ? gaussian(random) * cue.delta[axis] : 0);
        for (std::size_t axis = 0; axis < 3; ++axis)
            next.velocity[axis] = cue.count ? gaussian(random) * cue.speed : cue.delta[axis] * cue.speed;
        // abs_particle still adds the entity origin; only offset rotation is
        // disabled. The source never rotates the velocity vector.
        if (!cue.absolute) {
            auto x = next.position[0], z = next.position[2];
            next.position[0] = x * cosine + z * sine;
            next.position[2] = z * cosine - x * sine;
        }
        for (std::size_t axis = 0; axis < 3; ++axis) next.position[axis] += cue.origin.position[axis];
        result.push_back(next);
    }
    return result;
}
std::array<ParticleSpawn, sweepArcSize> sweepArc(ParticleSpawn const& center, double yaw) {
    std::array<ParticleSpawn, sweepArcSize> result;
    auto angle = -yaw * std::numbers::pi / 180;
    auto cosine = std::cos(angle), sine = std::sin(angle);
    for (std::size_t i = 0; i < result.size(); ++i) {
        auto theta = (double(i) / (result.size() - 1) - .5) * 2.6;
        auto x = std::sin(theta) * .9;
        auto rise = std::cos(theta);
        auto z = -rise * .23;
        auto& dot = result[i];
        dot = center;
        dot.position[0] += x * cosine + z * sine;
        dot.position[1] += (rise - .64) * .57;
        dot.position[2] += z * cosine - x * sine;
        dot.velocity = {};
    }
    return result;
}

}
