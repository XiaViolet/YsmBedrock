#pragma once
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace ysm {
inline constexpr std::string_view roamingPrefix = "variable.roaming.";
enum class RoamingRole { Producer, Consumer };
enum class RoamingLimit { WireCompatible, LocalOnly };
using RoamingPayload = std::map<std::string, float>;

// One installation owns its state by value. Prepared field names never grow
// during evaluation; body/arm exchange copied, named F32 payloads only.
class MolangRoamingState {
    std::set<std::string> layout;
    RoamingRole ownerRole;
    RoamingLimit ownerLimit;
    RoamingPayload values;
    std::set<std::string> dirty;
    void requireRole(RoamingRole role) const;
    void validate(RoamingPayload const& payload) const;
public:
    MolangRoamingState(std::set<std::string> fields, RoamingRole role, RoamingLimit limit = RoamingLimit::WireCompatible);
    static void validateLayout(std::set<std::string> const& fields, RoamingLimit limit = RoamingLimit::WireCompatible);
    std::set<std::string> const& fields() const { return layout; }
    RoamingRole role() const { return ownerRole; }
    RoamingLimit limit() const { return ownerLimit; }
    RoamingPayload const& namedValues() const { return values; }
    float read(std::string const& name) const;
    void write(std::string const& name, float value);
    bool hasDelta() const;
    RoamingPayload snapshot() const;
    // Candidate installation only, before its first program executes. Native
    // lifecycle admission enforces this window and initializes arm afterward.
    void initializeProducer(RoamingPayload const& payload);
    RoamingPayload full();
    RoamingPayload delta();
    void applyFull(RoamingPayload const& payload);
    void applyPatch(RoamingPayload const& payload);
    static void validateNamedPayload(RoamingPayload const& payload, RoamingLimit limit = RoamingLimit::WireCompatible);
};

// The synchronous animation owner calls these while both exact installations
// are idle, under its frame lock. No work or borrowed state survives the call.
void connectRoamingArm(MolangRoamingState const& body, MolangRoamingState& arm);
std::optional<RoamingPayload> projectRoamingArm(MolangRoamingState& body, MolangRoamingState& arm);
}
