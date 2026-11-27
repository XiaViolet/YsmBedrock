#pragma once
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <stdexcept>
#include <utility>

namespace ysm {
template<class State, class Result> class PreviewWorker {
    using Task = std::packaged_task<Result(State&)>;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Task> pending;
    bool stopping = false;
    std::thread thread;
    void run() {
        State state{};
        for (;;) {
            std::optional<Task> task;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || pending.has_value(); });
                if (stopping && !pending) return;
                task = std::move(pending);
                pending.reset();
            }
            (*task)(state);
        }
    }
public:
    PreviewWorker() : thread([this] { run(); }) {}
    ~PreviewWorker() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            pending.reset();
        }
        wake.notify_one();
        thread.join();
    }
    PreviewWorker(PreviewWorker const&) = delete;
    PreviewWorker& operator=(PreviewWorker const&) = delete;
    template<class F> std::future<Result> submit(F&& fn) {
        Task task(std::forward<F>(fn));
        auto result = task.get_future();
        {
            std::lock_guard lock(mutex);
            if (stopping || pending) throw std::logic_error("Preview worker already has a pending job");
            pending.emplace(std::move(task));
        }
        wake.notify_one();
        return result;
    }
};
}
