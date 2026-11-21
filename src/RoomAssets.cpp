#include "RoomAssets.h"
#include "ContentHash.h"
#include "ModelLibrary.h"
#include "ModelResourceLimits.h"
#include <Windows.h>
#include <miniz.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <stdexcept>

namespace ysm {
namespace fs = std::filesystem;
namespace {
void require(bool ok, char const* reason) { if (!ok) throw std::runtime_error(reason); }

bool digestName(std::string_view name) {
    return name.size() == 64 && std::ranges::all_of(name, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool valid(RelayAsset const& asset) {
    return digestName(asset.digest) && asset.contentKey.starts_with("local1_")
        && digestName(std::string_view(asset.contentKey).substr(7))
        && (asset.extension == ".zip" || asset.extension == ".ysm")
        && asset.size && asset.size <= RoomAssets::maxFileBytes && asset.title.size() <= 512 && asset.sourceSpec == 2;
}
bool sameFile(RelayAsset const& a, RelayAsset const& b) {
    return a.contentKey == b.contentKey && a.digest == b.digest && a.extension == b.extension && a.size == b.size
        && a.sourceSpec == b.sourceSpec && a.originVersion == b.originVersion;
}
void describe(RelayAsset& asset, ModelPack const& pack) {
    asset.contentKey = pack.localContentKey;
    asset.sourceSpec = pack.descriptor.at("spec").get<unsigned>();
    asset.originVersion = pack.binaryOriginVersion();
}
bool matchesPack(RelayAsset const& asset, ModelPack const& pack) {
    return asset.contentKey == pack.localContentKey && asset.sourceSpec == pack.descriptor.at("spec").get<unsigned>()
        && asset.originVersion == pack.binaryOriginVersion();
}
bool reparse(fs::path const& path) {
    auto flags = GetFileAttributesW(path.c_str());
    return flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool regular(fs::path const& path) {
    std::error_code ec;
    return !reparse(path) && fs::is_regular_file(path, ec);
}
void safeRoot(fs::path const& path) {
    fs::path current;
    for (auto const& part : fs::absolute(path).lexically_normal()) {
        current /= part;
        require(!reparse(current), "Room storage path crosses a reparse point");
    }
}
void checkShareSource(fs::path const& path) {
    safeRoot(path);
    for (auto const& part : fs::weakly_canonical(path))
        require(_wcsicmp(part.c_str(), L"auth") != 0,
            "Session-authorized model root cannot be redistributed without a grant");
}
struct RemoveFile {
    fs::path path;
    ~RemoveFile() { std::error_code ignored; fs::remove(path, ignored); }
};
std::string uniqueName() {
    static std::atomic<std::uint64_t> id{};
    return std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(GetTickCount64()) + "-" + std::to_string(++id);
}
using File = std::unique_ptr<FILE, decltype(&fclose)>;
File openFile(fs::path const& path, wchar_t const* mode) {
    FILE* f{};
    require(!_wfopen_s(&f, path.c_str(), mode) && f, "Cannot open room model file");
    return File(f, &fclose);
}

// Each archive member is an input actually read by the importer. Packing a
// model directory must never expose its siblings or unrelated files within it.
void packDirectory(fs::path const& src, fs::path const& dest) {
    std::map<std::string, fs::path> members;
    ModelPack::loadRoom(src, &members);
    require(!members.empty() && members.size() <= 4096, "Invalid number of exported model resources");
    auto out = openFile(dest, L"wb");
    mz_zip_archive zip{};
    require(mz_zip_writer_init_cfile(&zip, out.get(), 0), "Cannot create room ZIP");
    struct Writer { mz_zip_archive* zip; ~Writer() { mz_zip_writer_end(zip); } } writer{&zip};
    // miniz writes local DOS dates. The same local calendar date gives a
    // reproducible archive across machines and independently of source mtimes.
    std::tm date{};
    date.tm_year = 100; date.tm_mon = 0; date.tm_mday = 1; date.tm_hour = 12; date.tm_isdst = -1;
    MZ_TIME_T time = std::mktime(&date);
    std::uint64_t total{};
    for (auto const& [name, path] : members) {
        checkShareSource(path);
        auto size = fs::file_size(path);
        require(size <= MaxModelResourceFile && (total += size) <= (256ull << 20), "Exported model exceeds size limit");
        auto f = openFile(path, L"rb");
        require(mz_zip_writer_add_cfile(&zip, name.c_str(), f.get(), size, &time, nullptr, 0,
            MZ_BEST_SPEED, nullptr, 0, nullptr, 0), "Cannot write model resource into room ZIP");
        require(zip.m_archive_size <= RoomAssets::maxFileBytes, "Exported model ZIP is too large");
    }
    require(mz_zip_writer_finalize_archive(&zip) && zip.m_archive_size <= RoomAssets::maxFileBytes,
        "Cannot finish room model ZIP");
}
std::string extensionFor(fs::path const& source) {
    if (fs::is_directory(source)) return ".zip";
    require(regular(source), "Room model source is not a regular file");
    if (!_wcsicmp(source.extension().c_str(), L".zip")) return ".zip";
    if (!_wcsicmp(source.extension().c_str(), L".ysm")) return ".ysm";
    throw std::runtime_error("Unsupported room model format");
}
void snapshot(fs::path const& source, fs::path const& destination) {
    if (fs::is_directory(source)) packDirectory(source, destination);
    else {
        auto size = fs::file_size(source);
        require(size && size <= RoomAssets::maxFileBytes, "Room model exceeds transfer limit");
        // Bound the copy even if an editor is writing the selected file.
        std::ifstream in(source, std::ios::binary);
        std::ofstream out(destination, std::ios::binary | std::ios::trunc);
        require(in && out, "Cannot snapshot room model");
        char block[65536];
        while (size) {
            auto count = static_cast<std::streamsize>(std::min<std::uintmax_t>(size, sizeof(block)));
            require(bool(in.read(block, count)) && bool(out.write(block, count)), "Cannot snapshot room model");
            size -= static_cast<std::uintmax_t>(count);
        }
        require(in.peek() == std::char_traits<char>::eof() && !in.bad() && bool(out.flush()), "Source changed during snapshot");
    }
}
}

struct RoomAssets::Impl {
    struct Transfer { RelayAsset asset; fs::path path; std::uint64_t written{}; bool paused{}; };
    struct Verified { RelayAsset asset; fs::file_time_type modified, used; };
    struct Verification { fs::path destination; std::uint64_t size; std::weak_ptr<PendingPublication> job; };
    fs::path root, stage;
    std::uint64_t capacity;
    std::map<std::string, Transfer> transfers;
    std::map<fs::path, Verified> verified;
    std::set<std::string> pins;
    std::atomic<unsigned> exporting{};
    std::vector<Verification> verifications;
    fs::path privateLibrary;
    std::chrono::steady_clock::time_point nextLibraryScan;
    std::map<std::pair<std::string, std::uintmax_t>, std::vector<fs::path>> privateArchives;
    struct Probe { fs::file_time_type modified; std::uintmax_t size; std::string digest; };
    std::map<fs::path, Probe> probes;
    std::set<std::string> triedPrivate;
    HANDLE lease = INVALID_HANDLE_VALUE;

    Impl(fs::path dir, std::uint64_t bytes) : root(fs::absolute(std::move(dir)).lexically_normal()), capacity(bytes) {
        safeRoot(root);
        fs::create_directories(root);
        for (auto const& entry : fs::directory_iterator(root)) {
            auto name = entry.path().filename().native();
            if (!name.starts_with(L".room-stage-") || reparse(entry.path()) || !entry.is_directory()) continue;
            // A running client holds its lease without sharing. A crashed
            // client releases the handle automatically, permitting recovery.
            auto guard = entry.path() / L"owner.lock";
            if (!regular(guard)) continue;
            HANDLE orphan = CreateFileW(guard.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (orphan == INVALID_HANDLE_VALUE) continue;
            CloseHandle(orphan);
            std::error_code ec;
            for (fs::directory_iterator file(entry.path(), ec), end; !ec && file != end; file.increment(ec)) {
                if (regular(file->path())) fs::remove(file->path(), ec);
            }
            fs::remove(entry.path(), ec);
        }
        stage = root / (".room-stage-" + uniqueName());
        require(fs::create_directory(stage), "Cannot create room transfer staging area");
        lease = CreateFileW((stage / L"owner.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lease == INVALID_HANDLE_VALUE) {
            std::error_code ec; fs::remove(stage, ec);
            throw std::runtime_error("Cannot lock room transfer staging area");
        }
    }
    ~Impl() {
        CloseHandle(lease);
        std::error_code ec;
        for (fs::directory_iterator file(stage, ec), end; !ec && file != end; file.increment(ec))
            if (regular(file->path())) fs::remove(file->path(), ec);
        fs::remove(stage, ec);
    }
    fs::path path(RelayAsset const& asset) const { return root / (asset.digest + asset.extension); }
    void forgetFinishedVerifications() {
        std::erase_if(verifications, [](Verification const& entry) { return entry.job.expired(); });
    }
    bool reserve(std::uint64_t additional, fs::path const& replacing = {}) {
        if (additional > capacity) return false;
        struct Candidate { fs::path path; fs::file_time_type used; std::uint64_t size; };
        std::vector<Candidate> candidates;
        // Count each final path once across receive, verification and atomic replacement.
        // Other admissions must still allow for the old file in case the repair fails.
        std::map<fs::path, std::uint64_t> reserved;
        for (auto const& [digest, transfer] : transfers) reserved[path(transfer.asset)] = transfer.paused ? transfer.written : transfer.asset.size;
        for (auto const& entry : verifications) {
            if (!entry.job.expired()) reserved[entry.destination] = entry.size;
        }
        if (!replacing.empty()) {
            auto& space = reserved[replacing];
            space = std::max(space, additional);
        }
        std::uint64_t total{};
        for (auto const& [dest, size] : reserved) total += size;
        for (auto const& entry : fs::directory_iterator(root)) {
            auto path = entry.path();
            auto name = path.stem().u8string();
            std::string digest(name.begin(), name.end());
            if (!digestName(digest) || (path.extension() != L".zip" && path.extension() != L".ysm") || !regular(path)) continue;
            auto size = entry.file_size();
            if (auto pending = reserved.find(path); pending != reserved.end()) {
                if (path != replacing && size > pending->second) total += size - pending->second;
                continue;
            }
            total += size;
            if (pins.contains(digest) || transfers.contains(digest)) continue;
            auto saved = verified.find(path);
            candidates.push_back({path, saved == verified.end() ? entry.last_write_time() : saved->second.used, size});
        }
        if (total <= capacity) return true;
        if (additional) {
            auto minimum = total;
            for (auto const& c : candidates) minimum -= c.size;
            if (minimum > capacity) return false;
        }
        std::ranges::sort(candidates, {}, &Candidate::used);
        for (auto const& c : candidates) {
            if (total <= capacity) break;
            std::error_code ec;
            if (fs::remove(c.path, ec)) { verified.erase(c.path); total -= c.size; }
        }
        return total <= capacity;
    }
    void remember(RelayAsset const& asset) {
        auto p = path(asset);
        verified[p] = {asset, fs::last_write_time(p), fs::file_time_type::clock::now()};
    }
    bool present(RelayAsset const& asset) {
        if (!valid(asset)) return false;
        auto p = path(asset);
        if (!regular(p) || fs::file_size(p) != asset.size) return false;
        auto v = verified.find(p);
        auto stamp = fs::last_write_time(p);
        if (v != verified.end() && v->second.modified == stamp) {
            v->second.used = fs::file_time_type::clock::now();
            return sameFile(asset, v->second.asset);
        }
        // Keep negative results for this exact file stamp too. A damaged
        // restart cache must not trigger a full hash/parse on every tick.
        auto stored = asset;
        stored.contentKey.clear();
        if (contentFileSha256(p, asset.size) == asset.digest) {
            try { describe(stored, ModelPack::loadRoom(p)); }
            catch (std::exception const&) { /* A valid replacement can repair this entry. */ }
        }
        if (!regular(p) || fs::file_size(p) != asset.size || fs::last_write_time(p) != stamp) return false;
        verified[p] = {stored, stamp, fs::file_time_type::clock::now()};
        return sameFile(stored, asset);
    }
    void commit(RelayAsset const& asset, fs::path const& source) {
        auto final = path(asset);
        require(!reparse(final), "Unsafe room cache destination");
        // A corrupt file can be repaired. A wrong claimed content key cannot
        // overwrite a sound file: source has already passed both identity checks.
        require(MoveFileExW(source.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
            "Cannot atomically commit verified room model");
        remember(asset);
    }
};
struct RoomAssets::PendingPublication {
    std::shared_ptr<Impl> owner;
    fs::path staged;
    RelayAsset asset;
    bool received;
    PendingPublication(std::shared_ptr<Impl> files, fs::path temp, RelayAsset manifest, bool incoming)
        : owner(std::move(files)), staged(std::move(temp)), asset(std::move(manifest)), received(incoming) {
        if (!received) ++owner->exporting;
    }
    ~PendingPublication() {
        std::error_code ec;
        fs::remove(staged, ec);
        if (!received) --owner->exporting;
    }
};
RoomAssets::RoomAssets(fs::path cacheRoot, std::uint64_t capacityBytes)
    : impl(std::make_shared<Impl>(std::move(cacheRoot), capacityBytes)) {}
RoomAssets::~RoomAssets() = default;
void RoomAssets::setCapacity(std::uint64_t bytes) { impl->capacity = bytes; impl->reserve(0); }
void RoomAssets::pin(std::set<std::string> digests) { impl->pins = std::move(digests); }
bool RoomAssets::has(RelayAsset const& asset) {
    try { return impl->present(asset); } catch (std::exception const&) { return false; }
}
fs::path RoomAssets::readyPath(RelayAsset const& asset) {
    require(has(asset), "Room model has not been verified");
    return impl->path(asset);
}
ModelPack RoomAssets::load(RelayAsset const& asset) {
    auto result = ModelPack::loadRoom(readyPath(asset));
    require(matchesPack(asset, result), "Room model content or format changed during preparation");
    return result;
}
std::vector<unsigned char> RoomAssets::read(RelayAsset const& asset, std::uint64_t offset, std::size_t maximum) {
    require(offset < asset.size && maximum && maximum <= maxChunkBytes, "Invalid room chunk range");
    std::ifstream input(readyPath(asset), std::ios::binary);
    auto size = static_cast<std::size_t>(std::min<std::uint64_t>(maximum, asset.size - offset));
    std::vector<unsigned char> bytes(size);
    require(bool(input.seekg(static_cast<std::streamoff>(offset)).read(reinterpret_cast<char*>(bytes.data()), size)),
        "Cannot read room model chunk");
    return bytes;
}
void RoomAssets::append(RelayAsset const& asset, std::uint64_t offset, std::span<unsigned char const> bytes) {
    require(valid(asset) && !bytes.empty() && bytes.size() <= maxChunkBytes && offset < asset.size
        && bytes.size() <= asset.size - offset, "Invalid incoming room chunk");
    auto it = impl->transfers.find(asset.digest);
    if (it == impl->transfers.end()) {
        impl->forgetFinishedVerifications();
        auto destination = impl->path(asset);
        auto active=std::count_if(impl->transfers.begin(),impl->transfers.end(),[](auto const& t){return !t.second.paused;});
        require(!offset && active + impl->verifications.size() < 4 && impl->transfers.size()<8
            && std::ranges::none_of(impl->verifications, [&](auto const& job) { return job.destination == destination; })
            && impl->reserve(asset.size, destination),
            "Room cache full or unexpected chunk offset");
        auto file = impl->stage / (uniqueName() + asset.extension);
        it = impl->transfers.emplace(asset.digest, Impl::Transfer{asset, file}).first;
    }
    auto& tx = it->second;
    require(sameFile(asset, tx.asset) && offset == tx.written, "Room model metadata or chunk order changed");
    if(tx.paused){
        require(impl->reserve(asset.size,impl->path(asset)) && regular(tx.path) && fs::file_size(tx.path)==offset,
            "Cannot resume staged room model");
        tx.paused=false;
    }
    std::ofstream output(tx.path, std::ios::binary | (offset ? std::ios::app : std::ios::trunc));
    output.write(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    output.close();
    if (!output) { cancel(asset); throw std::runtime_error("Cannot write room model chunk"); }
    tx.written += bytes.size();
}
void RoomAssets::pause(RelayAsset const& asset) {
    auto it=impl->transfers.find(asset.digest);
    if(it!=impl->transfers.end() && sameFile(asset,it->second.asset))it->second.paused=true;
}
bool RoomAssets::reserveTransfer(RelayAsset const& asset) {
    return valid(asset) && impl->reserve(asset.size,impl->path(asset));
}
void RoomAssets::cancel(RelayAsset const& asset) {
    auto it = impl->transfers.find(asset.digest);
    if (it == impl->transfers.end()) return;
    std::error_code ignored; fs::remove(it->second.path, ignored);
    impl->transfers.erase(it);
}
void RoomAssets::clearTransfers() {
    while (!impl->transfers.empty()) cancel(impl->transfers.begin()->second.asset);
}
std::future<RoomAssets::Publication> RoomAssets::beginFinish(RelayAsset const& asset) {
    auto it = impl->transfers.find(asset.digest);
    require(it != impl->transfers.end() && sameFile(asset, it->second.asset), "Unknown room transfer");
    auto tx = std::move(it->second);
    impl->transfers.erase(it);
    auto pending = std::make_shared<PendingPublication>(impl, std::move(tx.path), asset, true);
    require(tx.written == asset.size, "Room file size mismatch");
    impl->verifications.push_back({impl->path(asset), asset.size, pending});
    return std::async(std::launch::async, [pending]() -> Publication {
        auto const& a = pending->asset;
        require(regular(pending->staged) && fs::file_size(pending->staged) == a.size
            && contentFileSha256(pending->staged, a.size) == a.digest, "Room file size or SHA-256 mismatch");
        require(matchesPack(a, ModelPack::loadRoom(pending->staged)), "Room source content or format metadata mismatch");
        return pending;
    });
}
std::future<RoomAssets::Publication> RoomAssets::beginPublish(fs::path const& source, std::string title, bool selectable) {
    require(title.size() <= 512 && impl->exporting.load() < 2, "Too many room exports or title too long");
    auto extension = extensionFor(source);
    RelayAsset manifest{{}, {}, extension, 0, std::move(title), selectable};
    auto pending = std::make_shared<PendingPublication>(impl, impl->stage / ("export-" + uniqueName() + extension),
        std::move(manifest), false);
    return std::async(std::launch::async, [pending, source]() -> Publication {
        checkShareSource(source);
        snapshot(source, pending->staged);
        auto size = fs::file_size(pending->staged);
        require(size && size <= maxFileBytes, "Invalid room model advertisement");
        auto& asset = pending->asset;
        asset.size = static_cast<std::size_t>(size);
        describe(asset, ModelPack::loadRoom(pending->staged));
        asset.digest = contentFileSha256(pending->staged, size);
        return pending;
    });
}
RelayAsset RoomAssets::commitPublish(Publication const& publication) {
    require(publication && publication->owner == impl && valid(publication->asset), "Invalid room publication");
    auto const& asset = publication->asset;
    auto dest = impl->path(asset);
    auto old = impl->verified.find(dest);
    if (old != impl->verified.end() && sameFile(old->second.asset, asset) && regular(dest)
        && fs::file_size(dest) == asset.size && fs::last_write_time(dest) == old->second.modified) return asset;
    require(regular(publication->staged) && fs::file_size(publication->staged) == asset.size, "Prepared room file unavailable");
    require(impl->reserve(asset.size, dest), "Room cache capacity exceeded");
    impl->commit(asset, publication->staged);
    return asset;
}
void RoomAssets::finish(RelayAsset const& asset) { commitPublish(beginFinish(asset).get()); }
RelayAsset RoomAssets::publish(fs::path const& source, std::string title, bool selectable) {
    return commitPublish(beginPublish(source, std::move(title), selectable).get());
}
bool RoomAssets::reuseLocal(RelayAsset const& asset, fs::path const& candidate) {
    if (has(asset)) return true;
    if (!valid(asset)) return false;
    try {
        checkShareSource(candidate);
        if (extensionFor(candidate) != asset.extension) return false;
        if (regular(candidate) && fs::file_size(candidate) != asset.size) return false;
        RemoveFile cleanup{impl->stage / ("local-" + uniqueName() + asset.extension)};
        snapshot(candidate, cleanup.path);
        if (fs::file_size(cleanup.path) != asset.size || contentFileSha256(cleanup.path, asset.size) != asset.digest
            || !matchesPack(asset, ModelPack::loadRoom(cleanup.path)) || !impl->reserve(asset.size, impl->path(asset))) return false;
        impl->commit(asset, cleanup.path);
        return true;
    } catch (std::exception const&) { return false; }
}
bool RoomAssets::reuseLibrary(RelayAsset const& asset, fs::path const& library) {
    if (has(asset)) return true;
    if (!valid(asset)) return false;
    try {
        auto root = fs::absolute(library).lexically_normal();
        auto now = std::chrono::steady_clock::now();
        if (impl->privateLibrary != root || now >= impl->nextLibraryScan) {
            impl->privateLibrary = root;
            impl->nextLibraryScan = now + std::chrono::seconds(30);
            impl->privateArchives.clear();
            impl->triedPrivate.clear();
            safeRoot(root);
            for (auto const& file : ModelLibrary::localArchiveFiles(root)) {
                if (!regular(file)) continue;
                std::error_code ec;
                auto size = fs::file_size(file, ec);
                if (!ec && size && size <= maxFileBytes)
                    impl->privateArchives[{extensionFor(file), size}].push_back(file);
            }
            if (impl->probes.size() > 4096) impl->probes.clear();
        }
        if (impl->triedPrivate.size() >= 1024 || !impl->triedPrivate.insert(asset.digest).second) return false;
        if (auto group = impl->privateArchives.find({asset.extension, asset.size}); group != impl->privateArchives.end()) {
            for (auto const& candidate : group->second) {
                std::error_code ec;
                auto modified = fs::last_write_time(candidate, ec);
                if (ec || !regular(candidate)) continue;
                auto old = impl->probes.find(candidate);
                if (old == impl->probes.end() || old->second.modified != modified || old->second.size != asset.size) {
                    try {
                        checkShareSource(candidate);
                        auto hash = contentFileSha256(candidate, asset.size);
                        old = impl->probes.insert_or_assign(candidate, Impl::Probe{modified, asset.size, std::move(hash)}).first;
                    } catch (std::exception const&) { continue; }
                }
                if (old->second.digest == asset.digest && reuseLocal(asset, candidate)) return true;
            }
        }
        for (auto const& folder : ModelLibrary::loadedDirectories(root, asset.contentKey))
            if (reuseLocal(asset, folder)) return true;
    } catch (std::exception const&) { /* An unavailable local source falls back to the negotiated download. */ }
    return false;
}

fs::path RoomAssets::collect(RelayAsset const& asset, fs::path const& library) {
    auto src = readyPath(asset);
    safeRoot(library);
    fs::create_directories(library);
    auto dst = library / ("room-" + asset.digest + asset.extension);
    auto matches = [&] { return regular(dst) && fs::file_size(dst) == asset.size
        && contentFileSha256(dst, asset.size) == asset.digest; };
    if (matches()) return dst;
    require(!fs::exists(dst) && !reparse(dst), "Model library destination already exists with different content");
    RemoveFile temp{library / (".room-collect-" + uniqueName() + ".tmp")};
    fs::copy_file(src, temp.path);
    require(fs::file_size(temp.path) == asset.size && contentFileSha256(temp.path, asset.size) == asset.digest,
        "Room cache changed during collection");
    require(MoveFileExW(temp.path.c_str(), dst.c_str(), MOVEFILE_WRITE_THROUGH) || matches(),
        "Cannot finalize collected model");
    return dst;
}
}
