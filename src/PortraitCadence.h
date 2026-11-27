#pragma once
#include <chrono>
#include <algorithm>

namespace ysm {
class PortraitCadence {
    using Clock = std::chrono::steady_clock;
    Clock::time_point next{};
    int fps = 60;
public:
    int rate() const { return fps; }
    void reset() { next = {}; }
    void rate(int value) {
        int limit = std::clamp(value, 5, 60);
        if (limit != fps) { fps = limit; reset(); }
    }
    bool due(Clock::time_point now) const { return next == Clock::time_point{} || now >= next; }
    void submitted(Clock::time_point now) {
        auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
        if (due(now)) next = next == Clock::time_point{} || now - next >= period ? now + period : next + period;
    }
};
}
