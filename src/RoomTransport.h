#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class IClientInstance;
class Level;
class Player;

namespace ysm {

inline constexpr std::string_view roomHostPeer = "@host";
enum class RoomRole { None, Host, Guest };
enum class RoomWireKind { Joined, Left, Data };
struct RoomWireEvent {
    RoomWireKind kind;
    std::string peer;
    std::vector<std::uint8_t> payload;
};

// A connection can carry application frames only after it echoes both endpoint challenges.
std::string roomPlayerKey(Player& player);
class RoomTransport {
public:
    RoomTransport();
    ~RoomTransport();
    RoomTransport(RoomTransport const&) = delete;
    RoomTransport& operator=(RoomTransport const&) = delete;
    bool start();
    void stop();
    void reset();
    void serverTick(Level& level);
    void clientTick(IClientInstance& client, Player* local);
    RoomRole role() const;
    std::optional<int> latencyMs() const;
    std::string localId() const;
    std::vector<RoomWireEvent> drain();
    bool send(std::string_view peer, std::span<std::uint8_t const> payload, bool control = true,
              std::function<bool()> authorized = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ysm
