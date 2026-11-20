#pragma once
#include "BedrockWeather.h"
class Level;
namespace ysm {
void clearNativeWeather();
void initializeNativeWeather(Level const* level, double rain, double lightning);
void receiveNativeWeather(Level const* level, int event, int data);
BedrockWeatherSnapshot readNativeWeather(Level const& level);
}
