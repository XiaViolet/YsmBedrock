#include "RoomTransportProbe.h"
#include "RoomTransportQueue.h"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace ysm::room_wire;
void require(bool result, char const* message) {
    if (!result) throw std::runtime_error(message);
}
Nonce number(std::uint8_t seed) { Nonce n{}; n[0] = seed; return n; }
Header roundTrip(Header const& h) {
    auto parsed = decode(encode(h));
    require(parsed.has_value(), "wire handshake failed to decode");
    return *parsed;
}
GuestProbe connect(HostProbe& host, std::uint8_t client, std::uint8_t server) {
    GuestProbe g{Pair{number(client), {}}};
    auto hello = roundTrip({Op::Hello, g.pair.client, {}});
    auto ack = host.hello(hello, [=] { return number(server); });
    require(ack.has_value(), "host should acknowledge valid HELLO");
    require(!host.live.matches(*ack) && !g.ready, "HELLO cannot authorize data");
    auto confirm = g.ack(roundTrip(*ack));
    require(confirm.has_value(), "guest should confirm ACK");
    require(!g.ready, "ACK cannot authorize guest to send data");
    auto ready = host.confirm(roundTrip(*confirm));
    require(ready && g.finish(roundTrip(*ready)), "both directions must echo fresh challenges");
    auto repeated = host.confirm(*confirm);
    require(repeated && g.finish(roundTrip(*repeated)), "lost READY can be retransmitted");
    return g;
}
}
int main() {
    using namespace ysm::room_wire;
    HostProbe hostForA, hostForB;
    auto a = connect(hostForA, 1, 2);
    auto b = connect(hostForB, 3, 4);
    require(!hostForB.live.matches(a.pair.make(Op::Data)), "wrong connection must reject A's frame");
    require(!b.pair.matches(hostForA.live.make(Op::Data)), "B must reject another guest's nonce");
    auto old = a.pair;
    auto pending = hostForA.hello({Op::Hello, number(5), {}}, [] { return number(6); });
    require(pending && hostForA.live.matches(old.make(Op::Data)), "unconfirmed HELLO cannot evict current state");
    require(!hostForA.confirm({Op::Confirm, number(5), number(99)}), "incorrect echo cannot authorize session");
    auto fresh = connect(hostForA, 5, 6);
    require(!hostForA.live.matches(old.make(Op::Data)) && hostForA.live.matches(fresh.pair.make(Op::Data)), "late data cannot apply to rejoin");
    require(!fresh.ack(old.make(Op::Ack)), "late ACK cannot alter new session");
    require(!HostProbe{}.hello({Op::Hello, number(8), {}}, [] { return Nonce{}; }), "RNG failure must fail closed");

    auto head = encode(fresh.pair.make(Op::Data));
    std::vector<std::uint8_t> data(head.begin(), head.end());
    require(!decode(data), "header-only application frame must be denied");
    data.resize(headerSize + maxPayload, 0xff);
    require(decode(data).has_value(), "64 KiB payload must be accepted");
    data.push_back(0);
    require(!decode(data), "oversized frame must be denied");
    data.resize(headerSize + 1);
    data[0] = static_cast<std::uint8_t>(Op::Confirm);
    require(!decode(data), "unexpected protocol tail must be denied");
    data.resize(headerSize);
    data[0] = 0xaa;
    require(!decode(data), "unknown packet kind must be denied");
    require(!decode(std::span(data).first(17)), "short handshake must be denied");
    require(!decode(encode({Op::Hello, {}, {}})), "all-zero challenge must be denied");
    Outbox q;
    std::array<std::uint8_t, 1> bulk{3}, immediate{1}, nextControl{2};
    bool permit = true;
    require(q.push(bulk, false, [&] { return permit; }) && q.push(immediate, true, {}) && q.push(nextControl, true, {}), "queue admission");
    std::size_t budget = 32;
    q.flush(budget, [](auto const&) { return false; });
    require(q.sizeBytes() == 3 && budget == 32, "temporarily unsendable frames remain queued");
    std::vector<std::uint8_t> observed;
    permit = false;
    q.flush(budget, [&](auto const& frame) { observed.push_back(frame.front()); return true; });
    require(observed == std::vector<std::uint8_t>{1, 2}, "control FIFO before bulk; revoked chunk not sent");
    require(q.empty() && q.sizeBytes() == 0 && budget == 30, "rejected chunk releases memory without consuming wire budget");
    std::vector<std::uint8_t> block(maxPayload, 42);
    for (int i = 0; i < 7; ++i) require(q.push(block, false, {}), "bulk room");
    require(!q.push(block, false, {}) && q.push(block, true, {}), "room for control at saturated bulk limit");
    require(!q.push(immediate, true, {}) && !q.push(immediate, false, {}), "total bound at full queue");
    std::cout << "PASS RoomTransport wire v1: guest A and B round trips; lost READY, stale/rejoined and wrong-peer messages;"
                 " RNG failure and frame limits; control precedence, retry and permission revocation. Production code offline, no Minecraft session.\n";
}
