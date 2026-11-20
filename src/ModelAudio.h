#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ysm {
struct SoundCue {
    std::string owner, name;
    bool firstPerson = false;
    std::optional<std::string> id;
    bool stopId = false, force = false, looping = false;
    float volume = 1, pitch = 1;
    SoundCue() = default;
    SoundCue(std::string scope, std::string selector, bool arm = false)
        : owner(std::move(scope)), name(std::move(selector)), firstPerson(arm) {}
};
struct AudioInfo {
    bool opus = false;
    unsigned channels = 0;
    std::uint32_t rate = 0;
    std::uint64_t frames = 0;
    unsigned preSkip = 0;
    int gain = 0;
};
struct ModelAudio {
    AudioInfo info;
    std::vector<unsigned char> encoded;
    std::vector<unsigned char> wave;
};
struct GameSound {
    std::string name;
    float pitchMultiplier = 1;
};
struct PlayingSoundState {
    using Position = std::array<float, 3>;
    float authoredVolume;
    Position position;
    float volume;
    static float scaledVolume(float authored, int percent) { return authored * percent / 100.f; }
    // Advance the stored state only after the original handle accepts the update.
    template <class Submit>
    bool follow(Position current, int percent, Submit&& submit) {
        auto gain = scaledVolume(authoredVolume, percent);
        bool moved = current != position, adjusted = gain != volume;
        if ((!moved && !adjusted) || !submit(moved ? &current : nullptr, adjusted ? &gain : nullptr)) return false;
        position = current;
        volume = gain;
        return true;
    }
};
// Only namespaced selectors route to game resources. Bare names belong to the model.
std::optional<GameSound> resolveGameSound(std::string_view selector);
// Unsupported media is skipped by Java. A damaged recognized stream is a model error.
std::optional<AudioInfo> inspectAudio(std::span<unsigned char const> bytes);
std::vector<unsigned char> decodeOpusWave(ModelAudio const& clip, std::size_t budget);
}
