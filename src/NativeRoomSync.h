#pragma once
#include "RoomProtocol.h"
#include "RoomTransport.h"
#include <filesystem>

namespace ysm {
struct RoomView {
    RelayFrame frame;
    std::map<std::string,std::filesystem::path> ready;
    std::map<std::string,std::string> states, peerStates;
    std::vector<RoomOffer> offers;
    std::map<std::string,std::string> deniedModels;
    std::string message;
    bool connected{}, autoReceive=true;
    std::uint64_t acceptedTxBytes{}, receivedBytes{};
};
// The client tick owns transport I/O. The serial worker owns the protocol,
// authorization and disk cache. Only copied values cross this boundary.
class NativeRoomSync {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    NativeRoomSync();
    ~NativeRoomSync();
    void start(std::filesystem::path const& root);
    void stop();
    void reset();
    void serverTick(Level&);
    void tick(IClientInstance&, Player*);
    bool active() const;
    bool host() const;
    bool hasEndpoints() const;
    bool backpressured() const;
    std::string localId() const;
    std::optional<int> latencyMs() const;
    std::shared_ptr<RoomView const> view() const;
    void update(std::string source, std::string title, RelayPose pose, std::set<std::string> visible);
    void command(std::string command);
};
}
