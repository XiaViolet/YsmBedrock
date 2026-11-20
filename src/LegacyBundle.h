#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ysm {
using LegacyFiles = std::map<std::string, std::vector<unsigned char>, std::less<>>;
bool isLegacyV3(std::filesystem::path const& path);
std::shared_ptr<LegacyFiles const> readLegacyV3(std::filesystem::path const& path);
class LegacyBundle {
    struct Entry {
        std::size_t offset, size, wrappedKeySize, cipherSize;
    };
    std::vector<unsigned char> container;
    std::map<std::string, Entry, std::less<>> index;
    bool v2{};
public:
    explicit LegacyBundle(std::filesystem::path const& path);
    std::vector<std::string> paths() const;
    bool contains(std::string_view name) const { return index.contains(name); }
    std::vector<unsigned char> read(std::string const& name, std::size_t maximum) const;
};
}
