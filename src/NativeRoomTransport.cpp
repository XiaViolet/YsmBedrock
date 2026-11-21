#include "RoomTransport.h"
#include "RoomTransportProbe.h"
#include "RoomTransportQueue.h"
#include "RoomByteRate.h"
#include "ll/api/network/packet/Packet.h"
#include "ll/api/service/Bedrock.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/deps/core/utility/BinaryStream.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/entity/components/UserEntityIdentifierComponent.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/NetworkPeer.h"
#include "mc/network/NetworkSystem.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/PlayerListEntry.h"
#include "mc/world/level/Level.h"
#include <Windows.h>
#include <bcrypt.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

namespace ysm {
namespace {
using namespace room_wire;
using ll::network::PacketSide;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
auto nowMillis(){return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();}

constexpr auto packetName = std::string_view{"ysm:room"};
constexpr auto wireId = ll::network::makePacketRuntimeId(packetName, 1);
constexpr std::size_t maxQueueBytes = 2u << 20;
constexpr std::size_t maxPeerBytes = 512u << 10;
constexpr std::size_t maxPeers = 64;
constexpr std::size_t maxPackets = 512;

Nonce nonce() {
    Nonce value{};
    if (BCryptGenRandom(nullptr, value.data(), static_cast<ULONG>(value.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return {};
    return value;
}

class WirePacket final : public ll::network::Packet {
public:
    Header frame{};
    Bytes body;
    std::span<std::uint8_t const> sending;

    std::string_view getName() const override { return packetName; }
    ll::network::PacketRuntimeId getRuntimeId() const override { return wireId; }

    void write(BinaryStream& stream) const override {
        auto prefix = encode(frame);
        auto payload = sending.empty() ? std::span<std::uint8_t const>(body) : sending;
        stream.writeUnsignedVarInt(static_cast<unsigned>(prefix.size() + payload.size()), nullptr, nullptr);
        stream.write(reinterpret_cast<char const*>(prefix.data()), prefix.size());
        if (!payload.empty()) stream.write(reinterpret_cast<char const*>(payload.data()), payload.size());
    }
    Bedrock::Result<void> read(ReadOnlyBinaryStream& stream) override {
        auto length = stream.getUnsignedVarInt();
        if (!length) return nonstd::make_unexpected(length.error());
        auto invalid = []() -> Bedrock::Result<void> {
            return nonstd::make_unexpected(Bedrock::ErrorInfo<std::error_code>{std::make_error_code(std::errc::bad_message)});
        };
        if (*length < headerSize || *length > headerSize + maxPayload) return invalid();
        Bytes data(*length);
        auto result = stream.read(data.data(), data.size());
        if (!result) return result;
        auto parsed = decode(data);
        if (!parsed) return invalid();
        frame = *parsed;
        body.assign(data.begin() + headerSize, data.end());
        return {};
    }
};

struct Incoming {
    NetworkIdentifier endpoint;
    Header frame;
    Bytes body;
};
struct Inbox {
    std::mutex mutex;
    std::array<std::vector<Incoming>, 2> queues;
    std::array<std::size_t, 2> sizes{};
    static std::size_t index(PacketSide side) { return side == PacketSide::Server ? 0 : 1; }
    void push(NetworkIdentifier const& endpoint, PacketSide side, WirePacket const& p) {
        std::lock_guard lock(mutex);
        auto i = index(side);
        auto& q = queues[i];
        auto size = headerSize + p.body.size();
        if (q.size() >= maxPackets || sizes[i] + size > maxQueueBytes) return;
        std::size_t peerBytes = size, count = 1;
        for (auto const& item : q) if (item.endpoint == endpoint) {
            peerBytes += item.body.size() + headerSize;
            if (++count > 128 || peerBytes > maxPeerBytes) return;
        }
        q.push_back({endpoint, p.frame, p.body});
        sizes[i] += size;
    }
    std::vector<Incoming> take(PacketSide side) {
        std::lock_guard lock(mutex);
        auto i = index(side);
        sizes[i] = 0;
        return std::exchange(queues[i], {});
    }
    void clear() {
        std::lock_guard lock(mutex);
        for (auto& q : queues) q.clear();
        sizes = {};
    }
};

class Handler final : public ll::network::IPacketHandler {
public:
    std::atomic<std::shared_ptr<Inbox>> inbox;
    void handle(NetworkIdentifier const& id, NetEventCallback&, PacketSide side, ll::network::Packet const& packet) const override {
        if (packet.mSenderSubId != SubClientId::PrimaryClient) return;
        if (auto receiver = inbox.load()) receiver->push(id, side, static_cast<WirePacket const&>(packet));
    }
};
Handler& handler() { static Handler value; return value; }

bool writePacket(NetworkIdentifier const* client, Header frame, std::span<std::uint8_t const> body = {}) {
    WirePacket packet;
    bool supported = client ? packet.canSendTo(*client) : packet.canSendToServer();
    if (!supported) return false;
    packet.frame = frame;
    packet.sending = body;
    if (client) packet.sendToClient(*client, SubClientId::PrimaryClient);
    else packet.sendToServer();
    return true;
}

struct Peer {
    NetworkIdentifier endpoint;
    HostProbe probe;
    Outbox outgoing;
    RoomByteRate downloadRate;
};
} // namespace

std::string roomPlayerKey(Player& player) {
    auto connection = player.getEntityContext().tryGetComponent<UserEntityIdentifierComponent>();
    auto const& roster = player.getLevel().getPlayerList();
    auto const& actorId = player.getOrCreateUniqueID();
    auto member = connection ? roster.find(*connection->mClientUUID) : roster.end();
    if (member == roster.end() || *member->second.mId != actorId) {
        member = std::find_if(roster.begin(), roster.end(), [&](auto const& entry) { return *entry.second.mId == actorId; });
    }
    if (member != roster.end()) {
        auto const& uuid = member->first;
        if (uuid.a || uuid.b) return "uuid:" + uuid.asString();
        auto const& id = *member->second.mXUID;
        if (!id.empty() && id != "0") return id;
    }
    if (!connection) return {};
    auto const& uuid = *connection->mClientUUID;
    if (uuid.a || uuid.b) return "uuid:" + uuid.asString();
    auto id = player.getXuid();
    return id != "0" ? id : std::string{};
}

struct RoomTransport::Impl {
    mutable std::mutex mutex;
    std::shared_ptr<Inbox> inbox;
    RoomRole role = RoomRole::None;
    Level const* level = nullptr;
    std::string local;
    std::map<std::string, Peer, std::less<>> peers;
    std::string cursor;
    GuestProbe guest;
    std::optional<NetworkIdentifier> upstream;
    Outbox outgoing;
    Clock::time_point nextHello{};
    ProbeDeadline handshake;
    unsigned recoveryAttempts{};
    std::optional<bool> serverCapability;
    std::map<std::string, bool, std::less<>> rosterCapabilities;
    Clock::time_point nextPresence{};
    Clock::time_point nextLatency{};
    std::optional<int> latency;
    RoomByteRate uploadRate;
    std::vector<RoomWireEvent> events;
    std::set<std::string, std::less<>> reported;
    std::size_t eventBytes = 0;

    void clear() {
        if (inbox) inbox->clear();
        role = RoomRole::None;
        level = nullptr;
        local.clear();
        peers.clear();
        cursor.clear();
        guest = {};
        upstream.reset();
        outgoing = {};
        nextHello = {};
        handshake = {};
        recoveryAttempts = 0;
        serverCapability.reset();
        rosterCapabilities.clear();
        nextPresence = {};
        nextLatency = {};
        latency.reset();
        uploadRate={};
        events.clear();
        reported.clear();
        eventBytes = 0;
    }
    void event(RoomWireKind kind, std::string_view peer, Bytes data = {}) {
        if (kind == RoomWireKind::Data && (eventBytes + data.size() > maxQueueBytes || events.size() >= maxPackets)) return;
        if (kind == RoomWireKind::Left) {
            // Drop queued data from a departed identity before admitting its next connection.
            std::erase_if(events, [&](RoomWireEvent const& e) {
                if (e.peer != peer) return false;
                eventBytes -= e.payload.size();
                return true;
            });
            if (!reported.contains(peer)) return;
        }
        eventBytes += data.size();
        events.push_back({kind, std::string(peer), std::move(data)});
    }
};

RoomTransport::RoomTransport() : impl(std::make_unique<Impl>()) {}
RoomTransport::~RoomTransport() { stop(); }

bool RoomTransport::start() {
    std::lock_guard lock(impl->mutex);
    if (impl->inbox) return true;
    auto& registrar = ll::network::PacketRegistrar::getInstance();
    if (!registrar.registerPacket(packetName, wireId, [] { return std::make_unique<WirePacket>(); }, PacketSide::Both, true)) return false;
    if (!registrar.registerHandler(packetName, wireId, handler(), PacketSide::Both)) {
        registrar.unregisterPacket(wireId);
        return false;
    }
    impl->inbox = std::make_shared<Inbox>();
    handler().inbox.store(impl->inbox);
    if (auto mod = ll::mod::NativeMod::current()) mod->getLogger().info("Room transport registered ysm:room v1 (optional, client/server)");
    return true;
}
void RoomTransport::stop() {
    std::lock_guard lock(impl->mutex);
    if (!impl->inbox) return;
    handler().inbox.store(nullptr);
    auto& registrar = ll::network::PacketRegistrar::getInstance();
    registrar.unregisterHandler(wireId);
    registrar.unregisterPacket(wireId);
    impl->clear();
    impl->inbox.reset();
}
void RoomTransport::reset() { std::lock_guard lock(impl->mutex); impl->clear(); }
RoomRole RoomTransport::role() const { std::lock_guard lock(impl->mutex); return impl->role; }
std::optional<int> RoomTransport::latencyMs() const { std::lock_guard lock(impl->mutex); return impl->latency; }
std::uint64_t RoomTransport::uploadBytesPerSecond() const {std::lock_guard lock(impl->mutex);return impl->uploadRate.perSecond(nowMillis());}
std::map<std::string,std::uint64_t> RoomTransport::peerDownloads() const {
    std::lock_guard lock(impl->mutex);auto now=nowMillis();
    std::map<std::string,std::uint64_t> rates;
    for(auto const& [id,p]:impl->peers)if(auto rate=p.downloadRate.perSecond(now))rates.emplace(id,rate);
    return rates;
}
std::string RoomTransport::localId() const { std::lock_guard lock(impl->mutex); return impl->local; }
std::vector<RoomWireEvent> RoomTransport::drain() {
    std::lock_guard lock(impl->mutex);
    for (auto const& event : impl->events) {
        if (event.kind == RoomWireKind::Joined) impl->reported.insert(event.peer);
        if (event.kind == RoomWireKind::Left) impl->reported.erase(event.peer);
    }
    impl->eventBytes = 0;
    return std::exchange(impl->events, {});
}
bool RoomTransport::send(std::string_view peer, std::span<std::uint8_t const> data, bool control,
                         std::function<bool()> authorized,std::size_t modelBytes) {
    if (data.empty() || data.size() > maxPayload) return false;
    std::lock_guard lock(impl->mutex);
    if (impl->role == RoomRole::Guest && peer == roomHostPeer && impl->guest.ready)
        return impl->outgoing.push(data, control, std::move(authorized),modelBytes);
    auto it = impl->peers.find(peer);
    if (impl->role != RoomRole::Host || it == impl->peers.end() || it->second.probe.live.client == Nonce{}) return false;
    std::size_t queued = data.size();
    for (auto const& item : impl->peers) queued += item.second.outgoing.sizeBytes();
    auto limit = control ? maxQueueBytes : maxQueueBytes - maxPayload;
    return queued <= limit && it->second.outgoing.push(data, control, std::move(authorized),modelBytes);
}

void RoomTransport::clientTick(IClientInstance& client, Player* local) {
    if (!client.isPrimaryClient()) return;
    std::lock_guard lock(impl->mutex);
    if (!impl->inbox) return;
    if (!local) {
        if (impl->role != RoomRole::None) impl->clear();
        return;
    }
    auto role = ll::service::getServerInstance() ? RoomRole::Host : RoomRole::Guest;
    if (impl->level != &local->getLevel() || impl->role != role || impl->local.empty()) {
        impl->clear();
        impl->local = roomPlayerKey(*local);
        if (impl->local.empty()) return;
        impl->level = &local->getLevel();
        impl->role = role;
        if (auto mod = ll::mod::NativeMod::current())
            mod->getLogger().info("Room world entered: role={}, multiplayerClient={}",
                role == RoomRole::Host ? "host" : "guest", client.isMultiPlayerClient());
    }
    auto frames = impl->inbox->take(PacketSide::Client);
    if (role != RoomRole::Guest) return;
    auto capable = ll::network::Packet::canSendToServer(wireId);
    if (impl->serverCapability != capable) {
        impl->serverCapability = capable;
        if (auto mod = ll::mod::NativeMod::current()) mod->getLogger().info("Room host advertises ysm:room={}", capable);
    }
    if (!capable) {
        if (impl->guest.ready) impl->event(RoomWireKind::Left, roomHostPeer);
        impl->guest = {};
        impl->upstream.reset();
        impl->outgoing = {};
        impl->nextHello = {};
        impl->latency.reset();
        impl->handshake = {};
        impl->recoveryAttempts = 0;
        return;
    }
    auto now = Clock::now();
    for (auto& f : frames) {
        if (impl->upstream && !(*impl->upstream == f.endpoint)) continue;
        if (f.frame.op == Op::Ack) {
            auto confirm = impl->guest.ack(f.frame);
            if (confirm) {
                impl->upstream = f.endpoint;
                writePacket(nullptr, *confirm);
            }
        } else if (impl->upstream && f.frame.op == Op::Ready) {
            bool wasReady = impl->guest.ready;
            if (impl->guest.finish(f.frame)) {
                impl->handshake.since = now;
                impl->recoveryAttempts = 0;
                if (!wasReady) {
                    impl->event(RoomWireKind::Joined, roomHostPeer);
                    if (auto mod = ll::mod::NativeMod::current()) mod->getLogger().info("Room transport guest received READY from host");
                }
            }
        } else if (f.frame.op == Op::Data && impl->guest.ready && impl->guest.pair.matches(f.frame)) {
            impl->event(RoomWireKind::Data, roomHostPeer, std::move(f.body));
        }
    }
    if (impl->handshake.expired(now, impl->guest.ready)) {
        if (impl->guest.ready) {
            impl->event(RoomWireKind::Left, roomHostPeer);
        } else if (impl->recoveryAttempts < 3) {
            // LL may retain the client's "registration already answered" flag
            // while the host has dropped the previous connection's return route.
            // Advertise our own receiving handler again using the public API.
            auto& registrar = ll::network::PacketRegistrar::getInstance();
            registrar.unregisterHandler(wireId, PacketSide::Client);
            auto registered = registrar.registerHandler(packetName, wireId, handler(), PacketSide::Client);
            ++impl->recoveryAttempts;
            if (auto mod = ll::mod::NativeMod::current())
                mod->getLogger().info("Room retry {}: refreshed client receive capability={}, had_ack={}",
                    impl->recoveryAttempts, registered, impl->upstream.has_value());
        }
        impl->guest = {};
        impl->upstream.reset();
        impl->outgoing = {};
        impl->nextHello = {};
        impl->latency.reset();
        impl->handshake = {};
    }
    if (now >= impl->nextHello) {
        if (impl->guest.pair.client == Nonce{}) impl->guest.pair.client = nonce();
        if (impl->guest.pair.client != Nonce{} && writePacket(nullptr, {Op::Hello, impl->guest.pair.client, {}})
            && !impl->handshake.since) impl->handshake.since = now;
        impl->nextHello = now + std::chrono::seconds(impl->guest.ready ? 5 : 1);
    }
    if (impl->guest.ready) {
        if (impl->upstream && now >= impl->nextLatency) {
            impl->nextLatency = now + std::chrono::seconds(1);
            impl->latency.reset();
            if (auto system = ll::service::getNetworkSystem(true)) {
                if (auto* peer = system->getPeerForUser(*impl->upstream)) {
                    auto ping = peer->getNetworkStatus().mCurrentPing->count();
                    if (ping >= 0 && ping <= std::numeric_limits<int>::max()) impl->latency = static_cast<int>(ping);
                }
            }
        }
        std::size_t budget = 128u << 10;
        auto sent=impl->outgoing.flush(budget, [&](Bytes const& data) {
            return writePacket(nullptr, impl->guest.pair.make(Op::Data), data);
        });
        impl->uploadRate.add(nowMillis(),sent);
    }
}

void RoomTransport::serverTick(Level& level) {
    if (level.isClientSide()) return;
    std::lock_guard lock(impl->mutex);
    if (!impl->inbox || impl->role != RoomRole::Host) return;
    auto frames = impl->inbox->take(PacketSide::Server);
    auto now = Clock::now();
    bool needsSend = std::any_of(impl->peers.begin(), impl->peers.end(), [](auto const& p) { return !p.second.outgoing.empty(); });
    bool checkPresence = now >= impl->nextPresence;
    if (frames.empty() && !needsSend && !checkPresence) return;
    if (checkPresence) impl->nextPresence = now + std::chrono::milliseconds(500);
    std::map<std::string, std::optional<NetworkIdentifier>, std::less<>> roster;
    level.forEachPlayer([&](Player& player) {
        auto connection = player.getEntityContext().tryGetComponent<UserEntityIdentifierComponent>();
        if (!connection || connection->mClientSubId != SubClientId::PrimaryClient) return true;
        auto key = roomPlayerKey(player);
        if (!key.empty() && key != impl->local) {
            auto [it, inserted] = roster.try_emplace(std::move(key), *connection->mNetworkId);
            if (!inserted) it->second.reset();
        }
        return true;
    });
    std::erase_if(impl->peers, [&](auto const& item) {
        auto it = roster.find(item.first);
        if (it != roster.end() && it->second && *it->second == item.second.endpoint
            && ll::network::Packet::canSendTo(wireId, item.second.endpoint)) return false;
        if (item.second.probe.live.client != Nonce{}) impl->event(RoomWireKind::Left, item.first);
        return true;
    });
    if (checkPresence) {
        std::erase_if(impl->rosterCapabilities, [&](auto const& item) { return !roster.contains(item.first); });
        for (auto const& [key, endpoint] : roster) {
            bool available = endpoint && ll::network::Packet::canSendTo(wireId, *endpoint);
            auto [it, fresh] = impl->rosterCapabilities.try_emplace(key, available);
            if (fresh || it->second != available) {
                it->second = available;
                if (auto mod = ll::mod::NativeMod::current()) mod->getLogger().info("Room roster peer advertises ysm:room={}", available);
            }
        }
    }
    for (auto& f : frames) {
        auto owner = std::find_if(roster.begin(), roster.end(), [&](auto const& p) { return p.second && *p.second == f.endpoint; });
        if (owner == roster.end() || !ll::network::Packet::canSendTo(wireId, f.endpoint)) continue;
        auto const& key = owner->first;
        auto it = impl->peers.find(key);
        if (it == impl->peers.end()) {
            if (f.frame.op != Op::Hello || impl->peers.size() >= maxPeers) continue;
            it = impl->peers.emplace(key, Peer{f.endpoint, {}, {}}).first;
        }
        auto& peer = it->second;
        auto& probe = peer.probe;
        if (f.frame.op == Op::Hello) {
            if (auto ack = probe.hello(f.frame, nonce)) writePacket(&peer.endpoint, *ack);
        } else if (f.frame.op == Op::Confirm) {
            bool duplicate = probe.live.matches(f.frame);
            bool existing = probe.live.client != Nonce{};
            auto candidate = probe;
            if (auto ready = candidate.confirm(f.frame)) {
                if (!writePacket(&peer.endpoint, *ready)) continue;
                probe = candidate;
                if (!duplicate) {
                    if (existing) impl->event(RoomWireKind::Left, key);
                    peer.outgoing = {};
                    impl->event(RoomWireKind::Joined, key);
                    if (auto mod = ll::mod::NativeMod::current()) mod->getLogger().info("Room transport host confirmed game connection and sent READY");
                }
            }
        } else if (f.frame.op == Op::Data && probe.live.matches(f.frame)) {
            impl->event(RoomWireKind::Data, key, std::move(f.body));
        }
    }
    // Cap aggregate work and rotate the starting connection between server ticks.
    auto it = impl->peers.upper_bound(impl->cursor);
    std::size_t budget = 128u << 10;
    for (auto remaining = impl->peers.size(); remaining && budget; --remaining) {
        if (it == impl->peers.end()) it = impl->peers.begin();
        auto& [id, peer] = *it++;
        if (peer.outgoing.empty()) continue;
        std::size_t allowance = impl->peers.size()==1?budget:std::min<std::size_t>(budget, maxPayload);
        auto before = allowance;
        auto sent=peer.outgoing.flush(allowance, [&](Bytes const& data) {
            return writePacket(&peer.endpoint, peer.probe.live.make(Op::Data), data);
        });
        impl->uploadRate.add(nowMillis(),sent);
        peer.downloadRate.add(nowMillis(),sent);
        budget -= before - allowance;
        impl->cursor = id;
    }
}
} // namespace ysm
