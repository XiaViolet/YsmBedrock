#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace ysm {
void appendJavaItemTags(std::string_view id, std::vector<std::string>& tags);
void appendJavaEntityTags(std::string_view id, std::vector<std::string>& tags);
std::string_view javaEntityId(std::string_view bedrockId, int boatVariant = -1);
bool hasJavaBlockTag(std::string_view bedrockId, std::string_view tag);
bool hasJavaBiomeTag(std::string_view bedrockId, std::string_view tag);
std::string canonicalRegistryId(std::string_view id);
std::string javaItemCategory(std::string_view nativeCategory, std::vector<std::string> const& tags);

// Java's client-player ElytraModel state, retained across model changes.
// advance() corresponds to one rendered equipped wing layer, not one game tick.
class JavaElytraPose {
    float roll = 0;
public:
    float degrees() const;
    void advance(bool gliding, bool crouching, std::array<double, 3> const& velocity);
};
class JavaElytraInput {
    JavaElytraPose pose;
    std::uint64_t lastFrame = std::numeric_limits<std::uint64_t>::max();
    float query = 0;
    bool advanced = false;
public:
    float sample(std::uint64_t frame, bool renderLayer, bool equipped,
        bool gliding, bool crouching, std::array<double, 3> const& velocity);
};
}
