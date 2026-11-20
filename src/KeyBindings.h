#pragma once
#include <array>
#include <mutex>
#include <set>

namespace ysm {
enum class KeyAction { None, ModelMenu, Roulette };

// Consume ordered game input events. Reading Windows' asynchronous key state
// here races quick press/release pairs that are already queued by the game.
class KeyBindings {
    std::array<bool, 256> held{};
    mutable std::mutex mutex;
public:
    static int glfwKey(int key) {
        if (key == 32 || (key >= 48 && key <= 57) || (key >= 65 && key <= 90)) return key;
        if (key >= 112 && key <= 135) return 290 + key - 112;
        if (key >= 96 && key <= 105) return 320 + key - 96;
        switch (key) {
        case 27: return 256; case 13: return 257; case 9: return 258; case 8: return 259;
        case 45: return 260; case 46: return 261; case 39: return 262; case 37: return 263;
        case 40: return 264; case 38: return 265; case 33: return 266; case 34: return 267;
        case 36: return 268; case 35: return 269;
        case 20: return 280; case 145: return 281; case 144: return 282; case 44: return 283; case 19: return 284;
        case 110: return 330; case 111: return 331; case 106: return 332; case 109: return 333; case 107: return 334;
        case 16: case 160: return 340; case 17: case 162: return 341; case 18: case 164: return 342;
        case 91: return 343; case 161: return 344; case 163: return 345; case 165: return 346;
        case 92: return 347; case 93: return 348;
        case 186: return 59; case 187: return 61; case 188: return 44; case 189: return 45;
        case 190: return 46; case 191: return 47; case 192: return 96;
        case 219: return 91; case 220: return 92; case 221: return 93; case 222: return 39;
        default: return -1;
        }
    }
    std::set<int> glfwKeys() const {
        std::scoped_lock lock(mutex);
        std::set<int> result;
        for (int key = 0; key < static_cast<int>(held.size()); ++key)
            if (held[key]) if (auto translated = glfwKey(key); translated >= 32) result.insert(translated);
        return result;
    }
    KeyAction feed(int key, bool down) {
        std::scoped_lock lock(mutex);
        if (key < 0 || key >= static_cast<int>(held.size())) return KeyAction::None;
        auto previous = held[key];
        held[key] = down;
        if (!down || previous) return KeyAction::None;
        bool control = held[0x11] || held[0xA2] || held[0xA3];
        bool shift = held[0x10] || held[0xA0] || held[0xA1];
        bool alt = held[0x12] || held[0xA4] || held[0xA5];
        if (control || shift || held[0x5B] || held[0x5C]) return KeyAction::None;
        if (key == 'Y' && alt) return KeyAction::ModelMenu;
        if (key == 'Z' && !alt) return KeyAction::Roulette;
        return KeyAction::None;
    }
    void reset() {
        std::scoped_lock lock(mutex);
        held.fill(false);
    }
};
}
