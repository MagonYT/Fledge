#pragma once
// Visibility: frustum planes, a loose spatial grid for frustum queries, and a software occlusion buffer
// (CPU depth raster of occluder triangles + a max-depth hierarchy). Everything is conservative: an object is
// only culled when it is certainly invisible. See docs/WORLD_STREAMING.md ("LOD & culling").
#include <array>
#include <unordered_map>
#include <vector>

#include "fledge/core/math.h"
#include "fledge/core/mesh_data.h"

namespace fledge::world {

struct Frustum {
    std::array<Vec4, 6> planes;  // ax + by + cz + d >= 0 inside (normalised)
    static Frustum fromViewProj(const Mat4& viewProj);  // OpenGL clip conventions (z in [-w, w])
    bool sphere(Vec3 c, float r) const;
    bool box(Vec3 mn, Vec3 mx) const;
};

// Uniform loose grid: items are bucketed by the cell of their sphere centre; cells are tested against the
// frustum as boxes grown by the largest radius in them.
class SpatialGrid {
public:
    explicit SpatialGrid(float cellSize = 64) : m_cell(cellSize) {}
    void clear() { m_cells.clear(); }
    void insert(uint32_t item, Vec3 centre, float radius);
    // Appends items whose sphere intersects the frustum.
    void query(const Frustum& f, const std::vector<Vec3>& centres, const std::vector<float>& radii, std::vector<uint32_t>& out) const;
    size_t cellCount() const { return m_cells.size(); }

private:
    struct Cell {
        std::vector<uint32_t> items;
        float maxRadius = 0;
        int x = 0, y = 0, z = 0;
    };
    float m_cell;
    std::unordered_map<uint64_t, Cell> m_cells;
};

class OcclusionBuffer {
public:
    OcclusionBuffer(int width = 256, int height = 128);
    void begin(const Mat4& viewProj);  // clears to the far plane
    // World-space triangle soup (with a model matrix). Triangles crossing the near plane are skipped
    // (skipping occluders is always safe).
    void addOccluder(const std::vector<Vec3>& positions, const std::vector<uint32_t>& indices, const Mat4& model);
    // Reads positions from the mesh. No per-frame copy of the vertex array.
    void addOccluder(const MeshData& mesh, const Mat4& model);
    void finish();  // builds the max-depth hierarchy
    // true if the world AABB may be visible (conservative)
    bool visible(Vec3 mn, Vec3 mx) const;
    int width() const { return m_w; }
    int height() const { return m_h; }
    size_t trianglesRasterized() const { return m_tris; }
    float depthAt(int x, int y) const { return m_depth[static_cast<size_t>(y) * m_w + x]; }

private:
    void raster(Vec3 a, Vec3 b, Vec3 c);
    void rasterIndexed(const std::vector<uint32_t>& indices);
    std::vector<Vec4> m_clip;
    int m_w, m_h;
    Mat4 m_vp;
    std::vector<float> m_depth;                // NDC depth in [0, 1] (0 near), nearest occluder per pixel
    std::vector<std::vector<float>> m_mips;    // max depth per 2x2
    std::vector<std::pair<int, int>> m_sizes;
    size_t m_tris = 0;
};

} // namespace fledge::world
