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

namespace {
double bounded(double value, double fallback, double lo, double hi) {
    return std::clamp(std::isfinite(value) ? value : fallback, lo, hi);
}
HydrologyConfig safeHydrology(const HydrologyConfig& c) {
    HydrologyConfig d;
    return {bounded(c.rainMmPerHour, d.rainMmPerHour, 0, 10000),
            bounded(c.surfaceCapacityMm, d.surfaceCapacityMm, 0.001, 1000),
            bounded(c.puddleCapacityMm, d.puddleCapacityMm, 0.001, 1000),
            bounded(c.evaporationMmPerHour, d.evaporationMmPerHour, 0, 10000),
            bounded(c.drainageMmPerHour, d.drainageMmPerHour, 0, 10000)};
}
} // namespace

HydrologyState stepHydrology(HydrologyState s, double rain, double dt, const HydrologyConfig& config) {
    const auto c = safeHydrology(config);
    s.surfaceWaterMm = bounded(s.surfaceWaterMm, 0, 0, c.surfaceCapacityMm);
    s.puddleWaterMm = bounded(s.puddleWaterMm, 0, 0, c.puddleCapacityMm);
    dt = bounded(dt, 0, 0, 86400);
    rain = bounded(rain, 0, 0, 1);
    const double evaporation = c.evaporationMmPerHour / 3600;
    const double input = rain * c.rainMmPerHour / 3600;
    const double net = input - evaporation;
    const double loss = c.drainageMmPerHour / 3600 + evaporation;
    // Rain first replenishes the surface film. Only overflow feeds depressions.
    // Split exactly at saturation so large and small steps agree for constant rain.
    double filling = dt;
    if (net > 0) filling = std::min(dt, (c.surfaceCapacityMm - s.surfaceWaterMm) / net);
    s.puddleWaterMm = std::max(0.0, s.puddleWaterMm - loss * filling);
    if (net > 0) {
        s.puddleWaterMm = std::clamp(s.puddleWaterMm + (net - loss) * (dt - filling), 0.0, c.puddleCapacityMm);
    }
    s.surfaceWaterMm = std::clamp(s.surfaceWaterMm + net * dt, 0.0, c.surfaceCapacityMm);
    return s;
}

HydrologySurface hydrologySurface(const HydrologyState& state, float authoredWetness, float authoredPuddles,
                                  const HydrologyConfig& config) {
    const auto c = safeHydrology(config);
    const auto s = stepHydrology(state, 0, 0, c);
    const double baseWet = bounded(authoredWetness, 0, 0, 1);
    const double ceiling = bounded(authoredPuddles, 0, 0, 1);
    const double water = s.puddleWaterMm / c.puddleCapacityMm;
    const double wet = std::max({baseWet, s.surfaceWaterMm / c.surfaceCapacityMm, water});
    // Preserve authored standing-water appearance; rain storage grows beyond it.
    // A surviving pool must not disappear merely because its surface film dried.
    const double coverage = ceiling * std::max(baseWet, water);
    return {static_cast<float>(wet), static_cast<float>(wet > 0 ? coverage / wet : 0)};
}

void TerrainWater::reset() {
    m_active = false;
    m_depth.fill(0);
    m_remainder = m_rainIntegral = m_dropped = 0;
    m_reverse = false;
}

bool TerrainWater::initialize(const std::array<double, cells>& bed, double spacing) {
    reset();
    if (!std::isfinite(spacing) || spacing < 0.01 || spacing > 10000) return false;
    for (double h : bed) if (!std::isfinite(h) || std::abs(h) > 1e7) return false;
    m_bed = bed;
    m_spacing = spacing;
    m_active = true;
    return true;
}

double TerrainWater::depth(int x, int z) const {
    return m_active && x >= 0 && z >= 0 && x < resolution && z < resolution ? m_depth[z * resolution + x] : 0;
}
bool TerrainWater::setDepth(int x, int z, double metres) {
    if (!m_active || x < 0 || z < 0 || x >= resolution || z >= resolution ||
        !std::isfinite(metres) || metres < 0 || metres > 1000) return false;
    m_depth[z * resolution + x] = metres;
    return true;
}
double TerrainWater::volume() const {
    double sum = 0;
    if (m_active) for (double d : m_depth) sum += d;
    return sum * m_spacing * m_spacing;
}

void TerrainWater::tick(double rain, const HydrologyConfig& config) {
    const auto c = safeHydrology(config);
    const double input = rain * c.rainMmPerHour * tickSeconds / 3600000;
    const double loss = (c.evaporationMmPerHour + c.drainageMmPerHour) * tickSeconds / 3600000;
    for (double& d : m_depth) d = std::max(0.0, d + input - loss);
    // Diffusive hydraulic-head relaxation (diffusivity 2 m^2/s). The half-head
    // limiter cannot overshoot pair equilibrium; donor limiter cannot go dry.
    // In-place face sweeps avoid flux scratch buffers. Alternating direction
    // reduces ordering bias. This is not a shallow-water momentum solver.
    const double fraction = std::min(0.5, 2.0 * tickSeconds / (m_spacing * m_spacing));
    auto exchange = [&](int a, int b) {
        const double head = (m_bed[a] - m_bed[b]) + (m_depth[a] - m_depth[b]);
        const double transfer = std::clamp(head * fraction, -m_depth[b], m_depth[a]);
        m_depth[a] -= transfer;
        m_depth[b] += transfer;
    };
    for (int k = 0; k < cells; ++k) {
        const int i = m_reverse ? cells - 1 - k : k;
        if (i % resolution + 1 < resolution) exchange(i, i + 1);
        if (i / resolution + 1 < resolution) exchange(i, i + resolution);
    }
    m_reverse = !m_reverse;
}

int TerrainWater::advance(double rain, double dt, const HydrologyConfig& config) {
    if (!m_active || !std::isfinite(dt) || dt <= 0) return 0;
    constexpr double budget = maxSubsteps * tickSeconds;
    const double accepted = std::min(dt, budget);
    m_dropped = std::min(1e12, m_dropped + (dt - accepted));
    rain = bounded(rain, 0, 0, 1);
    double remaining = accepted;
    int steps = 0;
    while (remaining > 0 && steps < maxSubsteps) {
        const double part = std::min(remaining, tickSeconds - m_remainder);
        m_rainIntegral += rain * part;
        m_remainder += part;
        remaining -= part;
        if (m_remainder + 1e-12 < tickSeconds) break;
        tick(std::clamp(m_rainIntegral / tickSeconds, 0.0, 1.0), config);
        m_remainder = m_rainIntegral = 0;
        ++steps;
    }
    // The residual after four ticks is always less than one tick.
    m_remainder += remaining;
    m_rainIntegral += rain * remaining;
    return steps;
}

HydrologyState TerrainWater::equivalentStorage(const HydrologyConfig& config) const {
    const auto c = safeHydrology(config);
    HydrologyState s;
    if (m_active) for (double d : m_depth) {
        const double mm = d * 1000;
        s.surfaceWaterMm += std::min(mm, c.surfaceCapacityMm) / cells;
        s.puddleWaterMm += std::clamp(mm - c.surfaceCapacityMm, 0.0, c.puddleCapacityMm) / cells;
    }
    return s;
}

} // namespace fledge::gameplay
