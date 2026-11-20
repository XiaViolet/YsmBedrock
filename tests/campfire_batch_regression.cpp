#include "ModelParticles.h"
#include <cassert>
#include <iostream>

using namespace ysm;

int main() {
    auto cue = [](char const* name, double x, int lifetime, bool hand = false) {
        auto result = prepareParticle(name, {}, true);
        result.firstPerson = hand;
        result.spawns.push_back({{x, 1, 2}, {0, 0.5, -0.2}, lifetime});
        return result;
    };
    auto smoke = [&](double x, int ticks) { return cue("campfire_cosy_smoke", x, ticks); };
    std::vector<ParticleCue> group;
    for (int i = 0; i < 130; ++i) group.push_back(smoke(i * 0.001, i + 1));
    group.push_back(cue("note", 0, 15));
    group.push_back(smoke(0, 5));
    group.push_back(smoke(32, 8));
    group.push_back(cue("campfire_cosy_smoke", 32, 9, true));
    group.push_back(cue("campfire_signal_smoke", 32, 7));
    batchCampfireSmoke(group);
    assert(group.size() == 7);
    assert(group[0].spawns.size() == 128 && group[1].spawns.size() == 2);
    auto ordered = 0;
    for (int i : {0, 1}) for (auto const& p : group[i].spawns) {
        assert(p.position[0] == ordered * 0.001 && p.velocity[2] == -0.2 && p.lifetime == ordered + 1);
        ++ordered;
    }
    assert(ordered == 130 && group[2].mapping->id == "note");
    assert(group[3].spawns.front().position[0] == 0 && group[4].spawns.front().position[0] == 32);
    assert(group[5].firstPerson && group[6].mapping->id == "campfire_signal_smoke");
    std::cout << "130 spawns retained in order; size, position, selector and first-person boundaries passed.\n";
}
