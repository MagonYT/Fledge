#include "fledge/gameplay/environment.h"

#include <algorithm>
#include <cmath>

namespace fledge::gameplay {

namespace {
float smooth(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
Vec3 mix3(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
} // namespace

SkyLight skyAt(float hour, float latitudeDeg, int dayOfYear) {
    constexpr float kDeg = 3.14159265f / 180.0f;
    hour = std::fmod(std::fmod(hour, 24.0f) + 24.0f, 24.0f);
    float decl = 23.44f * kDeg * std::sin(2 * 3.14159265f * static_cast<float>(284 + dayOfYear) / 365.0f);
    float h = (hour - 12.0f) * 15.0f * kDeg;
    float lat = latitudeDeg * kDeg;
    float east = -std::cos(decl) * std::sin(h);
    float north = std::sin(decl) * std::cos(lat) - std::cos(decl) * std::sin(lat) * std::cos(h);
    float up = std::sin(decl) * std::sin(lat) + std::cos(decl) * std::cos(lat) * std::cos(h);
    SkyLight s;
    s.toSun = normalize(Vec3{east, up, -north});
    s.elevationDeg = std::asin(std::clamp(s.toSun.y, -1.0f, 1.0f)) / kDeg;
    float day = smooth(-4, 6, s.elevationDeg);   // 0 night .. 1 day
    float golden = 1.0f - smooth(2, 20, s.elevationDeg);  // warm light near the horizon
    s.night = s.elevationDeg < -4;
    if (s.elevationDeg > -4) {
        s.lightDirection = s.toSun * -1.0f;
        s.color = mix3(Vec3{1.0f, 0.97f, 0.92f}, Vec3{1.0f, 0.55f, 0.3f}, golden);
        s.intensity = 3.2f * smooth(-4, 12, s.elevationDeg) + 0.05f;
    } else {  // moonlight: opposite the sun, cool and dim
        Vec3 moon = normalize(Vec3{-s.toSun.x, std::max(0.35f, -s.toSun.y), -s.toSun.z});
        s.lightDirection = moon * -1.0f;
        s.color = {0.55f, 0.65f, 1.0f};
        s.intensity = 0.25f;
    }
    s.environment = 0.06f + 0.94f * day;
    s.exposure = (1.0f - day) * 1.2f;
    return s;
}

const char* toString(WeatherKind w) {
    switch (w) {
        case WeatherKind::Cloudy: return "Cloudy";
        case WeatherKind::Overcast: return "Overcast";
        case WeatherKind::Rain: return "Rain";
        case WeatherKind::Storm: return "Storm";
        case WeatherKind::Fog: return "Fog";
        default: return "Clear";
    }
}
WeatherKind weatherFromString(const std::string& s) {
    for (int i = 0; i <= 5; ++i)
        if (s == toString(static_cast<WeatherKind>(i))) return static_cast<WeatherKind>(i);
    return WeatherKind::Clear;
}

Weather weatherPreset(WeatherKind k) {
    switch (k) {
        case WeatherKind::Cloudy: return {0.45f, 0, 0.3f, 0.1f};
        case WeatherKind::Overcast: return {0.85f, 0, 0.35f, 0.2f};
        case WeatherKind::Rain: return {0.9f, 0.7f, 0.5f, 0.3f};
        case WeatherKind::Storm: return {1.0f, 1.0f, 0.9f, 0.4f};
        case WeatherKind::Fog: return {0.6f, 0, 0.05f, 0.9f};
        default: return {0, 0, 0.1f, 0};
    }
}

Weather blend(const Weather& a, const Weather& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {a.cloud + (b.cloud - a.cloud) * t, a.rain + (b.rain - a.rain) * t, a.wind + (b.wind - a.wind) * t, a.haze + (b.haze - a.haze) * t};
}

SkyLight applyWeather(SkyLight s, const Weather& w) {
    float c = std::clamp(std::max(w.cloud, w.haze), 0.0f, 1.0f);
    s.intensity *= 1.0f - 0.8f * c;
    s.environment *= 1.0f - 0.35f * c;
    s.exposure += 0.4f * c;
    Vec3 grey{0.85f, 0.88f, 0.92f};
    s.color = s.color + (grey - s.color) * (0.6f * c);
    return s;
}

} // namespace fledge::gameplay
