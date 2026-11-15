#pragma once
// Procedural city district (docs/GAMEPLAY.md "City generator"): a grid of road segments (one spline per block
// edge, so every crossing is a real intersection in the road graph), buildings on lots with static colliders,
// parks with trees, and a ground slab. Used by the City Sandbox template and the generate_city_district MCP tool.
#include <cstdint>
#include <string>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/math.h"
#include "fledge/scene/scene.h"

namespace fledge::gameplay {

struct CityParams {
    int blocksX = 4, blocksZ = 4;
    float blockSize = 64;    // distance between road centre lines (m)
    float roadWidth = 12, sidewalk = 3;
    float minHeight = 8, maxHeight = 42;
    float parkChance = 0.15f;
    uint32_t seed = 7;
    Vec3 origin{0, 0, 0};    // corner of the district (roads start here)
    std::string name = "District";
    bool ground = true;
};

// A node of the generated hierarchy: {"name", "components": {...}, "children": [...]}.
struct CityResult {
    Json root;  // one root entity (the district) with children
    int roads = 0, buildings = 0, parks = 0;
    Vec3 center;
    std::vector<Vec3> spawnPoints;    // on sidewalks (player / NPC spawns)
    std::vector<Vec3> parkingSpots;   // kerbside, facing along the road (x, z) + yaw in y
};
CityResult generateCity(const CityParams& p);
CityParams cityParamsFromJson(const Json& j);  // {"blocksX", "blocksZ", "blockSize", "roadWidth", "sidewalk", "seed", "origin", "name", ...}
// Adds the "generate" block of a template.json to a scene: {"city": {...}, "vehicles": [{"preset", "position", "yaw", "name", "color"}]}.
void applyGenerateSpec(Scene& scene, const Json& generate);
// A drivable vehicle (chassis with Collider + RigidBody + Vehicle, body / cabin meshes, Wheel0..3) as a node tree.
// preset: car | truck | bike. yaw: radians (0 = facing -Z). The chassis is lifted so the wheels rest on y = position.y.
Json vehicleNode(const std::string& preset, Vec3 position, float yaw, const std::string& name, Vec3 color = {0.75f, 0.12f, 0.1f});
// Counts nodes in a generated hierarchy (tests, tool output).
int countNodes(const Json& node);
// Creates the hierarchy in a scene (not undoable: the editor uses EditorSession::createEntity instead). Returns the root.
EntityId spawnTree(Scene& scene, const Json& node, EntityId parent = kNullEntity);

} // namespace fledge::gameplay
