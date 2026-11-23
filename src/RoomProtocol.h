#pragma once
#include "ModelRelay.h"
#include <memory>
#include <set>

namespace ysm {
// A connection-confirmed player id is supplied by the transport. A guest uses
// the empty endpoint for its single authenticated host connection.
struct RoomWire {
    std::string peer;
    std::vector<unsigned char> bytes;
    bool control = true;
    std::string assetKey, assetDigest;
    std::size_t modelBytes{};
};
enum class RoomPriority { Visible, Current, Selected, Background };
struct RoomRules {
    bool autoReceiveCurrent = true;
    std::set<std::string> deniedKeys;
    bool sharedOnly = false;
    static constexpr int defaultBytesPerSecond = 1024 * 1024;
    std::size_t bytesPerSecond = defaultBytesPerSecond;
    std::set<std::string> sessionDenied;
};
struct RoomFiles {
    std::function<bool(RelayAsset const&)> has;
    std::function<std::vector<unsigned char>(RelayAsset const&, std::size_t, std::size_t)> read;
    std::function<void(RelayAsset const&, std::size_t, std::span<unsigned char const>)> append;
    std::function<void(RelayAsset const&)> finish;
    std::function<void(RelayAsset const&)> cancel;
    // Optional asynchronous finish, called from the same owner thread.
    // Return 0 while verifying, +1 on commit, or -1 on failure.
    std::function<int(RelayAsset const&)> pollFinish;
    std::function<void(RelayAsset const&)> pause;
    std::function<bool(RelayAsset const&)> reserve;
};
struct RoomProgress {
    std::size_t received{}, total{}, upstream{};
    std::uint64_t bytesPerSecond{}, upstreamBytesPerSecond{};
};
struct RoomPeerRate {std::uint64_t upload{},download{};};
using RoomPeerRates=std::map<std::string,RoomPeerRate>;
struct RoomDisplay {
    std::string stream, stage;
    bool operator==(RoomDisplay const&) const = default;
};
using RoomDisplays = std::map<std::string,RoomDisplay>;
struct RoomOffer { std::string peer; RelayAsset asset; };
class RoomProtocol {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    static constexpr std::size_t chunkSize = 16 * 1024, maxWire = 64 * 1024;
    // freshToken is a fresh random 32-character lowercase hex token per connection/world.
    RoomProtocol(bool host, std::string self, std::string freshToken, RoomFiles files);
    ~RoomProtocol();
    RoomProtocol(RoomProtocol const&) = delete;
    // Single owner thread, including file callbacks. nowMs is monotonic milliseconds.
    void joined(std::string const& confirmedPeer);
    void left(std::string const& confirmedPeer);
    void receive(std::string const& confirmedPeer, std::span<unsigned char const> bytes, std::uint64_t nowMs);
    std::vector<RoomWire> tick(std::uint64_t nowMs);
    void setRules(RoomRules rules);
    // Only pass explicitly shared models. On guests, these are offers to the host.
    void share(std::vector<RelayAsset> assets);
    std::vector<RoomOffer> offers() const;
    // Host settings remain editable when a denied model or its wearer leaves the catalog.
    std::map<std::string,std::string> deniedModels() const;
    bool acceptOffer(std::string const& peer, std::string const& contentKey);
    // Pass an empty asset to restore vanilla or withdraw an unavailable/protected model.
    void publish(std::optional<RelayAsset> asset, RelayPose pose);
    void serverArrow(std::string const& shooter,std::int64_t entity,bool crossbow);
    void setVisible(std::set<std::string> peerIds);
    void select(std::string contentKey); // raise a catalog entry before switching or collecting
    void retry(std::string const& subject, std::string const& stream);
    bool canRetryUpload() const;
    void retryUpload();
    std::map<std::string,RoomProgress> progress() const;
    std::uint64_t downloadBytesPerSecond() const;
    // Host measures each connection. downToPeer counts bytes passed to Bedrock.
    void updateRates(std::map<std::string,std::uint64_t> const& downToPeer,std::uint64_t ownUpload,std::uint64_t nowMs);
    RoomPeerRates peerRates() const;
    void reportDisplays(RoomDisplays const& displays);
    std::map<std::string,RoomDisplays> observers() const;
    RelayFrame const& frame() const; // permitted state only, excludes self
    RoomPriority priority(std::string const& contentKey) const;
    // Keys are localContentKeys. States: waiting, downloading, verifying, cached, denied, failed.
    std::map<std::string,std::string> const& statuses() const;
    // Only public outcomes are exposed for refused players. Disallowed asset
    // metadata and private presentation never travel to the other guests.
    std::map<std::string,std::string> const& peerStates() const;
    bool maySendChunk(std::string const& peer,std::string const& key,std::string const& digest) const;
    std::string const& session() const;
    bool connected() const;
};
}
