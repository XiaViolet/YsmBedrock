#pragma once
#include "ModelPack.h"
#include "MolangRoaming.h"
#include <cstdint>
#include <optional>
#include <set>

namespace ysm {
// Connection-scoped, authoritative assignments. A player's current selection
// is never consulted when resolving an already assigned entity.
struct ReplacedModelAssignment {
    std::int64_t owner = 0;
    std::shared_ptr<ModelPack const> pack;
    RoamingPayload roaming;
    std::uint64_t revision = 0;
    std::string shootItemId;
    std::string savedSource;
    bool restored = false;
};
struct SavedReplacedAssignment {
    ReplacedModelKind kind;
    std::int64_t owner;
    std::string source;
    RoamingPayload roaming;
    Json encode() const;
    static SavedReplacedAssignment decode(Json const& input);
};
class ReplacedModelAssignments {
public:
    using Key = std::pair<ReplacedModelKind, std::int64_t>;
private:
    struct Mount { std::int64_t vehicle; bool first; };
    std::set<Key> changed;
    using OwnerSource = std::pair<std::int64_t, std::string>;
    std::map<OwnerSource, std::shared_ptr<RoamingPayload>> serverRoaming;
    std::map<Key, std::shared_ptr<RoamingPayload>> savedRoaming;
    void assign(Key key, std::shared_ptr<ReplacedModelAssignment const> value);
    std::map<std::int64_t, std::shared_ptr<ReplacedModelAssignment const>> owners;
    std::map<std::int64_t, Mount> mounts;
    std::map<Key, std::shared_ptr<ReplacedModelAssignment const>> assignments;
    std::uint64_t serial = 0;
public:
    void updateOwner(std::int64_t owner, std::shared_ptr<ModelPack const> pack, RoamingPayload const& roaming);
    void updatePeer(std::int64_t owner, std::shared_ptr<ModelPack const> pack, RoamingPayload const& roaming,
        std::optional<std::pair<std::int64_t, std::int64_t>> mount);
    void removeOwner(std::int64_t owner);
    bool ownerActive(std::int64_t owner) const { return owners.contains(owner); }
    bool canPresent(ReplacedModelAssignment const& assignment) const;
    void mounted(std::int64_t vehicle, std::int64_t rider, std::int64_t firstPassenger);
    void dismounted(std::int64_t rider);
    void projectileOwnerSet(std::int64_t projectile, std::int64_t owner);
    void projectileClientOwnerSet(std::int64_t projectile, std::int64_t owner, std::string shootItemId);
    std::shared_ptr<ReplacedModelAssignment const> find(ReplacedModelKind kind, std::int64_t entity) const;
    void forget(ReplacedModelKind kind, std::int64_t entity);
    std::set<Key> takeChanged();
    std::optional<SavedReplacedAssignment> saved(Key key) const;
    void restore(std::int64_t entity, SavedReplacedAssignment const& record, std::shared_ptr<ModelPack const> pack);
    void restoredClientHand(std::int64_t entity, std::int64_t owner, std::string const& hand);
    void loadedSource(std::string const& source, std::shared_ptr<ModelPack const> pack);
    Json report() const;
    void reset();
};
}
