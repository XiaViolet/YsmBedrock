#include "PortraitCadence.h"
#include "PreviewWorker.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <vector>

int main() {
    using Clock = std::chrono::steady_clock;
    auto at = [](double t) { return Clock::time_point{} + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(t)); };
    ysm::PortraitCadence cadence;
    assert(cadence.rate() == 60);
    for (int game : {60,120,144}) for (int fps : {5,15,30,60,100}) {
        cadence.rate(fps);
        cadence.reset();
        int submitted = 0;
        for (int i = 0; i < game * 2; ++i) {
            auto t = at(double(i)/game);
            if (cadence.due(t)) { cadence.submitted(t); ++submitted; }
        }
        assert(submitted == 2 * std::min(fps,60));
    }
    cadence.reset();
    std::vector<double> sampled;
    for (double t : {0.,.001,2.,2.001,2.016,2.017}) if (cadence.due(at(t))) {
        sampled.push_back(t); cadence.submitted(at(t));
    }
    assert((sampled == std::vector<double>{0.,2.,2.017}));
    cadence.rate(1); assert(cadence.rate() == 5);
    struct State { int count = 0; std::thread::id owner; };
    ysm::PreviewWorker<State,int> worker;
    std::promise<void> gate, entered;
    auto release = gate.get_future();
    auto started = entered.get_future();
    auto caller = std::this_thread::get_id();
    auto first = worker.submit([&](State& state) {
        state.owner = std::this_thread::get_id();
        entered.set_value(); release.wait();
        assert(state.owner != caller); return ++state.count;
    });
    started.wait();
    assert(first.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    gate.set_value(); assert(first.get() == 1);
    auto second = worker.submit([](State& state) { assert(state.owner == std::this_thread::get_id()); return ++state.count; });
    assert(second.get() == 2);
    auto error = worker.submit([](State&) -> int { throw std::runtime_error("test"); });
    bool caught = false;
    try { error.get(); } catch (std::runtime_error const&) { caught = true; }
    assert(caught);
    assert(worker.submit([](State& state) { return ++state.count; }).get() == 3);
    std::cout << "Preview cadence and worker regression passed\n";
}
