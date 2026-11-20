#pragma once
#include "ModelAudio.h"

class SoundSystemBase;
namespace ysm {
struct NativeSoundReadback {
    std::optional<PlayingSoundState::Position> position;
    std::optional<float> volume;
};
bool updatePlayingSound(SoundSystemBase& system, std::uint64_t handle,
    PlayingSoundState::Position const* position, float const* volume, NativeSoundReadback* readback);
}
