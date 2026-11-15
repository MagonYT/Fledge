#pragma once
// Day/night cycle and weather (docs/GAMEPLAY.md "Time of day & weather"): sun / moon position from the hour,
// light colour and intensity, sky brightness, and weather presets that dim the light and add rain + wind.
#include <string>

#include "fledge/core/math.h"

namespace fledge::gameplay {

struct SkyLight {
    Vec3 toSun;            // unit vector towards the sun (sky shader, horizon tests)
    Vec3 lightDirection;   // direction the directional light shines (towards the ground): sun by day, moon at night
    Vec3 color{1, 1, 1};
    float intensity = 3;
    float environment = 1;  // sky / image-based light multiplier
    float exposure = 0;     // EV offset (nights are lifted a little so games stay playable)
    float elevationDeg = 0;
    bool night = false;
};
// hour 0..24, latitude in degrees, dayOfYear 1..365. North is -Z, east is +X.
SkyLight skyAt(float hour, float latitudeDeg = 40, int dayOfYear = 172);

enum class WeatherKind { Clear = 0, Cloudy = 1, Overcast = 2, Rain = 3, Storm = 4, Fog = 5 };
const char* toString(WeatherKind w);
WeatherKind weatherFromString(const std::string& s);
struct Weather {
    float cloud = 0, rain = 0, wind = 0.1f, haze = 0;  // 0..1
};
Weather weatherPreset(WeatherKind k);
Weather blend(const Weather& a, const Weather& b, float t);
// Applies weather to the sky light (dimmer, flatter, slightly brighter exposure under clouds).
SkyLight applyWeather(SkyLight s, const Weather& w);

} // namespace fledge::gameplay
