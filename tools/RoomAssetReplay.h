#pragma once
#include "RoomAssets.h"
#include "ModelLibrary.h"
#include <fstream>

namespace ysm {
// Run the production on-disk receiver twice to exercise provider -> host -> guest.
inline Json roomAssetReplay(std::filesystem::path const& base, std::filesystem::path const& source) {
    namespace fs = std::filesystem;
    if (fs::exists(base)) throw std::runtime_error("Room replay requires a new temporary directory");
    fs::create_directories(base);
    RoomAssets provider(base / "provider"), host(base / "host"), guest(base / "guest");
    auto asset = provider.publish(source, "replay", true);
    auto copy = [&](RoomAssets& from, RoomAssets& to) {
        std::size_t at{}, chunks{};
        while (at < asset.size) {
            auto chunk = from.read(asset, at, std::min<std::size_t>(257, asset.size - at));
            to.append(asset, at, chunk);
            at += chunk.size(); ++chunks;
        }
        if (to.has(asset)) throw std::runtime_error("Incomplete transfer became available");
        auto prepared = to.beginFinish(asset).get();
        if (to.has(asset)) throw std::runtime_error("Uncommitted verified transfer was already exposed");
        to.commitPublish(prepared);
        return chunks;
    };
    auto chunks = copy(provider, host);
    copy(host, guest);
    guest.readyPath(asset);
    RoomAssets localReuse(base / "private");
    bool directoryMiss = !fs::is_directory(source) || !localReuse.reuseLibrary(asset, source.parent_path());
    if (fs::is_directory(source)) {
        ModelLibrary library(source.parent_path(), base / "selection.json");
        auto name = source.filename().u8string();
        if (!library.select(std::string(name.begin(), name.end())).pack)
            throw std::runtime_error("Could not load private model folder through the library");
    }
    RoomAssets secondLookup(base / "private-after-select");
    bool reused = secondLookup.reuseLibrary(asset, source.parent_path());
    auto wrong = asset; wrong.contentKey.back() = wrong.contentKey.back() == 'a' ? 'b' : 'a';
    auto wrongSpec = asset; ++wrongSpec.sourceSpec;
    auto wrongOrigin = asset;
    wrongOrigin.originVersion = asset.originVersion ? std::nullopt : std::optional<unsigned>(0);
    auto verified = guest.load(asset).localContentKey;
    auto denied = [&](auto&& action) {
        try { action(); return false; } catch (std::exception const&) { return true; }
    };
    std::future<RoomAssets::Publication> stale;
    auto oldRoot = base / "departed-room";
    {
        RoomAssets departed(oldRoot);
        stale = departed.beginPublish(source, "outdated");
    }
    auto obsolete = stale.get();
    auto wrongWorld = denied([&] { guest.commitPublish(obsolete); });
    obsolete.reset();
    bool noStaleFile = fs::is_empty(oldRoot);
    RoomAssets broken(base / "broken");
    bool formatRejected = !guest.has(wrongSpec) && !guest.has(wrongOrigin) && !broken.reuseLocal(wrongOrigin, source);
    for (std::size_t at = 0; at < asset.size;) {
        auto chunk = provider.read(asset, at, std::min<std::size_t>(RoomAssets::maxChunkBytes, asset.size - at));
        broken.append(wrongOrigin, at, chunk);
        at += chunk.size();
    }
    formatRejected = denied([&] { broken.finish(wrongOrigin); }) && formatRejected;
    auto first = provider.read(asset, 0, std::min<std::size_t>(asset.size, RoomAssets::maxChunkBytes));
    auto wrongOffset = denied([&] { broken.append(asset, 1, first); });
    broken.append(asset, 0, first);
    broken.clearTransfers();
    auto canceled = denied([&] { broken.finish(asset); });
    first[0] ^= 1;
    broken.append(asset, 0, first);
    for (auto at = first.size(); at < asset.size;) {
        auto chunk = provider.read(asset, at, std::min<std::size_t>(RoomAssets::maxChunkBytes, asset.size - at));
        broken.append(asset, at, chunk); at += chunk.size();
    }
    auto rejected = denied([&] { broken.finish(asset); });
    RoomAssets rollback(base / "rollback", asset.size - 1);
    bool capacity = denied([&] { rollback.append(asset, 0, first); });
    bool collectedOnlyOnRequest = !fs::exists(base / "library");
    auto saved = guest.collect(asset, base / "library");
    auto savedAgain = guest.collect(asset, base / "library");
    auto deniedContent = !guest.has(wrong);
    fs::path persistent = base / "reopen";
    {
        RoomAssets cold(persistent);
        copy(provider, cold);
    }
    bool restart{};
    {
        RoomAssets reopened(persistent);
        restart = !reopened.has(wrongOrigin) && !reopened.has(wrong) && reopened.has(asset);
        auto path = reopened.readyPath(asset);
        std::fstream corrupt(path, std::ios::binary | std::ios::in | std::ios::out);
        char head{};
        corrupt.read(&head, 1);
        head ^= 1; corrupt.seekp(0).write(&head, 1);
    }
    RoomAssets repaired(persistent, asset.size);
    repaired.pin({asset.digest});
    auto fallback = !repaired.has(asset);
    bool localRepair{};
    {
        auto root = base / "repair-private";
        RoomAssets privateRepair(root, asset.size);
        privateRepair.pin({asset.digest});
        auto damaged = root / (asset.digest + asset.extension);
        fs::copy_file(persistent / damaged.filename(), damaged);
        std::ofstream(damaged, std::ios::binary | std::ios::app).put('x');
        localRepair = !privateRepair.has(asset) && privateRepair.reuseLocal(asset, source) && privateRepair.has(asset);
    }
    bool parallelRepair{};
    {
        auto root = base / "repair-concurrent";
        auto file = asset.digest + asset.extension;
        RoomAssets concurrent(root, asset.size * 2);
        concurrent.pin({asset.digest});
        fs::copy_file(persistent / file, root / file);
        // Start, then cancel, an independent download at both boundaries of the repair.
        auto other = asset;
        other.digest.front() = other.digest.front() == 'a' ? 'b' : 'a';
        auto chunk = provider.read(asset, 0, 1);
        concurrent.append(asset, 0, chunk);
        concurrent.append(other, 0, chunk);
        concurrent.cancel(other);
        for (auto at = chunk.size(); at < asset.size;) {
            auto next = provider.read(asset, at, RoomAssets::maxChunkBytes);
            concurrent.append(asset, at, next);
            at += next.size();
        }
        auto prepared = concurrent.beginFinish(asset).get();
        concurrent.append(other, 0, chunk);
        concurrent.commitPublish(prepared);
        concurrent.cancel(other);
        parallelRepair = concurrent.has(asset) && !concurrent.has(other);
    }
    copy(provider, repaired);
    guest.pin({asset.digest});
    guest.setCapacity(0);
    auto pinned = guest.has(asset);
    guest.pin({});
    guest.setCapacity(0);
    auto evicted = !guest.has(asset);
    auto path = saved.u8string();
    return {{"digest", asset.digest}, {"content", asset.contentKey}, {"size", asset.size}, {"chunks", chunks},
        {"spec", asset.sourceSpec}, {"binary_origin", asset.originVersion ? Json(*asset.originVersion) : Json(nullptr)},
        {"false_format_rejected", formatRejected},
        {"guest", verified}, {"cached", host.has(asset)}, {"private_reused", reused}, {"unopened_directory_private", directoryMiss},
        {"wrong_key_rejected", deniedContent}, {"restart_hit", restart}, {"disk_corruption_repaired", fallback && repaired.has(asset)},
        {"stale_world_rejected", wrongWorld}, {"abandoned_stage_cleaned", noStaleFile},
        {"wrong_offset_rejected", wrongOffset}, {"canceled", canceled}, {"damaged_rejected", rejected && !broken.has(asset)},
        {"capacity_enforced", capacity}, {"pinned", pinned}, {"evicted", evicted}, {"full_cache_local_repair", localRepair},
        {"parallel_repair", parallelRepair},
        {"manual_collection", collectedOnlyOnRequest},
        {"collection_idempotent", savedAgain == saved}, {"collected", std::string(path.begin(), path.end())}};
}
}
