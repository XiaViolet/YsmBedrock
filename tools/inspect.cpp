#include "WheelGeometry.h"
#include "ExtraAction.h"
#include "PortraitCadence.h"
#include "ModelPack.h"
#include "ModelLibrary.h"
#include "AnimationSampler.h"
#include "Molang.h"
#include "EntityQueries.h"
#include "ClientGeometry.h"
#include "HandAnimationController.h"
#include "AnimationControllers.h"
#include "CodedAnimationPlayer.h"
#include "ModelRelay.h"
#include "RoomAssetReplay.h"
#include "ImageCodec.h"
#include "KeyBindings.h"
#include "BedrockWeather.h"
#include "MolangPhysics.h"
#include "MolangRoamingSession.h"
#include "ReplacedModelAssignments.h"
#include "RenderSpace.h"
#include "ShoulderLocator.h"
#include "HandSlotBinding.h"
#include <glm/gtc/type_ptr.hpp>
#include "YsmUi.h"
#include "ModelPreview.h"
#include "PreviewDepthRenderer.h"
#include "ContentHash.h"
#include "UiPointer.h"
#include "UiMesh.h"
#include "UiMouseCapture.h"
#include "UiTextAtlas.h"
#include <fstream>
#include <iostream>
#include <future>
#include <thread>

static std::string utf8(wchar_t const* input) {
    auto text = std::filesystem::path(input).u8string();
    return std::string(reinterpret_cast<char const*>(text.data()), text.size());
}
static ysm::Json targetAssignments(ysm::Json const& input) {
    auto kind = [](std::string const& value) {
        if (value == "vehicle") return ysm::ReplacedModelKind::Vehicle;
        if (value == "projectile") return ysm::ReplacedModelKind::Projectile;
        throw std::runtime_error("Unknown replaced target kind");
    };
    std::map<std::string, std::shared_ptr<ysm::ModelPack const>> packs;
    std::map<std::string, std::weak_ptr<ysm::ModelPack const>> lifetime;
    for (auto const& [key, value] : input.at("packs").items()) {
        auto pack = std::make_shared<ysm::ModelPack>();
        pack->localContentKey = key;
        pack->librarySource = key;
        for (auto const& item : value.at("targets")) {
            ysm::ReplacedModelTarget target;
            target.id = item.at("id").get<std::string>();
            target.kind = kind(item.at("kind").get<std::string>());
            target.matches = item.at("matches").get<std::vector<std::string>>();
            pack->targets.push_back(std::move(target));
        }
        lifetime[key] = pack;
        packs[key] = std::move(pack);
    }
    ysm::ReplacedModelAssignments assignments;
    auto result = ysm::Json::array();
    std::map<std::string, ysm::Json> savedEntities;
    for (auto const& event : input.at("events")) {
        auto op = event.at("op").get<std::string>();
        ysm::Json resolved;
        if (op == "owner" || op == "peer") {
            auto pack = event.at("pack").is_null() ? nullptr : packs.at(event.at("pack").get<std::string>());
            auto values = event.value("roaming", ysm::Json::object()).get<ysm::RoamingPayload>();
            if (op == "peer") {
                std::optional<std::pair<std::int64_t, std::int64_t>> mount;
                if (event.contains("vehicle")) mount.emplace(event.at("vehicle").get<std::int64_t>(), event.at("first").get<std::int64_t>());
                assignments.updatePeer(event.at("owner"), std::move(pack), values, mount);
            } else assignments.updateOwner(event.at("owner"), std::move(pack), values);
        } else if (op == "mount") assignments.mounted(event.at("vehicle"), event.at("rider"), event.at("first"));
        else if (op == "dismount") assignments.dismounted(event.at("rider"));
        else if (op == "projectile") assignments.projectileOwnerSet(event.at("entity"), event.at("owner"));
        else if (op == "projectile_client_spawn") {
            assignments.projectileClientOwnerSet(event.at("entity"), event.at("owner"), event.at("shoot_item_id"));
            if (event.contains("equipped"))
                assignments.restoredClientHand(event.at("entity"), event.at("owner"), event.at("equipped"));
        } else if (op == "restore_spawn_hand") assignments.restoredClientHand(event.at("entity"), event.at("owner"), event.at("equipped"));
        else if (op == "forget") assignments.forget(kind(event.at("kind")), event.at("entity"));
        else if (op == "remove_owner") assignments.removeOwner(event.at("owner"));
        else if (op == "drop_pack") packs.erase(event.at("pack").get<std::string>());
        else if (op == "save_entity") {
            auto value = assignments.saved({kind(event.at("kind")), event.at("entity")});
            if (value) savedEntities[event.at("object")] = value->encode();
        } else if (op == "load_entity") {
            auto source = ysm::SavedReplacedAssignment::decode(savedEntities.at(event.at("object")));
            auto pack = packs.find(source.source);
            assignments.restore(event.at("entity"), source, event.value("resource_ready", true) && pack != packs.end() ? pack->second : nullptr);
        } else if (op == "load_source") {
            auto source = event.at("pack").get<std::string>();
            assignments.loadedSource(source, packs.at(source));
        } else if (op == "reset") assignments.reset();
        else if (op == "resolve") {
            auto selectedKind = kind(event.at("kind"));
            auto selected = assignments.find(selectedKind, event.at("entity"));
            if (selected && assignments.canPresent(*selected)) {
                auto tags = event.value("tags", std::vector<std::string>{});
                if (auto target = selected->pack->findTarget(selectedKind, event.at("type"), tags))
                    resolved = {{"pack", selected->pack->localContentKey}, {"target", target->id},
                        {"owner", selected->owner}, {"roaming", selected->roaming}, {"revision", selected->revision},
                        {"shoot_item_id", selected->shootItemId}};
            }
        } else throw std::runtime_error("Unknown assignment operation");
        auto report = assignments.report();
        report["resolved"] = std::move(resolved);
        report["live_packs"] = ysm::Json::object();
        for (auto const& [key, weak] : lifetime) report["live_packs"][key] = !weak.expired();
        result.push_back(std::move(report));
    }
    return result;
}
static ysm::Json valueJson(ysm::MolangValue const& value) {
    if (!value.present()) return nullptr;
    if (value.isString()) return value.string();
    if (value.isArray()) {
        auto result = ysm::Json::array();
        for (auto const& item : value.array()) result.push_back(valueJson(item));
        return result;
    }
    if (value.isRecord()) {
        auto result = ysm::Json::object();
        for (auto const& [name, field] : value.record().fields) result[name] = valueJson(field);
        return result;
    }
    return value.number();
}
static ysm::MolangValue inputValue(ysm::Json const& value, std::size_t depth = 0) {
    if (depth > 16) throw std::runtime_error("Input value nesting budget exceeded");
    if (value.is_null()) return {};
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? 1.0 : 0.0;
    if (value.is_number()) return value.get<double>();
    if (value.size() > 16384) throw std::runtime_error("Input value size budget exceeded");
    if (value.is_array()) {
        std::vector<ysm::MolangValue> items;
        for (auto const& item : value) items.push_back(inputValue(item, depth + 1));
        return ysm::MolangValue(std::move(items));
    }
    std::map<std::string, ysm::MolangValue> fields;
    auto const& source = value.contains("$fields") ? value.at("$fields") : value;
    if (!source.is_object()) throw std::runtime_error("Record fields must be an object");
    for (auto const& [name, field] : source.items()) fields[ysm::MolangContext::canonical(name)] = inputValue(field, depth + 1);
    auto schema = value.value("$schema", fields.size() == 3 && fields.contains("x") && fields.contains("y") && fields.contains("z")
        ? std::string("ysm.vector") : std::string("host.record"));
    return ysm::MolangValue(std::move(schema), std::move(fields));
}
static ysm::EntityQueryInput entityInput(ysm::Json const& source) {
    if (!source.is_object()) throw std::runtime_error("Entity query input must be an object");
    ysm::EntityQueryInput result;
    result.position = source.value("position", std::array<double, 3>{});
    if (source.contains("render_position"))
        result.renderPosition = source.at("render_position").get<std::array<double, 3>>();
    if (source.contains("camera_position"))
        result.cameraPosition = source.at("camera_position").get<std::array<double, 3>>();
    result.cameraRotation = source.value("camera_rotation", std::array<double, 2>{});
    result.hitTargetId = source.value("hit_target_id", "");
    result.hitTargetType = source.value("hit_target_type", "");
    result.biomeId = ysm::canonicalRegistryId(source.value("biome", ""));
    result.player = source.value("is_player", true);
    result.living = source.value("is_living", result.player);
    result.velocity = source.value("velocity", std::array<double, 3>{});
    if (source.contains("walk_animation_speed")) result.walkAnimationSpeed = source.at("walk_animation_speed").get<double>();
    result.riptide = source.value("riptide", false);
    result.hurt = source.value("hurt", false);
    result.onClimbable = source.value("on_climbable", false);
    result.moveInput = source.value("move_input", std::array<double, 3>{});
    result.framesPerSecond = source.value("fps", 0.0);
    result.yaw = source.value("yaw", 0.0);
    result.viewYaw = source.value("view_yaw", result.yaw);
    result.bodyYaw = source.value("body_yaw", 0.0);
    result.headPitch = source.value("head_pitch", 0.0);
    result.headYaw = source.value("head_yaw", 0.0);
    if (source.contains("raw_head_pitch") || source.contains("native_head_yaw"))
        result.setHeadAngles(source.value("raw_head_pitch", 0.0), result.bodyYaw, source.value("native_head_yaw", result.bodyYaw));
    result.health = source.value("health", 0.0);
    result.maxHealth = source.value("max_health", 0.0);
    result.foodLevel = source.value("food_level", 0.0);
    result.playerLevel = source.value("player_level", 0);
    result.usingItem = source.value("using_item", false);
    result.itemUseTicks = source.value("item_use_ticks", 0);
    result.itemMaxUseTicks = source.value("item_max_use_ticks", 0);
    result.equipmentCount = source.value("equipment_count", 0);
    result.spectator = source.value("spectator", false);
    result.onFire = source.value("on_fire", false);
    result.hasRider = source.value("has_rider", false);
    result.worldTime = source.value("world_time", 0.0);
    result.tick = source.value("tick", std::int64_t{});
    result.uuidLeast = source.value("uuid_least", std::int64_t{});
    result.renderTicks = source.value("render_ticks", static_cast<double>(result.tick) + source.value("partial_tick", 0.0));
    result.rainLevel = source.value("rain_level", 0.0);
    result.thunderLevel = source.value("thunder_level", 0.0);
    if (source.contains("bedrock_weather")) {
        auto const& weather = source.at("bedrock_weather");
        result.setBedrockWeather(weather.value("rain", 0.0), weather.value("lightning", 0.0));
    }
    result.blockY = source.value("block_y", 0);
    result.blockingHeight = source.value("blocking_height", 0);
    result.onGround = source.value("on_ground", false);
    result.crouching = source.value("crouching", false);
    result.passenger = source.value("passenger", false);
    result.inWater = source.value("in_water", false);
    result.inRain = source.value("in_rain", false);
    result.swimming = source.value("swimming", false);
    result.flying = source.value("flying", false);
    result.gliding = source.value("gliding", false);
    if (source.contains("elytra_rot_z")) result.elytraRotationZ = source.at("elytra_rot_z").get<double>();
    result.emulateElytra = source.value("emulate_elytra", false);
    result.elytraEquipped = source.value("elytra_equipped", false);
    result.renderElytraLayer = source.value("render_elytra_layer", true);
    result.renderFrame = source.value("render_frame", std::uint64_t{});
    result.sprinting = source.value("sprinting", false);
    result.crawling = source.value("crawling", false);
    result.sleeping = source.value("sleeping", false);
    result.alive = source.value("alive", true);
    result.eating = source.value("eating", false);
    result.fishing = source.value("fishing", false);
    result.skyVisible = source.value("sky_visible", false);
    result.renderingInInventory = source.value("rendering_in_inventory", false);
    result.viewPerspective = std::clamp(source.value("view_perspective", 1), 0, 2);
    return result;
}
static void inputContext(ysm::MolangContext& context, ysm::Json const& input, ysm::EntityQueries& entityQueries) {
    if (input.contains("entity")) entityQueries.bind(context, entityInput(input.at("entity")));
    if (input.contains("blocks")) {
        using Block = std::pair<std::string, std::vector<std::string>>;
        std::map<std::array<int, 3>, Block> blocks;
        for (auto const& entry : input.at("blocks"))
            blocks[entry.at("position").get<std::array<int, 3>>()] =
                {entry.at("id").get<std::string>(), entry.value("tags", std::vector<std::string>{})};
        context.blockLookup = [blocks = std::move(blocks)](auto const& position, auto tags, bool requireAll) -> ysm::MolangValue {
            auto found = blocks.find(position);
            if (found == blocks.end()) return {};
            auto const& [id, membership] = found->second;
            return ysm::blockQueryResult(id, tags, [&](auto const& tag) {
                return std::find(membership.begin(), membership.end(), tag) != membership.end();
            }, requireAll);
        };
    }
    auto queries = input.value("queries", ysm::Json::object());
    auto inputs = input.value("variables", ysm::Json::object());
    for (auto const& [name, value] : queries.items())
        context.queries[ysm::MolangContext::canonical(name)] = inputValue(value);
    for (auto const& [name, value] : inputs.items())
        context.writeVariable(ysm::MolangContext::canonical(name), inputValue(value));
    if (input.contains("bone_rotations")) {
        context.invalidateBonePivots();
        for (auto const& [name, rotation] : input.at("bone_rotations").items())
            context.boneChannels[name].rotation = rotation.get<std::array<double, 3>>();
    }
    if (input.contains("bone_scales")) {
        context.invalidateBonePivots();
        for (auto const& [name, scale] : input.at("bone_scales").items())
            context.boneChannels[name].scale = scale.get<std::array<double, 3>>();
    }
    if (input.contains("position_delta")) context.positionDelta = input["position_delta"].get<std::array<double, 3>>();
    if (input.contains("pressed_keys")) context.pressedKeys = input.at("pressed_keys").get<std::set<int>>();
    if (input.contains("in_game")) context.keyboardEnabled = input.at("in_game").get<bool>();
    if (input.contains("mod_versions")) context.modVersions = input.at("mod_versions").get<std::map<std::string, std::string>>();
}
static ysm::Json variables(ysm::MolangContext const& context) {
    auto result = ysm::Json::object();
    for (auto const& [name, value] : context.variables) result[name] = valueJson(value);
    for (auto const& [name, value] : context.localVariables) result[name] = valueJson(value);
    if (context.roaming) for (auto const& [name, value] : context.roaming->namedValues())
        result[std::string(ysm::roamingPrefix) + name] = value;
    return result;
}
static ysm::Json boneVisuals(ysm::MolangContext const& context) {
    auto result = ysm::Json::object();
    for (auto const& [name, visual] : context.boneVisuals)
        result[name] = {{"color", visual.color}, {"alpha", visual.alpha}, {"glow", visual.glow}};
    return result;
}
static ysm::Json roamingReport(ysm::MolangRoamingState const& state) {
    return {{"role", state.role() == ysm::RoamingRole::Producer ? "producer" : "consumer"},
            {"fields", state.fields()}, {"values", state.namedValues()},
            {"dirty", state.role() == ysm::RoamingRole::Producer ? ysm::Json(state.hasDelta()) : ysm::Json()}};
}
static ysm::Json roamingState(ysm::Json const& input) {
    auto role = input.value("role", "producer");
    if (role != "producer" && role != "consumer") throw std::runtime_error("Invalid Roaming role");
    ysm::MolangRoamingState state(input.at("fields").get<std::set<std::string>>(),
        role == "producer" ? ysm::RoamingRole::Producer : ysm::RoamingRole::Consumer,
        input.value("local_only", false) ? ysm::RoamingLimit::LocalOnly : ysm::RoamingLimit::WireCompatible);
    auto steps = ysm::Json::array();
    for (auto const& operation : input.at("operations")) {
        ysm::Json step;
        try {
            auto name = operation.at("op").get<std::string>();
            if (name == "write") state.write(operation.at("name").get<std::string>(), operation.at("value").get<float>());
            else if (name == "read") step["result"] = state.read(operation.at("name").get<std::string>());
            else if (name == "snapshot") step["result"] = state.snapshot();
            else if (name == "full") step["result"] = state.full();
            else if (name == "delta") step["result"] = state.delta();
            else if (name == "apply_full") state.applyFull(operation.at("values").get<ysm::RoamingPayload>());
            else if (name == "apply_patch") state.applyPatch(operation.at("values").get<ysm::RoamingPayload>());
            else throw std::runtime_error("Invalid Roaming operation");
        } catch (std::exception const& error) { step["error"] = error.what(); }
        step["state"] = roamingReport(state);
        steps.push_back(std::move(step));
    }
    return {{"steps", steps}};
}
static ysm::Json roamingSession(ysm::Json const& input) {
    std::map<std::string, ysm::MolangRoamingSession> sessions;
    std::map<std::string, ysm::RoamingInstallationId> identities;
    std::map<std::string, ysm::MolangContext> contexts;
    std::map<std::string, ysm::MolangRoamingSession::Prepared> candidates;
    auto frames = ysm::Json::array();
    for (auto const& operation : input.at("operations")) {
        auto& session = sessions[operation.value("session", "main")];
        ysm::Json frame = ysm::Json::object();
        try {
            auto op = operation.at("op").get<std::string>();
            if (op == "prepare") {
                auto id = operation.at("id").get<std::string>();
                auto identity = std::make_shared<ysm::RoamingInstallation>();
                auto fields = operation.at("fields").get<std::set<std::string>>();
                auto prepared = session.prepare(operation.at("model").get<std::string>(), identity,
                    fields, operation.value("local_only", false));
                ysm::MolangContext context;
                context.roaming.emplace(std::move(fields), ysm::RoamingRole::Producer,
                    operation.value("local_only", false) ? ysm::RoamingLimit::LocalOnly : ysm::RoamingLimit::WireCompatible);
                context.roaming->initializeProducer(prepared.values());
                frame["initial_values"] = prepared.values();
                frame["has_baseline"] = prepared.hasBaseline();
                contexts.insert_or_assign(id, std::move(context));
                identities.insert_or_assign(id, std::move(identity));
                candidates.insert_or_assign(operation.value("ticket", id), std::move(prepared));
            } else if (op == "activate") session.activate(std::move(candidates.at(operation.at("ticket").get<std::string>())));
            else if (op == "drop") candidates.erase(operation.at("ticket").get<std::string>());
            else if (op == "end") session.modelEnded(identities.at(operation.at("id").get<std::string>()));
            else if (op == "write") {
                auto& context = contexts.at(operation.at("id").get<std::string>());
                context.writeVariable(ysm::MolangContext::canonical("r." + operation.at("name").get<std::string>()),
                    inputValue(operation.at("value")));
            } else if (op == "normal" || op == "action" || op == "observe") {
                auto id = operation.at("id").get<std::string>();
                auto& owner = contexts.at(id);
                std::optional<ysm::MolangContext> observation;
                if (op == "observe") observation = owner.forObservation();
                auto& current = observation ? *observation : owner;
                current.beginFrame(.05);
                auto before = current.failures;
                frame["result"] = valueJson(ysm::MolangProgram(operation.value("source", "0")).evaluate(current));
                current.finishFrame();
                frame["failures"] = current.failures - before;
                if (op == "normal" && current.failures == before) {
                    auto delta = current.roaming->delta();
                    frame["delta"] = delta;
                    frame["adopted"] = session.adopt(identities.at(id), delta);
                }
                frame["variables"] = variables(current);
            } else if (op == "adopt") frame["adopted"] = session.adopt(
                identities.at(operation.at("id").get<std::string>()), operation.at("values").get<ysm::RoamingPayload>());
            else if (op == "snapshot") {
                auto id = operation.value("id", "");
                auto sampled = session.snapshot(operation.at("model").get<std::string>(),
                    id.empty() ? ysm::RoamingInstallationId{} : identities.at(id),
                    id.empty() ? ysm::RoamingPayload{} : contexts.at(id).roaming->snapshot());
                frame["available"] = sampled.has_value();
                if (sampled) {
                    frame["values"] = sampled->values;
                    frame["exact_installation"] = sampled->installation != nullptr;
                }
            } else if (op == "first_full") frame["reload"] = session.firstBaseline(
                operation.at("model").get<std::string>(), operation.at("values").get<ysm::RoamingPayload>());
            else if (op == "clear") frame["reload"] = session.clearBaselines(operation.contains("model")
                ? std::optional<std::string>(operation.at("model").get<std::string>()) : std::nullopt);
            else if (op == "disconnect") session.resetConnection();
            else if (op == "move_from") session.moveFrom(sessions.at(operation.at("from").get<std::string>()));
            else throw std::runtime_error("Invalid Roaming session operation");
        } catch (std::exception const& error) { frame["error"] = error.what(); }
        frame["namespaces"] = session.namespaces();
        frames.push_back(std::move(frame));
    }
    return {{"frames", frames}};
}
static ysm::Json expression(std::string const& source, ysm::Json const& input) {
    ysm::MolangProgram program(source);
    ysm::MolangContext context;
    ysm::EntityQueries entityQueries;
    std::vector<std::pair<std::string, std::string>> functions;
    auto functionInput = input.value("functions", ysm::Json::object());
    for (auto const& [name, body] : functionInput.items()) functions.emplace_back(name, body.get<std::string>());
    context.library = std::make_shared<ysm::MolangFunctionLibrary const>(functions);
    auto roamingNames = context.library->roamingFields();
    auto programNames = program.roamingFields();
    roamingNames.insert(programNames.begin(), programNames.end());
    context.roaming.emplace(std::move(roamingNames), ysm::RoamingRole::Producer);
    inputContext(context, input, entityQueries);
    auto frames = input.value("frames", ysm::Json::array({ysm::Json::object()}));
    ysm::MolangValue result;
    auto values = ysm::Json::array();
    auto frameVariables = ysm::Json::array();
    for (auto const& frame : frames) {
        auto physicsTime = frame.contains("physics_time") ? std::optional<double>(frame.at("physics_time").get<double>()) : std::nullopt;
        context.beginFrame(frame.value("delta_time", input.value("delta_time", 0.0)), physicsTime);
        inputContext(context, frame, entityQueries);
        result = program.evaluate(context);
        context.finishFrame();
        values.push_back(valueJson(result));
        if (input.value("trace_variables", false)) frameVariables.push_back(variables(context));
    }
    auto report = ysm::Json{{"value", valueJson(result)}, {"values", values}, {"variables", variables(context)},
        {"diagnostics", context.diagnostics}, {"compile_diagnostics", context.library->compileDiagnostics()}, {"failures", context.failures}};
    if (input.value("trace_variables", false)) report["frame_variables"] = std::move(frameVariables);
    return report;
}
static ysm::Json animation(ysm::Json const& document, std::string const& state, double time, ysm::Json const& input, ysm::Json const& geometry = ysm::Json()) {
    ysm::AnimationSampler sampler(document, geometry.is_null() ? input.value("geometry", ysm::Json()) : geometry);
    ysm::AnimationEvaluation evaluation;
    evaluation.expressions.roaming.emplace(sampler.roamingFields(), ysm::RoamingRole::Producer);
    ysm::EntityQueries entityQueries;
    inputContext(evaluation.expressions, input, entityQueries);
    auto samples = input.value("samples", ysm::Json::array({time}));
    ysm::Pose pose;
    for (auto const& sample : samples) {
        std::vector<ysm::AnimationLayer> overlays;
        auto layerInput = sample.is_object() ? sample.value("layers", input.value("layers", ysm::Json::array())) : input.value("layers", ysm::Json::array());
        for (auto const& layer : layerInput) overlays.push_back({layer.at("name").get<std::string>(), layer.value("time", 0.0), layer.value("additive_rotation", false), layer.value("timeline_id", ""), layer.value("restart", false)});
        if (sample.is_object()) {
            inputContext(evaluation.expressions, sample, entityQueries);
            double point = sample.at("time").get<double>();
            pose = sampler.sample(sample.value("state", state), sample.value("state_time", point), point, evaluation, overlays);
        } else {
            double point = sample.get<double>();
            pose = sampler.sample(state, point, point, evaluation, overlays);
        }
    }
    return {{"pose", ysm::AnimationSampler::report(pose)}, {"unsupported_expression_channels", sampler.unsupportedChannels()},
        {"compile_diagnostics", sampler.compileDiagnostics()}, {"evaluation_diagnostics", evaluation.expressions.diagnostics},
        {"expression_failures", evaluation.expressions.failures}, {"variables", variables(evaluation.expressions)}};
}
static ysm::Json document(std::filesystem::path const& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream || stream.tellg() > 32 * 1024 * 1024) throw std::runtime_error("Invalid animation document");
    stream.seekg(0);
    return ysm::Json::parse(stream);
}
static ysm::HandInput handInput(ysm::Json const& frame) {
    ysm::HandInput state;
    auto readItem = [](ysm::Json const& source) {
        ysm::HandItem item{source.value("id", ""), source.value("category", ""), source.value("use_action", ""),
            source.value("tags", std::vector<std::string>{}), source.value("identity", ""),
            source.value("charged", false), source.value("fishing", false), std::max(0, source.value("max_durability", 0)), 0, {}};
        item.damage = std::clamp(source.value("damage", 0), 0, item.maxDurability);
        auto enchantments = source.value("enchantments", ysm::Json::object());
        if (!enchantments.is_object()) throw std::runtime_error("Invalid enchantment map");
        for (auto const& [key, value] : enchantments.items()) {
            auto id = ysm::canonicalRegistryId(key);
            auto level = value.get<int>();
            if (id.empty() || level < 0) throw std::runtime_error("Invalid enchantment");
            if (level > 0) item.enchantments[id] = level;
        }
        return item;
    };
    auto mount = [](ysm::Json const& value) -> std::optional<ysm::HandInput::Mount> {
        if (value.is_null()) return {};
        return ysm::HandInput::Mount{value.at("id").get<std::string>(), value.value("legacy_animation", ""),
            value.value("tags", std::vector<std::string>{}), value.value("alive", true)};
    };
    state.vehicle = mount(frame.value("vehicle", ysm::Json()));
    state.passenger = mount(frame.value("passenger_entity", ysm::Json()));
    auto items = frame.value("items", ysm::Json::array());
    if (!items.is_array() || items.size() > 2) throw std::runtime_error("Invalid hand item array");
    for (std::size_t hand = 0; hand < items.size(); ++hand) state.items[hand] = readItem(items[hand]);
    auto armor = frame.value("armor", ysm::Json::object());
    if (!armor.is_object()) throw std::runtime_error("Invalid armor item map");
    for (auto const& [slot, source] : armor.items()) {
        if (slot != "head" && slot != "chest" && slot != "legs" && slot != "feet") throw std::runtime_error("Invalid armor slot");
        state.armor[slot] = readItem(source);
    }
    state.usingHand = frame.value("using_hand", -1);
    state.swingingHand = frame.value("swinging_hand", -1);
    state.swingProgress = frame.value("swing_progress", 0.0);
    state.armorValue = frame.value("armor_value", 0);
    state.shieldBlockCooldown = frame.value("shield_block_cooldown", false);
    state.sleeping = frame.value("sleeping", false);
    state.enabled = frame.value("enabled", true);
    if (frame.value("native_bedrock", false)) {
        for (auto& item : state.items) ysm::adaptBedrockHandItem(item);
        for (auto& [slot, item] : state.armor) ysm::adaptBedrockHandItem(item);
        for (auto* mount : {&state.vehicle, &state.passenger})
            if (*mount) ysm::appendJavaEntityTags((*mount)->id, (*mount)->tags);
    }
    return state;
}
static ysm::Json hands(ysm::Json const& animations, ysm::Json const& input,
    ysm::RoamingLimit limit = ysm::RoamingLimit::WireCompatible) {
    ysm::HandAnimationController controller(animations);
    ysm::AnimationSampler sampler(animations);
    ysm::AnimationEvaluation evaluation;
    evaluation.expressions.roaming.emplace(sampler.roamingFields(), ysm::RoamingRole::Producer, limit);
    ysm::EntityQueries entityQueries;
    auto frames = ysm::Json::array();
    for (auto const& frame : input.at("frames")) {
        auto state = handInput(frame);
        double time = frame.at("time").get<double>();
        auto layers = controller.update(time, state);
        inputContext(evaluation.expressions, frame, entityQueries);
        auto pose = sampler.sample("idle", time, time, evaluation, layers);
        auto active = ysm::Json::array();
        for (auto const& layer : layers) active.push_back({{"name", layer.name}, {"time", layer.time}, {"owner", layer.timelineId}, {"restart", layer.restart}});
        frames.push_back({{"layers", active}, {"native_melee_spear_pose", controller.usesNativeSpearPose()},
            {"pose", ysm::AnimationSampler::report(pose)}, {"variables", variables(evaluation.expressions)}});
    }
    return {{"frames", frames}, {"expression_failures", evaluation.expressions.failures},
        {"evaluation_diagnostics", evaluation.expressions.diagnostics}, {"compile_diagnostics", sampler.compileDiagnostics()}};
}
static ysm::Json controllers(ysm::Json const& document, ysm::Json const& input, bool player = false,
    ysm::ControllerPolicy policy = ysm::ControllerPolicy::Strict, bool firstPerson = false,
    std::optional<ysm::ReplacedModelKind> replaced = {}, ysm::ModelUiBindings const* profileForms = nullptr) {
    ysm::AnimationSampler sampler(document, input.value("geometry", ysm::Json()));
    ysm::AnimationControllers runtime(document, policy);
    ysm::AnimationEvaluation evaluation;
    auto roamingLimit = input.value("local_only", false) ? ysm::RoamingLimit::LocalOnly : ysm::RoamingLimit::WireCompatible;
    evaluation.expressions.roaming.emplace(runtime.roamingFields(sampler, firstPerson, roamingLimit),
        firstPerson ? ysm::RoamingRole::Consumer : ysm::RoamingRole::Producer, roamingLimit);
    ysm::EntityQueries entityQueries;
    ysm::HandAnimationController hands(document);
    ysm::Pose previous;
    auto frames = ysm::Json::array();
    ysm::ExtraAction extraRequest;
    double extraStarted=0;
    bool timingOnly = input.value("timing_only", false);
    for (auto const& frame : input.at("frames")) {
        auto sampleStarted = std::chrono::steady_clock::now();
        double time = frame.at("time").get<double>();
        bool observing = frame.value("observe", false);
        std::optional<ysm::MolangContext> observation;
        auto observerQueries = entityQueries;
        if (observing) observation = evaluation.expressions.forObservation();
        auto& current = observing ? *observation : evaluation.expressions;
        auto hostFrame = frame;
        if (replaced) {
            if (!hostFrame.contains("entity")) hostFrame["entity"] = ysm::Json::object();
            hostFrame["entity"]["is_player"] = false;
        }
        inputContext(current, hostFrame, observing ? observerQueries : entityQueries);
        current.localSync = frame.value("local_sync", input.value("local_sync", false));
        ysm::Pose pose;
        auto handled = ysm::Json::object();
        if (player || replaced) {
            if (observing) pose = runtime.observePlayer(sampler, current);
            else if (replaced) {
                auto const& entity = hostFrame.at("entity");
                ysm::ReplacedAnimationInput state;
                state.present = entity.value("present", true);
                state.inWater = entity.value("in_water", false);
                state.onGround = entity.value("on_ground", false);
                state.onFire = entity.value("on_fire", false);
                state.inGround = entity.value("in_ground", false);
                state.hasPassengers = entity.value("has_passengers", false);
                state.shootItemId = entity.value("shoot_item_id", "");
                state.velocity = entity.value("velocity", ysm::Triple{});
                state.onGroundTicks = entity.value("on_ground_time", 0);
                state.biting = entity.value("is_biting", false);
                state.spectralArrow = entity.value("is_spectral_arrow", false);
                state.throwableItem = entity.value("throwable_item", "");
                state.hookedEntity = entity.value("hooked_in", "");
                pose = runtime.sampleReplaced(sampler, *replaced, time, evaluation, state);
            }
            else if (firstPerson) pose = runtime.sampleFirstPerson(sampler, time, evaluation, handInput(frame));
            else {
                auto state = handInput(frame);
                auto layers = hands.update(time, state);
                auto base = frame.value("automatic_state", false) ? entityInput(frame.at("entity")).mainAnimation() : frame.value("state", "idle");
                if (auto selected=frame.find("play_extra");selected!=frame.end()) {
                    auto clip=selected->get<std::string>();
                    if(!sampler.hasClip(clip))throw std::runtime_error("Unknown extra action in replay");
                    extraRequest.play(std::move(clip));
                    extraStarted=time;
                }
                if (auto m=frame.find("movement_input");m!=frame.end()) {
                    extraRequest.stopOnMovement(m->value("locked",false),m->value("strafe",0.f),
                        m->value("forward",0.f),m->value("jump",false),m->value("sneak",false));
                    handled["extra_action"]={{"name",extraRequest.name},{"serial",extraRequest.serial}};
                }
                pose = runtime.samplePlayer(sampler, base, frame.value("state_time", time),
                    time, evaluation, state, layers, frame.value("extra", extraRequest.name), frame.value("extra_time", time-extraStarted));
            }
        } else {
            if (observing) throw std::runtime_error("Use the player endpoint for render observations");
            ysm::AnimationSampler::beginFrame(time, evaluation);
            auto roots = frame.value("controllers", input.value("controllers", std::vector<std::string>{"player.main"}));
            for (auto const& root : roots) handled[root] = runtime.apply(root, sampler, time, pose, previous, evaluation);
            sampler.finishPose(pose, evaluation.expressions);
            evaluation.expressions.finishFrame();
        }
        for (auto const& event : observing ? ysm::Json::array() : frame.value("sync_events", ysm::Json::array())) {
            if (!event.is_array()) throw std::runtime_error("Sync ingress requires an argument array");
            std::vector<ysm::MolangValue> arguments;
            for (auto const& value : event) {
                if (!value.is_number()) throw std::runtime_error("Sync ingress requires finite numbers");
                arguments.emplace_back(value.get<double>());
            }
            evaluation.expressions.dispatchSync(arguments);
        }
        auto sampleUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - sampleStarted).count();
        ysm::Json sounds = ysm::Json::array(), particles = ysm::Json::array();
        if (!observing) {
            previous = pose;
            for (auto const& cue : evaluation.expressions.soundCues) sounds.push_back({{"owner", cue.owner}, {"sound", cue.name},
                {"id", cue.id ? ysm::Json(*cue.id) : ysm::Json()}, {"stop_id", cue.stopId}, {"force", cue.force},
                {"looping", cue.looping}, {"volume", cue.volume}, {"pitch", cue.pitch}});
            evaluation.expressions.soundCues.clear();
            for (auto const& cue : evaluation.expressions.takeParticleCues()) {
                ysm::Json spawns = ysm::Json::array();
                for (auto const& spawn : cue.spawns) {
                    ysm::Json sample = {{"position", spawn.position}, {"velocity", spawn.velocity}, {"lifetime", spawn.lifetime}};
                    if (cue.options.blockDestination) {
                        sample["block_center"] = *cue.options.blockDestination;
                        sample["vibration_velocity_per_second"] = ysm::blockVibrationVelocity(spawn, *cue.options.blockDestination);
                    }
                    spawns.push_back(std::move(sample));
                }
                particles.push_back({{"name", cue.name}, {"native_type", cue.nativeType},
                    {"coverage", ysm::particleCoverageName(cue.coverage())}, {"emitter", cue.mapping->emitter},
                    {"resource", cue.options.resource}, {"color", cue.options.color ? ysm::Json(*cue.options.color) : ysm::Json()},
                    {"end_color", cue.options.endColor ? ysm::Json(*cue.options.endColor) : ysm::Json()},
                    {"scale", cue.options.scale},
                    {"offset", cue.offset}, {"delta", cue.delta}, {"speed", cue.speed}, {"count", cue.count},
                    {"lifetime", cue.lifetime}, {"absolute", cue.absolute}, {"spawns", spawns}});
            }
        }
        if (timingOnly) {
            ysm::Json row = {{"sample_us", sampleUs}, {"pose_bones", pose.size()},
                {"variables", current.variables.size() + current.localVariables.size()}};
            if (profileForms) {
                auto started = std::chrono::steady_clock::now();
                auto values = profileForms->observe(current);
                row["form_observation_us"] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
                row["form_count"] = values.size();
            }
            frames.push_back(std::move(row));
            continue;
        }
        frames.push_back({{"pose", ysm::AnimationSampler::report(pose)}, {"variables", variables(current)},
            {"bone_visuals", boneVisuals(current)}, {"logical_bone_visuals", boneVisuals(evaluation.expressions)},
            {"sync_dispatches", evaluation.expressions.syncDispatches},
            {"handled", handled}, {"controllers", runtime.report()}, {"observation", observing}, {"sound_cues", std::move(sounds)},
            {"particle_cues", std::move(particles)},
            {"observation_failures", observing ? current.failures - evaluation.expressions.failures : 0},
            {"observation_diagnostics", observing ? ysm::Json(current.diagnostics) : ysm::Json::array()},
            {"logical_variables", variables(evaluation.expressions)}, {"owner_frame", evaluation.expressions.frame},
            {"owner_time", evaluation.lastTime}, {"physics_time", evaluation.expressions.physics ? evaluation.expressions.physics->time() : 0},
            {"physics_states", evaluation.expressions.physics ? evaluation.expressions.physics->size() : 0},
            {"timeline_cursors", evaluation.timelineCursors.size()}});
    }
    return {{"frames", frames}, {"expression_failures", evaluation.expressions.failures},
        {"evaluation_diagnostics", evaluation.expressions.diagnostics}, {"compile_diagnostics", sampler.compileDiagnostics()}};
}
static ysm::Json pairedAnimations(ysm::Json const& source, ysm::Json const& input) {
    auto const& bodyDocument = source.at("body");
    auto const& armDocument = source.at("arm");
    ysm::AnimationSampler bodySampler(bodyDocument, input.value("body_geometry", ysm::Json()));
    ysm::AnimationSampler armSampler(armDocument, input.value("arm_geometry", ysm::Json()));
    ysm::AnimationControllers bodyControllers(bodyDocument, ysm::ControllerPolicy::SourcePackage);
    ysm::AnimationControllers armControllers(armDocument, ysm::ControllerPolicy::SourcePackage);
    ysm::HandAnimationController hands(bodyDocument);
    ysm::AnimationEvaluation body, arm;
    auto roamingLimit = input.value("local_only", false) ? ysm::RoamingLimit::LocalOnly : ysm::RoamingLimit::WireCompatible;
    body.expressions.roaming.emplace(bodyControllers.roamingFields(bodySampler, false, roamingLimit), ysm::RoamingRole::Producer, roamingLimit);
    arm.expressions.roaming.emplace(armControllers.roamingFields(armSampler, true, roamingLimit), ysm::RoamingRole::Consumer, roamingLimit);
    if (input.contains("initial_roaming")) body.expressions.roaming->initializeProducer(input.at("initial_roaming").get<ysm::RoamingPayload>());
    ysm::connectRoamingArm(*body.expressions.roaming, *arm.expressions.roaming);
    auto initialBody = roamingReport(*body.expressions.roaming);
    auto initialArm = roamingReport(*arm.expressions.roaming);
    ysm::EntityQueries bodyQueries, armQueries;
    ysm::Pose armPose;
    double armOrigin = -1, lastArmTime = -1;
    auto frames = ysm::Json::array();
    for (auto const& frame : input.at("frames")) {
        double time = frame.at("time").get<double>();
        auto state = handInput(frame);
        inputContext(body.expressions, frame, bodyQueries);
        auto layers = hands.update(time, state);
        auto beforeFrame = body.expressions.frame;
        auto bodyPose = bodyControllers.samplePlayer(bodySampler, frame.value("state", "idle"),
            frame.value("state_time", time), time, body, state, layers, "", 0);
        std::optional<ysm::RoamingPayload> projected;
        if (body.expressions.frame != beforeFrame && body.expressions.failures == body.expressions.frameFailures)
            projected = ysm::projectRoamingArm(*body.expressions.roaming, *arm.expressions.roaming);
        if (frame.value("first_person", true) && lastArmTime != time) {
            if (armOrigin < 0) armOrigin = time;
            arm.expressions.queries = body.expressions.queries;
            arm.expressions.positionDelta = body.expressions.positionDelta;
            arm.expressions.modVersions = body.expressions.modVersions;
            arm.expressions.pressedKeys = body.expressions.pressedKeys;
            arm.expressions.keyboardEnabled = body.expressions.keyboardEnabled;
            if (frame.contains("arm_input")) inputContext(arm.expressions, frame.at("arm_input"), armQueries);
            armPose = armControllers.sampleFirstPerson(armSampler, time - armOrigin, arm, state);
            lastArmTime = time;
        }
        frames.push_back({{"body_pose", ysm::AnimationSampler::report(bodyPose)}, {"arm_pose", ysm::AnimationSampler::report(armPose)},
            {"body_variables", variables(body.expressions)}, {"arm_variables", variables(arm.expressions)},
            {"body_bone_visuals", boneVisuals(body.expressions)}, {"arm_bone_visuals", boneVisuals(arm.expressions)},
            {"body_roaming", roamingReport(*body.expressions.roaming)}, {"arm_roaming", roamingReport(*arm.expressions.roaming)},
            {"roaming_projected", projected.has_value()}, {"roaming_delta", projected ? ysm::Json(*projected) : ysm::Json()},
            {"body_frame", body.expressions.frame}, {"arm_frame", arm.expressions.frame},
            {"body_time", body.lastTime}, {"arm_time", arm.lastTime},
            {"body_physics_states", body.expressions.physics ? body.expressions.physics->size() : 0},
            {"arm_physics_states", arm.expressions.physics ? arm.expressions.physics->size() : 0},
            {"body_controllers", bodyControllers.report()}, {"arm_controllers", armControllers.report()}});
    }
    return {{"frames", frames}, {"initial_body_roaming", initialBody}, {"initial_arm_roaming", initialArm},
        {"expression_failures", body.expressions.failures + arm.expressions.failures},
        {"body_diagnostics", body.expressions.diagnostics}, {"arm_diagnostics", arm.expressions.diagnostics},
        {"body_compile_diagnostics", bodySampler.compileDiagnostics()}, {"arm_compile_diagnostics", armSampler.compileDiagnostics()}};
}
static ysm::Json coded(ysm::Json const& document, ysm::Json const& input) {
    ysm::AnimationSampler sampler(document, input.value("geometry", ysm::Json()));
    ysm::CodedAnimationPlayer player("probe");
    ysm::AnimationEvaluation evaluation;
    evaluation.expressions.roaming.emplace(sampler.roamingFields(), ysm::RoamingRole::Producer);
    ysm::EntityQueries entityQueries;
    ysm::Pose previous;
    auto frames = ysm::Json::array();
    for (auto const& frame : input.at("frames")) {
        auto time = frame.at("time").get<double>();
        ysm::AnimationSampler::beginFrame(time, evaluation);
        sampler.bindFunctions(evaluation);
        inputContext(evaluation.expressions, frame, entityQueries);
        ysm::Pose pose;
        auto lower = frame.value("pose", input.value("pose", ysm::Json::object()));
        for (auto const& [name, value] : lower.items()) {
            auto& bone = pose[name];
            bone.position = value.value("position", ysm::Triple{});
            bone.rotation = value.value("rotation", ysm::Triple{});
            bone.scale = value.value("scale", ysm::Triple{1, 1, 1});
        }
        std::optional<ysm::AnimationLayer> request;
        if (frame.contains("animation")) {
            request.emplace();
            request->name = frame.at("animation").get<std::string>();
            request->restart = frame.value("restart", false);
            request->additiveRotation = frame.value("additive_rotation", false);
            auto loop = frame.value("loop", "authored");
            if (loop == "loop") request->loop = ysm::AnimationLoop::Loop;
            else if (loop == "once") request->loop = ysm::AnimationLoop::Once;
            else if (loop == "hold") request->loop = ysm::AnimationLoop::Hold;
            else if (loop != "authored") throw std::runtime_error("Invalid coded loop override");
        }
        auto command = frame.value("command", "continue");
        auto state = command == "pause" ? ysm::CodedPlayState::Pause : command == "stop" ? ysm::CodedPlayState::Stop : ysm::CodedPlayState::Continue;
        if (command != "pause" && command != "stop" && command != "continue") throw std::runtime_error("Invalid coded play command");
        auto blend = frame.value("transition", input.value("transition", ysm::Json(0)));
        double length = 0;
        std::map<double, double> points;
        if (blend.is_object()) {
            for (auto const& [stamp, value] : blend.items()) {
                double position = std::stod(stamp);
                points[position] = value.get<double>();
                length = std::max(length, position);
            }
        } else length = blend.get<double>();
        player.process(sampler, request ? &*request : nullptr, time, pose, previous, evaluation, length, state, points);
        evaluation.expressions.finishFrame();
        previous = pose;
        frames.push_back({{"pose", ysm::AnimationSampler::report(pose)}, {"player", player.report()},
            {"variables", variables(evaluation.expressions)}});
    }
    return {{"frames", frames}, {"expression_failures", evaluation.expressions.failures},
        {"evaluation_diagnostics", evaluation.expressions.diagnostics}, {"compile_diagnostics", sampler.compileDiagnostics()}};
}
static ysm::Json modelLibrary(std::filesystem::path const& models, std::filesystem::path const& selection, ysm::Json const& input) {
    ysm::ModelLibrary library(models, selection);
    std::optional<ysm::ModelPack> current;
    auto frames = ysm::Json::array();
    for (auto const& action : input.value("actions", ysm::Json::array({"initial"}))) {
        auto operation = action.is_string() ? action.get<std::string>() : action.at("action").get<std::string>();
        ysm::Json attempt = nullptr;
        std::string error;
        try {
            bool asynchronous = operation.starts_with("async_");
            auto command = asynchronous ? operation.substr(6) : operation;
            if (command == "initial" || command == "next" || command == "prepare_next" || command == "restart" || command == "select") {
                if (command == "restart") { library = ysm::ModelLibrary(models, selection); current.reset(); }
                std::stop_source cancellation;
                std::vector<std::string> controllerDiagnostics;
                if (action.is_object() && action.value("cancel", false)) cancellation.request_stop();
                auto validate = [&](ysm::ModelPack const& pack) {
                    ysm::ClientGeometry::build(pack);
                    ysm::AnimationSampler body(pack.bodyAnimations(), pack.mainGeometry), arm(pack.firstPersonAnimations());
                    ysm::AnimationControllers controllers(pack.controllers, ysm::ControllerPolicy::SourcePackage);
                    auto roamingNames = controllers.roamingFields(body, false, ysm::RoamingLimit::LocalOnly);
                    auto armNames = controllers.roamingFields(arm, true, ysm::RoamingLimit::LocalOnly);
                    roamingNames.insert(armNames.begin(), armNames.end());
                    ysm::MolangRoamingState::validateLayout(roamingNames, ysm::RoamingLimit::LocalOnly);
                    controllerDiagnostics = controllers.report().at("diagnostics").get<std::vector<std::string>>();
                    if (action.is_object() && action.value("cancel_after_validation", false)) cancellation.request_stop();
                };
                auto origin = std::this_thread::get_id();
                bool offThread = false;
                auto load = [&] {
                    offThread = std::this_thread::get_id() != origin;
                    if (command == "select") return library.select(action.at("source").get<std::string>(), validate, cancellation.get_token());
                    return command == "initial" || command == "restart"
                        ? library.initial(validate, cancellation.get_token()) : library.next(validate, cancellation.get_token());
                };
                auto started = std::chrono::steady_clock::now();
                auto choice = asynchronous ? std::async(std::launch::async, load).get() : load();
                auto prepareUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started).count();
                if (choice.pack) for (auto const& message : controllerDiagnostics) choice.diagnostics.push_back(message);
                attempt = {{"source", choice.source}, {"prepared", choice.pack.has_value()}, {"cancelled", choice.cancelled},
                           {"diagnostics", choice.diagnostics}, {"catalog", choice.catalog}, {"prepared_off_thread", offThread},
                           {"prepare_us", prepareUs},
                           {"controller_diagnostics", controllerDiagnostics}};
                if (choice.pack) attempt["model"] = choice.pack->summary();
                if (command != "prepare_next" && choice.pack) { library.accept(choice); current = std::move(choice.pack); }
                else if (!choice.cancelled && command != "prepare_next") library.observe(choice);
            } else if (operation == "browse") {
                auto choice = library.browse(action.at("source").get<std::string>());
                attempt = {{"source", choice.source}, {"prepared", choice.pack.has_value()}};
                library.observe(choice);
            } else if (operation == "read_card") {
                if (auto request = library.takeUnreadCard()) {
                    auto info = ysm::ModelLibrary::readCard(*request);
                    if (info) library.applyCard(*request, std::move(*info));
                    attempt = {{"source", request->source}, {"read", info.has_value()}};
                }
            } else if (operation == "texture") {
                if (!current) throw std::runtime_error("No selected model");
                if (!current->selectTexture(action.at("name").get<std::string>())) throw std::runtime_error("Unknown texture");
                library.rememberTexture(*current);
            } else if (operation == "save") library.save();
            else throw std::runtime_error("Unknown model-library operation: " + operation);
        } catch (std::exception const& failure) { error = failure.what(); }
        frames.push_back({{"operation", operation}, {"error", error}, {"attempt", std::move(attempt)},
                          {"model", current ? current->summary() : ysm::Json()}, {"library", library.report()}});
    }
    return {{"frames", frames}};
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 4 && std::wstring_view(argv[1]) == L"--room-assets-test") {
            std::cout << ysm::roomAssetReplay(argv[2], argv[3]).dump() << '\n';
            return 0;
        }
        if (argc == 3 && std::wstring(argv[1]) == L"--relay-preparation") {
            ysm::RelayPreparationQueue queue;
            ysm::Json choices = ysm::Json::array();
            for (auto const& step : document(argv[2])) {
                ysm::RelayFrame frame;
                for (auto const& peer : step.at("players")) {
                    ysm::RelayPose pose;
                    pose.xuid = peer.at("id");
                    pose.session = peer.value("session", "a");
                    pose.contentKey = peer.value("model", "body");
                    pose.texture = peer.value("texture", "");
                    frame.players.emplace(pose.xuid, pose);
                }
                auto now = std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(step.at("ms").get<long long>());
                auto skip = step.value("skip", std::set<std::string>{});
                auto pose = queue.next(frame, now, [&](auto const& id) { return !skip.contains(id); });
                choices.push_back(pose ? ysm::Json(pose->xuid) : ysm::Json(nullptr));
                if (pose && step.value("fail", false)) queue.failed(*pose, now);
            }
            std::cout << choices.dump() << '\n';
        } else if (argc == 5 && std::wstring(argv[1]) == L"--relay-probe") {
            auto relay = ysm::ModelRelay::readConfig(argv[2]);
            if (!relay) throw std::runtime_error("Relay is disabled");
            auto input = document(argv[3]);
            ysm::RelayPose local;
            local.xuid = input.at("xuid").get<std::string>();
            local.session = input.at("session").get<std::string>();
            if (auto receipts = input.find("receipts"); receipts != input.end())
                for (auto const& [id, value] : receipts->items())
                    local.receipts.emplace(id, ysm::RelayReceipt{value.at(0).get<std::string>(),
                        value.at(1).get<std::string>(), value.at(2).get<std::uint64_t>()});
            local.contentKey = input.value("model", "");
            local.texture = input.value("texture", "");
            local.action = input.value("action", "");
            local.actionSerial = input.value("action_serial", std::uint64_t{0});
            local.actionTime = input.value("action_time", 0.0);
            local.shoulderBirds = input.value("shoulder_birds", std::array<std::int64_t,2>{});
            local.roaming = input.value("roaming", ysm::RoamingPayload{});
            for (auto const& event : input.value("events", ysm::Json::array()))
                local.events.push_back({event.at("sequence").get<std::uint64_t>(), event.at("args").get<std::vector<float>>()});
            auto frame = relay->exchange(local, input.at("serial").get<std::uint64_t>());
            auto result = ysm::Json{{"approved", frame.approved}, {"confirmed_event", frame.acknowledgedEvent},
                {"catalog", frame.assets.size()},
                {"selectable_catalog", std::ranges::count_if(frame.assets, [](auto const& item) { return item.second.selectable; })},
                {"players", ysm::Json::array()}};
            for (auto const& [id, player] : frame.players) {
                auto row = ysm::Json{{"xuid", id}, {"model", player.contentKey}, {"action", player.action},
                    {"texture", player.texture}, {"roaming", player.roaming}, {"shoulder_birds",player.shoulderBirds}, {"events", ysm::Json::array()}};
                for (auto const& event : player.events)
                    row["events"].push_back({{"sequence", event.sequence}, {"args", event.arguments}});
                if (input.value("download", false)) {
                    auto pack = relay->obtain(frame.assets.at(player.contentKey), argv[4]);
                    if (!player.texture.empty()) pack.selectTexture(player.texture);
                    row["model_summary"] = pack.summary();
                }
                result["players"].push_back(std::move(row));
            }
            std::cout << result.dump() << '\n';
        } else if (argc == 4 && std::wstring(argv[1]) == L"--ui-text-atlas") {
            std::ifstream stream{std::filesystem::path(argv[2])};auto input=ysm::Json::parse(stream);std::vector<ysm::UiPaint> paint;
            for(auto const& p:input.at("labels"))paint.push_back({{p.value("x",0.f),p.value("y",0.f),p.value("w",100.f),p.value("h",14.f)},p.value("color",0xff243e37u),p.at("text").get<std::string>(),p.value("scale",1.f),p.value("centered",false)});
            auto atlas=ysm::rasterizeUiText(paint,input.value("density",1.f));std::ofstream output{std::filesystem::path(argv[3]),std::ios::binary};output.write(reinterpret_cast<char const*>(atlas.image.rgba.data()),atlas.image.rgba.size());
            auto quads=ysm::Json::array();for(auto const& q:atlas.quads)quads.push_back({{"x",q.rect.x},{"y",q.rect.y},{"w",q.rect.w},{"h",q.rect.h},{"uv",q.uv}});
            std::cout<<ysm::Json{{"width",atlas.image.width},{"height",atlas.image.height},{"density",atlas.pixelDensity},{"quads",quads}}.dump()<<'\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--ui-mouse-capture") {
            ysm::UiMouseCapture capture;std::array<int,2> owners{};auto result=ysm::Json::array();
            for(auto const& e:ysm::Json::parse(utf8(argv[2]))){auto client=&owners.at(e.value("client",0));auto op=e.at("op").get<std::string>();
                if(op=="open")capture.open(client);else if(op=="close")capture.close();
                else if(op=="grab")result.push_back({{"blocked",capture.suppress(client)},{"owner",capture.owns(client)}});
                else throw std::runtime_error("Unknown mouse capture operation");
            }std::cout<<result.dump()<<'\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--portrait-cadence") {
            auto input=ysm::Json::parse(utf8(argv[2]));
            ysm::PortraitCadence clock;
            clock.rate(input.value("fps",30));
            auto submitted=ysm::Json::array();
            for(double t:input.at("times").get<std::vector<double>>()) {
                auto now=std::chrono::steady_clock::time_point{}+std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(t));
                if(clock.due(now)) {clock.submitted(now);submitted.push_back(t);}
            }
            std::cout<<submitted.dump()<<'\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--ui-frame-gate") {
            ysm::UiFrameGate gate;auto result=ysm::Json::array();for(auto const& event:ysm::Json::parse(utf8(argv[2]))){if(event=="begin")gate.begin();else if(event=="render")result.push_back(gate.claim());else throw std::runtime_error("Unknown UI frame event");}std::cout<<result.dump()<<'\n';
        } else if (argc == 4 && std::wstring(argv[1]) == L"--import-model") {
            std::filesystem::path models(argv[3]);
            ysm::ModelLibrary library(models, models.parent_path() / "selection.json");
            auto choice = library.importSource(argv[2]);
            if (!choice.pack) throw std::runtime_error(choice.diagnostics.empty() ? "Cannot import model" : choice.diagnostics.back());
            std::cout << ysm::Json{{"source", choice.source}}.dump() << '\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--ui-session") {
            auto input=ysm::Json::parse(utf8(argv[2])); ysm::YsmUi ui; ysm::UiSnapshot snapshot;ysm::UiPointer pointer;
            std::optional<ysm::ModelPack> pack;
            if (auto source = input.find("pack"); source != input.end()) {
                auto const& path = source->get_ref<std::string const&>();
                pack = ysm::ModelPack::load(std::filesystem::path(std::u8string(reinterpret_cast<char8_t const*>(path.data()), path.size())));
                snapshot.descriptor = pack->descriptor;
                for(auto const& [group,doc]:pack->animations.items()) if(group!="fp_arm")
                    for(auto const& [name,clip]:doc.at("animations").items()) snapshot.clips.push_back(name);
                snapshot.texture = pack->currentTexture().name;
                for (auto const& texture : pack->textures) snapshot.textures.push_back(texture.name);
            }
            auto update=[&](ysm::Json const& j) {
                snapshot.generation=j.value("generation",snapshot.generation);snapshot.source=j.value("source",snapshot.source);
                snapshot.locale=ysm::modelLocale(j.value("locale",snapshot.locale));
                snapshot.enabled=j.value("enabled",snapshot.enabled);
                snapshot.browseLoading=j.value("browse_loading",snapshot.browseLoading);
                if (j.contains("browsing")) {
                    snapshot.browsing.reset();
                    if (auto const& selected=j.at("browsing"); selected.is_object()) {
                        auto candidate=std::make_shared<ysm::LibraryPreview>();
                        candidate->source=selected.at("source").get<std::string>();
                        candidate->name=selected.value("name",candidate->source);
                        candidate->animation=selected.value("animation",std::string{});
                        candidate->clips=selected.value("clips",candidate->clips);
                        snapshot.browsing=std::move(candidate);
                    }
                }
                snapshot.descriptor=j.value("descriptor",snapshot.descriptor);snapshot.language=j.value("language",snapshot.language);
                if (pack) snapshot.language=ysm::modelLanguage(pack->languages,snapshot.locale);
                snapshot.catalog=j.value("catalog",snapshot.catalog);snapshot.settings=j.value("settings",ysm::YsmUi::defaultSettings());
                snapshot.players=j.value("players",snapshot.players);snapshot.room=j.value("room",snapshot.room);
                snapshot.values=j.value("values",snapshot.values);snapshot.textures=j.value("textures",snapshot.textures);snapshot.clips=j.value("clips",snapshot.clips);
                ui.update(snapshot);
            };
            update(input);auto frames=ysm::Json::array();
            for(auto const& e:input.at("events")) {
                auto op=e.at("op").get<std::string>();
                if(op=="open"){pointer.reset();ui.open(e.value("wheel",false));}else if(op=="close"){pointer.reset();ui.close();}
                else if(op=="focus_lost"){pointer.reset();ui.cancelPointer();}
                else if(op=="update")update(e);
                else if(op=="key")ui.key(e.at("key").get<int>(),e.value("down",true),e.value("control",false),e.value("text",std::string{}),e.value("shift",false));
                else if(op=="mouse")ui.mouse(e.value("button",0),e.value("value",0),e.at("x").get<float>(),e.at("y").get<float>());
                else if(op=="window_mouse") {
                    std::optional<ysm::UiPoint> window;
                    if(e.contains("window_x"))window=ysm::UiPoint{e.at("window_x").get<float>(),e.at("window_y").get<float>()};
                    auto w=e.at("client_width").get<float>(),h=e.at("client_height").get<float>();
                    auto gw=input.value("width",640.f),gh=input.value("height",360.f);
                    std::optional<ysm::UiPoint> point;
                    if(e.contains("touch_x"))
                        point=pointer.contact({e.at("touch_x").get<float>(),e.at("touch_y").get<float>()},w,h,gw,gh);
                    else if(e.value("poll",false)) {
                        if(!pointer.usesTouch()) point=pointer.frame(window.value(),w,h,gw,gh);
                    } else point=pointer.feed(e.value("button",0),e.value("x",0.f),e.value("y",0.f),window,w,h,gw,gh,
                        {e.value("dx",0.f),e.value("dy",0.f)},e.value("force_relative",false));
                    if(point) ui.mouse(e.value("button",0),e.value("value",0),point->x,point->y);
                }
                else if(op!="draw")throw std::runtime_error("Unknown UI event");
                auto scene=ui.draw(input.value("width",640.f),input.value("height",360.f),input.value("hotbar_top",-1.f));auto commands=ysm::Json::array();
                for(auto const& c:ui.takeCommands())commands.push_back({{"kind",c.kind},{"key",c.key},{"value",c.value},{"label",c.label},{"generation",c.generation}});
                auto frame=ui.report();
                frame["pointer_source"]=pointer.source();
                frame["candidate_preview"]=scene.libraryPreview;
                frame["live_player_preview"]=scene.livePlayerPreview;
                frame["preview_clip"]=scene.previewClip;
                frame["animate_preview"]=scene.animatePreview;
                frame["commands"]=commands;frame["text"]=ysm::Json::array();
                auto split=std::min(scene.overlayStart,scene.paint.size());auto paint=std::span<ysm::UiPaint const>(scene.paint);
                auto main=ysm::buildUiMesh(paint.first(split)),cursor=ysm::buildUiMesh(paint.subspan(split));
                frame["mesh_vertices"]=main.size()+cursor.size();frame["mesh_batches"]=ysm::uiMeshBatches(main.size())+ysm::uiMeshBatches(cursor.size());
                for(auto const& p:scene.paint)if(!p.text.empty())frame["text"].push_back({{"text",p.text},{"x",p.rect.x},{"y",p.rect.y},{"w",p.rect.w},{"h",p.rect.h}});
                if(input.value("paint",false)) {
                    frame["paint"]=ysm::Json::array();
                    for(auto const& p:scene.paint){frame["paint"].push_back({{"x",p.rect.x},{"y",p.rect.y},{"w",p.rect.w},{"h",p.rect.h},{"color",p.color},{"text",p.text},{"scale",p.scale},{"centered",p.centered},{"radius",p.radius}});
                        if(p.wheelSector>=0) {
                            auto sector=ysm::wheelSector(p.rect,p.wheelSector);
                            auto& polygon=frame["paint"].back()["polygon"];
                            for(auto v:sector.inner)polygon.push_back({v.x,v.y});
                            for(auto it=sector.outer.rbegin();it!=sector.outer.rend();++it)polygon.push_back({it->x,it->y});
                        }
                    }
                    frame["overlay_start"]=scene.overlayStart;
                    if(scene.preview)frame["preview"]={{"x",scene.preview->x},{"y",scene.preview->y},{"w",scene.preview->w},{"h",scene.preview->h}};
                }
                frames.push_back(std::move(frame));
            }std::cout<<frames.dump(2)<<'\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--ui-bindings") {
            auto input=ysm::Json::parse(utf8(argv[2]));ysm::ModelUiBindings bindings(input.at("descriptor"));ysm::MolangContext context;
            context.roaming.emplace(bindings.roamingFields(),ysm::RoamingRole::Producer,ysm::RoamingLimit::LocalOnly);auto frames=ysm::Json::array();
            for(auto const& e:input.at("events")) {
                bool changed=false;if(e.value("op",std::string{})=="action")changed=bindings.act(context,e.at("route").get<std::string>(),e.value("value",0.0),e.value("label",-1));
                if(e.contains("form_snapshot"))
                    bindings.applyScalars(context, {}, e.at("form_snapshot").get<std::map<std::string,float>>());
                frames.push_back({{"changed",changed},{"values",bindings.observe(context)},{"roaming",context.roaming->snapshot()},
                    {"form_snapshot",bindings.snapshotScalars(context)},{"failures",context.failures}});
            }std::cout<<frames.dump(2)<<'\n';
        } else if (argc == 4 && std::wstring(argv[1]) == L"--preview-mesh") {
            auto options = ysm::Json::parse(utf8(argv[3]));
            auto pack = ysm::ModelPack::load(argv[2]);
            ysm::ModelPreview preview(pack);
            auto clip = options.value("clip", std::string("walk"));
            preview.samplePose(clip, 0, pack.selectedTexture);
            ysm::Json frames = ysm::Json::array();
            for (double time : options.at("times").get<std::vector<double>>()) {
                auto mesh = preview.mesh(clip, time, {}, pack.selectedTexture);
                auto const& v = mesh.vertices;
                bool valid = mesh.skin && mesh.skin == pack.currentTexture().uv && !v.empty() && v.size() % 3 == 0;
                for (auto const& pt : v)
                    valid = valid && pt.z >= 0 && pt.z <= 1 && std::isfinite(pt.x) && std::isfinite(pt.y) && std::isfinite(pt.u) && std::isfinite(pt.v)
                        && pt.x >= 0 && pt.x <= 1 && pt.y >= 0 && pt.y <= 1;
                static_assert(sizeof(ysm::PreviewMeshVertex) == 24);
                auto hash = ysm::contentSha256({reinterpret_cast<unsigned char const*>(v.data()), v.size() * sizeof(v[0])});
                frames.push_back({{"time",time},{"valid",valid},{"vertices",v.size()},{"sha256",hash},
                    {"background",!!mesh.background},{"foreground",!!mesh.foreground}});
            }
            std::cout << frames.dump() << '\n';
        } else if ((argc == 4 || argc == 5) && std::wstring(argv[1]) == L"--model-preview") {
            auto options=argc==5?ysm::Json::parse(utf8(argv[4])):ysm::Json::object();auto pack=ysm::ModelPack::load(argv[2]);
            if(options.contains("texture"))pack.selectTexture(options.at("texture").get<std::string>());
            ysm::ModelPreview preview(pack);auto clip=options.value("clip",std::string{});
            ysm::PreviewCamera view{options.value("yaw",15.f),options.value("pitch",-7.f),options.value("zoom",1.f),
                options.value("pan_x",0.f),options.value("pan_y",0.f)};
            preview.samplePose(clip,0,pack.selectedTexture);
            auto time = options.value("time", 1.0);
            ysm::TextureImage image;
            if (options.value("gpu", false)) {
                std::optional<ysm::PreviewPose> live;
                if (options.contains("live_forms")) {
                    ysm::ModelUiBindings bindings(pack.descriptor);
                    ysm::AnimationSampler sampler(pack.bodyAnimations(),pack.mainGeometry);
                    ysm::AnimationControllers controllers(pack.controllers,ysm::ControllerPolicy::SourcePackage);
                    ysm::AnimationEvaluation eval;
                    eval.expressions.physics=std::make_shared<ysm::MolangPhysics>();
                    auto fields=controllers.roamingFields(sampler,false,ysm::RoamingLimit::LocalOnly);
                    for(auto const& field:bindings.roamingFields()) fields.insert(field);
                    eval.expressions.roaming.emplace(std::move(fields),ysm::RoamingRole::Producer,ysm::RoamingLimit::LocalOnly);
                    ysm::HandInput hands;hands.enabled=true;
                    ysm::EntityQueries queries;
                    ysm::EntityQueryInput entity;
                    entity.onGround=true; entity.health=entity.maxHealth=20;
                    auto step=[&](double t) {
                        entity.tick=std::int64_t(t*20);entity.renderTicks=t*20;
                        queries.bind(eval.expressions,entity);
                        eval.expressions.queries["query.life_time"]=t;
                        eval.expressions.queries["ysm.texture_name"]=pack.currentTexture().name;
                        return controllers.samplePlayer(sampler,"idle",t,t,eval,hands,{},"",0);
                    };
                    step(0);
                    for(auto const& form:options.at("live_forms"))
                        if(!bindings.act(eval.expressions,form.at("route"),form.value("value",0.),form.value("label",-1)))
                            throw std::runtime_error("Unknown live form route");
                    auto bones=step(time);
                    live.emplace(ysm::PreviewPose{std::move(bones),eval.expressions.boneVisuals});
                }
                auto mesh = preview.mesh(clip, time, view, pack.selectedTexture,live ? &*live : nullptr);
                // Reproduce the preceding release's missing depth with identical geometry and sampling.
                if (options.value("flatten_depth", false)) for (auto& vertex : mesh.vertices) vertex.z = .5f;
                ysm::PreviewDepthRenderer renderer;
                image = renderer.render(mesh, options.value("side", 224u));
            } else if (options.contains("shoulder_pack")) {
                auto bird = ysm::ModelPack::load(std::filesystem::u8path(options.at("shoulder_pack").get<std::string>()));
                ysm::PreviewPose live{ysm::AnimationSampler(pack.bodyAnimations(), pack.mainGeometry).sample(clip, time), {}};
                ysm::Pose perched;
                ysm::PreviewShoulders parts{&bird.mainGeometry.at("minecraft:geometry").at(0), &perched};
                if (options.value("left", false)) parts.skins[0] = bird.currentTexture().uv.get();
                if (options.value("right", false)) parts.skins[1] = bird.currentTexture().uv.get();
                image = preview.render(clip, time, view, pack.selectedTexture, false, &live, parts);
            } else {
                image = preview.render(clip, time, view, pack.selectedTexture, options.value("decorations", true));
            }
            std::filesystem::path output(argv[3]);std::ofstream out(output,std::ios::binary);
            if(output.extension()==L".rgba")out.write(reinterpret_cast<char*>(image.rgba.data()),image.rgba.size());
            else {out<<"P6\n"<<image.width<<' '<<image.height<<"\n255\n";for(std::size_t i=0;i<image.rgba.size();i+=4)out.write(reinterpret_cast<char*>(image.rgba.data()+i),3);}
            auto report=ysm::Json{{"width",image.width},{"height",image.height}};
            if(options.value("inspect",false))report["evaluation"]=preview.report();
            std::cout<<report.dump()<<'\n';
        } else if (argc == 2 && std::wstring(argv[1]) == L"--particle-coverage") {
            auto result = ysm::Json::array();
            for (auto const& mapping : ysm::javaParticleMappings())
                result.push_back({{"id", mapping.id}, {"coverage", ysm::particleCoverageName(mapping.coverage)},
                    {"native_type", mapping.kind}, {"emitter", mapping.emitter},
                    {"parameterized", mapping.parameters != ysm::ParticleParameters::Simple}});
            std::cout << result.dump(2) << '\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--sound-follow") {
            auto input = ysm::Json::parse(utf8(argv[2]));
            auto authored = input.at("volume").get<float>();
            ysm::PlayingSoundState state{authored, input.at("position").get<ysm::PlayingSoundState::Position>(),
                ysm::PlayingSoundState::scaledVolume(authored, input.at("percent").get<int>())};
            auto result = ysm::Json::array();
            for (auto const& tick : input.at("ticks")) {
                bool moved = false, adjusted = false;
                bool applied = state.follow(tick.at("position").get<ysm::PlayingSoundState::Position>(),
                    tick.at("percent").get<int>(), [&](auto* pos, float const* volume) {
                        moved = pos != nullptr;
                        adjusted = volume != nullptr;
                        return tick.value("ready", true);
                    });
                result.push_back({{"position", state.position}, {"volume", state.volume},
                    {"moved", moved}, {"adjusted", adjusted}, {"applied", applied}});
            }
            std::cout << result.dump() << '\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--sound-selectors") {
            auto result = ysm::Json::array();
            for (auto const& selector : ysm::Json::parse(utf8(argv[2]))) {
                auto sound = ysm::resolveGameSound(selector.get<std::string>());
                result.push_back(sound ? ysm::Json{{"name", sound->name}, {"pitch", sound->pitchMultiplier}} : ysm::Json());
            }
            std::cout << result.dump(2) << '\n';
        } else if (argc == 5 && std::wstring(argv[1]) == L"--model-library") {
            std::cout << modelLibrary(argv[2], argv[3], ysm::Json::parse(utf8(argv[4]))).dump(2) << '\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--portrait-resource") {
            std::filesystem::path path(argv[2]);
            std::ifstream stream(path, std::ios::binary);
            if (!stream) throw std::runtime_error("Cannot read portrait resource");
            std::string bytes{std::istreambuf_iterator<char>(stream), {}};
            if (path.extension() == L".json") {
                auto value = ysm::PreviewShoulders::readJson(bytes);
                std::cout << ysm::Json{{"loaded", value.is_object()}}.dump() << '\n';
            } else {
                auto skin = ysm::PreviewShoulders::readSkin(utf8(argv[2]), bytes);
                std::cout << ysm::Json{{"rgba_bytes", skin.rgba.size()}, {"encoded_capacity", skin.encoded.capacity()}}.dump() << '\n';
            }
        } else if (argc == 3 && std::wstring(argv[1]) == L"--image") {
            std::ifstream stream(std::filesystem::path(argv[2]), std::ios::binary | std::ios::ate);
            if (!stream) throw std::runtime_error("Cannot read image file");
            auto size = stream.tellg();
            if (size < 0 || size > 32 * 1024 * 1024) throw std::runtime_error("Encoded texture too large");
            std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
            stream.seekg(0);
            if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) throw std::runtime_error("Cannot read image file");
            auto image = ysm::decodeTexture(utf8(argv[2]), std::move(bytes), 256 * 1024 * 1024);
            unsigned alphaMin = 255, alphaMax = 0;
            for (std::size_t index = 3; index < image.rgba.size(); index += 4) {
                alphaMin = std::min(alphaMin, static_cast<unsigned>(image.rgba[index]));
                alphaMax = std::max(alphaMax, static_cast<unsigned>(image.rgba[index]));
            }
            auto report = ysm::Json{{"format", image.format}, {"width", image.width}, {"height", image.height},
                {"rgba_bytes", image.rgba.size()}, {"first_pixel", {image.rgba[0], image.rgba[1], image.rgba[2], image.rgba[3]}},
                {"alpha_min", alphaMin}, {"alpha_max", alphaMax}};
            if (image.rgba.size() <= 256) report["pixels"] = image.rgba;
            std::cout << report.dump(2) << '\n';
        } else if (argc == 4 && std::wstring(argv[1]) == L"--select-texture") {
            auto pack = ysm::ModelPack::load(argv[2]);
            if (!pack.selectTexture(utf8(argv[3]))) throw std::runtime_error("Unknown texture: " + utf8(argv[3]));
            std::cout << pack.summary().dump(2) << '\n';
        } else if (argc == 4 && std::wstring(argv[1]) == L"--coded-animation-file") {
            std::cout << coded(document(argv[2]), ysm::Json::parse(utf8(argv[3]))).dump(2) << '\n';
        } else if (argc == 4 && (std::wstring(argv[1]) == L"--vehicle-animation-file"
                || std::wstring(argv[1]) == L"--projectile-animation-file")) {
            auto kind = std::wstring(argv[1]) == L"--vehicle-animation-file" ? ysm::ReplacedModelKind::Vehicle : ysm::ReplacedModelKind::Projectile;
            std::cout << controllers(document(argv[2]), ysm::Json::parse(utf8(argv[3])), false,
                ysm::ControllerPolicy::SourcePackage, false, kind).dump(2) << '\n';
        } else if (argc == 4 && (std::wstring(argv[1]) == L"--target-animations"
                || std::wstring(argv[1]) == L"--target-client-geometry")) {
            auto pack = ysm::ModelPack::load(argv[2]);
            auto input = ysm::Json::parse(utf8(argv[3]));
            auto name = input.at("kind").get<std::string>();
            if (name != "vehicle" && name != "projectile") throw std::runtime_error("Invalid replaced target kind");
            auto kind = name == "vehicle" ? ysm::ReplacedModelKind::Vehicle : ysm::ReplacedModelKind::Projectile;
            auto tags = input.value("entity_tags", std::vector<std::string>{});
            auto* target = pack.findTarget(kind, input.at("entity_type").get<std::string>(), tags, input.value("boat_variant", -1));
            if (!target) throw std::runtime_error("No matching replaced model target");
            if (std::wstring(argv[1]) == L"--target-client-geometry") {
                std::cout << ysm::ClientGeometry::buildTarget(*target).report().dump(2) << '\n';
                return 0;
            }
            auto document = pack.targetAnimations(*target);
            if (!input.contains("geometry")) input["geometry"] = target->geometry;
            if (!input.contains("frames")) input["frames"] = ysm::Json::array();
            auto report = controllers(document, input, false, ysm::ControllerPolicy::SourcePackage, false, kind);
            report["target"] = target->summary();
            if (input.value("include_texture_hash", false))
                report["target"]["texture"]["rgba_sha256"] = ysm::contentSha256(target->texture.uv->rgba);
            if (input.value("include_document", false)) report["document"] = std::move(document);
            std::cout << report.dump(2) << '\n';
        } else if (argc == 4 && std::wstring(argv[1]) == L"--source-player-animation-file") {
            std::cout << controllers(document(argv[2]), ysm::Json::parse(utf8(argv[3])), true, ysm::ControllerPolicy::SourcePackage).dump(2) << '\n';
        } else if (argc == 4 && std::wstring(argv[1]) == L"--first-person-animation-file") {
            std::cout << controllers(document(argv[2]), ysm::Json::parse(utf8(argv[3])), true, ysm::ControllerPolicy::SourcePackage, true).dump(2) << '\n';
        } else if (argc == 4 && std::wstring(argv[1]) == L"--paired-animation-file") {
            std::cout << pairedAnimations(document(argv[2]), ysm::Json::parse(utf8(argv[3]))).dump(2) << '\n';
        } else if (argc == 4 && std::wstring(argv[1]) == L"--source-controller-file") {
            std::cout << controllers(document(argv[2]), ysm::Json::parse(utf8(argv[3])), false, ysm::ControllerPolicy::SourcePackage).dump(2) << '\n';
        } else if (argc == 4 && std::wstring(argv[1]) == L"--player-animation-file") {
            std::cout << controllers(document(argv[2]), ysm::Json::parse(utf8(argv[3])), true).dump(2) << '\n';
        } else if (argc == 4 && std::wstring(argv[1]) == L"--player-animations") {
            auto pack = ysm::ModelPack::load(argv[2]);
            auto data = pack.bodyAnimations();
            data["animation_controllers"] = pack.controllers["animation_controllers"];
            auto input = ysm::Json::parse(utf8(argv[3]));
            if (!input.contains("geometry")) input["geometry"] = pack.mainGeometry;
            input["local_only"] = true;
            if (!input.contains("queries")) input["queries"] = ysm::Json::object();
            if (!input["queries"].contains("ysm.texture_name")) input["queries"]["ysm.texture_name"] = pack.currentTexture().name;
            if (!input.at("frames").empty()) {
                auto& first = input["frames"][0];
                if (!first.contains("queries")) first["queries"] = ysm::Json::object();
                if (!first["queries"].contains("ysm.texture_name")) first["queries"]["ysm.texture_name"] = input["queries"]["ysm.texture_name"];
            }
            std::optional<ysm::ModelUiBindings> profileForms;
            if (input.value("profile_forms", false)) profileForms.emplace(pack.descriptor);
            std::cout << controllers(data, input, true, ysm::ControllerPolicy::SourcePackage,
                false, {}, profileForms ? &*profileForms : nullptr).dump(2) << '\n';
        } else if (argc == 4 && std::wstring(argv[1]) == L"--controller-file") {
            std::cout << controllers(document(argv[2]), ysm::Json::parse(utf8(argv[3]))).dump(2) << '\n';
        } else if (argc == 4 && std::wstring(argv[1]) == L"--hand-animation-file") {
            std::cout << hands(document(argv[2]), ysm::Json::parse(utf8(argv[3]))).dump(2) << '\n';
        } else if (argc == 4 && std::wstring(argv[1]) == L"--hand-animations") {
            std::cout << hands(ysm::ModelPack::load(argv[2]).bodyAnimations(), ysm::Json::parse(utf8(argv[3])),
                ysm::RoamingLimit::LocalOnly).dump(2) << '\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--animation-sets") {
            auto pack = ysm::ModelPack::load(argv[2]);
            std::cout << ysm::Json{{"body", pack.bodyAnimations()}, {"first_person", pack.firstPersonAnimations()}}.dump(2) << '\n';
        } else if (argc == 4 && std::wstring(argv[1]) == L"--hand-masks") {
            auto geometry = ysm::ClientGeometry::build(ysm::ModelPack::load(argv[2]));
            auto scales = ysm::Json::parse(utf8(argv[3]));
            auto fromPose = [&](ysm::Json const& values) {
                ysm::Pose pose;
                for (auto const& [name, scale] : values.items()) pose[name].scale = scale.get<ysm::Triple>();
                return geometry.visibleHandMasks(pose);
            };
            if (scales.is_array()) {
                ysm::HandVisibilityByPass passes;
                passes.update(false, geometry.handDrawMasks);
                passes.update(true, geometry.handDrawMasks);
                ysm::Json samples = ysm::Json::array();
                for (auto const& frame : scales) {
                    passes.update(frame.at("inventory").get<bool>(), fromPose(frame.at("scales")));
                    samples.push_back({passes.get(false, false), passes.get(true, false)});
                }
                std::cout << samples.dump() << '\n';
            } else std::cout << ysm::Json(fromPose(scales)).dump() << '\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--client-geometry") {
            std::cout << ysm::ClientGeometry::build(ysm::ModelPack::load(argv[2])).report().dump(2) << '\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--hand-slot-bindings") {
            auto cases = ysm::Json::parse(utf8(argv[2]));
            ysm::Json results = ysm::Json::array();
            auto matrix = [](ysm::Json const& input) {
                return glm::make_mat4(input.get<std::array<float,16>>().data());
            };
            for (auto const& test : cases) {
                if (!ysm::usesEquipmentSlotBone(test.at("binding").get<std::string>())) {
                    results.push_back(nullptr);
                    continue;
                }
                std::optional<glm::mat4> old;
                if (test.contains("old_parent")) old = matrix(test.at("old_parent"));
                auto child = matrix(test.at("child"));
                auto resolved = ysm::retargetSlotBone(child, old ? &*old : nullptr, matrix(test.at("target")),
                    test.value("identity", false), test.value("same", false)).value_or(child);
                if (test.contains("local")) {
                    auto const& local = test.at("local");
                    auto vector = [&](char const* key) { return glm::make_vec3(local.at(key).get<std::array<float,3>>().data()); };
                    auto rotation = vector("rotation");
                    if (ysm::usesAnimatedLanceRoll(test.value("vanilla_charge", false), rotation)) rotation.z = 0;
                    resolved = ysm::slotRootAtGrip(matrix(test.at("target")), vector("position"), rotation, vector("scale"));
                }
                auto data = glm::value_ptr(resolved);
                results.push_back(std::vector<float>(data, data + 16));
            }
            std::cout << results.dump() << '\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--shoulder-passengers") {
            ysm::ShoulderOccupants seats;
            ysm::Json result = ysm::Json::array();
            for (auto const& frame : ysm::Json::parse(utf8(argv[2]))) {
                auto const& riders = frame.is_object() ? frame.at("riders") : frame;
                auto ids = riders.get<std::vector<std::int64_t>>();
                std::array<std::int64_t, 2> empty{};
                auto restored = frame.is_object() ? frame.value("saved", empty) : empty;
                auto remote = frame.is_object() ? frame.value("remote", empty) : empty;
                seats.update(ids, restored, remote);
                result.push_back(seats.sides);
            }
            std::cout << result.dump() << '\n';
        } else if (argc == 4 && (std::wstring(argv[1]) == L"--shoulder-locators" || std::wstring(argv[1]) == L"--passenger-locators")) {
            auto rig = document(argv[2]);
            auto frames = document(argv[3]);
            ysm::AnimationSampler sampler(ysm::Json::object({{"animations", ysm::Json::object()}}), rig);
            ysm::AnimationEvaluation evaluation;
            sampler.bindFunctions(evaluation);
            auto& context = evaluation.expressions;
            auto initial = context.boneChannels;
            ysm::Json results = ysm::Json::array();
            for (auto const& frame : frames) {
                context.boneChannels = initial;
                for (auto const& [name, channels] : frame.at("bones").items()) {
                    auto& pose = context.boneChannels[name];
                    pose.position = channels.value("position", pose.position);
                    pose.rotation = channels.value("rotation", pose.rotation);
                    pose.scale = channels.value("scale", pose.scale);
                }
                auto delta = frame.value("vehicle_minus_rider", std::array<float, 3>{});
                auto matrix = std::wstring(argv[1]) == L"--passenger-locators"
                    ? ysm::passengerLocator(context, frame.at("seat").get<int>(), frame.value("yaw", 0.f),
                        {delta[0], delta[1], delta[2]}, frame.value("custom_player", true))
                    : ysm::shoulderLocator(context, frame.at("left").get<bool>());
                if (!matrix) { results.push_back(nullptr); continue; }
                ysm::Json values = ysm::Json::array();
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row) values.push_back((*matrix)[column][row]);
                results.push_back(std::move(values));
            }
            std::cout << results.dump() << '\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--render-geometry") {
            std::cout << ysm::ModelPack::load(argv[2]).renderGeometry().dump(2) << '\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--weather-events") {
            ysm::BedrockWeatherState weather;
            ysm::EntityQueries binder;
            auto result = ysm::Json::array();
            for (auto const& event : ysm::Json::parse(utf8(argv[2]))) {
                if (event.value("reset", false)) weather.reset();
                else if (event.contains("join")) {
                    auto const& initial = event.at("join");
                    weather.initialize(initial.value("rain", 0.0), initial.value("lightning", 0.0));
                } else weather.applyEvent(event.at("event").get<int>(), event.value("data", 0));
                auto value = weather.snapshot();
                ysm::MolangContext context;
                ysm::EntityQueryInput input;
                input.setBedrockWeather(value.rain, value.lightning);
                binder.bind(context, input);
                result.push_back({{"rain", value.rain}, {"lightning", value.lightning},
                    {"initialized", value.initialized}, {"events", value.events},
                    {"last_event", value.lastEvent}, {"last_data", value.lastData},
                    {"source", ysm::weatherOriginName(value.origin)}, {"weather", context.read("ysm.weather").number()}});
            }
            std::cout << result.dump(2) << '\n';
        } else if (argc == 3 && (std::wstring(argv[1]) == L"--input-events" || std::wstring(argv[1]) == L"--keyboard-events")) {
            ysm::KeyBindings bindings;
            bool trace = std::wstring(argv[1]) == L"--keyboard-events";
            auto result = ysm::Json::array();
            for (auto const& event : ysm::Json::parse(utf8(argv[2]))) {
                auto action = ysm::KeyAction::None;
                if (event.value("reset", false)) bindings.reset();
                else action = bindings.feed(event.at("key").get<int>(), event.at("down").get<bool>());
                static constexpr char const* names[]{"none", "model_menu", "roulette"};
                if (trace) result.push_back({{"action", names[static_cast<int>(action)]}, {"keys", bindings.glfwKeys()}});
                else result.push_back(names[static_cast<int>(action)]);
            }
            std::cout << result.dump(2) << '\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--native-render-space") {
            auto results = ysm::Json::array();
            for (auto const& value : ysm::Json::parse(utf8(argv[2]))) {
                auto rotation = ysm::nativeProjectileRotation(value.value("previous_yaw", 0.0), value.value("yaw", 0.0),
                    value.value("previous_pitch", 0.0), value.value("pitch", 0.0), value.value("alpha", 1.0),
                    value.value("render_body_yaw", 0.0));
                std::optional<ysm::ProjectileBlockContact> contact;
                if (value.contains("block_contact")) {
                    auto const& item = value.at("block_contact");
                    contact = {item.at("embedded").get<bool>(), item.value("position", std::array<double, 3>{})};
                }
                results.push_back({{"in_ground", ysm::projectileReachedBlock(value.value("native_ground", false),
                    contact, value.value("render_position", std::array<double, 3>{}))},
                    {"position", ysm::nativeAnimationPosition(value.value("position", std::array<double, 3>{}))},
                    {"player_y_offset", ysm::sourcePlayerRenderYOffset(value.value("crouching", false), value.value("world_body", false))},
                    {"yaw", rotation.yaw}, {"pitch", rotation.pitch}});
            }
            std::cout << results.dump(2) << '\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--target-assignments") {
            std::cout << targetAssignments(ysm::Json::parse(utf8(argv[2]))).dump(2) << '\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--roaming-session") {
            std::cout << roamingSession(ysm::Json::parse(utf8(argv[2]))).dump(2) << '\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--roaming-state") {
            std::cout << roamingState(ysm::Json::parse(utf8(argv[2]))).dump(2) << '\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--collapsed-skin-matrix") {
            auto result = ysm::Json::array();
            for (auto const& value : ysm::Json::parse(utf8(argv[2]))) {
                auto matrix = value.get<std::array<float, 16>>();
                result.push_back(ysm::collapsedSkinMatrix(matrix));
            }
            std::cout << result.dump() << '\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--bone-visual-plan") {
            std::vector<ysm::BoneVisual> visuals;
            for (auto const& value : ysm::Json::parse(utf8(argv[2]))) {
                ysm::BoneVisual visual;
                visual.color = value.value("color", visual.color);
                visual.alpha = value.value("alpha", 255);
                visual.glow = value.value("glow", -1);
                visuals.push_back(visual);
            }
            auto result = ysm::Json::array();
            for (auto const& pass : ysm::planBoneVisualPasses(visuals)) result.push_back({{"bones", pass.bones},
                {"color", pass.visual.color}, {"alpha", pass.visual.alpha}, {"glow", pass.visual.glow}});
            std::cout << result.dump(2) << '\n';
        } else if (argc == 3 && std::wstring(argv[1]) == L"--expr-file") {
            auto data = document(argv[2]);
            std::cout << expression(data.at("source").get<std::string>(), data.value("context", ysm::Json::object())).dump(2) << '\n';
        } else if (argc >= 3 && std::wstring(argv[1]) == L"--expr") {
            auto input = argc == 4 ? ysm::Json::parse(utf8(argv[3])) : ysm::Json::object();
            std::cout << expression(utf8(argv[2]), input).dump(2) << '\n';
        } else if (argc >= 5 && std::wstring(argv[1]) == L"--animation-file") {
            auto input = argc == 6 ? ysm::Json::parse(utf8(argv[5])) : ysm::Json::object();
            std::cout << animation(document(argv[2]), utf8(argv[3]), std::stod(std::wstring(argv[4])), input).dump(2) << '\n';
        } else if (argc == 2 || argc == 4 || argc == 5) {
            auto pack = ysm::ModelPack::load(argv[1]);
            if (argc == 2) std::cout << pack.summary().dump(2) << '\n';
            else {
                auto input = argc == 5 ? ysm::Json::parse(utf8(argv[4])) : ysm::Json::object();
                std::cout << animation(pack.bodyAnimations(), utf8(argv[2]), std::stod(std::wstring(argv[3])), input, pack.mainGeometry).dump(2) << '\n';
            }
        } else { std::cerr << "Usage: ysm-inspect <package> [animation seconds [context JSON]] | --expr <source> [context JSON] | --expr-file <json> | --animation-file <json> <state> <seconds> [context JSON]\n"; return 2; }
        return 0;
    } catch (std::exception const& error) {
        std::cerr << "Model rejected: " << error.what() << '\n';
        return 1;
    }
}
