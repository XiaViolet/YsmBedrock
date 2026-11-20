#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace ysm {
// The local player and relay share this request, including repeated plays of the same clip.
struct ExtraAction {
    std::string name;
    std::uint64_t serial = 0;

    void play(std::string clip) {
        name=std::move(clip);
        ++serial;
    }
    bool stop() {
        if (name.empty()) return false;
        name.clear();
        ++serial;
        return true;
    }
    bool stopOnMovement(bool locked, float strafe, float forward, bool jump, bool sneak) {
        // PlayerMoveEvent.isMoveKey reads the player's input impulses, independent of world velocity.
        if (locked || !(std::abs(strafe)>1e-5f || std::abs(forward)>1e-5f || jump || sneak)) return false;
        return stop();
    }
};
}
