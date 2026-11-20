#include "NativeWeather.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/WeatherManager.h"
#include "mc/world/level/IWeatherManagerProxy.h"
#include "mc/deps/shared_types/legacy/LevelEvent.h"
#include <mutex>
#include <unordered_map>

namespace ysm {
static_assert(static_cast<int>(SharedTypes::Legacy::LevelEvent::StartRaining) == 3001);
static_assert(static_cast<int>(SharedTypes::Legacy::LevelEvent::StartThunderstorm) == 3002);
static_assert(static_cast<int>(SharedTypes::Legacy::LevelEvent::StopRaining) == 3003);
static_assert(static_cast<int>(SharedTypes::Legacy::LevelEvent::StopThunderstorm) == 3004);
namespace {
std::mutex weatherMutex;
std::unordered_map<Level const*, BedrockWeatherState> weatherByLevel;
BedrockWeatherState& stateFor(Level const& level) {
    auto& state = weatherByLevel[&level];
    if (!state.snapshot().initialized) {
        // Fallback for enabling the mod in a level whose StartGame was already
        // received. Never overwrite later packet targets with this initial data.
        auto const& initial = level.mWeatherManager->mWeatherManagerProxy;
        state.initialize(initial->getRainLevel(), initial->getLightningLevel(), WeatherOrigin::InitialWorld);
    }
    return state;
}
}
void clearNativeWeather() {
    std::scoped_lock lock(weatherMutex);
    weatherByLevel.clear();
}
void initializeNativeWeather(Level const* level, double rain, double lightning) {
    if (!level) return;
    std::scoped_lock lock(weatherMutex);
    weatherByLevel[level].initialize(rain, lightning);
}
void receiveNativeWeather(Level const* level, int event, int data) {
    if (!level || event < 3001 || event > 3004) return;
    std::scoped_lock lock(weatherMutex);
    stateFor(*level).applyEvent(event, data);
}
BedrockWeatherSnapshot readNativeWeather(Level const& level) {
    std::scoped_lock lock(weatherMutex);
    return stateFor(level).snapshot();
}
}
