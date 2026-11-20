#include "MolangPhysics.h"
#include "Molang.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace ysm {
// C++ adaptation of the pinned YSM FirstOrder, SecondOrder (MicroCraft),
// PhysicsManager and ModelBuiltins.physics, under Apache-2.0.
void MolangPhysics::advance(double seconds, bool stabilizeFirstOrder) {
    auto ticks = static_cast<float>(seconds * 20);
    validClock = std::isfinite(ticks) && seconds >= 0;
    // Keep the host's wider animation clock usable. An unrepresentable F32
    // physics clock fails only a program that actually reads physics.
    if (!validClock) return;
    if (lastTicks <= 0) { lastTicks = ticks; return; }
    if (ticks <= lastTicks) return;
    float delta = (ticks - lastTicks) / 20.0f;
    lastTicks = ticks;
    std::size_t budget = 200000;
    for (auto& [name, state] : states) {
        state.failure.clear();
        if (state.first) {
            float alpha = delta / state.a;
            // The legacy rig may pick a response shorter than a skipped
            // render interval. Euler becomes unstable at delta/response >= 2.
            // A step of one response reaches the held input. Any remaining
            // time at that input keeps the same value.
            if (stabilizeFirstOrder && state.a > 0 && (alpha >= 2 || !std::isfinite(state.value))) {
                state.value = state.input;
                continue;
            }
            state.value = (1 - alpha) * state.value + alpha * state.input;
            continue;
        }
        float frequency = std::clamp(state.a, 0.0f, 5.0f), damping = std::clamp(state.b, 0.0f, 1.0f);
        if (frequency == 0) {
            // Java produces a NaN step count, casts it to zero and retains
            // the simulation, while still recording the current input.
            state.inputFunction = state.input;
            continue;
        }
        constexpr float pi = std::numbers::pi_v<float>;
        float k1 = damping / pi / frequency;
        float k2 = 1 / (2 * pi * frequency) / (2 * pi * frequency);
        float k3 = state.c * damping / 2 / pi / frequency;
        float maximum = static_cast<float>(std::sqrt(4 * k2 + k1 * k1)) - k1;
        auto cycles = std::ceil(static_cast<double>(delta / maximum));
        // Java's float NaN -> int cast yields zero. Tiny nonzero frequencies
        // can likewise overflow intermediate coefficients without advancing.
        if (std::isnan(cycles) || cycles <= 0) { state.inputFunction = state.input; continue; }
        if (!std::isfinite(cycles) || cycles > 10000 || cycles > budget) {
            state.failure = "Physics step budget exceeded";
            continue;
        }
        auto count = static_cast<int>(cycles);
        budget -= count;
        float derivative = (state.input - state.inputFunction) / delta;
        state.inputFunction = state.input;
        float step = delta / static_cast<float>(count);
        for (int i = 0; i < count; ++i) {
            state.value = state.value + step * state.velocity;
            state.velocity = state.velocity + step * (k3 * derivative + state.input - state.value - k1 * state.velocity) / k2;
        }
    }
}
double MolangPhysics::evaluate(bool first, std::span<MolangValue const> args) {
    if (!validClock) throw std::runtime_error("Invalid physics clock");
    if (args.size() < 2 || args.size() > 255) throw std::runtime_error("Physics requires 2..255 arguments");
    if (!args[0].isString()) throw std::runtime_error("Physics name must be a string");
    std::array<float, 4> values{0, 1, 1, 1};
    for (std::size_t i = 1; i < args.size(); ++i) {
        float value = static_cast<float>(args[i].number());
        if (!std::isfinite(value))
            throw std::runtime_error("Physics arguments must be finite floats");
        if (i <= values.size()) values[i - 1] = value;
    }
    auto name = args[0].string();
    if (name.empty()) return 0;
    auto found = states.find(name);
    if (found == states.end()) {
        if (states.size() >= 4096 || name.size() > 1024 * 1024 - nameBytes)
            throw std::runtime_error("Physics name budget exceeded");
        states.emplace(name, State{first, values[0], values[1], values[2], values[3]});
        nameBytes += name.size();
        // Source creation returns the input, while its simulation starts at 0.
        return values[0];
    }
    auto& state = found->second;
    // First creation fixes the dynamics type for a shared name. Every read,
    // including a repeated one, updates arguments for the next owner step.
    state.input = values[0]; state.a = values[1]; state.b = values[2]; state.c = values[3];
    if (!state.failure.empty()) throw std::runtime_error(state.failure);
    if (!std::isfinite(state.value)) throw std::runtime_error("Non-finite physics state: " + name);
    return state.value;
}
}
