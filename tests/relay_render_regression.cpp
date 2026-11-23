#include "NativeModelRelay.inl"
#include "BoneVisual.h"

#include <iostream>
#include <stdexcept>

namespace {
struct Actor {};
struct Skin {
    unsigned uploads{}, bindings{};
};
struct Peer {
    Actor* actor;
    Skin skin;
    unsigned animationTexture{};
};
using Snapshot = ysm::detail::RelayRenderPeers<Actor, Peer>;

void require(bool result, char const* label) {
    if (!result) throw std::runtime_error(label);
}
void uploadAndBind(Snapshot const& snapshot, Skin& selected) {
    snapshot.forEachPeer([&](Peer& peer) {
        if (&peer.skin == &selected) peer.animationTexture = ++peer.skin.uploads;
    });
    snapshot.forEachPeer([&](Peer& peer) { if (&peer.skin == &selected) ++peer.skin.bindings; });
}
}

int main() {
    using namespace std::chrono_literals;
    using Cache=ysm::detail::RelayWarmModels<std::shared_ptr<int>>;
    Cache cache;
    auto now=std::chrono::steady_clock::time_point{};
    auto compiled=std::make_shared<int>(42);
    Cache::Key key{"digest","skin"};
    cache.keep(key,compiled,now);
    cache.prune(now+1s,[](auto const&){return true;});
    require(cache.find(key)==compiled && !cache.find({"digest","other"}),"fast return must reuse the exact compiled resources");
    cache.keep({"b","skin"},compiled,now+1s);
    cache.keep({"c","skin"},compiled,now+2s);
    require(!cache.find(key),"bounded warm cache must evict its oldest model");
    cache.prune(now+31s,[](auto const& k){return k.first!="c";});
    require(!cache.find({"c","skin"}) && !cache.find({"b","skin"}),"revocation and idle expiry must release resources");
    std::cout << "PASS compiled model reuse, isolated textures, bounded eviction, permission revocation and expiry\n";
    Actor aliceActor, beaActor;
    auto alice = std::make_shared<Peer>(Peer{&aliceActor, {}});
    auto bea = std::make_shared<Peer>(Peer{&beaActor, {}});
    std::map<std::string, std::shared_ptr<Peer>> committed;
    Snapshot firstInstall(committed, alice);
    uploadAndBind(firstInstall, alice->skin);
    require(alice->skin.uploads == 1 && alice->skin.bindings == 1, "both callbacks during first install");
    require(firstInstall.actors.empty(), "keep drawing the vanilla player until the skin succeeds");

    committed.emplace("alice", alice);
    Snapshot installingBea(committed, bea);
    uploadAndBind(installingBea, alice->skin);
    uploadAndBind(installingBea, bea->skin);
    require(installingBea.actors.size() == 1 && installingBea.actors.contains(&aliceActor), "existing peer visible");
    require(alice->skin.uploads == 2 && alice->skin.bindings == 2, "committed peer keeps both hooks");
    require(bea->skin.uploads == 1 && bea->skin.bindings == 1, "second peer receives install callbacks once");

    Snapshot rollback(committed);
    uploadAndBind(rollback, bea->skin);
    require(bea->skin.uploads == 1 && bea->skin.bindings == 1, "rolled-back peer no longer receives callbacks");
    committed.emplace("bea", bea);
    Snapshot ready(committed);
    uploadAndBind(ready, bea->skin);
    require(ready.actors.at(&beaActor) == bea && bea->skin.uploads == 2 && bea->skin.bindings == 2,
            "published once and bound to the right actor");
    require(alice->animationTexture == 2 && bea->animationTexture == 2, "animation retains last texture revision for each wearer");
    struct PartName { std::string value; auto const& getString() const { return value; } };
    struct Part { PartName const* mName; };
    PartName root{"root"}, label{"__ysm_body_42"}, other{"other"};
    Part rootPart{&root}, copied{&label}, foreignPart{&other};
    std::array<Part const*,2> parts{&rootPart,&copied};
    std::array<ysm::BoneVisual,2> result;
    auto resolve = [&](std::string const& key, ysm::BoneVisual& visual) {
        if (key == label.value) { visual.glow = 15; return true; }
        return key == root.value;
    };
    require(ysm::resolveOwnedPalette(parts,result,resolve) && result[1].glow==15, "cloned peer mesh restores the glow bone");
    parts[1]=&rootPart;
    require(!ysm::resolveOwnedPalette(parts,result,resolve),"vanilla mesh excluded");
    parts={&copied,&foreignPart};
    require(!ysm::resolveOwnedPalette(parts,result,resolve),"mixed unknown mesh excluded");
    std::cout << "PASS cloned glow palette, vanilla and mixed palette exclusions"<<'\n';
    std::cout << "PASS first install, existing peer, second-peer install callbacks, rollback, commit\n";
}
