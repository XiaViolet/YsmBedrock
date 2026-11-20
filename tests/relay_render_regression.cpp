#include "NativeModelRelay.inl"

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
};
using Snapshot = ysm::detail::RelayRenderPeers<Actor, Peer>;

void require(bool result, char const* label) {
    if (!result) throw std::runtime_error(label);
}
void uploadAndBind(Snapshot const& snapshot, Skin& selected) {
    snapshot.forEachSkin([&](Skin& skin) { if (&skin == &selected) ++skin.uploads; });
    snapshot.forEachSkin([&](Skin& skin) { if (&skin == &selected) ++skin.bindings; });
}
}

int main() {
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
    std::cout << "PASS first install, existing peer, second-peer install callbacks, rollback, commit\n";
}
