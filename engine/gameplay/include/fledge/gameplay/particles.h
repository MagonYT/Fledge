#pragma once
// Particles (docs/GAMEPLAY.md "Particles"): CPU-simulated emitters (spawn shape + cone, gravity, drag towards the
// wind, size / colour / opacity over life) that produce camera-facing sprites for the renderer, presets (smoke,
// fire, sparks, dust, steam, rain, snow), and the weather's precipitation volume that follows the camera.
// Deterministic for a given seed and dt sequence.
#include <cstdint>
#include <string>
#include <vector>

#include "fledge/core/math.h"
#include "fledge/gameplay/environment.h"

namespace fledge::gameplay {

enum class EmitterShape { Point = 0, Sphere = 1, Box = 2 };

struct EmitterParams {
    float rate = 20;              // particles per second (continuous)
    int maxParticles = 500;       // alive at once; spawning stops at the cap
    float lifetime = 2;           // seconds
    float lifetimeJitter = 0.25f; // +- fraction of lifetime
    Vec3 direction{0, 1, 0};      // emission axis (emitter-local)
    float speed = 2;              // m/s along the cone
    float speedJitter = 0.25f;    // +- fraction of speed
    float spreadDeg = 20;         // cone half-angle (0 = straight along direction, 180 = all around)
    EmitterShape shape = EmitterShape::Point;
    Vec3 extent{0.5f, 0.5f, 0.5f};  // Sphere: radius = extent.x; Box: half extents (emitter-local)
    float gravity = 0;            // multiples of 9.81 m/s^2 downwards (negative rises: smoke, flames)
    float drag = 0;               // 1/s: velocity relaxes towards the wind
    float startSize = 0.2f, endSize = 0.6f;  // m
    Vec3 startColor{1, 1, 1}, endColor{1, 1, 1};
    float startOpacity = 1, endOpacity = 0;
    float intensity = 1;          // colour multiplier (> 1 glows through bloom)
    float stretch = 0;            // velocity streaks: sprite length += speed * stretch (s)
    bool additive = false;        // fire / sparks
    bool worldSpace = true;       // false: particles move with the emitter
};

struct Particle {
    Vec3 position, velocity;      // world space (worldSpace) or emitter-local
    float age = 0, life = 1;
};

// What the renderer draws (framework converts it to fledge::ParticleItem).
struct ParticleSprite {
    Vec3 position, velocity;
    float size = 0.1f, stretch = 0;
    Vec4 color{1, 1, 1, 1};       // linear rgb * intensity, a = opacity
    bool additive = false;
};

class ParticleEmitter {
public:
    explicit ParticleEmitter(uint32_t seed = 1) : m_rng(seed ? seed : 1) {}
    // Ages and moves the particles (gravity, drag towards `wind`), then spawns rate * dt new ones at `emitter`.
    void update(float dt, const EmitterParams& p, const Mat4& emitter, Vec3 wind = {0, 0, 0});
    // Spawns `count` particles at once (up to maxParticles).
    void burst(int count, const EmitterParams& p, const Mat4& emitter);
    void clear();
    void shift(Vec3 delta) {  // floating-origin rebase (world-space emitters)
        for (auto& q : m_particles) q.position += delta;
    }
    const std::vector<Particle>& particles() const { return m_particles; }
    size_t size() const { return m_particles.size(); }
    // Appends one sprite per particle (size / colour / opacity interpolated over its life).
    void sprites(const EmitterParams& p, const Mat4& emitter, std::vector<ParticleSprite>& out) const;

private:
    void spawn(const EmitterParams& p, const Mat4& emitter);
    float random();  // [0, 1)
    uint32_t m_rng;
    float m_accum = 0;
    std::vector<Particle> m_particles;
};

// Presets: "Smoke", "Fire", "Sparks", "Dust", "Steam", "Rain", "Snow" (unknown names give the defaults).
EmitterParams particlePreset(const std::string& name);
const std::vector<std::string>& particlePresetNames();

// Rain around the camera, driven by Weather::rain and Weather::wind: drops spawn in a box above the viewer and
// are recycled when they fall below it or drift out of the box, so density stays constant while moving.
class Precipitation {
public:
    static constexpr int kMaxDrops = 4000;
    static constexpr float kRadius = 25, kTop = 16, kBottom = 4;  // m around / above / below the camera
    explicit Precipitation(uint32_t seed = 7) : m_rng(seed ? seed : 7) {}
    void update(float dt, const Weather& w, Vec3 camera);
    // brightness: sky light scale (night rain is dim)
    void sprites(std::vector<ParticleSprite>& out, float brightness) const;
    size_t size() const { return m_drops.size(); }
    void shift(Vec3 delta) {
        for (auto& d : m_drops) d.position += delta;
    }
    // Horizontal wind (m/s) for a weather state; also tilts emitters and smoke.
    static Vec3 windVelocity(const Weather& w);

private:
    float random();
    uint32_t m_rng;
    std::vector<Particle> m_drops;
};

// Height fog for a weather state (haze + rain), tinted by the sky light: density 0 = no weather fog. The colour is
// for an environment intensity of 1 (multiply by the project's environmentIntensity).
struct WeatherFog {
    float density = 0;        // extinction per meter at ground level
    float heightFalloff = 0.05f;
    Vec3 color{0.7f, 0.74f, 0.8f};
};
WeatherFog weatherFog(const Weather& w, const SkyLight& s);

} // namespace fledge::gameplay
