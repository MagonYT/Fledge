#include "fledge/gameplay/camera.h"

#include <algorithm>
#include <cmath>

namespace fledge::gameplay {

const char* toString(RigMode m) {
    switch (m) {
        case RigMode::Orbit: return "Orbit";
        case RigMode::FirstPerson: return "FirstPerson";
        case RigMode::Fixed: return "Fixed";
        case RigMode::TopDown: return "TopDown";
        default: return "Follow";
    }
}

Quat lookRotation(Vec3 from, Vec3 to) {
    Vec3 d = to - from;
    float len = length(d);
    if (len < 1e-5f) return Quat{};
    d = d / len;
    float yaw = std::atan2(-d.x, -d.z), pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f));
    return (Quat::fromAxisAngle({0, 1, 0}, yaw) * Quat::fromAxisAngle({1, 0, 0}, pitch)).normalized();
}

namespace {
Quat slerpQ(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995f) return Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}.normalized();
    float th = std::acos(d), s = std::sin(th);
    float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb}.normalized();
}
Vec3 vec3Of(const Json& j, Vec3 def = {}) {
    if (!j.is_array() || j.size() != 3) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
} // namespace

CameraPose rigPose(const RigParams& p, Vec3 target, float targetYaw, const CameraCollision& collide) {
    CameraPose out;
    Vec3 look = target + Vec3{0, p.lookHeight, 0};
    switch (p.mode) {
        case RigMode::FirstPerson: {
            out.position = look;
            out.rotation = (Quat::fromAxisAngle({0, 1, 0}, targetYaw + p.yaw) * Quat::fromAxisAngle({1, 0, 0}, p.pitch)).normalized();
            return out;
        }
        case RigMode::TopDown: {
            out.position = target + Vec3{0, std::max(p.distance, 5.0f) * 2.5f, 0.01f};
            out.rotation = lookRotation(out.position, target);
            return out;
        }
        case RigMode::Fixed: {
            out.position = target + Vec3{0, p.height, p.distance};
            out.rotation = lookRotation(out.position, look);
            return out;
        }
        default: break;
    }
    float yaw = p.mode == RigMode::Follow ? targetYaw + p.yaw : p.yaw;
    Quat r = (Quat::fromAxisAngle({0, 1, 0}, yaw) * Quat::fromAxisAngle({1, 0, 0}, p.pitch)).normalized();
    Vec3 back = r.rotate({0, 0, 1});  // camera sits behind the look point
    float dist = p.distance;
    if (collide)
        if (auto hit = collide(look, back, dist)) dist = std::max(p.minDistance, *hit - 0.3f);
    out.position = look + back * dist + Vec3{0, p.height - p.lookHeight, 0} * (dist / std::max(p.distance, 1e-3f));
    out.rotation = lookRotation(out.position, look);
    return out;
}

CameraPose smoothPose(const CameraPose& cur, const CameraPose& want, float smoothing, float dt) {
    if (smoothing <= 0) return want;
    float a = 1.0f - std::exp(-smoothing * dt);
    CameraPose o;
    o.position = cur.position + (want.position - cur.position) * a;
    o.rotation = slerpQ(cur.rotation, want.rotation, a);
    o.fovDegrees = cur.fovDegrees + (want.fovDegrees - cur.fovDegrees) * a;
    return o;
}

Sequence Sequence::fromJson(const Json& j) {
    Sequence s;
    if (!j.is_object() || !j.contains("keys") || !j["keys"].is_array()) return s;
    for (auto& k : j["keys"]) {
        if (!k.is_object()) continue;
        SequenceKey key;
        key.time = k.value("time", 0.0f);
        key.position = vec3Of(k.value("position", Json()));
        key.fov = k.value("fov", 60.0f);
        if (k.contains("target")) key.rotation = lookRotation(key.position, vec3Of(k["target"]));
        else if (k.contains("rotation")) {
            Vec3 e = vec3Of(k["rotation"]);
            key.rotation = Quat::fromEuler({radians(e.x), radians(e.y), radians(e.z)});
        }
        s.m_keys.push_back(key);
    }
    std::sort(s.m_keys.begin(), s.m_keys.end(), [](const SequenceKey& a, const SequenceKey& b) { return a.time < b.time; });
    return s;
}

Json Sequence::toJson() const {
    Json keys = Json::array();
    for (auto& k : m_keys) {
        Vec3 e = k.rotation.toEuler();
        keys.push_back({{"time", k.time}, {"position", {k.position.x, k.position.y, k.position.z}}, {"rotation", {degrees(e.x), degrees(e.y), degrees(e.z)}},
                        {"fov", k.fov}});
    }
    return {{"keys", keys}};
}

CameraPose Sequence::sample(float t) const {
    CameraPose out;
    if (m_keys.empty()) return out;
    if (t <= m_keys.front().time || m_keys.size() == 1) return {m_keys.front().position, m_keys.front().rotation, m_keys.front().fov};
    if (t >= m_keys.back().time) return {m_keys.back().position, m_keys.back().rotation, m_keys.back().fov};
    size_t i = 1;
    while (i < m_keys.size() && m_keys[i].time < t) ++i;
    const SequenceKey &k1 = m_keys[i - 1], &k2 = m_keys[i];
    const SequenceKey& k0 = i >= 2 ? m_keys[i - 2] : k1;
    const SequenceKey& k3 = i + 1 < m_keys.size() ? m_keys[i + 1] : k2;
    float u = (t - k1.time) / std::max(1e-4f, k2.time - k1.time);
    float u2 = u * u, u3 = u2 * u;
    out.position = (k1.position * 2.0f + (k2.position - k0.position) * u + (k0.position * 2.0f - k1.position * 5.0f + k2.position * 4.0f - k3.position) * u2 +
                    (k1.position * 3.0f - k0.position - k2.position * 3.0f + k3.position) * u3) *
                   0.5f;
    out.rotation = slerpQ(k1.rotation, k2.rotation, u);
    out.fovDegrees = k1.fov + (k2.fov - k1.fov) * u;
    return out;
}

} // namespace fledge::gameplay
