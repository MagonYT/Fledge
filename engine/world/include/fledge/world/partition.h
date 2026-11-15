#pragma once
// World Partition: one logical world, stored / cooked as a grid of chunks (docs/WORLD_STREAMING.md).
//
//   - every ROOT entity (with its children) belongs to the chunk that contains its world position;
//   - persistent entities (Streaming.persistent / alwaysLoaded, cameras, directional lights, the
//     WorldPartition entity itself) are never streamed;
//   - entities on a data layer (Streaming.layer) only load while that layer is active;
//   - a chunk records its entity states (undo-compatible Scene::entityState) and its asset dependencies.
// Positions in chunks are absolute world positions; loading subtracts the floating origin.
#include <compare>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/scene/components.h"
#include "fledge/scene/scene.h"

namespace fledge::world {

struct DVec3 {
    double x = 0, y = 0, z = 0;
    DVec3 operator+(DVec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    DVec3 operator-(DVec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    bool operator==(const DVec3&) const = default;
    static DVec3 from(Vec3 v) { return {v.x, v.y, v.z}; }
    Vec3 toVec3() const { return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)}; }
};

struct ChunkCoord {
    int x = 0, z = 0;
    auto operator<=>(const ChunkCoord&) const = default;
    std::string name() const;  // "x_z" e.g. "-3_12"
    static std::optional<ChunkCoord> parse(std::string_view name);
};

ChunkCoord chunkOf(double x, double z, double cellSize);
// Horizontal distance from (px, pz) to the chunk's square (0 when inside).
double distanceToChunk(ChunkCoord c, double cellSize, double px, double pz);

struct PartitionSettings {
    double cellSize = 128, loadRadius = 384, unloadRadius = 512;
    size_t memoryBudget = 256ull << 20;
    double frameBudgetMs = 2.0;
    double rebaseDistance = 2048;
    std::set<std::string> activeLayers;
    static PartitionSettings from(const WorldPartitionComponent& c);
};

struct ChunkData {
    ChunkCoord coord;
    std::vector<EntityId> roots;
    Json entities = Json::array();  // [{"id", "layer", "state"}], parents before children
    std::set<std::string> deps;     // asset refs used (meshes, materials, textures)
    size_t bytes = 0;               // serialized size (memory budget unit)
    std::string text() const;       // .fchunk file contents
};

struct Partition {
    PartitionSettings settings;
    std::map<ChunkCoord, ChunkData> chunks;
    std::vector<EntityId> persistent;  // roots that are never streamed
    size_t totalBytes = 0;
};

// The WorldPartition component of the scene, if any (first entity that has one).
const WorldPartitionComponent* findPartitionSettings(Scene& scene, EntityId* owner = nullptr);
bool isPersistentRoot(Scene& scene, EntityId root);
Partition buildPartition(Scene& scene, const PartitionSettings& settings);

// Applies a chunk's entities (skipping inactive layers) with the floating origin subtracted from root
// positions. Returns the roots that were created.
std::vector<EntityId> applyChunk(Scene& scene, const Json& chunk, DVec3 origin, const std::set<std::string>& activeLayers);
Json parseChunk(std::string_view text);

// Cook step: splits a scene JSON into the persistent scene + one .fchunk per chunk + an index.
struct CookedWorld {
    Json scene;                                    // without the streamed entities
    std::vector<std::pair<std::string, std::string>> chunks;  // file name ("x_z.fchunk") -> text
    Json index;                                    // {"format":"fworld", cellSize, chunks:[{name,x,z,bytes,deps}]}
};
Result<CookedWorld> cookWorld(const Json& sceneJson);
// "<dir>/<scene>.world/" next to the scene: index.json and the chunk files.
std::string worldDirFor(const std::string& scenePath);

// One-click "Convert scene to open world": adds a World Settings entity with WorldPartition (if missing)
// and marks cameras / directional lights persistent. Returns the number of chunks the scene splits into.
size_t convertToOpenWorld(Scene& scene, float cellSize = 128.0f);

} // namespace fledge::world
