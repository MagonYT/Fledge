#pragma once
// Minimal math library. Conventions:
//  - right-handed, +Y up, camera looks down -Z
//  - Mat4 is column-major (m[col][row]) to match OpenGL/GLSL
//  - angles in radians unless a name says Deg
#include <cmath>
#include <cstring>
#include <cstdint>

namespace fledge {

constexpr float kPi = 3.14159265358979323846f;
constexpr float radians(float deg) { return deg * (kPi / 180.0f); }
constexpr float degrees(float rad) { return rad * (180.0f / kPi); }

struct Vec2 {
    float x = 0, y = 0;
    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
    constexpr bool operator==(const Vec2&) const = default;
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    constexpr explicit Vec3(float s) : x(s), y(s), z(s) {}
    constexpr Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator*(Vec3 o) const { return {x * o.x, y * o.y, z * o.z}; }
    constexpr Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(Vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    constexpr bool operator==(const Vec3&) const = default;
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    constexpr Vec4() = default;
    constexpr Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr Vec4(Vec3 v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    constexpr Vec3 xyz() const { return {x, y, z}; }
    constexpr Vec4 operator+(Vec4 o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    constexpr Vec4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
    constexpr bool operator==(const Vec4&) const = default;
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};

inline constexpr float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline constexpr float dot(Vec4 a, Vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline constexpr Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(Vec3 v) {
    float l = length(v);
    return l > 1e-8f ? v / l : Vec3{0, 0, 0};
}
inline Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline bool nearlyEqual(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
inline bool nearlyEqual(Vec3 a, Vec3 b, float eps = 1e-4f) {
    return nearlyEqual(a.x, b.x, eps) && nearlyEqual(a.y, b.y, eps) && nearlyEqual(a.z, b.z, eps);
}

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    constexpr Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    static Quat fromAxisAngle(Vec3 axis, float angle) {
        Vec3 n = normalize(axis);
        float s = std::sin(angle * 0.5f);
        return {n.x * s, n.y * s, n.z * s, std::cos(angle * 0.5f)};
    }
    // Euler angles in radians, e = {pitch (about X), yaw (about Y), roll (about Z)}.
    // q = qY(yaw) * qX(pitch) * qZ(roll)  (roll applied first, yaw last).
    static Quat fromEuler(Vec3 e) {
        return fromAxisAngle({0, 1, 0}, e.y) * fromAxisAngle({1, 0, 0}, e.x) * fromAxisAngle({0, 0, 1}, e.z);
    }
    Vec3 toEuler() const; // inverse of fromEuler (pitch, yaw, roll)

    constexpr Quat operator*(Quat q) const {
        return {w * q.x + x * q.w + y * q.z - z * q.y, w * q.y - x * q.z + y * q.w + z * q.x,
                w * q.z + x * q.y - y * q.x + z * q.w, w * q.w - x * q.x - y * q.y - z * q.z};
    }
    Vec3 rotate(Vec3 v) const {
        Vec3 u{x, y, z};
        Vec3 t = cross(u, v) * 2.0f;
        return v + t * w + cross(u, t);
    }
    Quat normalized() const {
        float l = std::sqrt(x * x + y * y + z * z + w * w);
        return l > 0 ? Quat{x / l, y / l, z / l, w / l} : Quat{};
    }
    constexpr Quat conjugate() const { return {-x, -y, -z, w}; }
    constexpr bool operator==(const Quat&) const = default;
};

inline Vec3 Quat::toEuler() const {
    // Matches fromEuler: R = Ry(yaw) * Rx(pitch) * Rz(roll)
    Quat q = normalized();
    float sinPitch = 2.0f * (q.w * q.x - q.y * q.z);
    sinPitch = sinPitch > 1.0f ? 1.0f : (sinPitch < -1.0f ? -1.0f : sinPitch);
    float pitch = std::asin(sinPitch);
    float yaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    float roll = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.x * q.x + q.z * q.z));
    return {pitch, yaw, roll};
}

struct Mat4 {
    Vec4 c[4]; // columns

    constexpr Mat4() : c{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}} {}
    static constexpr Mat4 identity() { return {}; }
    const float* data() const { return &c[0].x; }
    float& at(int row, int col) { return c[col][row]; }
    float at(int row, int col) const { return c[col][row]; }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += at(row, k) * o.at(k, col);
                r.at(row, col) = s;
            }
        return r;
    }
    Vec4 operator*(Vec4 v) const {
        Vec4 r;
        for (int row = 0; row < 4; ++row) r[row] = at(row, 0) * v.x + at(row, 1) * v.y + at(row, 2) * v.z + at(row, 3) * v.w;
        return r;
    }
    Vec3 transformPoint(Vec3 p) const {
        Vec4 r = (*this) * Vec4(p, 1.0f);
        return r.w != 0 ? r.xyz() / r.w : r.xyz();
    }

    static Mat4 translation(Vec3 t) {
        Mat4 m;
        m.c[3] = {t.x, t.y, t.z, 1};
        return m;
    }
    static Mat4 scale(Vec3 s) {
        Mat4 m;
        m.c[0].x = s.x; m.c[1].y = s.y; m.c[2].z = s.z;
        return m;
    }
    static Mat4 rotation(Quat q) {
        Mat4 m;
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        m.c[0] = {1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy), 0};
        m.c[1] = {2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx), 0};
        m.c[2] = {2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy), 0};
        return m;
    }
    // OpenGL-style clip space (z in [-1,1]).
    static Mat4 perspective(float fovY, float aspect, float zNear, float zFar) {
        Mat4 m;
        float f = 1.0f / std::tan(fovY * 0.5f);
        m.c[0] = {f / aspect, 0, 0, 0};
        m.c[1] = {0, f, 0, 0};
        m.c[2] = {0, 0, (zFar + zNear) / (zNear - zFar), -1};
        m.c[3] = {0, 0, (2 * zFar * zNear) / (zNear - zFar), 0};
        return m;
    }
    // OpenGL-style orthographic projection (z in [-1,1]).
    static Mat4 ortho(float left, float right, float bottom, float top, float zNear, float zFar) {
        Mat4 m;
        m.c[0] = {2.0f / (right - left), 0, 0, 0};
        m.c[1] = {0, 2.0f / (top - bottom), 0, 0};
        m.c[2] = {0, 0, -2.0f / (zFar - zNear), 0};
        m.c[3] = {-(right + left) / (right - left), -(top + bottom) / (top - bottom), -(zFar + zNear) / (zFar - zNear), 1};
        return m;
    }
    static Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
        Vec3 f = normalize(target - eye);
        Vec3 s = normalize(cross(f, up));
        Vec3 u = cross(s, f);
        Mat4 m;
        m.c[0] = {s.x, u.x, -f.x, 0};
        m.c[1] = {s.y, u.y, -f.y, 0};
        m.c[2] = {s.z, u.z, -f.z, 0};
        m.c[3] = {-dot(s, eye), -dot(u, eye), dot(f, eye), 1};
        return m;
    }
    Mat4 transposed() const {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) r.at(i, j) = at(j, i);
        return r;
    }
    Mat4 inverse() const; // general 4x4 inverse
};

inline Mat4 Mat4::inverse() const {
    const float* m = data();
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 r;
    if (std::fabs(det) < 1e-12f) return r;
    det = 1.0f / det;
    float* o = &r.c[0].x;
    for (int i = 0; i < 16; ++i) o[i] = inv[i] * det;
    return r;
}

// Rotation (upper 3x3 must be orthonormal) -> quaternion.
inline Quat quatFromMatrix(const Mat4& m) {
    float m00 = m.at(0, 0), m11 = m.at(1, 1), m22 = m.at(2, 2);
    float tr = m00 + m11 + m22;
    Quat q;
    if (tr > 0) {
        float s = std::sqrt(tr + 1.0f) * 2.0f;
        q = {(m.at(2, 1) - m.at(1, 2)) / s, (m.at(0, 2) - m.at(2, 0)) / s, (m.at(1, 0) - m.at(0, 1)) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = {0.25f * s, (m.at(0, 1) + m.at(1, 0)) / s, (m.at(0, 2) + m.at(2, 0)) / s, (m.at(2, 1) - m.at(1, 2)) / s};
    } else if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = {(m.at(0, 1) + m.at(1, 0)) / s, 0.25f * s, (m.at(1, 2) + m.at(2, 1)) / s, (m.at(0, 2) - m.at(2, 0)) / s};
    } else {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = {(m.at(0, 2) + m.at(2, 0)) / s, (m.at(1, 2) + m.at(2, 1)) / s, 0.25f * s, (m.at(1, 0) - m.at(0, 1)) / s};
    }
    return q.normalized();
}

// IEEE 754 half <-> float (round-to-nearest-even not guaranteed; fine for color data).
inline uint16_t floatToHalf(float f) {
    uint32_t x;
    static_assert(sizeof(x) == sizeof(f));
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFF) == 0xFF) return static_cast<uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));  // inf/nan
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);                                            // overflow -> inf
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        mant |= 0x800000u;
        return static_cast<uint16_t>(sign | (mant >> (14 - exp)));
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
}
inline float halfToFloat(uint16_t h) {
    uint32_t sign = (static_cast<uint32_t>(h) & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1F, mant = h & 0x3FFu, x;
    if (exp == 0) {
        if (mant == 0) x = sign;
        else {  // subnormal
            exp = 127 - 15 + 1;
            while (!(mant & 0x400u)) {
                mant <<= 1;
                --exp;
            }
            x = sign | (exp << 23) | ((mant & 0x3FFu) << 13);
        }
    } else if (exp == 31) {
        x = sign | 0x7F800000u | (mant << 13);
    } else {
        x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

struct Transform {
    Vec3 position{0, 0, 0};
    Quat rotation{};
    Vec3 scale{1, 1, 1};

    Mat4 matrix() const { return Mat4::translation(position) * Mat4::rotation(rotation) * Mat4::scale(scale); }
    Vec3 forward() const { return rotation.rotate({0, 0, -1}); }
    Vec3 right() const { return rotation.rotate({1, 0, 0}); }
    Vec3 up() const { return rotation.rotate({0, 1, 0}); }
};

// TRS decomposition (exact for matrices without shear; with non-uniform scale
// under rotated parents the result is the closest TRS approximation).
inline Transform decompose(const Mat4& m) {
    Transform t;
    t.position = m.c[3].xyz();
    Vec3 cx = m.c[0].xyz(), cy = m.c[1].xyz(), cz = m.c[2].xyz();
    t.scale = {length(cx), length(cy), length(cz)};
    if (dot(cross(cx, cy), cz) < 0) t.scale.x = -t.scale.x;
    Mat4 r;
    auto safe = [](float s) { return std::fabs(s) > 1e-8f ? s : 1e-8f; };
    r.c[0] = Vec4(cx / safe(t.scale.x), 0);
    r.c[1] = Vec4(cy / safe(t.scale.y), 0);
    r.c[2] = Vec4(cz / safe(t.scale.z), 0);
    t.rotation = quatFromMatrix(r);
    return t;
}

} // namespace fledge
