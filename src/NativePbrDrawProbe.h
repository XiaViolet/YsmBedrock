#pragma once
#include "SkinRuntime.h"
#include <filesystem>

namespace ysm {
void startPbrDrawProbe(std::filesystem::path const& directory, SkinRuntime const& localSkin);
void stopPbrDrawProbe();
}
