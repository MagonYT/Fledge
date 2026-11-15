#pragma once
// Roads: Catmull-Rom splines -> road + sidewalk meshes, and the road graph (intersections = nodes) that
// traffic and pedestrians use. See docs/WORLD_STREAMING.md.
#include <vector>

#include "fledge/core/math.h"
#include "fledge/core/mesh_data.h"
#include "fledge/scene/components.h"
#include "fledge/scene/scene.h"

namespace fledge::world {

std::vector<Vec3> splineControlPoints(const SplineComponent& s);
Vec3 catmullRom(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float t);
// Evenly spaced points (about `spacing` m apart) along the spline.
std::vector<Vec3> sampleSpline(const std::vector<Vec3>& points, bool closed, float spacing);

struct RoadMeshes {
    MeshData road;       // carriageway (asphalt), slightly above the spline
    MeshData sidewalks;  // raised sidewalks with curbs (empty if sidewalk == 0)
    MeshData markings;   // centre line dashes
};
RoadMeshes buildRoadMeshes(const SplineComponent& s);

struct RoadGraph {
    struct Node {
        Vec3 position;
        std::vector<int> edges;
    };
    struct Edge {
        int a = -1, b = -1;
        std::vector<Vec3> polyline;  // world space, from a to b
        float length = 0, width = 8, speedLimit = 13.9f, sidewalk = 2;
        int lanes = 2;
        EntityId spline = kNullEntity;
    };
    std::vector<Node> nodes;
    std::vector<Edge> edges;

    // Splines of kind "road" (world space). Endpoints closer than `snap` metres share a node (intersection);
    // a spline endpoint lying on another road's interior splits that road there.
    static RoadGraph build(Scene& scene, float snap = 4.0f);
    int nearestNode(Vec3 p) const;
    std::vector<int> shortestPath(int from, int to) const;  // node ids, empty if unreachable (Dijkstra)
    int intersections() const;  // nodes with 3+ edges
    // Closest point on any edge (for "snap to road").
    Vec3 closestPoint(Vec3 p, int* edge = nullptr, float* along = nullptr) const;
};

} // namespace fledge::world
