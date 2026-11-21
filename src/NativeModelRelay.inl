#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>

namespace ysm::detail {

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
