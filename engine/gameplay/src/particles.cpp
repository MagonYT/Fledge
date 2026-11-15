#include "fledge/gameplay/particles.h"

#include <algorithm>
#include <cmath>

namespace fledge::gameplay {

namespace {
constexpr float kGravity = 9.81f;
uint32_t xorshift(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
Vec3 rotateDir(const Mat4& m, Vec3 v) { return (m * Vec4(v, 0.0f)).xyz(); }
} // namespace

float ParticleEmitter::random() { return static_cast<float>(xorshift(m_rng) >> 8) * (1.0f / 16777216.0f); }

void ParticleEmitter::spawn(const EmitterParams& p, const Mat4& emitter) {
    if (static_cast<int>(m_particles.size()) >= std::max(p.maxParticles, 0)) return;
    Particle q;
    // position in the spawn shape (emitter-local)
    Vec3 local{0, 0, 0};
    if (p.shape == EmitterShape::Sphere) {
        Vec3 d;
        do d = Vec3{random() * 2 - 1, random() * 2 - 1, random() * 2 - 1};
        while (dot(d, d) > 1.0f);
        local = d * p.extent.x;
    } else if (p.shape == EmitterShape::Box) {
        local = Vec3{(random() * 2 - 1) * p.extent.x, (random() * 2 - 1) * p.extent.y, (random() * 2 - 1) * p.extent.z};
    }
    // direction in a cone around the axis (uniform over the spherical cap)
    Vec3 axis = normalize(p.direction);
    if (dot(axis, axis) < 0.5f) axis = {0, 1, 0};
    Vec3 helper = std::fabs(axis.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    Vec3 t1 = normalize(cross(helper, axis)), t2 = cross(axis, t1);
    float cosMax = std::cos(radians(std::clamp(p.spreadDeg, 0.0f, 180.0f)));
    float cosT = 1.0f - random() * (1.0f - cosMax);
    float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
    float phi = random() * 2.0f * kPi;
    Vec3 dir = axis * cosT + t1 * (sinT * std::cos(phi)) + t2 * (sinT * std::sin(phi));
    float speed = p.speed * (1.0f + (random() * 2 - 1) * p.speedJitter);
    q.life = std::max(0.01f, p.lifetime * (1.0f + (random() * 2 - 1) * p.lifetimeJitter));
    if (p.worldSpace) {
        q.position = emitter.transformPoint(local);
        q.velocity = normalize(rotateDir(emitter, dir)) * speed;
    } else {
        q.position = local;
        q.velocity = dir * speed;
    }
    m_particles.push_back(q);
}

void ParticleEmitter::update(float dt, const EmitterParams& p, const Mat4& emitter, Vec3 wind) {
    if (dt <= 0) return;
    // wind and gravity are world-space; local-space emitters see them in emitter axes
    Vec3 gravity{0, -kGravity * p.gravity, 0};
    if (!p.worldSpace) {
        Mat4 inv = emitter.inverse();
        gravity = rotateDir(inv, gravity);
        wind = rotateDir(inv, wind);
    }
    float keep = p.drag > 0 ? std::exp(-p.drag * dt) : 1.0f;
    for (auto& q : m_particles) {
        q.age += dt;
        q.velocity += gravity * dt;
        if (p.drag > 0) q.velocity = wind + (q.velocity - wind) * keep;
        q.position += q.velocity * dt;
    }
    std::erase_if(m_particles, [](const Particle& q) { return q.age >= q.life; });
    m_accum += std::max(p.rate, 0.0f) * dt;
    int n = static_cast<int>(m_accum);
    m_accum -= static_cast<float>(n);
    for (int i = 0; i < n; ++i) spawn(p, emitter);
}

void ParticleEmitter::burst(int count, const EmitterParams& p, const Mat4& emitter) {
    for (int i = 0; i < count; ++i) spawn(p, emitter);
}

void ParticleEmitter::clear() {
    m_particles.clear();
    m_accum = 0;
}

void ParticleEmitter::sprites(const EmitterParams& p, const Mat4& emitter, std::vector<ParticleSprite>& out) const {
    out.reserve(out.size() + m_particles.size());
    for (const auto& q : m_particles) {
        float t = std::clamp(q.age / q.life, 0.0f, 1.0f);
        ParticleSprite s;
        s.position = p.worldSpace ? q.position : emitter.transformPoint(q.position);
        s.velocity = p.worldSpace ? q.velocity : rotateDir(emitter, q.velocity);
        s.size = p.startSize + (p.endSize - p.startSize) * t;
        s.stretch = p.stretch;
        Vec3 c = lerp(p.startColor, p.endColor, t) * p.intensity;
        // fade in over the first 10 % of the life so spawns never pop
        float opacity = (p.startOpacity + (p.endOpacity - p.startOpacity) * t) * std::min(1.0f, t * 10.0f + 0.1f);
        s.color = Vec4(c, std::clamp(opacity, 0.0f, 1.0f));
        s.additive = p.additive;
        out.push_back(s);
    }
}

EmitterParams particlePreset(const std::string& name) {
    EmitterParams p;
    if (name == "Smoke") {
        p.rate = 12;
        p.maxParticles = 200;
        p.lifetime = 5;
        p.speed = 1.2f;
        p.spreadDeg = 15;
        p.shape = EmitterShape::Sphere;
        p.extent = {0.3f, 0.3f, 0.3f};
        p.gravity = -0.03f;
        p.drag = 0.4f;
        p.startSize = 0.5f;
        p.endSize = 3.0f;
        p.startColor = {0.35f, 0.35f, 0.35f};
        p.endColor = {0.55f, 0.55f, 0.56f};
        p.startOpacity = 0.6f;
        p.endOpacity = 0;
    } else if (name == "Fire") {
        p.rate = 60;
        p.maxParticles = 300;
        p.lifetime = 0.9f;
        p.speed = 1.5f;
        p.spreadDeg = 12;
        p.shape = EmitterShape::Sphere;
        p.extent = {0.35f, 0.35f, 0.35f};
        p.gravity = -0.15f;
        p.drag = 1.0f;
        p.startSize = 0.6f;
        p.endSize = 0.15f;
        p.startColor = {1.0f, 0.55f, 0.15f};
        p.endColor = {0.8f, 0.15f, 0.03f};
        p.startOpacity = 0.7f;
        p.endOpacity = 0;
        p.intensity = 2.5f;
        p.additive = true;
    } else if (name == "Sparks") {
        p.rate = 40;
        p.maxParticles = 300;
        p.lifetime = 0.8f;
        p.speed = 6;
        p.speedJitter = 0.5f;
        p.spreadDeg = 50;
        p.gravity = 1;
        p.startSize = 0.03f;
        p.endSize = 0.01f;
        p.startColor = {1.0f, 0.8f, 0.4f};
        p.endColor = {1.0f, 0.3f, 0.05f};
        p.startOpacity = 1;
        p.endOpacity = 0.2f;
        p.intensity = 8;
        p.stretch = 0.03f;
        p.additive = true;
    } else if (name == "Dust") {
        p.rate = 8;
        p.maxParticles = 150;
        p.lifetime = 6;
        p.speed = 0.15f;
        p.spreadDeg = 180;
        p.shape = EmitterShape::Box;
        p.extent = {4, 1, 4};
        p.drag = 0.5f;
        p.startSize = 0.4f;
        p.endSize = 1.2f;
        p.startColor = p.endColor = {0.6f, 0.52f, 0.4f};
        p.startOpacity = 0.25f;
        p.endOpacity = 0;
    } else if (name == "Steam") {
        p.rate = 15;
        p.maxParticles = 150;
        p.lifetime = 2.5f;
        p.speed = 1.8f;
        p.spreadDeg = 10;
        p.gravity = -0.05f;
        p.drag = 0.8f;
        p.startSize = 0.3f;
        p.endSize = 1.6f;
        p.startColor = p.endColor = {0.9f, 0.92f, 0.95f};
        p.startOpacity = 0.4f;
        p.endOpacity = 0;
    } else if (name == "Rain") {
        p.rate = 600;
        p.maxParticles = 2000;
        p.lifetime = 1.5f;
        p.lifetimeJitter = 0.1f;
        p.direction = {0, -1, 0};
        p.speed = 9;
        p.speedJitter = 0.1f;
        p.spreadDeg = 2;
        p.shape = EmitterShape::Box;
        p.extent = {10, 0.5f, 10};
        p.startSize = p.endSize = 0.02f;
        p.startColor = p.endColor = {0.62f, 0.66f, 0.72f};
        p.startOpacity = p.endOpacity = 0.35f;
        p.stretch = 0.035f;
    } else if (name == "Snow") {
        p.rate = 150;
        p.maxParticles = 2000;
        p.lifetime = 10;
        p.direction = {0, -1, 0};
        p.speed = 1;
        p.speedJitter = 0.3f;
        p.spreadDeg = 25;
        p.shape = EmitterShape::Box;
        p.extent = {10, 0.5f, 10};
        p.drag = 0.6f;
        p.startSize = p.endSize = 0.05f;
        p.startColor = p.endColor = {0.95f, 0.95f, 1.0f};
        p.startOpacity = p.endOpacity = 0.9f;
    }
    return p;
}

const std::vector<std::string>& particlePresetNames() {
    static const std::vector<std::string> names{"Smoke", "Fire", "Sparks", "Dust", "Steam", "Rain", "Snow"};
    return names;
}

// ------------------------------------------------------------ precipitation ---
float Precipitation::random() { return static_cast<float>(xorshift(m_rng) >> 8) * (1.0f / 16777216.0f); }

Vec3 Precipitation::windVelocity(const Weather& w) { return normalize(Vec3{1.0f, 0.0f, 0.35f}) * (std::clamp(w.wind, 0.0f, 1.0f) * 10.0f); }

void Precipitation::update(float dt, const Weather& w, Vec3 camera) {
    if (dt <= 0) return;
    size_t target = static_cast<size_t>(std::clamp(w.rain, 0.0f, 1.0f) * static_cast<float>(kMaxDrops));
    Vec3 wind = windVelocity(w);
    auto respawn = [&](Particle& d, bool anywhere) {
        d.position = {camera.x + (random() * 2 - 1) * kRadius, camera.y + (anywhere ? random() * (kTop + kBottom) - kBottom : kTop - random() * 2.0f),
                      camera.z + (random() * 2 - 1) * kRadius};
        d.velocity = Vec3{wind.x * 0.8f, -(8.0f + random() * 2.0f), wind.z * 0.8f};
        d.age = 0;
        d.life = 1e9f;  // recycled by height, not age
    };
    for (auto& d : m_drops) {
        d.age += dt;
        d.position += d.velocity * dt;
        if (d.position.y < camera.y - kBottom) {
            respawn(d, false);
            continue;
        }
        // keep the column centred on the camera: wrap drops that leave the box sideways
        for (int axis : {0, 2}) {
            float off = d.position[axis] - camera[axis];
            if (off > kRadius) d.position[axis] -= 2 * kRadius;
            else if (off < -kRadius) d.position[axis] += 2 * kRadius;
        }
        if (d.position.y > camera.y + kTop + 2) respawn(d, true);  // camera dropped a lot (teleport)
    }
    if (m_drops.size() > target) {  // rain easing off: remove a share per second
        size_t remove = std::max<size_t>(1, static_cast<size_t>(static_cast<float>(kMaxDrops) * dt * 0.5f));
        m_drops.resize(m_drops.size() - std::min(remove, m_drops.size() - target));
    }
    while (m_drops.size() < target) {  // rain starting: fill the whole column so it never arrives as a sheet
        Particle d;
        respawn(d, true);
        m_drops.push_back(d);
    }
}

void Precipitation::sprites(std::vector<ParticleSprite>& out, float brightness) const {
    out.reserve(out.size() + m_drops.size());
    Vec3 c = Vec3{0.62f, 0.66f, 0.72f} * std::max(brightness, 0.02f);
    for (const auto& d : m_drops) {
        ParticleSprite s;
        s.position = d.position;
        s.velocity = d.velocity;
        s.size = 0.02f;
        s.stretch = 0.035f;
        s.color = Vec4(c, 0.35f);
        out.push_back(s);
    }
}

WeatherFog weatherFog(const Weather& w, const SkyLight& s) {
    WeatherFog f;
    float haze = std::clamp(w.haze, 0.0f, 1.0f), rain = std::clamp(w.rain, 0.0f, 1.0f);
    f.density = 0.07f * haze * haze + 0.004f * rain;  // Fog preset ~0.06 (about 50 m visibility), Rain ~0.009
    f.heightFalloff = 0.04f + 0.06f * (1.0f - haze);  // thick fog hugs the ground less tightly
    Vec3 grey{0.7f, 0.74f, 0.8f};
    Vec3 tinted = grey * s.color;  // warm at dusk, blue under the moon
    f.color = lerp(grey, tinted, 0.35f) * (0.85f * std::max(s.environment, 0.0f));
    return f;
}

} // namespace fledge::gameplay
