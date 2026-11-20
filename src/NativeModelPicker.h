#pragma once
#include <filesystem>
#include <optional>
#include <stop_token>
namespace ysm {
std::optional<std::filesystem::path> pickModelSource(bool folder, void* owner, std::stop_token stop);
}
