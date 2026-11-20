#include "ModelRelay.h"
#include "ContentHash.h"
#include <Windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <algorithm>
#include <cmath>
#include <charconv>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace ysm {
void RelayPreparationQueue::failed(RelayPose const& peer, Clock::time_point now) {
    failures[peer.xuid] = {peer.session, peer.contentKey, peer.texture, now + std::chrono::seconds(2)};
}

RelayPose const* RelayPreparationQueue::next(RelayFrame const& frame, Clock::time_point now,
    std::function<bool(std::string const&)> const& eligible) {
    std::erase_if(failures, [&](auto const& item) {
        auto found = frame.players.find(item.first);
        auto const& retry = item.second;
        return now >= retry.after || found == frame.players.end() || found->second.session != retry.session
            || found->second.contentKey != retry.model || found->second.texture != retry.texture;
    });
    auto it = frame.players.upper_bound(lastTried);
    for (auto remaining = frame.players.size(); remaining; --remaining) {
        if (it == frame.players.end()) it = frame.players.begin();
        auto const& [id, pose] = *it++;
        if (!failures.contains(id) && eligible(id)) {
            lastTried = id;
            return &pose;
        }
    }
    return nullptr;
}

namespace {
constexpr std::size_t maxAsset = 128u << 20;
constexpr std::size_t maxState = 128u << 10;
constexpr std::uint64_t maxCounter = (std::uint64_t(1) << 53) - 1;

bool hex(std::string_view key, std::size_t width) {
    return key.size() == width && std::ranges::all_of(key, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool contentKey(std::string_view key) { return key.starts_with("local1_") && hex(key.substr(7), 64); }
bool playerId(std::string_view id) {
    return !id.empty() && id.size() <= 20 && id.front() != '0'
        && std::ranges::all_of(id, [](char c) { return c >= '0' && c <= '9'; });
}
std::string stringField(Json const& document, char const* key, std::size_t limit) {
    auto text = document.at(key).get<std::string>();
    if (text.size() > limit || text.find('\0') != text.npos) throw std::runtime_error("Invalid relay string");
    return text;
}
std::uint64_t numberField(Json const& input) {
    if (!input.is_number_unsigned() || input.get<std::uint64_t>() > maxCounter)
        throw std::runtime_error("Invalid relay counter");
    return input.get<std::uint64_t>();
}
std::vector<unsigned char> readFile(std::filesystem::path const& path, std::size_t limit) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    auto size = stream.tellg();
    if (!stream || size < 0 || static_cast<std::uint64_t>(size) > limit)
        throw std::runtime_error("Invalid relay file size");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), size)) throw std::runtime_error("Cannot read relay file");
    return bytes;
}
struct HttpHandle {
    HINTERNET handle{};
    ~HttpHandle() { if (handle) WinHttpCloseHandle(handle); }
    operator HINTERNET() const { return handle; }
};
RelayPose readPeer(Json const& document) {
    RelayPose peer;
    peer.xuid = stringField(document, "xuid", 20);
    peer.session = stringField(document, "session", 32);
    peer.contentKey = stringField(document, "model", 71);
    peer.texture = stringField(document, "texture", 512);
    peer.action = stringField(document, "action", 512);
    peer.serial = numberField(document.at("serial"));
    peer.actionSerial = numberField(document.at("action_serial"));
    peer.actionTime = document.at("action_time").get<double>();
    if (!playerId(peer.xuid) || !hex(peer.session, 32) || !contentKey(peer.contentKey)
        || !std::isfinite(peer.actionTime) || peer.actionTime < 0 || peer.actionTime > 86400)
        throw std::runtime_error("Invalid relay player state");
    if (auto seats = document.find("shoulder_birds"); seats != document.end()) {
        if (!seats->is_array() || seats->size() != 2) throw std::runtime_error("Invalid relay shoulders");
        for (std::size_t i = 0; i < 2; ++i) {
            auto const& slot = (*seats)[i];
            if (slot.is_null()) continue;
            auto text = slot.get<std::string>();
            auto& bird = peer.shoulderBirds[i];
            auto [end, ec] = std::from_chars(text.data(), text.data()+text.size(), bird);
            if (ec != std::errc{} || end != text.data()+text.size() || !bird || std::to_string(bird) != text)
                throw std::runtime_error("Invalid relay shoulder entity");
        }
        if (peer.shoulderBirds[0] && peer.shoulderBirds[0] == peer.shoulderBirds[1])
            throw std::runtime_error("Duplicate relay shoulder entity");
    }
    auto const& roaming = document.at("roaming");
    if (!roaming.is_object() || roaming.size() > 1024) throw std::runtime_error("Invalid relay model fields");
    for (auto const& [field, value] : roaming.items()) {
        if (!value.is_number()) throw std::runtime_error("Invalid relay model value");
        peer.roaming.emplace(field, value.get<float>());
    }
    MolangRoamingState::validateNamedPayload(peer.roaming, RoamingLimit::LocalOnly);
    auto const& events = document.at("events");
    if (!events.is_array() || events.size() > 64) throw std::runtime_error("Invalid relay event list");
    std::uint64_t previous = 0;
    for (auto const& event : events) {
        if (!event.is_array() || event.empty() || event.size() > 17) throw std::runtime_error("Invalid relay event");
        RelayEvent record{numberField(event.front()), {}};
        if (record.sequence <= previous) throw std::runtime_error("Unordered relay event");
        previous = record.sequence;
        for (std::size_t i = 1; i < event.size(); ++i) {
            if (!event[i].is_number()) throw std::runtime_error("Invalid relay event value");
            auto value = event[i].get<float>();
            if (!std::isfinite(value)) throw std::runtime_error("Non-finite relay event value");
            record.arguments.push_back(value);
        }
        peer.events.push_back(std::move(record));
    }
    return peer;
}
}

std::optional<ModelRelay> ModelRelay::readConfig(std::filesystem::path const& file) {
    if (!std::filesystem::exists(file)) return {};
    auto bytes = readFile(file, 16384);
    auto json = Json::parse(bytes, nullptr, false);
    if (!json.is_object()) throw std::runtime_error("Invalid relay settings file");
    if (!json.value("enabled", false)) return {};
    auto url = stringField(json, "relay", 1024);
    auto token = stringField(json, "token", 128);
    if (token.size() < 32 || !std::ranges::all_of(token, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    })) throw std::runtime_error("Invalid relay credential");
    if (std::ranges::any_of(url, [](unsigned char c) { return c < 33 || c >= 127; }))
        throw std::runtime_error("Relay address must be ASCII");
    std::wstring address(url.begin(), url.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength =
        parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(address.c_str(), static_cast<DWORD>(address.size()), 0, &parts)
        || !parts.dwHostNameLength || parts.dwUserNameLength || parts.dwPasswordLength
        || parts.dwExtraInfoLength || (parts.dwUrlPathLength &&
            std::wstring_view(parts.lpszUrlPath, parts.dwUrlPathLength) != L"/"))
        throw std::runtime_error("Invalid relay address");
    ModelRelay relay;
    relay.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    relay.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (!relay.secure && (parts.nScheme != INTERNET_SCHEME_HTTP || relay.host != L"127.0.0.1"))
        throw std::runtime_error("A relay outside this computer requires HTTPS");
    relay.port = parts.nPort;
    relay.authorization = L"Authorization: Bearer " + std::wstring(token.begin(), token.end());
    relay.serverHost = stringField(json, "bedrock_host", 512);
    relay.serverPort = json.at("bedrock_port").get<int>();
    if (relay.serverHost.empty() || relay.serverPort <= 0 || relay.serverPort > 65535)
        throw std::runtime_error("Invalid Bedrock server binding for relay");
    return relay;
}

std::string ModelRelay::newSession() {
    std::array<unsigned char, 16> entropy{};
    if (BCryptGenRandom(nullptr, entropy.data(), static_cast<ULONG>(entropy.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("Unable to start a relay session");
    std::string result(32, '0');
    constexpr std::string_view digits = "0123456789abcdef";
    for (std::size_t i = 0; i < entropy.size(); ++i) {
        result[2 * i] = digits[entropy[i] >> 4];
        result[2 * i + 1] = digits[entropy[i] & 15];
    }
    return result;
}

std::vector<unsigned char> ModelRelay::request(std::wstring const& path, std::string const& body, std::size_t limit) const {
    HttpHandle session{WinHttpOpen(L"YsmBedrock/1", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0)};
    if (!session || !WinHttpSetTimeouts(session, 1500, 1500, 2000, 3000))
        throw std::runtime_error("Unable to open relay connection");
    HttpHandle connection{WinHttpConnect(session, host.c_str(), port, 0)};
    if (!connection) throw std::runtime_error("Invalid relay endpoint");
    HttpHandle message{WinHttpOpenRequest(connection, body.empty() ? L"GET" : L"POST", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)};
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    auto headers = authorization + (body.empty() ? L"" : L"\r\nContent-Type: application/json");
    if (!message || !WinHttpSetOption(message, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled))
        || !WinHttpSendRequest(message, headers.c_str(), static_cast<DWORD>(headers.size()),
            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)
        || !WinHttpReceiveResponse(message, nullptr))
        throw std::runtime_error("Relay request failed");
    DWORD status = 0, bytes = sizeof(status);
    if (!WinHttpQueryHeaders(message, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        nullptr, &status, &bytes, nullptr) || status != 200)
        throw std::runtime_error("Relay returned HTTP " + std::to_string(status));
    DWORD available = 0;
    std::vector<unsigned char> result;
    while (WinHttpQueryDataAvailable(message, &available)) {
        if (!available) return result;
        if (available > limit - result.size()) throw std::runtime_error("Relay response exceeds its size limit");
        auto previous = result.size();
        result.resize(previous + available);
        DWORD read = 0;
        if (!WinHttpReadData(message, result.data() + previous, available, &read) || !read)
            throw std::runtime_error("Relay response ended early");
        result.resize(previous + read);
    }
    throw std::runtime_error("Cannot read relay response");
}

RelayFrame ModelRelay::exchange(RelayPose const& state, std::uint64_t serial) const {
    if (!playerId(state.xuid) || !hex(state.session, 32)) throw std::runtime_error("Invalid Bedrock player identity");
    Json events = Json::array();
    for (auto const& event : state.events) {
        Json row = Json::array({event.sequence});
        for (auto value : event.arguments) row.push_back(value);
        events.push_back(std::move(row));
    }
    auto receipts = Json::object();
    for (auto const& [peer, seen] : state.receipts)
        receipts[peer] = {seen.session, seen.model, seen.sequence};
    // Bedrock ActorUniqueIDs can exceed the exact integer range of JSON clients.
    auto shoulders = Json::array();
    for (auto bird : state.shoulderBirds) shoulders.push_back(bird ? Json(std::to_string(bird)) : Json(nullptr));
    auto body = Json{{"shoulder_birds",std::move(shoulders)}, {"protocol", 1}, {"xuid", state.xuid}, {"session", state.session}, {"serial", serial},
        {"model", state.contentKey}, {"texture", state.texture}, {"action", state.action},
        {"action_serial", state.actionSerial}, {"action_time", state.actionTime},
        {"roaming", state.roaming}, {"events", std::move(events)}, {"receipts", std::move(receipts)}}.dump();
    if (body.size() > maxState) throw std::runtime_error("Relay packet exceeds its size limit");
    auto response = Json::parse(request(L"/v1/state", body, 4 << 20));
    if (response.value("protocol", 0) != 1 || response.at("you").get<std::string>() != state.xuid)
        throw std::runtime_error("Relay identity does not match this player");
    RelayFrame result;
    result.approved = response.at("approved").get<bool>();
    result.acknowledgedEvent = numberField(response.at("event_cursor"));
    auto const& assets = response.at("models");
    auto const& players = response.at("players");
    if (!assets.is_array() || assets.size() > 64 || !players.is_array() || players.size() > 16)
        throw std::runtime_error("Invalid relay catalog or room size");
    for (auto const& item : assets) {
        RelayAsset asset{stringField(item, "key", 71), stringField(item, "digest", 64), stringField(item, "extension", 4),
            static_cast<std::size_t>(numberField(item.at("size"))), stringField(item, "title", 512), item.at("selectable").get<bool>()};
        if (!contentKey(asset.contentKey) || !hex(asset.digest, 64) || (asset.extension != ".zip" && asset.extension != ".ysm")
            || !asset.size || asset.size > maxAsset
            || !result.assets.emplace(asset.contentKey, asset).second)
            throw std::runtime_error("Invalid relay asset");
    }
    for (auto const& item : players) {
        auto peer = readPeer(item);
        if (peer.xuid == state.xuid || !result.assets.contains(peer.contentKey)
            || !result.players.emplace(peer.xuid, peer).second)
            throw std::runtime_error("Invalid relay player");
    }
    return result;
}

ModelPack ModelRelay::obtain(RelayAsset const& asset, std::filesystem::path const& cache) const {
    if (!contentKey(asset.contentKey) || !hex(asset.digest, 64) || (asset.extension != ".zip" && asset.extension != ".ysm")
        || !asset.size || asset.size > maxAsset)
        throw std::runtime_error("Invalid requested relay asset");
    std::filesystem::create_directories(cache);
    auto local = cache / (asset.digest + asset.extension);
    bool cached = false;
    if (std::filesystem::is_regular_file(local) && !std::filesystem::is_symlink(local)
        && std::filesystem::file_size(local) == asset.size) {
        auto bytes = readFile(local, asset.size);
        cached = contentSha256(bytes) == asset.digest;
    }
    if (!cached) {
        auto bytes = request(L"/v1/model/" + std::wstring(asset.digest.begin(), asset.digest.end()), {}, asset.size);
        if (bytes.size() != asset.size || contentSha256(bytes) != asset.digest)
            throw std::runtime_error("Relay model checksum mismatch");
        auto temporary = cache / (newSession() + ".stage");
        try {
            std::ofstream stream(temporary, std::ios::binary);
            if (!stream.write(reinterpret_cast<char const*>(bytes.data()), bytes.size()))
                throw std::runtime_error("Cannot cache relay model");
            stream.close();
            if (!stream || !MoveFileExW(temporary.c_str(), local.c_str(), MOVEFILE_REPLACE_EXISTING))
                throw std::runtime_error("Cannot publish relay model cache");
        } catch (...) {
            std::error_code error;
            std::filesystem::remove(temporary, error);
            throw;
        }
    }
    auto pack = ModelPack::load(local);
    if (pack.localContentKey != asset.contentKey) throw std::runtime_error("Relay model identity mismatch");
    return pack;
}
}
