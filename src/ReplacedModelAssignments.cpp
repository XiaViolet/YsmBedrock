#include "ReplacedModelAssignments.h"
#include <utility>

namespace ysm {
Json SavedReplacedAssignment::encode() const {
    return {{"kind", replacedModelKindName(kind)}, {"owner", owner}, {"owner_model_id", source}, {"roaming", roaming}};
}
SavedReplacedAssignment SavedReplacedAssignment::decode(Json const& input) {
    auto kind = input.at("kind").get<std::string>();
    if (kind != "vehicle" && kind != "projectile") throw std::invalid_argument("Unknown saved replacement kind");
    auto record = SavedReplacedAssignment{kind == "vehicle" ? ReplacedModelKind::Vehicle : ReplacedModelKind::Projectile,
        input.at("owner").get<std::int64_t>(), input.at("owner_model_id").get<std::string>(), input.at("roaming").get<RoamingPayload>()};
    MolangRoamingState::validateNamedPayload(record.roaming, RoamingLimit::LocalOnly);
    return record;
}
void ReplacedModelAssignments::assign(Key key, std::shared_ptr<ReplacedModelAssignment const> value) {
    auto& previous = assignments[key];
    if (previous == value) return;
    previous = std::move(value);
    auto live = serverRoaming.find({previous->owner, previous->savedSource});
    if (live != serverRoaming.end()) savedRoaming[key] = live->second;
    else savedRoaming.erase(key);
    changed.insert(key);
}
std::set<ReplacedModelAssignments::Key> ReplacedModelAssignments::takeChanged() { return std::exchange(changed, {}); }
std::optional<SavedReplacedAssignment> ReplacedModelAssignments::saved(Key key) const {
    auto value = find(key.first, key.second);
    if (!value) return {};
    auto live = savedRoaming.find(key);
    return SavedReplacedAssignment{key.first, value->owner, value->savedSource,
        live == savedRoaming.end() ? value->roaming : *live->second};
}
void ReplacedModelAssignments::restore(std::int64_t entity, SavedReplacedAssignment const& record,
    std::shared_ptr<ModelPack const> pack) {
    Key key{record.kind, entity};
    // The release persists the model ID and roaming on the server entity. The
    // client's new arrow instance samples the shooter's current hand on spawn.
    auto observed = find(record.kind, entity);
    std::string item = observed && observed->owner == record.owner ? observed->shootItemId : std::string{};
    if (pack && pack->librarySource != record.source) pack.reset();
    assignments[key] = std::make_shared<ReplacedModelAssignment const>(ReplacedModelAssignment{
        record.owner, std::move(pack), record.roaming, ++serial, std::move(item), record.source, true});
    // Deserialization builds a new server-side map, detached from the player's.
    savedRoaming.erase(key);
    changed.erase(key);
}
void ReplacedModelAssignments::restoredClientHand(std::int64_t entity, std::int64_t owner, std::string const& hand) {
    auto found = assignments.find({ReplacedModelKind::Projectile, entity});
    if (found == assignments.end() || !found->second->restored || found->second->owner != owner
        || hand.empty() || hand == found->second->shootItemId) return;
    auto next = *found->second;
    next.shootItemId = hand;
    found->second = std::make_shared<ReplacedModelAssignment const>(std::move(next));
}
void ReplacedModelAssignments::loadedSource(std::string const& source, std::shared_ptr<ModelPack const> pack) {
    if (!pack || source != pack->librarySource) return;
    for (auto& [key, assignment] : assignments) {
        if (assignment->pack || assignment->savedSource != source) continue;
        auto next = *assignment;
        next.pack = pack;
        next.revision = ++serial;
        assignment = std::make_shared<ReplacedModelAssignment const>(std::move(next));
    }
}
void ReplacedModelAssignments::updateOwner(std::int64_t owner, std::shared_ptr<ModelPack const> pack,
    RoamingPayload const& roaming) {
    MolangRoamingState::validateNamedPayload(roaming, RoamingLimit::LocalOnly);
    if (auto old = owners.find(owner); old != owners.end()) {
        auto const& previous = *old->second;
        bool samePack = (!pack && !previous.pack) || (pack && previous.pack
            && pack->localContentKey == previous.pack->localContentKey);
        if (samePack && (!pack || previous.savedSource == pack->librarySource) && previous.roaming == roaming) return;
    }
    auto source = pack ? pack->librarySource : std::string{};
    if (!source.empty()) {
        // In 2.6.5 the live server capability shares the owner's table while
        // the client renderer keeps the snapshot sent when tracking started.
        auto& live = serverRoaming[{owner,source}];
        if (!live) live = std::make_shared<RoamingPayload>(roaming);
        else if (*live != roaming) {
            *live = roaming;
            for (auto const& [key, bound] : savedRoaming)
                if (bound == live) changed.insert(key);
        }
    }
    auto next = std::make_shared<ReplacedModelAssignment const>(ReplacedModelAssignment{owner, std::move(pack), roaming, ++serial, {}, std::move(source)});
    owners[owner] = next;
    // Model/roaming updates reach the current first-passenger vehicle. Entities
    // from earlier rides and previously fired projectiles retain their snapshot.
    if (auto mount = mounts.find(owner); mount != mounts.end() && mount->second.first)
        assign({ReplacedModelKind::Vehicle, mount->second.vehicle}, std::move(next));
}
void ReplacedModelAssignments::updatePeer(std::int64_t owner, std::shared_ptr<ModelPack const> pack,
    RoamingPayload const& roaming, std::optional<std::pair<std::int64_t, std::int64_t>> mount) {
    // A simultaneous model change should affect only the currently ridden vehicle.
    if (mount) mounted(mount->first, owner, mount->second);
    else dismounted(owner);
    updateOwner(owner, std::move(pack), roaming);
}
void ReplacedModelAssignments::removeOwner(std::int64_t owner) {
    owners.erase(owner);
    mounts.erase(owner);
    std::erase_if(serverRoaming, [owner](auto const& entry) { return entry.first.first == owner; });
}
bool ReplacedModelAssignments::canPresent(ReplacedModelAssignment const& assignment) const {
    // Local entities keep their saved model after the owner's model is
    // disabled. Server assets stay subject to the visible, approved peer.
    return assignment.pack && (!assignment.savedSource.starts_with("server:") || ownerActive(assignment.owner));
}
void ReplacedModelAssignments::mounted(std::int64_t vehicle, std::int64_t rider, std::int64_t firstPassenger) {
    for (auto& [passenger, mount] : mounts)
        if (mount.vehicle == vehicle) mount.first = passenger == firstPassenger;
    mounts.insert_or_assign(rider, Mount{vehicle, rider == firstPassenger});
    auto owner = owners.find(rider);
    if (rider == firstPassenger && owner != owners.end())
        assign({ReplacedModelKind::Vehicle, vehicle}, owner->second);
}
void ReplacedModelAssignments::dismounted(std::int64_t rider) { mounts.erase(rider); }
void ReplacedModelAssignments::projectileOwnerSet(std::int64_t projectile, std::int64_t owner) {
    auto const current = owners.find(owner);
    if (current == owners.end()) return;
    auto const key = Key{ReplacedModelKind::Projectile, projectile};
    auto previous = assignments.find(key);
    if (previous == assignments.end() || previous->second->owner != owner)
        assign(key, current->second);
}
void ReplacedModelAssignments::projectileClientOwnerSet(std::int64_t projectile, std::int64_t owner, std::string shootItemId) {
    projectileOwnerSet(projectile, owner);
    auto assignment = assignments.find({ReplacedModelKind::Projectile, projectile});
    if (assignment == assignments.end() || assignment->second->owner != owner
        || !assignment->second->shootItemId.empty() || shootItemId.empty()) return;
    // The client owner event can arrive after a model change. Keep the
    // launch's earlier model and roaming snapshot when filling in this item.
    auto captured = *assignment->second;
    captured.shootItemId = std::move(shootItemId);
    assignment->second = std::make_shared<ReplacedModelAssignment const>(std::move(captured));
}
std::shared_ptr<ReplacedModelAssignment const> ReplacedModelAssignments::find(ReplacedModelKind kind, std::int64_t entity) const {
    auto found = assignments.find({kind, entity});
    return found == assignments.end() ? nullptr : found->second;
}
void ReplacedModelAssignments::forget(ReplacedModelKind kind, std::int64_t entity) {
    assignments.erase({kind, entity});
    savedRoaming.erase({kind, entity});
    if (kind == ReplacedModelKind::Vehicle)
        std::erase_if(mounts, [&](auto const& entry) { return entry.second.vehicle == entity; });
}
Json ReplacedModelAssignments::report() const {
    auto result = Json{{"owners", owners.size()}, {"mounts", mounts.size()}, {"assignments", Json::array()}};
    for (auto const& [key, value] : assignments)
        result["assignments"].push_back({{"kind", replacedModelKindName(key.first)}, {"entity", key.second},
            {"owner", value->owner}, {"pack", value->pack ? Json(value->pack->localContentKey) : Json()},
            {"saved_source", value->savedSource}, {"revision", value->revision}, {"roaming", value->roaming}, {"shoot_item_id", value->shootItemId}});
    return result;
}
void ReplacedModelAssignments::reset() {
    owners.clear(); mounts.clear(); assignments.clear(); changed.clear(); serverRoaming.clear(); savedRoaming.clear();
}
}
