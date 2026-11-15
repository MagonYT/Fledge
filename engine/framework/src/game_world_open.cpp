// GameWorld: open-world parts (terrain tiles, roads, foliage, streaming). See docs/WORLD_STREAMING.md.
#include <algorithm>
#include <cstring>
#include <cmath>
#include <format>

#include "fledge/core/log.h"
#include "fledge/framework/game_world.h"
#include "fledge/world/roads.h"

namespace fledge {

namespace {
uint64_t hashStr(uint64_t h, std::string_view s) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}
uint64_t hashJson(const Json& j) { return hashStr(1469598103934665603ull, j.dump()); }
} // namespace

rhi::MeshHandle GameWorld::procMesh(const std::string& ref, uint64_t hash, const std::function<MeshData()>& build) {
    auto it = m_procHash.find(ref);
    if (it != m_procHash.end() && it->second == hash) {
        auto m = m_meshCache.find(ref);
        if (m != m_meshCache.end()) return m->second;
    }
    if (auto m = m_meshCache.find(ref); m != m_meshCache.end()) {
        if (m->second) m_renderer->device().destroyMesh(m->second);
        m_meshCache.erase(m);
    }
    MeshData data = build();
    rhi::MeshHandle h;
    if (!data.empty()) {
        generateTangents(data);
        h = m_renderer->device().createMesh(data);
    }
    m_cpuMeshes[ref] = std::make_unique<MeshData>(std::move(data));
    m_meshCache[ref] = h;
    m_procHash[ref] = hash;
    return h;
}

const world::Heightfield* GameWorld::heightfield(EntityId e) {
    auto* tc = m_scene.get<TerrainComponent>(e);
    if (!tc) return nullptr;
    uint64_t h = hashStr(hashStr(1469598103934665603ull, tc->heights), tc->splat);
    h = hashStr(h, std::format("{} {} {} {} {}", tc->size, tc->resolution, tc->seed, tc->maxHeight, tc->tileSize));
    TerrainCache& c = m_terrains[e];
    if (c.hash != h || c.field.resolution() == 0) {
        c.field = world::Heightfield::fromComponent(*tc);
        c.hash = h;
        c.lods.clear();
        if (c.albedo && m_renderer) {
            m_renderer->device().destroyTexture(c.albedo);
            c.albedo = {};
        }
    }
    return &c.field;
}

std::optional<float> GameWorld::terrainHeight(float x, float z) {
    std::optional<float> best;
    auto worlds = m_scene.computeWorldMatrices();
    for (EntityId e : m_scene.entities()) {
        if (!m_scene.get<TerrainComponent>(e)) continue;
        const world::Heightfield* hf = heightfield(e);
        Mat4 inv = worlds[e].inverse();
        Vec3 l = inv.transformPoint({x, 0, z});
        if (!hf->contains(l.x, l.z)) continue;
        l.y = hf->heightAt(l.x, l.z);
        float y = worlds[e].transformPoint(l).y;
        if (!best || y > *best) best = y;
    }
    return best;
}

void GameWorld::invalidateProcedural(EntityId e) {
    std::string prefix = std::format("proc:{}/", e);
    for (auto it = m_procHash.begin(); it != m_procHash.end();) {
        if (startsWith(it->first, prefix)) {
            if (auto m = m_meshCache.find(it->first); m != m_meshCache.end()) {
                if (m->second && m_renderer) m_renderer->device().destroyMesh(m->second);
                m_meshCache.erase(m);
            }
            it = m_procHash.erase(it);
        } else {
            ++it;
        }
    }
    if (auto t = m_terrains.find(e); t != m_terrains.end()) {
        if (t->second.albedo && m_renderer) m_renderer->device().destroyTexture(t->second.albedo);
        m_terrains.erase(t);
    }
    m_foliage.erase(e);
    m_foliageKeys.erase(e);
    m_roadKeys.erase(e);
}

void GameWorld::addOpenWorldItems(std::vector<DrawItem>& items, const std::unordered_map<EntityId, Mat4>& worlds, Vec3 viewPos) {
    m_owStats = {};
    auto worldOf = [&](EntityId e) {
        auto it = worlds.find(e);
        return it == worlds.end() ? Mat4{} : it->second;
    };
    // ---- terrain tiles (distance LOD with hysteresis, skirts hide cracks)
    for (EntityId e : m_scene.entities()) {
        auto* tc = m_scene.get<TerrainComponent>(e);
        if (!tc) continue;
        const world::Heightfield* hf = heightfield(e);
        TerrainCache& cache = m_terrains[e];
        if (!cache.albedo) {
            const Vec3 layers[4] = {tc->layer0, tc->layer1, tc->layer2, tc->layer3};
            int texRes = std::clamp(hf->resolution() * 4, 64, 1024);
            auto rgba = hf->bakeAlbedo(texRes, layers);
            cache.albedo = m_renderer->device().createTexture2D(texRes, texRes, rgba.data());
        }
        Mat4 m = worldOf(e);
        int tiles = hf->tilesPerEdge(tc->tileSize);
        float tileWorld = hf->spacing() * tc->tileSize;
        for (int tz = 0; tz < tiles; ++tz)
            for (int tx = 0; tx < tiles; ++tx) {
                Vec3 centre{(tx + 0.5f) * tileWorld - hf->size() * 0.5f, 0, (tz + 0.5f) * tileWorld - hf->size() * 0.5f};
                centre.y = hf->heightAt(centre.x, centre.z);
                float d = length(m.transformPoint(centre) - viewPos);
                int& lod = cache.lods[{tx, tz}];
                auto it = cache.lods.find({tx, tz});
                lod = world::terrainLod(d, tileWorld, it != cache.lods.end() && m_owStats.terrainTiles >= 0 ? lod : -1, 4);
                std::string ref = std::format("proc:{}/terrain/{}_{}_{}", e, tx, tz, lod);
                auto mesh = procMesh(ref, cache.hash, [&] { return hf->tileMesh(tx, tz, lod, tc->tileSize); });
                if (!mesh) continue;
                if (const MeshData* md = m_cpuMeshes[ref].get()) m_frameOccluders.push_back({md, m});  // hills hide what is behind them
                DrawItem it2{mesh, m, {1, 1, 1}, cache.albedo};
                {
                    Vec3 wc = m.transformPoint(centre);
                    it2.boundsCenter = wc;
                    it2.boundsRadius = tileWorld * 0.75f + tc->maxHeight;
                }
                it2.roughness = 0.92f;
                it2.objectId = e;
                items.push_back(it2);
                ++m_owStats.terrainTiles;
            }
    }
    // ---- roads
    for (EntityId e : m_scene.entities()) {
        auto* sc = m_scene.get<SplineComponent>(e);
        if (!sc || sc->kind != "road") continue;
        // Change detection by value: hashing the component's JSON every frame cost ~0.5 ms for the City Sandbox roads.
        RoadKey& key = m_roadKeys[e];
        if (!key.valid || !(key.spline == *sc)) {
            key.spline = *sc;
            key.hash = hashJson(m_scene.entityToJson(e)["components"]["Spline"]);
            for (int k = 0; k < 3; ++k) key.refs[k] = std::format("proc:{}/{}", e, k == 0 ? "road" : k == 1 ? "sidewalk" : "marking");
            key.valid = true;
        }
        uint64_t h = key.hash;
        world::RoadMeshes meshes;
        bool built = false;
        auto get = [&](int which) {
            if (!built) {
                meshes = world::buildRoadMeshes(*sc);
                built = true;
            }
            return which == 0 ? meshes.road : which == 1 ? meshes.sidewalks : meshes.markings;
        };
        const Vec3 colors[3] = {{0.06f, 0.06f, 0.065f}, {0.42f, 0.41f, 0.40f}, {0.85f, 0.78f, 0.45f}};
        const float rough[3] = {0.85f, 0.9f, 0.6f};
        // cache check without building: only build when the hash changed
        bool fresh = true;
        for (int k = 0; k < 3; ++k) {
            auto it = m_procHash.find(key.refs[k]);
            fresh &= it != m_procHash.end() && it->second == h;
        }
        for (int k = 0; k < 3; ++k) {
            const std::string& ref = key.refs[k];
            auto mesh = fresh ? m_meshCache[ref] : procMesh(ref, h, [&] { return get(k); });
            if (!mesh) continue;
            DrawItem it{mesh, worldOf(e), colors[k]};
            it.roughness = rough[k];
            it.twoSided = true;
            it.objectId = e;
            items.push_back(it);
            ++m_owStats.roadMeshes;
        }
    }
    // ---- foliage (instances within 300 m; GPU instancing groups them in the renderer)
    std::optional<EntityId> terrainE;
    for (EntityId e : m_scene.entities())
        if (m_scene.get<TerrainComponent>(e)) {
            terrainE = e;
            break;
        }
    for (EntityId e : m_scene.entities()) {
        auto* fc = m_scene.get<FoliageComponent>(e);
        if (!fc) continue;
        Mat4 m = worldOf(e);
        uint64_t terrainHash = terrainE ? m_terrains[*terrainE].hash : 0;
        FoliageKey& key = m_foliageKeys[e];  // the same hash as before, recomputed only when an input changed
        if (!key.valid || !(key.foliage == *fc) || std::memcmp(key.world.data(), m.data(), sizeof(float) * 16) != 0 || key.terrainHash != terrainHash) {
            uint64_t hh = hashJson(m_scene.entityToJson(e)["components"]["Foliage"]);
            for (int k = 0; k < 16; ++k) hh = hashStr(hh, std::to_string(m.data()[k]));
            if (terrainE) hh = hashStr(hh, std::to_string(terrainHash));
            key = {*fc, m, terrainHash, hh, true};
        }
        uint64_t h = key.hash;
        auto& cache = m_foliage[e];
        if (cache.first != h) {
            Mat4 tw = terrainE ? worldOf(*terrainE) : Mat4{};
            cache.second = world::scatterFoliage(*fc, m, terrainE ? heightfield(*terrainE) : nullptr, terrainE ? &tw : nullptr);
            cache.first = h;
        }
        auto mesh = meshFor(fc->mesh);
        if (!mesh) continue;
        float meshRadius = 1.0f;
        if (const MeshData* md = cpuMesh(fc->mesh)) {
            Vec3 a, b;
            meshBounds(*md, a, b);
            meshRadius = length(b - a) * 0.5f;
        }
        for (const Mat4& inst : cache.second) {
            if (length(inst.c[3].xyz() - viewPos) > 300.0f) continue;
            DrawItem it{mesh, inst, fc->color};
            applyMaterial(it, fc->material);
            it.boundsCenter = inst.c[3].xyz();
            it.boundsRadius = meshRadius * length(inst.c[0].xyz());
            it.roughness = 0.8f;
            items.push_back(it);
            if (++m_owStats.foliageInstances >= 20000) break;
        }
    }
}

std::optional<Vec3> GameWorld::streamingViewer() {
    auto worlds = m_scene.computeWorldMatrices();
    for (EntityId e : m_scene.entities())
        if (auto* c = m_scene.get<CameraComponent>(e); c && c->primary) return worlds[e].c[3].xyz();
    if (viewerOverride) return viewerOverride;
    EntityId p = m_scene.findByName("Player");
    if (p != kNullEntity) return worlds[p].c[3].xyz();
    return std::nullopt;
}

void GameWorld::updateStreaming(float dt) {
    m_lastRebase = {0, 0, 0};
    if (!m_streaming) return;
    auto v = streamingViewer();
    if (!v) return;
    Vec3 vel{0, 0, 0};
    if (m_prevViewer && dt > 0) vel = (*v - *m_prevViewer) / dt;
    static double clock = 0;
    clock += dt;
    m_lastRebase = m_streaming->update(*v, vel, clock);
    m_prevViewer = *v - m_lastRebase;
    if (viewerOverride) *viewerOverride -= m_lastRebase;
}

} // namespace fledge
