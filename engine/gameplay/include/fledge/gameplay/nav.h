#pragma once
// Navigation meshes (Recast) and path queries (Detour). docs/GAMEPLAY.md "Navigation".
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fledge/core/math.h"

namespace fledge::gameplay {

struct NavSettings {
    float cellSize = 0.3f, cellHeight = 0.2f;
    float agentHeight = 1.8f, agentRadius = 0.4f, agentMaxClimb = 0.4f, agentMaxSlope = 45;
    float maxEdgeLength = 12, regionMinSize = 8, regionMergeSize = 20;
};

struct NavStats {
    int polygons = 0, vertices = 0, inputTriangles = 0;
    double buildMs = 0;
};

class NavMesh {
public:
    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // Builds from a world-space triangle soup (walkable + obstacle geometry together). False + error on failure.
    bool build(const std::vector<Vec3>& vertices, const std::vector<uint32_t>& indices, const NavSettings& s, std::string* error = nullptr);
    bool valid() const;
    const NavStats& stats() const { return m_stats; }

    // Straight path (corner points) from a to b; empty if no route. a and b snap to the mesh within `extents`.
    std::vector<Vec3> findPath(Vec3 a, Vec3 b, Vec3 extents = {2, 4, 2}) const;
    std::optional<Vec3> nearestPoint(Vec3 p, Vec3 extents = {2, 4, 2}) const;
    std::optional<Vec3> randomPoint(uint32_t seed) const;
    // Triangles of the navmesh polygons (debug draw).
    void debugTriangles(std::vector<Vec3>& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
    NavStats m_stats;
};

} // namespace fledge::gameplay
