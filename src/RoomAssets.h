#pragma once
#include "ModelRelay.h"
#include <memory>
#include <future>
#include <set>
#include <span>

namespace ysm {
// Own on a serial file worker. Room authorization and revision checks belong to
// the caller, including when has() succeeds. No method touches game objects.
class RoomAssets {
    struct Impl;
    std::shared_ptr<Impl> impl;
public:
    struct PendingPublication;
    using Publication = std::shared_ptr<PendingPublication const>;
    static constexpr std::uint64_t maxFileBytes = 128ull << 20;
    static constexpr std::size_t maxChunkBytes = 64u << 10;
    explicit RoomAssets(std::filesystem::path cacheRoot, std::uint64_t capacityBytes = 512ull << 20);
    ~RoomAssets();
    RoomAssets(RoomAssets const&) = delete;
    RoomAssets& operator=(RoomAssets const&) = delete;
    void setCapacity(std::uint64_t bytes);
    void pin(std::set<std::string> digests);
    bool has(RelayAsset const& asset);
    std::filesystem::path readyPath(RelayAsset const& asset);
    bool reuseLocal(RelayAsset const& asset, std::filesystem::path const& candidate);
    bool reuseLibrary(RelayAsset const& asset, std::filesystem::path const& library);
    std::vector<unsigned char> read(RelayAsset const& asset, std::uint64_t offset, std::size_t maximum);
    void append(RelayAsset const& asset, std::uint64_t offset, std::span<unsigned char const> bytes);
    void finish(RelayAsset const& asset);
    void cancel(RelayAsset const& asset);
    void clearTransfers();
    // Futures own only a private staged file and a lease. Re-check session,
    // revision and room policy on the serial worker before commitPublish().
    std::future<Publication> beginPublish(std::filesystem::path const& source, std::string title, bool selectable = false);
    std::future<Publication> beginFinish(RelayAsset const& asset);
    RelayAsset commitPublish(Publication const& publication);
    RelayAsset publish(std::filesystem::path const& source, std::string title, bool selectable = false);
    ModelPack load(RelayAsset const& asset);
    std::filesystem::path collect(RelayAsset const& asset, std::filesystem::path const& modelsDirectory);
};
}
