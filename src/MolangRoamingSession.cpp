#include "MolangRoamingSession.h"
#include <stdexcept>
#include <utility>

namespace ysm {
MolangRoamingSession::Prepared MolangRoamingSession::prepare(std::string key,
    RoamingInstallationId installation, std::set<std::string> fields, bool localOnly) const {
    if (key.empty() || !installation) throw std::runtime_error("Roaming preparation needs a model and installation");
    auto found = retained.find(key);
    RoamingPayload values = found == retained.end() ? RoamingPayload{} : found->second.values;
    // Validate against the complete body layout before changing either session
    // or installed state. An incompatible candidate leaves retention intact.
    auto limit = localOnly ? RoamingLimit::LocalOnly : RoamingLimit::WireCompatible;
    MolangRoamingState candidate(fields, RoamingRole::Producer, limit);
    candidate.initializeProducer(values);
    return Prepared(stamp, {std::move(key), std::move(installation), std::move(fields),
        found != retained.end() || localOnly, limit}, std::move(values), found == retained.end() ? 0 : found->second.revision);
}
void MolangRoamingSession::activate(Prepared&& prepared) {
    auto owner = std::exchange(prepared.owner, {});
    if (!owner || owner != stamp) throw std::runtime_error("Roaming candidate belongs to an ended connection");
    auto found = retained.find(prepared.candidate.key);
    auto serial = found == retained.end() ? 0 : found->second.revision;
    if (serial != prepared.baselineRevision) throw std::runtime_error("Roaming restore baseline changed during preparation");
    if (prepared.candidate.known && found == retained.end())
        retained.emplace(prepared.candidate.key, Retained{std::move(prepared.baseline), ++revision});
    active = std::move(prepared.candidate);
}
bool MolangRoamingSession::adopt(RoamingInstallationId const& installation, RoamingPayload const& delta) {
    if (!active || !active->known || !installation || active->installation != installation) return false;
    MolangRoamingState validator(active->fields, RoamingRole::Producer, active->limit);
    validator.initializeProducer(delta);
    if (delta.empty()) return true;
    auto& stored = retained.at(active->key);
    auto next = stored.values;
    for (auto const& [name, value] : delta) next[name] = value;
    stored.values.swap(next);
    stored.revision = ++revision;
    return true;
}
void MolangRoamingSession::modelEnded(RoamingInstallationId const& installation) {
    if (active && installation && active->installation == installation) active.reset();
}
void MolangRoamingSession::resetConnection() {
    stamp = std::make_shared<Stamp>();
    active.reset();
    retained.clear();
}
std::optional<RoamingSnapshot> MolangRoamingSession::snapshot(std::string const& key,
    RoamingInstallationId const& installation, RoamingPayload const& liveValues) const {
    auto found = retained.find(key);
    if (found == retained.end()) return {};
    if (active && active->known && active->key == key && installation && active->installation == installation) {
        MolangRoamingState validator(active->fields, RoamingRole::Producer, active->limit);
        validator.initializeProducer(liveValues);
        return RoamingSnapshot{liveValues, installation};
    }
    return RoamingSnapshot{found->second.values, {}};
}
bool MolangRoamingSession::firstBaseline(std::string const& key, RoamingPayload const& values) {
    MolangRoamingState::validateNamedPayload(values);
    if (key.empty()) throw std::runtime_error("Roaming baseline needs a model");
    if (retained.contains(key)) return false; // Ordinary self echoes keep local continuity.
    retained.emplace(key, Retained{values, ++revision});
    if (!active || active->key != key) return false;
    active->installation.reset();
    active->known = false;
    return true;
}
bool MolangRoamingSession::clearBaselines(std::optional<std::string> const& authoritativeKey) {
    if (authoritativeKey && authoritativeKey->empty()) throw std::runtime_error("Roaming baseline needs a model");
    bool reload = active.has_value();
    retained.clear();
    if (authoritativeKey) retained.emplace(*authoritativeKey, Retained{{}, ++revision});
    if (active) { active->installation.reset(); active->known = false; }
    stamp = std::make_shared<Stamp>();
    return reload;
}
void MolangRoamingSession::moveFrom(MolangRoamingSession& source) {
    if (&source == this) return;
    for (auto& [key, value] : source.retained)
        retained.insert_or_assign(key, Retained{std::move(value.values), ++revision});
    active.reset();
    stamp = std::make_shared<Stamp>();
    source.resetConnection();
}
}
