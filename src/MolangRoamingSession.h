#pragma once
#include "MolangRoaming.h"
#include <cstdint>
#include <memory>

namespace ysm {
struct RoamingInstallation {};
using RoamingInstallationId = std::shared_ptr<RoamingInstallation const>;
struct RoamingSnapshot {
    RoamingPayload values;
    RoamingInstallationId installation;
};

// Main-thread local-player session. Render workers return copied deltas; only
// an exact active installation can update a known restore baseline. No disk or
// network authority is inferred from a snapshot or a prepared candidate.
class MolangRoamingSession {
    struct Stamp {};
    struct Retained { RoamingPayload values; std::uint64_t revision; };
    struct Active {
        std::string key;
        RoamingInstallationId installation;
        std::set<std::string> fields;
        bool known;
        RoamingLimit limit;
    };
    std::map<std::string, Retained> retained;
    std::optional<Active> active;
    std::shared_ptr<Stamp const> stamp = std::make_shared<Stamp>();
    std::uint64_t revision = 0;
public:
    MolangRoamingSession() = default;
    MolangRoamingSession(MolangRoamingSession const&) = delete;
    MolangRoamingSession& operator=(MolangRoamingSession const&) = delete;
    MolangRoamingSession(MolangRoamingSession&&) = delete;
    MolangRoamingSession& operator=(MolangRoamingSession&&) = delete;
    class Prepared {
        friend class MolangRoamingSession;
        std::shared_ptr<Stamp const> owner;
        Active candidate;
        RoamingPayload baseline;
        std::uint64_t baselineRevision;
        Prepared(std::shared_ptr<Stamp const> value, Active next, RoamingPayload values, std::uint64_t serial)
            : owner(std::move(value)), candidate(std::move(next)), baseline(std::move(values)), baselineRevision(serial) {}
    public:
        Prepared(Prepared&&) = default;
        Prepared& operator=(Prepared&&) = default;
        Prepared(Prepared const&) = delete;
        Prepared& operator=(Prepared const&) = delete;
        RoamingPayload const& values() const { return baseline; }
        RoamingInstallationId const& installation() const { return candidate.installation; }
        bool hasBaseline() const { return candidate.known; }
    };
    Prepared prepare(std::string key, RoamingInstallationId installation,
        std::set<std::string> fields, bool localOnly) const;
    void activate(Prepared&& prepared);
    bool adopt(RoamingInstallationId const& installation, RoamingPayload const& delta);
    void modelEnded(RoamingInstallationId const& installation);
    void resetConnection();
    // Live values and identity must be sampled together after the owner's
    // render work joins. Reading a snapshot never creates restore eligibility.
    std::optional<RoamingSnapshot> snapshot(std::string const& key,
        RoamingInstallationId const& installation = {}, RoamingPayload const& liveValues = {}) const;
    // Authority adapter seams only. The current client has no authenticated
    // transport, so these are not called by native packet handlers yet.
    bool firstBaseline(std::string const& key, RoamingPayload const& values);
    bool clearBaselines(std::optional<std::string> const& authoritativeKey = {});
    void moveFrom(MolangRoamingSession& source);
    std::size_t namespaces() const { return retained.size(); }
};
}
