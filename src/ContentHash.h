#pragma once
#include <span>
#include <string>
#include <filesystem>
#include <cstdint>

namespace ysm {
// Private source-cache identity, not the Java MIXEL/BLAKE3 wire identity.
std::string contentSha256(std::span<unsigned char const> bytes);
std::string contentFileSha256(std::filesystem::path const& file, std::uint64_t maximum);
}
