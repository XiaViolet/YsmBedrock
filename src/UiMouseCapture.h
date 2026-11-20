#pragma once
#include <atomic>
#include <cstdint>

namespace ysm {
// A drawn overlay is still on hud_screen. Capture must remain vetoed until
// the overlay closes, not just released once during its opening key event.
// This hook-facing policy never takes the render/input mutex or calls native
// capture functions, which may themselves synchronously publish input.
class UiMouseCapture {
    std::atomic<void const*> owner = nullptr;
    std::atomic<std::uint64_t> blocked = 0;
public:
    void open(void const* client) { owner.store(client,std::memory_order_release); }
    void close() { owner.store(nullptr,std::memory_order_release); }
    bool owns(void const* client) const { return client && owner.load(std::memory_order_acquire)==client; }
    bool suppress(void const* client) {
        if(!owns(client))return false;
        blocked.fetch_add(1,std::memory_order_relaxed);return true;
    }
    std::uint64_t blockedCount() const { return blocked.load(std::memory_order_relaxed); }
};
}
