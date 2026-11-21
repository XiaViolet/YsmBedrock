#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace ysm {
// File payload delivered during the last eight 250 ms intervals. Empty
// intervals age out even when a transfer pauses or finishes.
class RoomByteRate {
    struct Bucket { std::uint64_t interval{}, bytes{}; };
    std::array<Bucket,8> buckets{};
public:
    void add(std::uint64_t ms, std::size_t bytes) {
        auto interval=ms/250;
        auto& bucket=buckets[interval%buckets.size()];
        if(bucket.interval!=interval)bucket={interval,0};
        bucket.bytes+=bytes;
    }
    std::uint64_t perSecond(std::uint64_t ms) const {
        auto interval=ms/250;
        std::uint64_t bytes{};
        for(auto const& b:buckets)
            if(b.interval<=interval && interval-b.interval<buckets.size())bytes+=b.bytes;
        return bytes/2;
    }
};
}
