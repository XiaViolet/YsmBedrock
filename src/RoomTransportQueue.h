#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace ysm::room_wire {
class Outbox {
public:
    static constexpr std::size_t maxBytes = 512u << 10;
    bool empty() const { return controls.empty() && chunks.empty(); }
    std::size_t sizeBytes() const { return bytes; }
    bool push(std::span<std::uint8_t const> data, bool control, std::function<bool()> authorized, std::size_t modelBytes=0) {
        auto limit = control ? maxBytes : maxBytes - (64u << 10);
        if (bytes > limit || data.size() > limit - bytes || controls.size() + chunks.size() >= (control ? 256u : 192u)) return false;
        auto& queue = control ? controls : chunks;
        queue.push_back({std::vector<std::uint8_t>(data.begin(), data.end()), std::move(authorized),modelBytes});
        bytes += data.size();
        return true;
    }
    template<class Send>
    std::size_t flush(std::size_t& budget, Send send) {
        std::size_t modelBytes{};
        while (!empty()) {
            auto& queue = controls.empty() ? chunks : controls;
            auto const& entry = queue.front();
            if (!entry.authorized || entry.authorized()) {
                if (entry.data.size() > budget || !send(entry.data)) break;
                budget -= entry.data.size();
                modelBytes+=entry.modelBytes;
            }
            bytes -= entry.data.size();
            queue.pop_front();
        }
        return modelBytes;
    }
private:
    struct Entry {
        std::vector<std::uint8_t> data;
        std::function<bool()> authorized;
        std::size_t modelBytes{};
    };
    std::deque<Entry> controls, chunks;
    std::size_t bytes = 0;
};
} // namespace ysm::room_wire
