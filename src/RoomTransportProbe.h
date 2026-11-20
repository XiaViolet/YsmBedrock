#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

namespace ysm::room_wire {

using Nonce = std::array<std::uint8_t, 16>;
enum class Op : std::uint8_t { Hello = 1, Ack, Confirm, Ready, Data };
struct Header {
    Op op{};
    Nonce client{}, host{};
};
constexpr std::size_t headerSize = 33;
constexpr std::size_t maxPayload = 64 * 1024;

inline std::optional<Header> decode(std::span<std::uint8_t const> bytes) {
    if (bytes.size() < headerSize || bytes.size() > headerSize + maxPayload) return {};
    Header frame{static_cast<Op>(bytes.front())};
    std::copy_n(bytes.begin() + 1, 16, frame.client.begin());
    std::copy_n(bytes.begin() + 17, 16, frame.host.begin());
    if (frame.op < Op::Hello || frame.op > Op::Data || frame.client == Nonce{}) return {};
    if ((frame.op == Op::Hello) != (frame.host == Nonce{})) return {};
    if ((frame.op == Op::Data) != (bytes.size() > headerSize)) return {};
    return frame;
}

inline std::array<std::uint8_t, headerSize> encode(Header const& h) {
    std::array<std::uint8_t, headerSize> bytes{};
    bytes[0] = static_cast<std::uint8_t>(h.op);
    std::copy(h.client.begin(), h.client.end(), bytes.begin() + 1);
    std::copy(h.host.begin(), h.host.end(), bytes.begin() + 17);
    return bytes;
}

struct Pair {
    Nonce client{}, host{};
    bool matches(Header const& h) const {
        return client != Nonce{} && host != Nonce{} && client == h.client && host == h.host;
    }
    Header make(Op op) const { return {op, client, host}; }
};

// The receiver keeps its existing session live until a replacement completes a round trip.
struct HostProbe {
    Pair live{}, pending{};

    template<class Random>
    std::optional<Header> hello(Header const& h, Random random) {
        if (h.op != Op::Hello || h.client == Nonce{} || h.host != Nonce{}) return {};
        if (live.client == h.client) return live.make(Op::Ack);
        if (pending.client != h.client) {
            auto next = random();
            if (next == Nonce{}) return {};
            pending = {h.client, next};
        }
        return pending.make(Op::Ack);
    }
    std::optional<Header> confirm(Header const& h) {
        if (h.op != Op::Confirm) return {};
        if (!live.matches(h)) {
            if (!pending.matches(h)) return {};
            live = pending;
            pending = {};
        }
        return live.make(Op::Ready);
    }
};

struct GuestProbe {
    Pair pair{};
    bool ready = false;
    std::optional<Header> ack(Header const& h) {
        if (h.op != Op::Ack || h.client != pair.client || h.client == Nonce{} || h.host == Nonce{}) return {};
        if (ready && h.host != pair.host) return {};
        pair.host = h.host;
        return pair.make(Op::Confirm);
    }
    bool finish(Header const& h) {
        if (h.op != Op::Ready || !pair.matches(h)) return false;
        ready = true;
        return true;
    }
};

// Bound an incomplete round trip as well as an established session. A repeated
// ACK alone is not proof that the pinned endpoint still reaches the host.
struct ProbeDeadline {
    using Clock = std::chrono::steady_clock;
    std::optional<Clock::time_point> since;
    bool expired(Clock::time_point now, bool ready) const {
        return since && now - *since >= std::chrono::seconds(ready ? 20 : 5);
    }
};

} // namespace ysm::room_wire
