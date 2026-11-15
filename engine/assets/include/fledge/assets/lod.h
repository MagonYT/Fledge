#pragma once
// Mesh LOD chains (meshoptimizer quadric simplification) and merged proxy meshes (HLOD).
#include <vector>

#include "fledge/core/math.h"
#include "fledge/core/mesh_data.h"

namespace fledge {

struct LodSettings {
    int levels = 3;                     // LOD 1..levels
    float ratio = 0.5f;                 // each level keeps about this share of the previous one's triangles
    float maxError = 0.05f;             // relative error cap per level
    uint32_t minTriangles = 64;         // meshes below this get no LODs
};

// Fills mesh.lods (keeps vertices). Stops early when simplification no longer reduces the triangle count.
void generateLods(MeshData& mesh, const LodSettings& s = {});
size_t triangleCount(const MeshData& m, int lod);  // lod 0 = full

// Screen-size based LOD choice: size = projected bounding-sphere radius / half the viewport height (1 = fills the
// screen). thresholds: switch to LOD i+1 below thresholds[i]. bias > 0 picks coarser LODs earlier. previous adds
// 10 % hysteresis around each threshold. forced >= 0 overrides.
int selectLod(float screenSize, int lodCount, float bias = 0, int previous = -1, int forced = -1);
inline float screenSizeOf(float radius, float distance, float projScaleY) {
    return distance <= radius ? 10.0f : radius * projScaleY / distance;
}

// HLOD: merges meshes (with their world matrices) into one, simplified to about `targetTriangles`.
struct ProxyPart {
    const MeshData* mesh;
    Mat4 world;
};
MeshData buildProxyMesh(const std::vector<ProxyPart>& parts, size_t targetTriangles);

} // namespace fledge
