#pragma once
// UI-independent viewport math for the editor: mouse rays, picking and
// transform-gizmo drags. Kept out of the ImGui code so it is unit-testable.
#include <functional>
#include <string>

#include "fledge/core/math.h"
#include "fledge/core/mesh_data.h"
#include "fledge/scene/scene.h"

namespace fledge {

struct Ray {
    Vec3 origin;
    Vec3 dir;  // normalized for world rays
    Vec3 at(float t) const { return origin + dir * t; }
};

// ndc x,y in [-1, 1] (y up). viewProj = projection * view (OpenGL clip conventions).
Ray screenRay(float ndcX, float ndcY, const Mat4& viewProj);
// World -> pixel coordinates (top-left origin). Returns false if behind the camera.
bool projectToScreen(const Mat4& viewProj, Vec3 p, float width, float height, Vec2& out);

bool rayAabb(const Ray& r, Vec3 mn, Vec3 mx, float& tNear);
bool rayTriangle(const Ray& r, Vec3 a, Vec3 b, Vec3 c, float& t);  // Moeller-Trumbore, two-sided
bool raySphere(const Ray& r, Vec3 center, float radius, float& t);
bool rayPlane(const Ray& r, Vec3 point, Vec3 normal, float& t);
// Parameter s of the point on the line (origin + s*axis) closest to the ray.
float closestAxisParam(const Ray& r, Vec3 origin, Vec3 axis);
float snapValue(float v, float step);

struct PickHit {
    EntityId entity = kNullEntity;
    float distance = 0;
    Vec3 point;
};
using MeshProvider = std::function<const MeshData*(const std::string& meshRef)>;
// Tests MeshRenderer meshes (bounds, then triangles) and gives entities without
// a mesh (lights, cameras, empties) a small pick sphere.
PickHit pickEntity(Scene& scene, const MeshProvider& meshes, const Ray& worldRay, float emptyRadius = 0.3f);

// ----------------------------------------------------------------- gizmo ---
enum class GizmoMode { Translate, Rotate, Scale };
enum class GizmoSpace { World, Local };

struct GizmoSettings {
    GizmoMode mode = GizmoMode::Translate;
    GizmoSpace space = GizmoSpace::World;
    bool snap = false;
    float translateSnap = 0.5f;
    float rotateSnapDeg = 15.0f;
    float scaleSnap = 0.1f;
};

Vec3 gizmoAxis(int axis, GizmoSpace space, const Quat& rotation);

// One drag of one axis handle.
class GizmoDrag {
public:
    void begin(const GizmoSettings& settings, int axis, const Transform& startWorld, const Ray& ray, Vec3 viewDir);
    Transform update(const Ray& ray) const;  // new world transform for the current mouse ray
    bool active() const { return m_active; }
    void end() { m_active = false; }
    int axis() const { return m_axis; }

private:
    GizmoSettings m_settings;
    Transform m_start;
    Vec3 m_axisDir;
    float m_startParam = 0;
    Vec3 m_startVec;  // rotate: vector from center to the start hit on the rotation plane
    int m_axis = -1;
    bool m_active = false;
};

} // namespace fledge
