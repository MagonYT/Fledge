#include "fledge/editor/viewport_math.h"

#include <algorithm>
#include <cmath>

#include "fledge/scene/components.h"

namespace fledge {

Ray screenRay(float ndcX, float ndcY, const Mat4& viewProj) {
    Mat4 inv = viewProj.inverse();
    Vec3 nearP = inv.transformPoint({ndcX, ndcY, -1.0f});
    Vec3 farP = inv.transformPoint({ndcX, ndcY, 1.0f});
    return {nearP, normalize(farP - nearP)};
}

bool projectToScreen(const Mat4& viewProj, Vec3 p, float width, float height, Vec2& out) {
    Vec4 c = viewProj * Vec4(p, 1.0f);
    if (c.w <= 1e-5f) return false;
    out = {(c.x / c.w * 0.5f + 0.5f) * width, (1.0f - (c.y / c.w * 0.5f + 0.5f)) * height};
    return true;
}

bool rayAabb(const Ray& r, Vec3 mn, Vec3 mx, float& tNear) {
    float t0 = -1e30f, t1 = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(r.dir[i]) < 1e-12f) {
            if (r.origin[i] < mn[i] || r.origin[i] > mx[i]) return false;
            continue;
        }
        float inv = 1.0f / r.dir[i];
        float a = (mn[i] - r.origin[i]) * inv, b = (mx[i] - r.origin[i]) * inv;
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return false;
    }
    if (t1 < 0) return false;
    tNear = t0 < 0 ? 0 : t0;
    return true;
}

bool rayTriangle(const Ray& r, Vec3 a, Vec3 b, Vec3 c, float& t) {
    Vec3 e1 = b - a, e2 = c - a;
    Vec3 p = cross(r.dir, e2);
    float det = dot(e1, p);
    if (std::fabs(det) < 1e-12f) return false;
    float inv = 1.0f / det;
    Vec3 s = r.origin - a;
    float u = dot(s, p) * inv;
    if (u < 0 || u > 1) return false;
    Vec3 q = cross(s, e1);
    float v = dot(r.dir, q) * inv;
    if (v < 0 || u + v > 1) return false;
    t = dot(e2, q) * inv;
    return t >= 0;
}

bool raySphere(const Ray& r, Vec3 center, float radius, float& t) {
    Vec3 oc = r.origin - center;
    float b = dot(oc, r.dir), c = dot(oc, oc) - radius * radius;
    float dd = dot(r.dir, r.dir);
    float disc = b * b - dd * c;
    if (disc < 0) return false;
    float s = std::sqrt(disc);
    t = (-b - s) / dd;
    if (t < 0) t = (-b + s) / dd;
    return t >= 0;
}

bool rayPlane(const Ray& r, Vec3 point, Vec3 normal, float& t) {
    float d = dot(normal, r.dir);
    if (std::fabs(d) < 1e-8f) return false;
    t = dot(point - r.origin, normal) / d;
    return t >= 0;
}

float closestAxisParam(const Ray& r, Vec3 origin, Vec3 axis) {
    // Closest points between line A (origin + s*axis) and line B (ray).
    Vec3 w = origin - r.origin;
    float a = dot(axis, axis), b = dot(axis, r.dir), c = dot(r.dir, r.dir);
    float d = dot(axis, w), e = dot(r.dir, w);
    float denom = a * c - b * b;
    if (std::fabs(denom) < 1e-8f) return 0;  // parallel
    return (b * e - c * d) / denom;
}

float snapValue(float v, float step) { return step > 0 ? std::round(v / step) * step : v; }

PickHit pickEntity(Scene& scene, const MeshProvider& meshes, const Ray& worldRay, float emptyRadius) {
    PickHit best;
    best.distance = 1e30f;
    auto world = scene.computeWorldMatrices();
    for (EntityId e : scene.entities()) {
        const Mat4& m = world[e];
        float t = 0;
        bool hit = false;
        const MeshData* mesh = nullptr;
        if (auto* mr = scene.get<MeshRendererComponent>(e); mr && mr->visible && meshes) mesh = meshes(mr->mesh);
        if (mesh && !mesh->empty()) {
            // Transform the ray into local space; keeping dir unnormalized preserves t.
            Mat4 inv = m.inverse();
            Ray local{inv.transformPoint(worldRay.origin), (inv * Vec4(worldRay.dir, 0.0f)).xyz()};
            Vec3 mn, mx;
            meshBounds(*mesh, mn, mx);
            float tb;
            if (!rayAabb(local, mn, mx, tb) || tb > best.distance) continue;
            for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
                float tt;
                if (rayTriangle(local, mesh->vertices[mesh->indices[i]].position, mesh->vertices[mesh->indices[i + 1]].position,
                                mesh->vertices[mesh->indices[i + 2]].position, tt) &&
                    (!hit || tt < t)) {
                    t = tt;
                    hit = true;
                }
            }
        } else if (!scene.get<MeshRendererComponent>(e)) {
            hit = raySphere(worldRay, m.c[3].xyz(), emptyRadius, t);
        }
        if (hit && t < best.distance) {
            best.entity = e;
            best.distance = t;
            best.point = worldRay.at(t);
        }
    }
    if (!best.entity) best.distance = 0;
    return best;
}

// ----------------------------------------------------------------- gizmo ---
Vec3 gizmoAxis(int axis, GizmoSpace space, const Quat& rotation) {
    Vec3 a = axis == 0 ? Vec3{1, 0, 0} : axis == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
    return space == GizmoSpace::Local ? normalize(rotation.rotate(a)) : a;
}

void GizmoDrag::begin(const GizmoSettings& settings, int axis, const Transform& startWorld, const Ray& ray, Vec3 viewDir) {
    m_settings = settings;
    // Scale always operates on the object's local axes.
    GizmoSpace space = settings.mode == GizmoMode::Scale ? GizmoSpace::Local : settings.space;
    m_axis = axis;
    m_start = startWorld;
    m_axisDir = gizmoAxis(axis, space, startWorld.rotation);
    m_active = true;
    if (settings.mode == GizmoMode::Rotate) {
        float t = 0;
        if (!rayPlane(ray, startWorld.position, m_axisDir, t) && !rayPlane(ray, startWorld.position, -viewDir, t)) {
            t = 0;  // degenerate: no plane hit; update() then yields no rotation
        }
        m_startVec = ray.at(t) - startWorld.position;
    } else {
        m_startParam = closestAxisParam(ray, startWorld.position, m_axisDir);
    }
}

Transform GizmoDrag::update(const Ray& ray) const {
    Transform t = m_start;
    if (!m_active) return t;
    switch (m_settings.mode) {
        case GizmoMode::Translate: {
            float d = closestAxisParam(ray, m_start.position, m_axisDir) - m_startParam;
            if (m_settings.snap) d = snapValue(d, m_settings.translateSnap);
            t.position = m_start.position + m_axisDir * d;
            break;
        }
        case GizmoMode::Scale: {
            float base = std::fabs(m_startParam) > 1e-3f ? m_startParam : 1.0f;
            float factor = closestAxisParam(ray, m_start.position, m_axisDir) / base;
            if (m_settings.snap) factor = snapValue(factor, m_settings.scaleSnap);
            factor = std::max(factor, 0.01f);
            t.scale[m_axis] = m_start.scale[m_axis] * factor;
            break;
        }
        case GizmoMode::Rotate: {
            float hitT;
            if (!rayPlane(ray, m_start.position, m_axisDir, hitT)) return t;
            Vec3 a = m_startVec - m_axisDir * dot(m_startVec, m_axisDir);
            Vec3 b = ray.at(hitT) - m_start.position;
            b = b - m_axisDir * dot(b, m_axisDir);
            if (length(a) < 1e-5f || length(b) < 1e-5f) return t;
            float angle = std::atan2(dot(cross(normalize(a), normalize(b)), m_axisDir), dot(normalize(a), normalize(b)));
            if (m_settings.snap) angle = radians(snapValue(degrees(angle), m_settings.rotateSnapDeg));
            t.rotation = (Quat::fromAxisAngle(m_axisDir, angle) * m_start.rotation).normalized();
            break;
        }
    }
    return t;
}

} // namespace fledge
