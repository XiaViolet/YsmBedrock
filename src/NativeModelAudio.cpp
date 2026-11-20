#include "NativeModelAudio.h"
#include "mc/deps/audio/SoundSystemBase.h"
#include <Windows.h>
#include <cstring>

namespace ysm {
namespace {
template <class T>
T readAt(void const* object, std::size_t offset) {
    T value;
    std::memcpy(&value, static_cast<char const*>(object) + offset, sizeof(value));
    return value;
}
}

bool updatePlayingSound(SoundSystemBase& system, std::uint64_t handle,
        PlayingSoundState::Position const* position, float const* volume, NativeSoundReadback* readback) {
    // 1.26.60.05: the public setter takes a server handle. Local model sounds
    // use the same native instances and must keep their local playback cursor.
    // Identify this exact FMOD implementation before reading the opaque SDK list.
    auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto table = readAt<std::uintptr_t const*>(&system, 0);
    if (reinterpret_cast<std::uintptr_t>(table) != image + 0x0f333930
        || table[69] != image + 0x07a76160 || table[36] != image + 0x07a6e320) return false;

    // SoundSystemFMOD::tryGetPlayingSoundAttributes(localHandle) acquires this
    // list's shared lock, iterates its instance pointers and compares getHandle().
    auto& lock = *reinterpret_cast<SRWLOCK*>(reinterpret_cast<char*>(&system) + 0x98);
    AcquireSRWLockShared(&lock);
    struct Release { SRWLOCK& lock; ~Release() { ReleaseSRWLockShared(&lock); } } release{lock};
    auto first = readAt<void* const*>(&system, 0x78);
    auto last = readAt<void* const*>(&system, 0x80);
    for (; first != last; ++first) {
        auto* instance = *first;
        if (!instance) continue;
        auto methods = readAt<std::uintptr_t const*>(instance, 0);
        // SoundInstanceFMOD created by playAt (including playEventAt's selected
        // resource). Verify the setters as well as the local handle accessor.
        if (methods[12] != image + 0x00300200 || methods[9] != image + 0x07a66120
            || methods[7] != image + 0x07a62e00 || methods[16] != image + 0x07a50980) continue;
        auto getHandle = reinterpret_cast<std::uint64_t (*)(void const*)>(methods[12]);
        if (getHandle(instance) != handle) continue;
        // An async event may already be registered while its channel is unavailable.
        auto* channel = reinterpret_cast<void* (*)(void const*)>(methods[16])(instance);
        if (!channel) return false;
        if (position)
            reinterpret_cast<void (*)(void*, PlayingSoundState::Position const*)>(methods[9])(instance, position);
        if (volume)
            reinterpret_cast<void (*)(void*, float)>(methods[7])(instance, *volume);
        if (readback) {
            auto fmod = GetModuleHandleW(L"fmod.dll");
            using Position = PlayingSoundState::Position;
            using GetPosition = int (*)(void*, Position*, Position*);
            using GetVolume = int (*)(void*, float*);
            auto getPosition = reinterpret_cast<GetPosition>(GetProcAddress(fmod, "FMOD_Channel_Get3DAttributes"));
            auto getVolume = reinterpret_cast<GetVolume>(GetProcAddress(fmod, "FMOD_Channel_GetVolume"));
            Position xyz{};
            float gain{};
            readback->position.reset();
            readback->volume.reset();
            if (getPosition && getPosition(channel, &xyz, nullptr) == 0) readback->position = xyz;
            if (getVolume && getVolume(channel, &gain) == 0) readback->volume = gain;
        }
        return true;
    }
    return false;
}
}
