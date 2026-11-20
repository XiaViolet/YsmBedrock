#pragma once
// Included only in the staged profiling build. No hooks or counters in the shipped mod.
#include "ModelPack.h"
#include <chrono>
#include <map>
#include <string_view>

namespace ysm::script_profile {
using Clock = std::chrono::steady_clock;
using Span = Clock::duration;
struct Counts {
    std::uint64_t count{};
    Span self{}, inclusive{}, dispatch{};
};
struct Stage {
    std::uint64_t count{};
    Span gross{}, rootInclusive{};
    std::map<std::string, Counts, std::less<>> calls;
};
struct Session {
    std::string clip, mode;
    unsigned warmup{}, frames{};
    std::map<std::string, Stage, std::less<>> stages;
    Json report() const {
        auto us = [](Span span) { return std::chrono::duration<double, std::micro>(span).count(); };
        Json result = {{"clip", clip}, {"mode", mode}, {"warmup", warmup}, {"frames", frames - std::min(frames,warmup)}};
        for (auto const& [name,s] : stages) {
            auto& value = result["stages"][name];
            value = {{"count",s.count},{"gross_us",us(s.gross)},{"root_call_us",us(s.rootInclusive)}};
            for(auto const& [fn,c] : s.calls)
                value["calls"][fn] = {{"count",c.count},{"self_us",us(c.self)},{"inclusive_us",us(c.inclusive)},
                    {"self_dispatch_us",us(c.dispatch)}};
        }
        return result;
    }
};
inline thread_local Session session;
struct Call;
inline thread_local Call* current{};
inline thread_local Stage* active{};
struct Clip {
    Stage *stage{}, *saved{};
    Clock::time_point start{};
    Clip(std::string const& name, char const* phase) {
        if (session.frames <= session.warmup || name != session.clip) return;
        stage = &session.stages[phase];
        saved = active;
        active = session.mode == "clip" ? nullptr : stage;
        start = Clock::now();
    }
    ~Clip() {
        if (!stage) return;
        stage->gross += Clock::now() - start;
        ++stage->count;
        active = saved;
    }
};
struct Call {
    Call* parent{};
    Counts* counts{};
    Clock::time_point resumed{};
    Span self{}, dispatch{}, children{};
    bool body{};
    void pause(Clock::time_point now) {
        auto part = now - resumed;
        self += part;
        if (body) dispatch += part;
    }
    explicit Call(std::string const& name) {
        if (!active || (current && session.mode == "roots")) return;
        parent = current;
        if (parent) parent->pause(Clock::now());
        auto found = active->calls.lower_bound(name);
        if (found == active->calls.end() || found->first != name)
            found = active->calls.try_emplace(found, name);
        counts = &found->second;
        current = this;
        resumed = Clock::now();
    }
    void beginDispatch() {
        if (!counts) return;
        auto now = Clock::now();
        pause(now);
        body = true;
        resumed = now;
    }
    ~Call() {
        if (!counts) return;
        pause(Clock::now());
        ++counts->count;
        auto total = self + children;
        counts->self += self;
        counts->inclusive += total;
        counts->dispatch += dispatch;
        current = parent;
        if (parent) {
            parent->children += total;
            parent->resumed = Clock::now();
        } else active->rootInclusive += total;
    }
};
}
