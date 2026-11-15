#pragma once
// CPU-side triangle mesh (render-API independent). Produced by importers and
// procedural generators, uploaded by the renderer.
#include <cstdint>
#include <string>
#include <vector>

#include "fledge/core/math.h"

namespace fledge {

struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
};

// A level of detail: a simplified index list over the SAME vertices (LOD 0 = MeshData::indices).
struct MeshLod {
    std::vector<uint32_t> indices;
    float error = 0;  // relative geometric error (0..1 of the mesh extent) reported by the simplifier
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Vec4> tangents;  // optional, per vertex (xyz tangent, w = bitangent sign); for future normal mapping
    std::vector<MeshLod> lods;   // optional LOD 1..N (cooked meshes carry them; see fledge/assets/lod.h)
    bool empty() const { return vertices.empty() || indices.empty(); }
    // Local AABB, filled on first meshBounds() (culling used to walk every vertex every frame).
    mutable Vec3 aabbMin{1e30f}, aabbMax{-1e30f};
    mutable bool aabbValid = false;
    void invalidateAabb() const { aabbValid = false; }
};

// Per-vertex tangents from UVs (Lengyel); fills mesh.tangents.
void generateTangents(MeshData& mesh);

// 1.3.3 normal hygiene (docs/GRAPHICS.md "Robustness rules", docs/ANIMATION.md "Zero-length normals"):
// vertices whose normal cannot be normalised (|n|^2 < 1e-12, NaN or Inf). Indices ascending.
std::vector<uint32_t> zeroNormalVertices(const MeshData& mesh);
// Opt-in fix: each such vertex gets the area-weighted normal of the triangles that use it, else of the triangles at its
// position (welded within 1e-6 of the mesh size). Valid normals are never touched; vertices with no usable triangle keep
// (0, 0, 0) (the renderer shades those with the face normal). Returns the vertices fixed.
int fixZeroNormals(MeshData& mesh);
// The import / load warning for `what` (e.g. "mesh 'Muzzle'"): count, total, up to 8 example indices and the fix; "" when
// `zero` is empty. fixed >= 0: fixZeroNormals ran and fixed that many; -1: not run (the advice names the fix); -2: no fix
// exists for this source (OBJ: the advice is to re-export).
std::string zeroNormalWarning(const std::string& what, size_t vertexCount, const std::vector<uint32_t>& zero, int fixed = -1);

// Axis-aligned bounds of a mesh in its local space.
inline void meshBounds(const MeshData& m, Vec3& mn, Vec3& mx) {
    if (!m.aabbValid) {
        m.aabbMin = Vec3{1e30f};
        m.aabbMax = Vec3{-1e30f};
        for (const auto& v : m.vertices)
            for (int i = 0; i < 3; ++i) {
                m.aabbMin[i] = v.position[i] < m.aabbMin[i] ? v.position[i] : m.aabbMin[i];
                m.aabbMax[i] = v.position[i] > m.aabbMax[i] ? v.position[i] : m.aabbMax[i];
            }
        if (m.vertices.empty()) m.aabbMin = m.aabbMax = Vec3{0};
        m.aabbValid = true;
    }
    mn = m.aabbMin;
    mx = m.aabbMax;
}

namespace primitives {
MeshData cube(float size = 1.0f);
MeshData plane(float size = 10.0f, float uvRepeat = 10.0f);
MeshData sphere(float radius = 0.5f, int segments = 32, int rings = 16);
// "builtin:cube" etc. Returns empty mesh for unknown names.
MeshData byName(const std::string& name);
} // namespace primitives

} // namespace fledge
