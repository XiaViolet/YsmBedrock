#include "ClientGeometry.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cctype>

namespace ysm {
double geometryTextureDimension(Json const& description, char const* key) {
    double value = description.is_object() ? description.value(key, 64.0) : 64.0;
    return value == 0 ? 64.0 : value;
}
static bool locator(std::string const& name, std::string const& prefix) {
    return name.starts_with(prefix) && std::all_of(name.begin() + prefix.size(), name.end(),
        [](unsigned char c) { return c >= '0' && c <= '9'; });
}
static void translate(Json& value, std::array<double, 3> const& offset) {
    if (!value.is_array() || value.size() != 3) return;
    for (int axis = 0; axis < 3; ++axis) value[axis] = value[axis].get<double>() + offset[axis];
}
static void rescaleUV(Json& cube, std::array<double, 2> const& factor) {
    if (!cube.contains("uv")) return;
    auto& uv = cube["uv"];
    if (uv.is_array()) {
        auto size = cube.at("size").get<std::array<double, 3>>();
        double x = std::floor(size[0]), y = std::floor(size[1]), z = std::floor(size[2]);
        double u = uv[0], v = uv[1];
        uv = {{"east", {{"uv", {u, v + z}}, {"uv_size", {z, y}}}},
            {"west", {{"uv", {u + z + x, v + z}}, {"uv_size", {z, y}}}},
            {"north", {{"uv", {u + z, v + z}}, {"uv_size", {x, y}}}},
            {"south", {{"uv", {u + z + x + z, v + z}}, {"uv_size", {x, y}}}},
            {"up", {{"uv", {u + z, v}}, {"uv_size", {x, z}}}},
            {"down", {{"uv", {u + z + x, v + z}}, {"uv_size", {x, -z}}}}};
    }
    for (auto& face : uv) for (auto key : {"uv", "uv_size"}) if (face.contains(key))
        for (int axis = 0; axis < 2; ++axis) face[key][axis] = face[key][axis].get<double>() * factor[axis];
}
bool signedClosedCube(Json const& cube, double boneInflate) {
    auto size = cube.find("size");
    auto uv = cube.find("uv");
    if (size == cube.end() || !size->is_array() || size->size() != 3 || uv == cube.end()) return false;
    auto field = cube.find("inflate");
    double inflate = field != cube.end() && field->is_number() ? field->get<double>() : boneInflate;
    bool signedExtent = false;
    for (auto const& axis : *size) {
        if (!axis.is_number()) return false;
        double extent = axis.get<double>() + 2 * inflate;
        if (extent == 0) return false;
        if (extent < 0) signedExtent = true;
    }
    if (!signedExtent) return false;
    if (uv->is_array()) return uv->size() == 2;
    if (!uv->is_object()) return false;
    for (auto face : {"north", "south", "east", "west", "up", "down"}) {
        auto declaration = uv->find(face);
        if (declaration == uv->end() || !declaration->is_object()) return false;
    }
    return true;
}
static void isolateSignedCubes(ClientGeometry& result) {
    auto& bones = result.document.at("minecraft:geometry").at(0).at("bones");
    Json leaves = Json::array();
    for (auto& bone : bones) {
        auto cubes = bone.find("cubes");
        if (cubes == bone.end() || cubes->empty()) continue;
        auto inflate = bone.value("inflate", 0.0);
        auto signedCube = [inflate](Json const& c) { return signedClosedCube(c, inflate); };
        auto count = static_cast<std::size_t>(std::count_if(cubes->begin(), cubes->end(), signedCube));
        if (!count) continue;
        auto name = bone.at("name").get<std::string>();
        if (count == cubes->size() && !bone.contains("poly_mesh") && !bone.contains("texture_meshes")) {
            result.oneSidedParts.insert(name);
            continue;
        }
        // Keep ordinary faces, flat cards and the authored transform on their
        // existing bone. The child carries only signed closed cuboids. Even two negative axes use this material in Java.
        auto leaf = "__ysm_signed_" + std::to_string(leaves.size());
        Json part = {{"name", leaf}, {"parent", name}, {"pivot", bone.value("pivot", Json::array({0,0,0}))},
            {"cubes", Json::array()}};
        Json remaining = Json::array();
        for (auto& cube : *cubes) (signedCube(cube) ? part["cubes"] : remaining).push_back(std::move(cube));
        *cubes = std::move(remaining);
        for (auto key : {"mirror", "inflate", "neverRender", "material"})
            if (bone.contains(key)) part[key] = bone.at(key);
        auto owner = result.opacityParts.find(name);
        result.opacityParts[leaf] = owner == result.opacityParts.end() ? name : owner->second;
        if (auto arm = result.armParts.find(name); arm != result.armParts.end()) result.armParts[leaf] = arm->second;
        result.oneSidedParts.insert(leaf);
        leaves.push_back(std::move(part));
    }
    for (auto& leaf : leaves) bones.push_back(std::move(leaf));
}
static void isolateOpacityParts(ClientGeometry& result) {
    auto& bones = result.document.at("minecraft:geometry").at(0).at("bones");
    std::set<std::string> parents;
    for (auto const& bone : bones) if (bone.contains("parent")) parents.insert(bone.at("parent"));
    Json leaves = Json::array();
    for (auto& bone : bones) {
        auto name = bone.at("name").get<std::string>();
        if (!result.bodyBones.contains(name) && !result.armBones.contains(name)) continue;
        if (!bone.contains("cubes") && !bone.contains("poly_mesh") && !bone.contains("texture_meshes")) continue;
        if (!parents.contains(name) && !bone.contains("locators") && !result.itemLocators.contains(name)
            && !name.starts_with("__ysm_rightitem") && !name.starts_with("__ysm_leftitem")) {
            result.opacityParts[name] = name;
            continue;
        }
        // Isolate a parent's own faces so a zero-alpha parent still poses
        // its descendants, hand locators and equipment normally.
        auto leaf = "__ysm_surface_" + std::to_string(leaves.size());
        Json part = {{"name", leaf}, {"parent", name}, {"pivot", bone.value("pivot", Json::array({0, 0, 0}))}};
        for (auto key : {"cubes", "poly_mesh", "texture_meshes"}) if (bone.contains(key)) {
            part[key] = std::move(bone.at(key));
            bone.erase(key);
        }
        for (auto key : {"mirror", "inflate", "neverRender", "material"})
            if (bone.contains(key)) part[key] = bone.at(key);
        result.opacityParts[leaf] = name;
        if (auto found = result.armParts.find(name); found != result.armParts.end()) result.armParts[leaf] = found->second;
        leaves.push_back(std::move(part));
    }
    for (auto& leaf : leaves) bones.push_back(std::move(leaf));
}
std::array<unsigned, 2> ClientGeometry::visibleHandMasks(Pose const& pose) const {
    auto masks = handDrawMasks;
    for (std::size_t side = 0; side < masks.size(); ++side) {
        for (unsigned i = 0; i < 8; ++i) {
            if (!(masks[side] & (1u << i))) continue;
            for (auto const& bone : handLocatorChains[side][i]) {
                auto it = pose.find(bone);
                if (it != pose.end() && it->second.scale == Triple{}) {
                    masks[side] &= ~(1u << i);
                    break;
                }
            }
        }
    }
    return masks;
}
ClientGeometry ClientGeometry::build(ModelPack const& pack) {
    ClientGeometry result;
    result.document = pack.renderGeometry();
    auto& geometry = result.document.at("minecraft:geometry").at(0);
    auto& bones = geometry.at("bones");
    std::map<std::string, std::string> renamed;
    std::map<std::string, std::string> nativeNames;
    std::map<std::string, std::string> parents;
    for (auto const& bone : bones) parents.emplace(bone.at("name").get<std::string>(), bone.value("parent", ""));
    for (auto const& [source, native] : {std::pair{"RightHandLocator", "rightitem"}, std::pair{"LeftHandLocator", "leftitem"}}) {
        auto found = std::find_if(bones.begin(), bones.end(), [&](auto const& bone) { return bone.at("name") == source; });
        if (found != bones.end()) {
            auto name = found->at("name").get<std::string>();
            nativeNames[name] = native;
            result.itemLocators[native] = name;
            auto& mask = result.handDrawMasks[native == std::string_view("leftitem") ? 1 : 0];
            mask = 1;
            // 2.6.5 enables copies 2 through 8 only when the main locator exists.
            for (unsigned copy = 2; copy <= 8; ++copy) {
                auto extra = name + std::to_string(copy);
                if (std::ranges::none_of(bones, [&](auto const& bone) { return bone.at("name") == extra; })) continue;
                nativeNames[extra] = std::string("__ysm_") + native + std::to_string(copy);
                mask |= 1u << (copy - 1);
            }
            auto& chains = result.handLocatorChains[native == std::string_view("leftitem") ? 1 : 0];
            for (unsigned i = 0; i < chains.size(); ++i) {
                if (!(mask & (1u << i))) continue;
                auto bone = i == 0 ? name : name + std::to_string(i + 1);
                while (!bone.empty()) {
                    if (chains[i].size() >= parents.size()) throw std::runtime_error("Cyclic hand locator hierarchy");
                    chains[i].push_back(bone);
                    bone = parents.at(bone);
                }
            }
        }
    }
    auto elytra = std::find_if(bones.begin(), bones.end(), [](auto const& bone) { return bone.at("name") == "ElytraLocator"; });
    if (elytra == bones.end()) elytra = std::find_if(bones.begin(), bones.end(),
        [](auto const& bone) { return locator(bone.at("name"), "ElytraLocator"); });
    if (elytra != bones.end()) nativeNames[elytra->at("name").get<std::string>()] = "__ysm_elytra";
    // The humanoid fallback item pass also needs a native head part. Use an
    // empty child carrier so native head adjustments cannot resize or rotate
    // the authored head mesh a second time.
    auto head = std::find_if(bones.begin(), bones.end(), [](auto const& bone) { return bone.at("name") == "Head"; });
    if (head == bones.end()) head = std::find_if(bones.begin(), bones.end(), [](auto const& bone) { return locator(bone.at("name"), "Head"); });
    if (head != bones.end()) result.headLocator = head->at("name").get<std::string>();
    auto headPivot = head == bones.end() ? Json::array({0, 24, 0}) : head->value("pivot", Json::array({0, 0, 0}));
    for (std::size_t index = 0; index < bones.size(); ++index) {
        auto original = bones[index].at("name").get<std::string>();
        auto native = nativeNames.contains(original) ? nativeNames.at(original) : "__ysm_body_" + std::to_string(index);
        renamed[original] = native;
        result.bodyBones[native] = original;
        if (locator(original, "RightArm")) result.meleeSpearArms.insert(native);
        if (!bones[index].contains("parent")) result.bodyRoot = native;
    }
    for (auto& bone : bones) {
        bone["name"] = renamed.at(bone["name"].get<std::string>());
        if (bone.contains("parent")) bone["parent"] = renamed.at(bone["parent"].get<std::string>());
    }
    bones.push_back(Json{{"name", "head"}, {"parent", result.headLocator.empty() ? result.bodyRoot : renamed.at(result.headLocator)},
        {"pivot", std::move(headPivot)}});
    auto const& arm = pack.armGeometry.at("minecraft:geometry").at(0);
    auto const& armBones = arm.at("bones");
    std::map<std::string, Json const*> lookup;
    for (auto const& bone : armBones) lookup[bone.at("name").get<std::string>()] = &bone;
    auto& bodyDescription = geometry["description"];
    auto armDescription = arm.value("description", Json::object());
    bodyDescription["texture_width"] = geometryTextureDimension(bodyDescription, "texture_width");
    bodyDescription["texture_height"] = geometryTextureDimension(bodyDescription, "texture_height");
    std::array<double, 2> uvFactor{
        geometryTextureDimension(bodyDescription, "texture_width") / geometryTextureDimension(armDescription, "texture_width"),
        geometryTextureDimension(bodyDescription, "texture_height") / geometryTextureDimension(armDescription, "texture_height")};
    for (auto const& part : {std::string("RightArm"), std::string("LeftArm"), std::string("Background")}) {
        std::vector<std::string> roots;
        for (auto const& bone : armBones) if (bone.at("name") == part) roots.push_back(bone.at("name"));
        if (roots.empty()) continue;
        auto carrier = part == "RightArm" ? "rightarm" : part == "LeftArm" ? "leftarm" : "__ysm_fp_background";
        result.armCarriers.insert(carrier);
        std::array<double, 3> anchor = part == "RightArm" ? std::array<double, 3>{-5,22,0}
            : part == "LeftArm" ? std::array<double, 3>{5,22,0} : std::array<double, 3>{0,0,0};
        auto origin = lookup.at(roots.front())->value("pivot", Json::array({0,0,0})).get<std::array<double,3>>();
        std::array<double, 3> offset{};
        if (part != "Background") for (int axis = 0; axis < 3; ++axis) offset[axis] = anchor[axis] - origin[axis];
        std::set<std::string> selected;
        for (auto const& [name, bone] : lookup) {
            auto current = name;
            while (!current.empty()) {
                if (std::find(roots.begin(), roots.end(), current) != roots.end()) { selected.insert(name); break; }
                current = lookup.at(current)->value("parent", "");
            }
        }
        std::map<std::string, std::string> armRenamed;
        for (auto const& name : selected) {
            auto native = std::string("__ysm_fp_") + carrier + "_" + std::to_string(result.armBones.size());
            armRenamed[name] = native;
            result.armBones[native] = name;
            result.armParts[native] = carrier;
        }
        bones.push_back(Json{{"name", carrier}, {"pivot", anchor}});
        if (part != "Background") {
            auto item = part == "RightArm" ? "rightitem" : "leftitem";
            auto native = std::string("__ysm_fp_") + item;
            auto wrist = anchor;
            wrist[0] += part == "RightArm" ? -1 : 1;
            wrist[1] -= 7;
            wrist[2] += 1;
            if (!result.itemLocators.contains(item))
                bones.push_back(Json{{"name", item}, {"parent", carrier}, {"pivot", wrist}});
            bones.push_back(Json{{"name", native}, {"parent", carrier}, {"pivot", wrist}});
            result.firstPersonItems[native] = item;
        }
        for (auto const& source : armBones) {
            auto name = source.at("name").get<std::string>();
            if (!selected.contains(name)) continue;
            auto bone = source;
            bone["name"] = armRenamed.at(name);
            auto parent = bone.value("parent", "");
            bone["parent"] = selected.contains(parent) ? armRenamed.at(parent) : carrier;
            if (bone.contains("pivot")) translate(bone["pivot"], offset);
            if (bone.contains("cubes")) for (auto& cube : bone["cubes"]) {
                translate(cube["origin"], offset);
                if (cube.contains("pivot")) translate(cube["pivot"], offset);
                rescaleUV(cube, uvFactor);
            }
            if (auto mesh = bone.find("poly_mesh"); mesh != bone.end() && mesh->is_object()) {
                if (auto positions = mesh->find("positions"); positions != mesh->end())
                    for (auto& position : *positions) translate(position, offset);
                if (!mesh->value("normalized_uvs", false))
                    if (auto uvs = mesh->find("uvs"); uvs != mesh->end())
                        for (auto& uv : *uvs) if (uv.is_array() && uv.size() == 2)
                            for (int axis = 0; axis < 2; ++axis) uv[axis] = uv[axis].get<double>() * uvFactor[axis];
            }
            bones.push_back(std::move(bone));
        }
    }
    if (pack.boneTransparency) isolateOpacityParts(result);
    isolateSignedCubes(result);
    // Even a pack without a first-person arm can use the native third-person
    // shoulder as a pose source. This carrier has no mesh and no FP binding.
    if (!result.meleeSpearArms.empty() && !result.armCarriers.contains("rightarm"))
        bones.push_back(Json{{"name", "rightarm"}, {"pivot", {-5,22,0}}});
    return result;
}
ClientGeometry ClientGeometry::buildTarget(ReplacedModelTarget const& target) {
    ClientGeometry result;
    result.document = target.geometry;
    auto& geometry = result.document.at("minecraft:geometry").at(0);
    auto& bones = geometry.at("bones");
    std::map<std::string, std::string> renamed;
    for (std::size_t i = 0; i < bones.size(); ++i) {
        auto original = bones[i].at("name").get<std::string>();
        auto native = "__ysm_target_" + std::to_string(i);
        renamed[original] = native;
        result.bodyBones[native] = original;
    }
    for (auto& bone : bones) {
        bone["name"] = renamed.at(bone.at("name").get<std::string>());
        if (bone.contains("parent")) bone["parent"] = renamed.at(bone.at("parent").get<std::string>());
    }
    if (target.kind == ReplacedModelKind::Projectile) {
        // GeoProjectilesRenderer rotates the whole authored model about the
        // entity origin, Y first then Z. Separate carriers preserve that order
        // and every authored root's pivot, rotation and animation channels.
        result.projectileYawCarrier = "__ysm_projectile_yaw";
        result.projectilePitchCarrier = "__ysm_projectile_pitch";
        for (auto& bone : bones) if (!bone.contains("parent")) bone["parent"] = result.projectilePitchCarrier;
        bones.push_back({{"name", result.projectileYawCarrier}, {"pivot", {0, 0, 0}}});
        bones.push_back({{"name", result.projectilePitchCarrier}, {"parent", result.projectileYawCarrier},
            {"pivot", {0, 0, 0}}});
    }
    auto& description = geometry["description"];
    for (auto key : {"texture_width", "texture_height"})
        if (!description.contains(key) || description.at(key) == 0) description[key] = 64;
    // Replaced entities own their authored origins and dimensions. Player arm,
    // head, equipment and model-size carriers must not enter this geometry.
    if (target.boneTransparency) isolateOpacityParts(result);
    isolateSignedCubes(result);
    return result;
}
Json ClientGeometry::report() const {
    return {{"geometry", document}, {"body_bones", bodyBones}, {"arm_bones", armBones},
        {"arm_parts", armParts}, {"arm_carriers", armCarriers}, {"item_locators", itemLocators}, {"hand_draw_masks", handDrawMasks}, {"opacity_parts", opacityParts}, {"one_sided_parts", oneSidedParts},
        {"first_person_items", firstPersonItems}, {"melee_spear_arms", meleeSpearArms},
        {"head_locator", headLocator}, {"body_root", bodyRoot},
        {"projectile_yaw_carrier", projectileYawCarrier}, {"projectile_pitch_carrier", projectilePitchCarrier}};
}
}
