#pragma once
#include <map>
#include <span>
#include <string>

namespace ysm {
class MolangValue;
// One manager belongs to the model owner, including its arm animation pass.
class MolangPhysics {
    struct State {
        bool first;
        float input, a, b, c;
        float inputFunction = 0, value = 0, velocity = 0;
        std::string failure;
    };
    std::map<std::string, State> states;
    float lastTicks = 0;
    std::size_t nameBytes = 0;
    bool validClock = true;
public:
    void advance(double seconds, bool stabilizeFirstOrder = false);
    double evaluate(bool first, std::span<MolangValue const> args);
    std::size_t size() const { return states.size(); }
    double time() const { return lastTicks / 20.0; }
};
}
