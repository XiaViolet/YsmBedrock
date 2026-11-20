// Uses the licensed YesSteveModel-Native reader. Files produced here exist
// only for the duration of the client's synchronous model adoption.
#include <v3/codec/envelope.h>
#include <v3/container/decoder.h>
#include <codec/image.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

using Json = nlohmann::ordered_json;
namespace legacy = ysm::legacy::v3::container;
namespace {
using Sink = int(*)(void*, char const*, std::size_t, unsigned char const*, std::size_t);
constexpr double degrees = 180.0 / std::numbers::pi;
constexpr double ticksPerSecond = 20;

struct Writer {
    void* context;
    Sink sink;
    void bytes(std::string const& path, std::span<unsigned char const> data) const {
        if (sink(context, path.data(), path.size(), data.data(), data.size()) != 1)
            throw std::runtime_error("Cannot adopt YSM resource " + path + " (" + std::to_string(data.size()) + " bytes)");
    }
    void text(std::string const& path, std::string const& value) const {
        bytes(path, {reinterpret_cast<unsigned char const*>(value.data()), value.size()});
    }
    std::string json(std::string path, Json const& value) const {
        bytes(path, Json::to_cbor(value));
        return path;
    }
    std::string image(std::string stem, legacy::LegacyImage const& image) const {
        using E = legacy::ImageEncoding;
        auto encoding = image.encoding;
        if (encoding == E::kRgba || encoding == E::kZtx) {
            std::vector<unsigned char> raw(8 + std::size_t(image.width) * image.height * 4);
            for (unsigned i = 0; i < 4; ++i) {
                raw[i] = static_cast<unsigned char>(image.width >> (8 * i));
                raw[4 + i] = static_cast<unsigned char>(image.height >> (8 * i));
            }
            if (encoding == E::kRgba) {
                if (image.bytes.size() != raw.size() - 8) throw std::runtime_error("Invalid legacy RGBA size");
                std::memcpy(raw.data() + 8, image.bytes.data(), image.bytes.size());
            } else {
                auto info = ysm::codec::ImageInfo{image.width, image.height, ysm::codec::ImageFormat::kZtx, image.frame_count};
                if (!ysm::codec::ImageDecode(image.bytes, info, {raw.data() + 8, raw.size() - 8}).ok())
                    throw std::runtime_error("Cannot decode legacy texture");
            }
            stem += ".ysmrgba";
            bytes(stem, raw);
            return stem;
        }
        stem += encoding == E::kPng ? ".png" : encoding == E::kWebp ? ".webp" : encoding == E::kAvif ? ".avif" : ".jpg";
        bytes(stem, {image.bytes.data(), image.bytes.size()});
        return stem;
    }
    Json texture(std::string const& base, std::string const& name, legacy::TextureSet const& texture) const {
        Json result = {{"uv", image(base + "/" + name, texture.uv.value)}};
        for (auto const& [type, layer] : texture.pbr) {
            auto key = type == legacy::PbrType::kNormal ? "normal" : "specular";
            result[key] = image(base + "/pbr_" + key, layer.value);
        }
        return result;
    }
};

Json object(auto const& input) {
    auto result = Json::object();
    for (auto const& [key, value] : input) result[key] = value;
    return result;
}
Json geometry(legacy::GeoModel const& source) {
    auto const& desc = source.properties;
    Json description{{"identifier", "geometry.ysm_legacy"}, {"texture_width", desc.texture_width},
        {"texture_height", desc.texture_height}};
    if (desc.visible_bounds_height > 0) description["visible_bounds_height"] = desc.visible_bounds_height;
    if (desc.visible_bounds_width > 0) description["visible_bounds_width"] = desc.visible_bounds_width;
    if (desc.visible_bounds_offset.size() == 3) description["visible_bounds_offset"] = desc.visible_bounds_offset;
    auto bones = Json::array();
    bones.get_ref<Json::array_t&>().reserve(source.bones.size());
    for (auto const& bone : source.bones) {
        Json entry{{"name", bone.name}, {"pivot", {-bone.pivot.x, bone.pivot.y, bone.pivot.z}},
            {"rotation", {-bone.rotation.x * degrees, -bone.rotation.y * degrees, bone.rotation.z * degrees}}};
        if (!bone.parent.empty()) entry["parent"] = bone.parent;
        if (!bone.cubes.empty()) {
            auto positions = Json::array(), normals = Json::array(), uvs = Json::array(), polys = Json::array();
            std::map<std::array<float, 3>, std::size_t> pointIndices, normalIndices;
            std::map<std::array<float, 2>, std::size_t> uvIndices;
            auto indexOf = [](auto& indices, Json& list, auto const& value) {
                auto [entry, inserted] = indices.emplace(value, list.size());
                if (inserted) list.push_back(value);
                return entry->second;
            };
            for (auto const& cube : bone.cubes) for (auto const& quad : cube.quads) {
                auto normal = indexOf(normalIndices, normals, std::array{-quad.normal.x, quad.normal.y, quad.normal.z});
                auto poly = Json::array();
                // Mirroring X also reverses winding.
                for (auto i : {3, 2, 1, 0}) {
                    auto const& vertex = quad.vertices[static_cast<std::size_t>(i)];
                    auto const& p = vertex.position;
                    auto position = indexOf(pointIndices, positions, std::array{-16 * p.x, 16 * p.y, 16 * p.z});
                    auto uv = indexOf(uvIndices, uvs, std::array{vertex.texture_u, vertex.texture_v});
                    poly.push_back({position, normal, uv});
                }
                polys.push_back(std::move(poly));
            }
            entry["poly_mesh"] = {{"normalized_uvs", true}, {"positions", std::move(positions)}, {"normals", std::move(normals)},
                {"uvs", std::move(uvs)}, {"polys", std::move(polys)}};
        }
        bones.push_back(std::move(entry));
    }
    Json entry{{"description", std::move(description)}, {"bones", std::move(bones)}};
    return {{"format_version", "1.12.0"}, {"minecraft:geometry", Json::array({std::move(entry)})}};
}
Json value(legacy::MolangValue const& source) {
    if (source.type == legacy::MolangValueType::kString) return source.string_value;
    return source.double_value;
}
Json triple(legacy::MolangVec3 const& source) { return {value(source.x), value(source.y), value(source.z)}; }
std::string seconds(double ticks) { return Json(ticks / ticksPerSecond).dump(); }
Json keyframes(std::vector<legacy::BoneKeyFrame> const& keys) {
    auto result = Json::object();
    for (auto const& key : keys) {
        auto& target = result[seconds(key.start_tick)];
        target["pre"] = triple(key.pre);
        target["post"] = triple(key.post.value_or(key.pre));
        if (key.easing == legacy::EasingType::kCatmullRom) target["lerp_mode"] = "catmullrom";
    }
    return result;
}
Json animations(legacy::AnimationFile const& source) {
    auto animations = Json::object();
    for (auto const& clip : source.animations) {
        Json animation{{"bones", Json::object()}};
        if (clip.length >= 0) {
            // Historical static tracks may carry +infinity. Preserve their
            // non-expiring lifetime while staying inside the sampler's F32 clock.
            auto maxSeconds = std::numeric_limits<float>::max() / 32.0;
            animation["animation_length"] = std::min(clip.length / ticksPerSecond, maxSeconds);
        }
        if (clip.loop == legacy::LoopType::kHoldOnLastFrame) animation["loop"] = "hold_on_last_frame";
        else animation["loop"] = clip.loop == legacy::LoopType::kLoop;
        if (clip.blend_weight) animation["blend_weight"] = value(*clip.blend_weight);
        auto& targets = animation["bones"].get_ref<Json::object_t&>();
        targets.reserve(clip.bones.size());
        std::unordered_map<std::string_view, std::size_t> boneIndices;
        boneIndices.reserve(clip.bones.size());
        for (auto const& bone : clip.bones) {
            auto target = Json::object();
            if (!bone.rotations.empty()) target["rotation"] = keyframes(bone.rotations);
            if (!bone.positions.empty()) target["position"] = keyframes(bone.positions);
            if (!bone.scales.empty()) target["scale"] = keyframes(bone.scales);
            auto [entry, inserted] = boneIndices.try_emplace(bone.bone_name, targets.size());
            if (inserted) targets.emplace_back(bone.bone_name, std::move(target));
            else (targets.begin() + entry->second)->second = std::move(target);
        }
        for (auto const& frame : clip.instructions) animation["timeline"][seconds(frame.start_tick)] = frame.data;
        for (auto const& frame : clip.sounds) animation["sound_effects"][seconds(frame.start_tick)] = {{"effect", frame.data}};
        animations[clip.name] = std::move(animation);
    }
    return {{"animations", std::move(animations)}};
}
Json controllers(legacy::AnimationControllerFile const& source) {
    auto controllers = Json::object();
    for (auto const& [name, controller] : source.controllers) {
        Json converted{{"initial_state", controller.initial_state}, {"states", Json::object()}};
        for (auto const& [stateName, state] : controller.states) {
            Json result{{"animations", Json::array()}, {"transitions", Json::array()}};
            for (auto const& entry : state.animations) {
                if (entry.condition.empty()) result["animations"].push_back(entry.name);
                else result["animations"].push_back({{entry.name, entry.condition}});
            }
            for (auto const& transition : state.transitions)
                result["transitions"].push_back({{transition.destination, transition.condition.empty() ? "1" : transition.condition}});
            if (!state.on_entry.empty()) result["on_entry"] = state.on_entry;
            if (!state.on_exit.empty()) result["on_exit"] = state.on_exit;
            if (state.blend_transition.linear_length) result["blend_transition"] = *state.blend_transition.linear_length;
            else for (auto [time, value] : state.blend_transition.points)
                result["blend_transition"][Json(time).dump()] = value;
            for (auto const& sound : state.sound_effects) result["sound_effects"].push_back({{"effect", sound}});
            converted["states"][stateName] = std::move(result);
        }
        controllers[name] = std::move(converted);
    }
    return {{"animation_controllers", std::move(controllers)}};
}
std::string group(legacy::PlayerAnimationType type) {
    static constexpr char const* groups[]{nullptr, "main", "arm", "extra", "tac", nullptr, "carryon", "parcool",
        "swem", "slashblade", "tlm", "fp_arm", "immersive_melodies", "irons_spell_books"};
    auto index = static_cast<std::size_t>(type);
    if (index >= std::size(groups) || !groups[index]) throw std::runtime_error("Unknown legacy animation group");
    return groups[index];
}
Json descriptor(legacy::LegacyModel const& model) {
    Json desc{{"spec", 2}, {"metadata", Json::object()}, {"properties", Json::object()}, {"files", Json::object()}};
    if (auto const& info = model.info.metadata) {
        Json authors = Json::array();
        for (auto const& author : info->authors)
            authors.push_back({{"name", author.name}, {"role", author.role}, {"contact", object(author.contact)}, {"comment", author.comment}});
        desc["metadata"] = {{"name", info->name}, {"tips", info->tips}, {"links", object(info->links)}, {"authors", std::move(authors)},
            {"license", {{"type", info->license.type}, {"description", info->license.description}}}};
    }
    auto const& input = model.info.properties;
    auto& props = desc["properties"];
    props = {{"height_scale", input.height_scale}, {"width_scale", input.width_scale}, {"free", input.free},
        {"default_texture", input.default_texture}, {"preview_animation", input.preview_animation},
        {"extra_animation", object(input.extra_animation)}, {"originVer", std::to_string(model.info.origin_version)}};
    for (auto const& button : input.extra_animation_buttons) {
        Json forms = Json::array();
        for (auto const& form : button.forms)
            forms.push_back({{"type", form.type}, {"title", form.title}, {"description", form.description}, {"value", form.value},
                {"min", form.minimum}, {"max", form.maximum}, {"step", form.step}, {"labels", object(form.labels)}});
        props["extra_animation_buttons"].push_back({{"id", button.id}, {"name", button.name},
            {"sound", button.sound}, {"config_forms", std::move(forms)}});
    }
    for (auto const& page : input.extra_animation_classify)
        props["extra_animation_classify"].push_back({{"id", page.id}, {"extra_animation", object(page.animations)}});
    return desc;
}

void writeModel(legacy::LegacyModel const& model, Writer const& out) {
    if (!model.player) throw std::runtime_error("Legacy YSM has no player model");
    auto manifest = descriptor(model);
    auto& files = manifest["files"];
    auto& player = files["player"];
    for (auto const& [kind, data] : model.player->geo_models) {
        auto name = kind == legacy::PlayerModelType::kMain ? "main" : "arm";
        player["model"][name] = out.json(std::string("geometry/") + name + ".json", geometry(data.value));
    }
    for (auto const& [kind, data] : model.player->animations) {
        auto name = group(kind);
        player["animation"][name] = out.json("animations/" + name + ".json", animations(data.value));
    }
    for (auto const& [key, data] : model.player->controllers) {
        auto path = "controllers/" + std::to_string(player["animation_controllers"].size()) + ".json";
        player["animation_controllers"].push_back(out.json(path, controllers(data.value)));
    }
    for (auto const& [key, data] : model.player->textures)
        player["texture"].push_back(out.texture("textures/" + std::to_string(player["texture"].size()), key, data));
    for (auto const& [key, data] : model.common.sounds) {
        auto const& sound = data.value;
        out.bytes("sounds/" + key + (sound.encoding == legacy::SoundEncoding::kOpus ? ".opus" : ".ogg"),
            {sound.bytes.data(), sound.bytes.size()});
    }
    for (auto const& [key, data] : model.common.user_functions) out.text("functions/" + key + ".molang", data.value);
    for (auto const& [key, data] : model.common.languages) out.json("lang/" + key + ".json", object(data.value.entries));
    auto replaced = [&](auto const& targets, char const* kind) {
        for (auto const& source : targets) {
            if (!source.geo || !source.texture) throw std::runtime_error("Incomplete legacy entity replacement");
            auto base = std::string(kind) + "/" + std::to_string(files[kind].size());
            Json entry{{"match", source.match}, {"model", out.json(base + "/geometry.json", geometry(source.geo->value))},
                {"texture", out.texture(base, "main", *source.texture)}};
            if (source.animation) entry["animation"] = out.json(base + "/animation.json", animations(source.animation->value));
            if (source.controller) entry["animation_controllers"] = Json::array({out.json(base + "/controller.json", controllers(source.controller->value))});
            files[kind].push_back(std::move(entry));
        }
    };
    replaced(model.vehicles, "vehicles");
    replaced(model.projectiles, "projectiles");
    out.json("ysm.json", manifest);
}
}

extern "C" __declspec(dllexport) int ysmBedrockReadV3(unsigned char const* data, std::size_t size,
    void* state, Sink sink, char* message, std::size_t messageSize) noexcept {
    try {
        ysm::legacy::v3::codec::ContainerReader reader({data, size});
        if (auto status = reader.Initialize(); !status.ok()) throw std::runtime_error(std::string(status.message()));
        auto model = legacy::Decode(reader);
        if (!model.ok()) throw std::runtime_error(std::string(model.status().message()));
        writeModel(*model, {state, sink});
        return 1;
    } catch (std::exception const& error) {
        if (messageSize) {
            std::strncpy(message, error.what(), messageSize - 1);
            message[messageSize - 1] = 0;
        }
    } catch (...) {}
    return 0;
}
