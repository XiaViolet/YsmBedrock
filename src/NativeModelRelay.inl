#pragma once

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace ysm::detail {

// Cache only immutable model resources. No Player, runtime controller or native binding
// survives an actor removal. Release unused resources even when the room stays open.
template<class Resources>
class RelayWarmModels {
    using Clock = std::chrono::steady_clock;
public:
    using Key = std::pair<std::string, std::string>;
    void keep(Key key, Resources resources, Clock::time_point now) {
        if (!entries.contains(key) && entries.size() >= 2) {
            auto oldest = std::min_element(entries.begin(), entries.end(), [](auto const& a, auto const& b) {
                return a.second.expires < b.second.expires;
            });
            entries.erase(oldest);
        }
        entries[std::move(key)] = {std::move(resources), now + std::chrono::seconds(30)};
    }
    Resources find(Key const& key) const {
        auto it = entries.find(key);
        return it == entries.end() ? Resources{} : it->second.resources;
    }
    template<class Allowed> void prune(Clock::time_point now, Allowed allowed) {
        std::erase_if(entries, [&](auto const& entry) {
            return now >= entry.second.expires || !allowed(entry.first);
        });
    }
    void clear() { entries.clear(); }
private:
    struct Entry { Resources resources; Clock::time_point expires; };
    std::map<Key,Entry> entries;
};

// Both skin callbacks may run synchronously while Bedrock installs a skin.
// Only the committed actors may take part in normal animation and drawing.
template<class Actor, class Peer>
struct RelayRenderPeers {
    std::map<Actor const*, std::shared_ptr<Peer>> actors;
    std::shared_ptr<Peer> installing;

    RelayRenderPeers(std::map<std::string, std::shared_ptr<Peer>> const& committed, std::shared_ptr<Peer> next = {})
        : installing(std::move(next)) {
        for (auto const& [id, peer] : committed) actors.emplace(peer->actor, peer);
    }

    template<class Visit>
    void forEachPeer(Visit&& visit) const {
        if (installing) visit(*installing);
        for (auto const& [actor, peer] : actors) visit(*peer);
    }
};

}
