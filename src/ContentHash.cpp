#include "ContentHash.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <limits>
#include <stdexcept>
#include <fstream>

namespace ysm {
namespace {
auto providerHandle() {
    struct Provider {
        BCRYPT_ALG_HANDLE handle = nullptr;
        Provider() {
            if (BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
                throw std::runtime_error("Cannot initialize source content hash");
        }
        ~Provider() { BCryptCloseAlgorithmProvider(handle, 0); }
    };
    static Provider const provider;
    return provider.handle;
}
std::string encode(std::array<unsigned char, 32> const& digest) {
    std::string result;
    result.reserve(64);
    for (auto byte : digest) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}
}
std::string contentSha256(std::span<unsigned char const> bytes) {
    if (bytes.size() > std::numeric_limits<ULONG>::max()) throw std::runtime_error("Source hash input exceeds limit");
    std::array<unsigned char, 32> digest{};
    if (BCryptHash(providerHandle(), nullptr, 0, const_cast<unsigned char*>(bytes.data()),
        static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0)
        throw std::runtime_error("Cannot hash source content");
    return encode(digest);
}

std::string contentFileSha256(std::filesystem::path const& file, std::uint64_t maximum) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open file for SHA-256");
    struct Hash {
        BCRYPT_HASH_HANDLE handle{};
        ~Hash() { if (handle) BCryptDestroyHash(handle); }
    } hash;
    if (BCryptCreateHash(providerHandle(), &hash.handle, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("Cannot initialize file hash");
    std::array<unsigned char, 65536> buffer;
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        auto count = static_cast<ULONG>(input.gcount());
        if (count > maximum || (count && BCryptHashData(hash.handle, buffer.data(), count, 0) < 0))
            throw std::runtime_error("File exceeds hash limit or cannot be hashed");
        maximum -= count;
    }
    std::array<unsigned char, 32> digest{};
    if (input.bad() || !input.eof() || BCryptFinishHash(hash.handle, digest.data(), digest.size(), 0) < 0)
        throw std::runtime_error("Cannot finish file hash");
    return encode(digest);
}
}
