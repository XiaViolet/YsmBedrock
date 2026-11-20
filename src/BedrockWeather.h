#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ysm {
enum class WeatherOrigin { Unknown, InitialWorld, StartGame, LevelEvent };
struct BedrockWeatherSnapshot {
    bool initialized = false;
    double rain = 0, lightning = 0;
    WeatherOrigin origin = WeatherOrigin::Unknown;
    std::uint64_t events = 0;
    int lastEvent = 0, lastData = 0;
};

// StartGame supplies initial levels; subsequent LevelEvents carry independent
// rain/thunder targets scaled by 65535. World settings alone do not track them.
// https://docs.levimc.org/bedboo/reference/differences/world/#weather-levels
class BedrockWeatherState {
    BedrockWeatherSnapshot value;
public:
    void reset() { value = {}; }
    void initialize(double rain, double lightning, WeatherOrigin origin = WeatherOrigin::StartGame) {
        auto strength = [](double input) { return std::isfinite(input) ? std::clamp(input, 0.0, 1.0) : 0.0; };
        value = {true, strength(rain), strength(lightning), origin};
    }
    bool applyEvent(int event, int data) {
        auto strength = static_cast<double>(std::clamp(data, 0, 65535)) / 65535.0;
        switch (event) {
        case 3001: value.rain = strength; break;
        case 3002: value.lightning = strength; break;
        case 3003: value.rain = 0; break;
        case 3004: value.lightning = 0; break;
        default: return false;
        }
        value.initialized = true;
        value.origin = WeatherOrigin::LevelEvent;
        ++value.events;
        value.lastEvent = event;
        value.lastData = data;
        return true;
    }
    BedrockWeatherSnapshot snapshot() const { return value; }
};

inline char const* weatherOriginName(WeatherOrigin origin) {
    switch (origin) {
    case WeatherOrigin::InitialWorld: return "initial_world";
    case WeatherOrigin::StartGame: return "start_game";
    case WeatherOrigin::LevelEvent: return "level_event";
    default: return "unknown";
    }
}
}
