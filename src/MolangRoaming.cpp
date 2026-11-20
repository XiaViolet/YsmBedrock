#include "MolangRoaming.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ysm {
void MolangRoamingState::validateLayout(std::set<std::string> const& fields, RoamingLimit limit) {
    // The YSM wire stays at 64 fields. Local-only legacy source packages may
    // retain larger layouts without truncating their author settings.
    auto maximum = limit == RoamingLimit::LocalOnly ? 1024u : 64u;
    if (fields.size() > maximum) throw std::runtime_error("Roaming layout exceeds " + std::to_string(maximum) + " fields");
    for (auto const& name : fields) {
        auto first = [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; };
        if (name.empty() || name.size() > 32 || !first(name.front())
            || !std::all_of(name.begin(), name.end(), [&](char c) { return first(c) || (c >= '0' && c <= '9'); }))
            throw std::runtime_error("Invalid Roaming field name: " + name);
    }
}
MolangRoamingState::MolangRoamingState(std::set<std::string> fields, RoamingRole role, RoamingLimit limit)
    : layout(std::move(fields)), ownerRole(role), ownerLimit(limit) { validateLayout(layout, limit); }
void MolangRoamingState::requireRole(RoamingRole role) const {
    if (ownerRole != role) throw std::runtime_error("Invalid Roaming producer/consumer operation");
}
void MolangRoamingState::validate(RoamingPayload const& payload) const {
    for (auto const& [name, value] : payload)
        if (!layout.contains(name) || !std::isfinite(value)) throw std::runtime_error("Invalid Roaming payload field: " + name);
}
float MolangRoamingState::read(std::string const& name) const {
    if (!layout.contains(name)) throw std::runtime_error("Undeclared Roaming field: " + name);
    auto found = values.find(name);
    return found == values.end() ? 0.0f : found->second;
}
void MolangRoamingState::write(std::string const& name, float value) {
    if (!layout.contains(name) || !std::isfinite(value)) throw std::runtime_error("Invalid Roaming payload field: " + name);
    values[name] = value;
    // A legal same-value assignment is a new write, even after delta().
    if (ownerRole == RoamingRole::Producer) dirty.insert(name);
}
bool MolangRoamingState::hasDelta() const { requireRole(RoamingRole::Producer); return !dirty.empty(); }
RoamingPayload MolangRoamingState::snapshot() const { requireRole(RoamingRole::Producer); return values; }
void MolangRoamingState::initializeProducer(RoamingPayload const& payload) {
    requireRole(RoamingRole::Producer);
    validate(payload);
    auto next = payload;
    values.swap(next);
    dirty.clear();
}
void MolangRoamingState::validateNamedPayload(RoamingPayload const& payload, RoamingLimit limit) {
    std::set<std::string> fields;
    for (auto const& [name, value] : payload) {
        fields.insert(name);
        if (!std::isfinite(value)) throw std::runtime_error("Non-finite Roaming payload");
    }
    validateLayout(fields, limit);
}
RoamingPayload MolangRoamingState::full() {
    auto result = snapshot();
    dirty.clear();
    return result;
}
RoamingPayload MolangRoamingState::delta() {
    requireRole(RoamingRole::Producer);
    RoamingPayload result;
    for (auto const& name : dirty) result.emplace(name, values.at(name));
    dirty.clear();
    return result;
}
void MolangRoamingState::applyFull(RoamingPayload const& payload) {
    requireRole(RoamingRole::Consumer);
    validate(payload);
    auto next = payload;
    values.swap(next);
}
void MolangRoamingState::applyPatch(RoamingPayload const& payload) {
    requireRole(RoamingRole::Consumer);
    validate(payload);
    auto next = values;
    for (auto const& [name, value] : payload) next[name] = value;
    values.swap(next);
}
static RoamingPayload intersection(RoamingPayload const& payload, MolangRoamingState const& arm) {
    RoamingPayload result;
    for (auto const& [name, value] : payload) if (arm.fields().contains(name)) result.emplace(name, value);
    return result;
}
void connectRoamingArm(MolangRoamingState const& body, MolangRoamingState& arm) {
    auto fields = body.fields();
    fields.insert(arm.fields().begin(), arm.fields().end());
    auto limit = body.limit() == RoamingLimit::LocalOnly && arm.limit() == RoamingLimit::LocalOnly
        ? RoamingLimit::LocalOnly : RoamingLimit::WireCompatible;
    MolangRoamingState::validateLayout(fields, limit);
    arm.applyFull(intersection(body.snapshot(), arm));
}
std::optional<RoamingPayload> projectRoamingArm(MolangRoamingState& body, MolangRoamingState& arm) {
    if (!body.hasDelta()) return {};
    if (arm.role() != RoamingRole::Consumer) throw std::runtime_error("Roaming arm must be a consumer");
    auto payload = body.delta();
    arm.applyPatch(intersection(payload, arm));
    // Preserve the complete producer payload, including body-only fields.
    return payload;
}
}
