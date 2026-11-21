#pragma once
#include "UiPointer.h"
#include <cstdint>
#include <deque>
#include <set>
#include <vector>

namespace ysm {
enum class TouchPhase { Down, Move, Up, Cancel };
struct MenuTouchSample {
    TouchPhase phase;
    UiPoint fraction;
};

// The window thread captures contacts. The render thread consumes them after
// laying out the current page. Coordinates are fractions of the game client.
class UiTouchQueue {
    bool open = false;
    std::set<std::uint32_t> captured;
    std::optional<std::uint32_t> primary;
    std::deque<MenuTouchSample> pending;
public:
    void setOpen(bool value) {
        open = value;
        primary.reset();
        pending.clear();
        // If a menu action closed the overlay on down, its up still belongs to us.
    }
    void focusLost() {
        bool wasActive = primary.has_value() || !pending.empty();
        captured.clear();
        primary.reset();
        pending.clear();
        if (open && wasActive) pending.push_back({TouchPhase::Cancel, {}});
    }
    bool owns(std::uint32_t id) const { return captured.contains(id); }
    bool hasContacts() const { return !captured.empty(); }
    bool receive(std::uint32_t id, bool first, TouchPhase phase, UiPoint fraction) {
        if (phase == TouchPhase::Down) {
            if (!open) return false;
            if (owns(id)) return true;
            if (captured.size() >= 32) return false;
            captured.insert(id);
            if (first && !primary) primary = id;
        }
        if (!owns(id)) return false;
        if (open && primary == id) {
            if (phase == TouchPhase::Move && !pending.empty() && pending.back().phase == phase)
                pending.back().fraction = fraction;
            else if (pending.size() < 64)
                pending.push_back({phase, fraction});
            else {
                pending.clear();
                pending.push_back({TouchPhase::Cancel, {}});
                primary.reset();
            }
        }
        if (phase == TouchPhase::Up || phase == TouchPhase::Cancel) {
            captured.erase(id);
            if (primary == id) primary.reset();
        }
        return true;
    }
    std::vector<MenuTouchSample> take() {
        std::vector<MenuTouchSample> batch;
        while (!pending.empty()) {
            // A previous tap can change the active page and its hit targets.
            if (!batch.empty() && pending.front().phase == TouchPhase::Down) break;
            batch.push_back(pending.front());
            pending.pop_front();
        }
        return batch;
    }
};
}
