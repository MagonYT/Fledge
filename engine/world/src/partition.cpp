#include "fledge/world/partition.h"

#include <cmath>
#include <format>
#include <sstream>

namespace fledge::world {

std::string ChunkCoord::name() const { return std::format("{}_{}", x, z); }

std::optional<ChunkCoord> ChunkCoord::parse(std::string_view n) {
    size_t u = n.find('_', 1);
    if (u == std::string_view::npos) return std::nullopt;
    try {
        return ChunkCoord{std::stoi(std::string(n.substr(0, u))), std::stoi(std::string(n.substr(u + 1)))};
    } catch (...) {
        return std::nullopt;
    }
}

ChunkCoord chunkOf(double x, double z, double cell) {
    return {static_cast<int>(std::floor(x / cell)), static_cast<int>(std::floor(z / cell))};
}

double distanceToChunk(ChunkCoord c, double cell, double px, double pz) {
    double x0 = c.x * cell, x1 = x0 + cell, z0 = c.z * cell, z1 = z0 + cell;
    double dx = px < x0 ? x0 - px : (px > x1 ? px - x1 : 0.0);
    double dz = pz < z0 ? z0 - pz : (pz > z1 ? pz - z1 : 0.0);
    return std::sqrt(dx * dx + dz * dz);
}

PartitionSettings PartitionSettings::from(const WorldPartitionComponent& c) {
    PartitionSettings s;
    s.cellSize = std::max(1.0f, c.cellSize);
    s.loadRadius = std::max(0.0f, c.loadRadius);
    s.unloadRadius = std::max(c.unloadRadius, c.loadRadius + c.cellSize * 0.25f);  // always some hysteresis
    s.memoryBudget = static_cast<size_t>(std::max(0.01f, c.memoryBudgetMB) * 1024.0 * 1024.0);
    s.frameBudgetMs = std::max(0.05f, c.frameBudgetMs);
    s.rebaseDistance = std::max(10.0f, c.rebaseDistance);
    std::istringstream in(c.activeLayers);
    for (std::string l; in >> l;) s.activeLayers.insert(l);
    return s;
}

std::string ChunkData::text() const {
    Json j = {{"format", "fchunk"}, {"version", 1}, {"x", coord.x}, {"z", coord.z}, {"entities", entities}};
    j["deps"] = Json(std::vector<std::string>(deps.begin(), deps.end()));
    return j.dump();
}

const WorldPartitionComponent* findPartitionSettings(Scene& scene, EntityId* owner) {
    for (EntityId e : scene.entities())
        if (auto* w = scene.get<WorldPartitionComponent>(e)) {
            if (owner) *owner = e;
            return w;
        }
    return nullptr;
}

bool isPersistentRoot(Scene& scene, EntityId root) {
    std::vector<EntityId> all = scene.descendants(root);
    all.push_back(root);
    for (EntityId e : all) {
        if (auto* s = scene.get<StreamingComponent>(e); s && (s->persistent || s->alwaysLoaded)) return true;
        if (scene.get<WorldPartitionComponent>(e) || scene.get<CameraComponent>(e)) return true;
        if (auto* l = scene.get<LightComponent>(e); l && l->type == LightType::Directional) return true;
        // terrains larger than a chunk and road networks are global (they are tiled / culled by the renderer)
        if (scene.get<TerrainComponent>(e)) return true;
    }
    return false;
}

Partition buildPartition(Scene& scene, const PartitionSettings& settings) {
    Partition p;
    p.settings = settings;
    auto worlds = scene.computeWorldMatrices();
    for (EntityId root : scene.roots()) {
        if (isPersistentRoot(scene, root)) {
            p.persistent.push_back(root);
            continue;
        }
        Vec3 pos = worlds[root].c[3].xyz();
        ChunkCoord c = chunkOf(pos.x, pos.z, settings.cellSize);
        ChunkData& ch = p.chunks[c];
        ch.coord = c;
        ch.roots.push_back(root);
        std::vector<EntityId> order{root};
        for (EntityId d : scene.descendants(root)) order.push_back(d);
        for (EntityId e : order) {
            Json st = scene.entityState(e);
            if (e == root) st["index"] = -1;
            std::string layer;
            if (auto* s = scene.get<StreamingComponent>(e)) layer = s->layer;
            if (auto* mr = scene.get<MeshRendererComponent>(e)) {
                if (!mr->mesh.empty()) ch.deps.insert(mr->mesh);
                if (!mr->material.empty()) ch.deps.insert(mr->material);
                if (!mr->texture.empty()) ch.deps.insert(mr->texture);
            }
            ch.entities.push_back({{"id", e}, {"layer", layer}, {"state", st}});
        }
    }
    for (auto& [c, ch] : p.chunks) {
        ch.bytes = ch.text().size();
        p.totalBytes += ch.bytes;
    }
    return p;
}

Json parseChunk(std::string_view text) {
    Json j = Json::parse(text, nullptr, false);
    if (j.is_discarded() || j.value("format", "") != "fchunk") return Json();
    return j;
}

std::vector<EntityId> applyChunk(Scene& scene, const Json& chunk, DVec3 origin, const std::set<std::string>& activeLayers) {
    std::vector<EntityId> roots;
    if (!chunk.is_object()) return roots;
    std::set<EntityId> skipped;
    for (auto& ent : chunk.value("entities", Json::array())) {
        EntityId id = ent.value("id", kNullEntity);
        std::string layer = ent.value("layer", "");
        const Json& st = ent["state"];
        EntityId parent = st.value("parent", kNullEntity);
        if ((!layer.empty() && !activeLayers.count(layer)) || skipped.count(parent)) {
            skipped.insert(id);
            continue;
        }
        if (!scene.applyEntityState(id, st)) continue;
        if (parent == kNullEntity) {
            if (auto* t = scene.get<TransformComponent>(id)) {  // absolute world -> floating-origin local (double math)
                DVec3 w = DVec3::from(t->position) - origin;
                t->position = w.toVec3();
            }
            roots.push_back(id);
        }
    }
    return roots;
}

std::string worldDirFor(const std::string& scenePath) { return scenePath + ".world"; }

Result<CookedWorld> cookWorld(const Json& sceneJson) {
    Scene scene;
    if (auto st = scene.fromJson(sceneJson); !st) return Result<CookedWorld>::failure(st.error);
    const WorldPartitionComponent* wp = findPartitionSettings(scene);
    if (!wp) return Result<CookedWorld>::failure("scene has no WorldPartition component");
    Partition part = buildPartition(scene, PartitionSettings::from(*wp));
    CookedWorld out;
    Json chunks = Json::array();
    for (auto& [c, ch] : part.chunks) {
        std::string name = c.name() + ".fchunk";
        out.chunks.push_back({name, ch.text()});
        chunks.push_back({{"name", name}, {"x", c.x}, {"z", c.z}, {"bytes", ch.bytes}, {"deps", Json(std::vector<std::string>(ch.deps.begin(), ch.deps.end()))}});
    }
    EntityId next = scene.nextId();
    for (auto& [c, ch] : part.chunks)
        for (EntityId r : ch.roots) scene.destroyEntity(r);
    out.scene = scene.toJson();
    out.scene["streamedWorld"] = true;
    out.index = {{"format", "fworld"}, {"version", 1}, {"cellSize", part.settings.cellSize}, {"nextId", next}, {"chunks", chunks}};
    return out;
}

size_t convertToOpenWorld(Scene& scene, float cellSize) {
    EntityId owner = kNullEntity;
    if (!findPartitionSettings(scene, &owner)) {
        owner = scene.createEntity("World Settings");
        WorldPartitionComponent wp;
        wp.cellSize = cellSize;
        wp.loadRadius = cellSize * 3;
        wp.unloadRadius = cellSize * 4;
        scene.add<WorldPartitionComponent>(owner, wp);
    }
    for (EntityId e : scene.entities()) {
        bool keep = scene.get<CameraComponent>(e) || (scene.get<LightComponent>(e) && scene.get<LightComponent>(e)->type == LightType::Directional);
        if (keep && !scene.get<StreamingComponent>(e)) scene.add<StreamingComponent>(e, StreamingComponent{false, true, ""});
    }
    return buildPartition(scene, PartitionSettings::from(*findPartitionSettings(scene))).chunks.size();
}

} // namespace fledge::world
