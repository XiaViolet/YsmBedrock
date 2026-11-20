// Compatibility with the official YesSteveModel-Native v1/v2 bundle reader.
// Format and decoding order follow modules/legacy/src/bundle.cc (Apache-2.0).
#include "LegacyBundle.h"
#include "ContentHash.h"
#include <Windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <miniz.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <stdexcept>

namespace ysm {
namespace {
using Bytes = std::span<unsigned char const>;
constexpr std::size_t headerSize = 24;
constexpr std::size_t blockSize = 16;
constexpr std::size_t archiveLimit = 128u << 20;
constexpr std::size_t entryLimit = 40u << 20;
constexpr std::array<unsigned char, 7> v3Marker{0xef, 0xbb, 0xbf, 'Y', 'S', 'G', 'P'};

void require(bool condition, char const* reason) {
    if (!condition) throw std::runtime_error(reason);
}
Bytes consume(Bytes& input, std::size_t size) {
    require(size <= input.size(), "Truncated legacy YSM package");
    auto part = input.first(size);
    input = input.subspan(size);
    return part;
}
std::uint32_t read32(Bytes& input) {
    std::uint32_t value = 0;
    for (auto byte : consume(input, 4)) value = (value << 8) | byte;
    return value;
}
struct Algorithm {
    BCRYPT_ALG_HANDLE handle{};
    explicit Algorithm(wchar_t const* name) {
        require(BCryptOpenAlgorithmProvider(&handle, name, nullptr, 0) == 0, "Cannot initialize the legacy YSM reader");
    }
    ~Algorithm() { BCryptCloseAlgorithmProvider(handle, 0); }
};
std::array<unsigned char, blockSize> md5(Bytes input) {
    static Algorithm const algorithm(BCRYPT_MD5_ALGORITHM);
    std::array<unsigned char, blockSize> digest{};
    require(BCryptHash(algorithm.handle, nullptr, 0, const_cast<unsigned char*>(input.data()),
        static_cast<ULONG>(input.size()), digest.data(), static_cast<ULONG>(digest.size())) == 0,
        "Cannot validate the legacy YSM checksum");
    return digest;
}
std::vector<unsigned char> decodeCbc(Bytes content, Bytes key, Bytes iv) {
    static Algorithm const algorithm(BCRYPT_AES_ALGORITHM);
    BCRYPT_KEY_HANDLE handle{};
    require(key.size() == blockSize && iv.size() == blockSize && !content.empty() && content.size() % blockSize == 0,
        "Invalid legacy YSM entry");
    require(BCryptGenerateSymmetricKey(algorithm.handle, &handle, nullptr, 0, const_cast<unsigned char*>(key.data()),
        static_cast<ULONG>(key.size()), 0) == 0, "Cannot initialize legacy YSM entry");
    struct Key {
        BCRYPT_KEY_HANDLE handle;
        ~Key() { BCryptDestroyKey(handle); }
    } cleanup{handle};
    auto chain = std::array<unsigned char, blockSize>{};
    std::ranges::copy(iv, chain.begin());
    ULONG bytes = 0;
    std::vector<unsigned char> result(content.size());
    require(BCryptDecrypt(handle, const_cast<unsigned char*>(content.data()), static_cast<ULONG>(content.size()),
        nullptr, chain.data(), static_cast<ULONG>(chain.size()), result.data(), static_cast<ULONG>(result.size()),
        &bytes, BCRYPT_BLOCK_PADDING) == 0, "Legacy YSM entry integrity check failed");
    result.resize(bytes);
    return result;
}
std::array<unsigned char, blockSize> wrappedKey(Bytes cipher) {
    std::uint64_t seed = 0;
    for (auto byte : md5(cipher)) seed = (seed << 8) | byte;
    constexpr std::uint64_t multiplier = 0x5deece66d;
    constexpr std::uint64_t mask = (std::uint64_t(1) << 48) - 1;
    seed = (seed ^ multiplier) & mask;
    std::array<unsigned char, blockSize> result{};
    for (std::size_t offset = 0; offset < result.size(); offset += 4) {
        seed = (seed * multiplier + 11) & mask;
        auto value = static_cast<std::uint32_t>(seed >> 16);
        for (std::size_t i = 0; i < 4; ++i) result[offset + i] = static_cast<unsigned char>(value >> (8 * i));
    }
    return result;
}
std::string pathName(Bytes encoded, bool v2) {
    std::string name(reinterpret_cast<char const*>(encoded.data()), encoded.size());
    if (v2) {
        DWORD size{};
        auto flags = CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT;
        require(CryptStringToBinaryA(name.data(), static_cast<DWORD>(name.size()), flags, nullptr, &size, nullptr, nullptr),
            "Invalid legacy YSM resource name");
        std::string result(size, '\0');
        require(CryptStringToBinaryA(name.data(), static_cast<DWORD>(name.size()), flags,
            reinterpret_cast<unsigned char*>(result.data()), &size, nullptr, nullptr), "Invalid legacy YSM resource name");
        name = std::move(result);
    }
    std::replace(name.begin(), name.end(), '\\', '/');
    require(!name.empty() && name.front() != '/' && name.find('\0') == name.npos && name.find(':') == name.npos,
        "Invalid legacy YSM resource path");
    for (auto const& part : std::filesystem::path(std::u8string(name.begin(), name.end())))
        require(part != "..", "Legacy YSM resource escapes its package");
    return name;
}
std::vector<unsigned char> inflate(Bytes data, std::size_t limit) {
    mz_stream stream{};
    require(mz_inflateInit(&stream) == MZ_OK, "Cannot initialize YSM resource decompression");
    struct Inflate {
        mz_stream& stream;
        ~Inflate() { mz_inflateEnd(&stream); }
    } cleanup{stream};
    stream.next_in = data.data();
    stream.avail_in = static_cast<unsigned int>(data.size());
    std::vector<unsigned char> result;
    std::array<unsigned char, 64u << 10> output;
    int status;
    do {
        stream.next_out = output.data();
        stream.avail_out = static_cast<unsigned int>(output.size());
        status = mz_inflate(&stream, MZ_NO_FLUSH);
        require(status == MZ_OK || status == MZ_STREAM_END, "Invalid compressed legacy YSM resource");
        auto part = output.size() - stream.avail_out;
        require(part <= limit - result.size(), "Legacy YSM resource exceeds its size limit");
        result.insert(result.end(), output.begin(), output.begin() + part);
    } while (status != MZ_STREAM_END);
    return result;
}
}

bool isLegacyV3(std::filesystem::path const& path) {
    std::ifstream stream(path, std::ios::binary);
    std::array<unsigned char, v3Marker.size()> actual{};
    return stream.read(reinterpret_cast<char*>(actual.data()), actual.size()) && actual == v3Marker;
}

std::shared_ptr<LegacyFiles const> readLegacyV3(std::filesystem::path const& path) {
    using Callback = int(*)(void*, char const*, std::size_t, unsigned char const*, std::size_t);
    using Decoder = int(*)(unsigned char const*, std::size_t, void*, Callback, char*, std::size_t);
    static auto const decoder = []() -> Decoder {
        HMODULE module = nullptr;
        auto address = reinterpret_cast<LPCWSTR>(&readLegacyV3);
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            address, &module)) return nullptr;
        std::wstring location(32768, L'\0');
        auto length = GetModuleFileNameW(module, location.data(), static_cast<DWORD>(location.size()));
        if (length == 0 || length >= location.size()) return nullptr;
        location.resize(length);
        auto library = std::filesystem::path(location).parent_path() / L"YsmLegacyV3.dll";
        auto handle = LoadLibraryExW(library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!handle) return nullptr;
        return reinterpret_cast<Decoder>(GetProcAddress(handle, "ysmBedrockReadV3"));
    }();
    require(decoder != nullptr, "Official YSM v3 reader library is unavailable");
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    auto size = stream.tellg();
    require(stream && size > 0 && static_cast<std::uint64_t>(size) <= 69206016u, "Invalid YSM v3 package size");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    require(static_cast<bool>(stream.read(reinterpret_cast<char*>(bytes.data()), size)), "Cannot read YSM v3 package");
    // Returning to an installed model can reuse its decoded resources.
    // Compare the archive bytes, so replacing it cannot return an old model.
    static std::mutex cacheMutex;
    static std::string cachedHash;
    static std::shared_ptr<LegacyFiles const> cachedFiles;
    auto hash = contentSha256(bytes);
    {
        std::scoped_lock lock(cacheMutex);
        if (cachedHash == hash) return cachedFiles;
    }
    struct Result {
        LegacyFiles files;
        std::size_t total{};
    } result;
    auto receive = [](void* context, char const* path, std::size_t nameSize, unsigned char const* data, std::size_t length) -> int {
        try {
            auto& result = *static_cast<Result*>(context);
            if (!nameSize || nameSize > 2048 || length > (64u << 20) + 8 || result.files.size() >= 8192
                || length > (380u << 20) - result.total) return 0;
            auto name = pathName({reinterpret_cast<unsigned char const*>(path), nameSize}, false);
            if (!result.files.emplace(std::move(name), std::vector<unsigned char>(data, data + length)).second) return 0;
            result.total += length;
            return 1;
        } catch (...) { return 0; }
    };
    std::array<char, 1024> error{};
    if (decoder(bytes.data(), bytes.size(), &result, receive, error.data(), error.size()) != 1)
        throw std::runtime_error(std::string("YSM v3 reader: ") + error.data());
    auto files = std::make_shared<LegacyFiles const>(std::move(result.files));
    if (result.total <= (64u << 20)) {
        std::scoped_lock lock(cacheMutex);
        cachedHash = std::move(hash);
        cachedFiles = files;
    }
    return files;
}

LegacyBundle::LegacyBundle(std::filesystem::path const& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    auto size = stream.tellg();
    require(stream && size > 0 && static_cast<std::uint64_t>(size) <= archiveLimit, "Invalid legacy YSM package size");
    container.resize(static_cast<std::size_t>(size));
    stream.seekg(0);
    require(static_cast<bool>(stream.read(reinterpret_cast<char*>(container.data()), size)), "Cannot read legacy YSM package");
    auto input = Bytes(container);
    require(input.size() > headerSize && std::string_view(reinterpret_cast<char const*>(input.data()), 4) == "YSGP",
        "Unsupported YSM package signature");
    consume(input, 4);
    auto version = read32(input);
    require(version == 1 || version == 2, "Unsupported legacy YSM package version");
    v2 = version == 2;
    auto checksum = consume(input, 16);
    require(std::ranges::equal(checksum, md5(input)), "Legacy YSM package checksum mismatch");
    while (!input.empty()) {
        auto length = read32(input);
        require(length && length <= 2048 && index.size() < 8192, "Invalid legacy YSM archive index");
        auto name = pathName(consume(input, length), v2);
        auto cipherLength = read32(input);
        auto wrappedLength = v2 ? read32(input) : blockSize;
        require(cipherLength && cipherLength <= entryLimit && cipherLength % blockSize == 0 &&
            wrappedLength && wrappedLength <= 4096 && wrappedLength % blockSize == 0, "Invalid legacy YSM archive entry length");
        std::size_t offset = container.size() - input.size();
        auto record = consume(input, wrappedLength + blockSize + cipherLength);
        require(index.emplace(std::move(name), Entry{offset, record.size(), wrappedLength, cipherLength}).second,
            "Duplicate legacy YSM archive entry");
    }
}

std::vector<std::string> LegacyBundle::paths() const {
    std::vector<std::string> result;
    result.reserve(index.size());
    for (auto const& [name, entry] : index) result.push_back(name);
    return result;
}

std::vector<unsigned char> LegacyBundle::read(std::string const& name, std::size_t maximum) const {
    auto const& entry = index.at(name);
    auto content = Bytes(container).subspan(entry.offset, entry.size);
    auto keyOrWrapped = consume(content, entry.wrappedKeySize);
    auto iv = consume(content, blockSize);
    auto cipher = consume(content, entry.cipherSize);
    if (!v2) return inflate(decodeCbc(cipher, keyOrWrapped, iv), maximum);
    auto key = decodeCbc(keyOrWrapped, wrappedKey(cipher), iv);
    require(key.size() >= blockSize, "Legacy YSM entry key is truncated");
    return inflate(decodeCbc(cipher, Bytes(key).first(blockSize), iv), maximum);
}
}
