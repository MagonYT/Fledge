// Fledge Script math: small value types for gameplay code (meters, radians unless noted).
#pragma once
#include <algorithm>
#include <cmath>

namespace fledge::script {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float Radians(float degrees) { return degrees * (kPi / 180.0f); }
inline constexpr float Degrees(float radians) { return radians * (180.0f / kPi); }
inline float Clamp(float v, float lo, float hi) { return std::clamp(v, lo, hi); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
// Frame-rate independent exponential smoothing factor: Lerp(a, b, Damp(10, dt)).
inline float Damp(float sharpness, float dt) { return 1.0f - std::exp(-sharpness * dt); }

struct Vec3 {
    float x = 0, y = 0, z = 0;
    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    static constexpr Vec3 Zero() { return {0, 0, 0}; }
    static constexpr Vec3 One() { return {1, 1, 1}; }
    static constexpr Vec3 Up() { return {0, 1, 0}; }
    static constexpr Vec3 Right() { return {1, 0, 0}; }
    static constexpr Vec3 Forward() { return {0, 0, -1}; }  // Fledge cameras and lights look down -Z

    constexpr Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(Vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    constexpr bool operator==(const Vec3&) const = default;

    float Length() const { return std::sqrt(x * x + y * y + z * z); }
    constexpr float LengthSquared() const { return x * x + y * y + z * z; }
    Vec3 Normalized() const {
        float l = Length();
        return l > 1e-6f ? *this / l : Vec3{};
    }
    static constexpr float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    static constexpr Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    static Vec3 Lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
    static float Distance(Vec3 a, Vec3 b) { return (a - b).Length(); }
};
inline constexpr Vec3 operator*(float s, Vec3 v) { return v * s; }

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    constexpr Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    static constexpr Quat Identity() { return {}; }
    static Quat FromAxisAngle(Vec3 axis, float radians) {
        Vec3 a = axis.Normalized();
        float s = std::sin(radians * 0.5f);
        return {a.x * s, a.y * s, a.z * s, std::cos(radians * 0.5f)};
    }
    // Same convention as the engine and the Inspector: yaw (Y) * pitch (X) * roll (Z), radians.
    static Quat FromEuler(float pitch, float yaw, float roll = 0) {
        return FromAxisAngle({0, 1, 0}, yaw) * FromAxisAngle({1, 0, 0}, pitch) * FromAxisAngle({0, 0, 1}, roll);
    }
    // Rotation whose forward (-Z) points along `forward`.
    static Quat LookRotation(Vec3 forward, Vec3 up = Vec3::Up()) {
        Vec3 f = forward.Normalized();
        if (f.LengthSquared() < 1e-8f) return {};
        float yaw = std::atan2(-f.x, -f.z);
        float pitch = std::asin(Clamp(f.y, -1.0f, 1.0f));
        (void)up;
        return FromEuler(pitch, yaw);
    }
    constexpr Quat operator*(Quat q) const {
        return {w * q.x + x * q.w + y * q.z - z * q.y, w * q.y - x * q.z + y * q.w + z * q.x, w * q.z + x * q.y - y * q.x + z * q.w,
                w * q.w - x * q.x - y * q.y - z * q.z};
    }
    Vec3 Rotate(Vec3 v) const {
        Vec3 u{x, y, z};
        Vec3 t = Vec3::Cross(u, v) * 2.0f;
        return v + t * w + Vec3::Cross(u, t);
    }
    Quat Normalized() const {
        float l = std::sqrt(x * x + y * y + z * z + w * w);
        return l > 1e-8f ? Quat{x / l, y / l, z / l, w / l} : Quat{};
    }
    constexpr Quat Conjugate() const { return {-x, -y, -z, w}; }
    static Quat Slerp(Quat a, Quat b, float t) {
        float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (d < 0) {
            b = {-b.x, -b.y, -b.z, -b.w};
            d = -d;
        }
        if (d > 0.9995f) return Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}.Normalized();
        float th = std::acos(d), s = std::sin(th);
        float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
        return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
    }
};

struct Color {
    float r = 1, g = 1, b = 1;
    constexpr bool operator==(const Color&) const = default;
};

} // namespace fledge::script
