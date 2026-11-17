#pragma once
// Day/night cycle and weather (docs/GAMEPLAY.md "Time of day & weather"): sun / moon position from the hour,
// light colour and intensity, sky brightness, and weather presets that dim the light and add rain + wind.
#include <string>
#include <array>

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

// Lumped hydrology, not a spatial fluid solver. Depths are equivalent millimetres
// over one representative surface; time is gameplay seconds (not day/night time).
struct HydrologyConfig {
    double rainMmPerHour = 120;       // at weather.rain == 1; deliberately storm-like
    double surfaceCapacityMm = 0.2;
    double puddleCapacityMm = 2;
    double evaporationMmPerHour = 3;
    double drainageMmPerHour = 12;    // puddles only; overflow beyond capacity runs off
};
struct HydrologyState {
    double surfaceWaterMm = 0;
    double puddleWaterMm = 0;
};
// Exact piecewise-linear reservoir update for constant rain over dt. Invalid dt
// does not advance time; a single call is capped at one day. Invalid config uses
// defaults, finite config is bounded. State is sanitized even when dt is invalid.
HydrologyState stepHydrology(HydrologyState state, double rain, double dt, const HydrologyConfig& config = {});
struct HydrologySurface {
    float wetness = 0;
    float puddles = 0;
};
// Authored wetness is a permanent floor, authored puddles is the coverage ceiling
// (zero disables puddles). Compensates for the shader's wetness * puddles mask.
HydrologySurface hydrologySurface(const HydrologyState& state, float authoredWetness, float authoredPuddles,
                                  const HydrologyConfig& config = {});

// Bounded CPU surface-water patch. Cell-centred bed/depth in metres, seconds,
// closed borders; no momentum, infiltration geology, obstacles or water meshes.
// Pairwise relaxation is conservative and positivity preserving at any slope.
class TerrainWater {
public:
    static constexpr int resolution = 64;
    static constexpr int cells = resolution * resolution;
    static constexpr double tickSeconds = 0.05;
    static constexpr int maxSubsteps = 4;
    // Invalid bed/spacing rejects the entire patch (fallback remains usable).
    bool initialize(const std::array<double, cells>& bedMetres, double cellMetres);
    void reset();
    bool active() const { return m_active; }
    // No allocations. At most four ticks/call; excess wall time is discarded,
    // not queued. Rain is integrated across fractional ticks. Invalid dt ignored.
    int advance(double rain, double dt, const HydrologyConfig& config = {});
    double depth(int x, int z) const;
    bool setDepth(int x, int z, double metres); // initialization/debug seeding
    double volume() const;                    // cubic metres
    HydrologyState equivalentStorage(const HydrologyConfig& config = {}) const;
    double droppedSeconds() const { return m_dropped; }
private:
    std::array<double, cells> m_bed{}, m_depth{};
    double m_spacing = 1, m_remainder = 0, m_rainIntegral = 0, m_dropped = 0;
    bool m_active = false, m_reverse = false;
    void tick(double rain, const HydrologyConfig& config);
};

} // namespace fledge::gameplay
