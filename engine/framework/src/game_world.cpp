#include "fledge/framework/game_world.h"
#include "fledge/gameplay/font.h"
#include "fledge/gameplay/localization.h"
#include "fledge/world/culling.h"
#include "gameplay_state.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

#include "fledge/core/log.h"

namespace fledge {

GameWorld::GameWorld(Vfs& vfs, AssetRegistry& registry)
    : m_anim(vfs), m_vfs(vfs), m_registry(registry), m_loader(std::make_shared<CookedAssetLoader>(vfs)) {
    m_script.raycast = [this](Vec3 o, Vec3 d, float maxDist) { return raycast(o, d, maxDist); };
    m_script.raycastFiltered = [this](Vec3 o, Vec3 d, float maxDist, EntityId ignore, uint32_t mask, bool hierarchy) {
        return raycast(o, d, maxDist, QueryFilter{ignore, hierarchy, mask});
    };
    m_script.sphereCast = [this](Vec3 o, float r, Vec3 d, float maxDist, EntityId ignore, uint32_t mask, bool hierarchy) {
        return sphereCast(o, r, d, maxDist, QueryFilter{ignore, hierarchy, mask});
    };
    // Bone sockets (Transform.attachBone) and Fledge Script bone queries read the animation system's model-space poses.
    m_scene.boneResolver = [this](EntityId owner, const std::string& bone) { return m_anim.socketBone(m_scene, owner, bone); };
    m_script.boneModel = [this](EntityId e, const std::string& bone) { return m_anim.socketBone(m_scene, e, bone); };
    m_script.boneNames = [this](EntityId e) { return m_anim.boneNames(m_scene, e); };
}

std::optional<script::RaycastHit> GameWorld::raycast(Vec3 origin, Vec3 dir, float maxDistance) { return raycastImpl(origin, dir, maxDistance, nullptr); }

std::optional<script::RaycastHit> GameWorld::raycast(Vec3 origin, Vec3 dir, float maxDistance, const QueryFilter& filter) {
    return raycastImpl(origin, dir, maxDistance, &filter);
}

const std::vector<EntityId>& GameWorld::queryIgnoreList(const QueryFilter& f) {
    m_queryIgnore.clear();
    if (f.ignore && m_scene.isValid(f.ignore)) {
        m_queryIgnore.push_back(f.ignore);
        if (f.ignoreHierarchy)
            for (EntityId d : m_scene.descendants(f.ignore)) m_queryIgnore.push_back(d);
    }
    return m_queryIgnore;
}

std::optional<script::RaycastHit> GameWorld::sphereCast(Vec3 origin, float radius, Vec3 dir, float maxDistance, const QueryFilter& filter) {
    if (!m_physics) return std::nullopt;  // shape casts need the simulation (play); edit-mode callers build a temporary PhysicsWorld
    const auto& ig = queryIgnoreList(filter);
    auto h = m_physics->sphereCastFiltered(origin, radius, dir, maxDistance, ig.data(), static_cast<int>(ig.size()), filter.layerMask);
    if (!h) return std::nullopt;
    return script::RaycastHit{h->entity, h->point, h->normal, h->distance};
}

std::optional<script::RaycastHit> GameWorld::raycastImpl(Vec3 origin, Vec3 dir, float maxDistance, const QueryFilter* filter) {
    float len = length(dir);
    if (len < 1e-6f || !(maxDistance > 0) || !std::isfinite(maxDistance)) return std::nullopt;
    dir = dir / len;
    std::optional<script::RaycastHit> best;
    const std::vector<EntityId>* ignore = filter ? &queryIgnoreList(*filter) : nullptr;
    // without a collider an entity counts as collision layer 0
    const bool layer0 = !filter || (filter->layerMask & 1u);
    auto ignored = [&](EntityId e) { return ignore && std::find(ignore->begin(), ignore->end(), e) != ignore->end(); };
    if (m_physics) {
        if (!filter) {
            if (auto h = m_physics->raycast(origin, dir, maxDistance)) best = script::RaycastHit{h->entity, h->point, h->normal, h->distance};
        } else if (auto h = m_physics->raycastFiltered(origin, dir, maxDistance, ignore->data(), static_cast<int>(ignore->size()), filter->layerMask)) {
            best = script::RaycastHit{h->entity, h->point, h->normal, h->distance};
        }
    }
    auto worlds = m_scene.computeWorldMatrices();
    for (EntityId e : m_scene.entities()) {
        auto* mr = m_scene.get<MeshRendererComponent>(e);
        if (!mr || !mr->visible) continue;
        auto* col = m_scene.get<ColliderComponent>(e);
        if (m_physics && col) continue;  // exact collider already tested
        if (filter) {  // edit mode tests the bounds of collider entities too: their layer still applies
            if (ignored(e)) continue;
            int layer = col && col->layer > 0 && col->layer < 32 ? col->layer : 0;
            if (!(filter->layerMask & (1u << layer))) continue;
        }
        const MeshData* md = cpuMesh(mr->mesh);
        if (!md || md->vertices.empty()) continue;
        Vec3 mn, mx;
        meshBounds(*md, mn, mx);
        const Mat4& m = worlds[e];
        Vec3 wmin{1e30f, 1e30f, 1e30f}, wmax{-1e30f, -1e30f, -1e30f};
        for (int i = 0; i < 8; ++i) {
            Vec3 p = m.transformPoint({(i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z});
            for (int k = 0; k < 3; ++k) {
                wmin[k] = std::min(wmin[k], p[k]);
                wmax[k] = std::max(wmax[k], p[k]);
            }
        }
        for (int k = 0; k < 3; ++k)  // flat planes still get a hittable slab
            if (wmax[k] - wmin[k] < 1e-3f) {
                wmin[k] -= 5e-4f;
                wmax[k] += 5e-4f;
            }
        float tmin = 0, tmax = maxDistance;
        int axis = -1;
        float sign = 1;
        bool hit = true;
        for (int k = 0; k < 3 && hit; ++k) {
            if (std::fabs(dir[k]) < 1e-8f) {
                if (origin[k] < wmin[k] || origin[k] > wmax[k]) hit = false;
                continue;
            }
            float inv = 1.0f / dir[k];
            float t0 = (wmin[k] - origin[k]) * inv, t1 = (wmax[k] - origin[k]) * inv;
            float s = -1;
            if (t0 > t1) {
                std::swap(t0, t1);
                s = 1;
            }
            if (t0 > tmin) {
                tmin = t0;
                axis = k;
                sign = s;
            }
            tmax = std::min(tmax, t1);
            if (tmin > tmax) hit = false;
        }
        if (!hit || axis < 0) continue;  // origin inside the box counts as no hit
        if (!best || tmin < best->distance) {
            script::RaycastHit h;
            h.entity = e;
            h.distance = tmin;
            h.point = origin + dir * tmin;
            h.normal = Vec3{0, 0, 0};
            h.normal[axis] = sign;
            best = h;
        }
    }
    // terrains (exact heightfield query)
    for (EntityId e : m_scene.entities()) {
        if (!layer0) break;
        auto* tc = m_scene.get<TerrainComponent>(e);
        if (!tc || m_physics || ignored(e)) continue;  // physics has the terrain collider
        const world::Heightfield* hf = heightfield(e);
        if (!hf) continue;
        const Mat4& m = worlds[e];
        Mat4 inv = m.inverse();
        Vec3 lo = inv.transformPoint(origin), ld = inv.transformPoint(origin + dir) - lo;
        float scale = length(ld);
        if (auto hit = hf->raycast(lo, ld, maxDistance * scale)) {
            Vec3 wp = m.transformPoint(*hit);
            float d = length(wp - origin);
            if (!best || d < best->distance) {
                script::RaycastHit h;
                h.entity = e;
                h.distance = d;
                h.point = wp;
                h.normal = normalize((m * Vec4(hf->normalAt(hit->x, hit->z), 0)).xyz());
                best = h;
            }
        }
    }
    return best;
}

void GameWorld::setAssetLoader(std::shared_ptr<AssetLoader> loader) {
    m_loader = std::move(loader);
    invalidateAll();
}

void GameWorld::invalidateAll() {
    releaseGpu();
    if (m_gp) m_gp->uiDocs.clear();
    m_anim.forgetAllCharacters();
    m_cpuMeshes.clear();
    m_materials.clear();
}

void GameWorld::invalidateAsset(const std::string& vpath) {
    auto matches = [&](const std::string& key) {  // LOD meshes are cached as "lod:<ref>#N"
        std::string ref = key;
        if (startsWith(ref, "lod:")) ref = ref.substr(4, ref.rfind('#') - 4);
        return m_registry.resolve(ref) == vpath;
    };
    // 1.3.2: a changed / new .fchar (file watcher, import) drops the cached character, including a failed load of a path that
    // did not exist yet, so Animators pick the file up at once (before, a character set before its file existed stayed missing)
    if (endsWith(toLower(vpath), ".fchar")) m_anim.forgetCharacter(vpath);
    if (m_gp && endsWith(toLower(vpath), ".fui"))
        std::erase_if(m_gp->uiDocs, [&](const auto& entry) { return matches(entry.first); });
    for (auto it = m_cpuMeshes.begin(); it != m_cpuMeshes.end();) it = matches(it->first) ? m_cpuMeshes.erase(it) : std::next(it);
    for (auto it = m_materials.begin(); it != m_materials.end();) it = matches(it->first) ? m_materials.erase(it) : std::next(it);
    if (!m_renderer) return;
    for (auto it = m_meshCache.begin(); it != m_meshCache.end();) {
        if (matches(it->first) && !startsWith(it->first, "builtin:")) {
            m_renderer->device().destroyMesh(it->second);
            it = m_meshCache.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_texCache.begin(); it != m_texCache.end();) {
        if (matches(it->first)) {
            m_renderer->device().destroyTexture(it->second);
            it = m_texCache.erase(it);
        } else {
            ++it;
        }
    }
}
GameWorld::~GameWorld() {
    m_audioDevice.reset();  // stop the device thread before the mixer goes away
    releaseGpu();
}

Status GameWorld::loadScene(const std::string& vpath) {
    auto text = m_vfs.readText(vpath);
    if (!text) return Status::failure("scene not found: " + vpath);
    Json j = Json::parse(*text, nullptr, false);
    if (j.is_discarded()) return Status::failure("scene is not valid JSON: " + vpath);
    if (auto st = m_scene.fromJson(j); !st) return st;
    m_scenePath = vpath;
    m_cookedWorldIndex = Json();
    m_cookedWorldDir.clear();
    if (j.value("streamedWorld", false)) {  // cooked open world: chunks live next to the scene
        m_cookedWorldDir = world::worldDirFor(vpath);
        if (auto idx = m_vfs.readText(m_cookedWorldDir + "/index.json")) {
            m_cookedWorldIndex = Json::parse(*idx, nullptr, false);
            if (m_cookedWorldIndex.is_object()) m_scene.reserveIds(m_cookedWorldIndex.value("nextId", EntityId{1}));
        }
    }
    FLEDGE_LOG_INFO("world", "Loaded scene '{}' ({} entities) from {}", m_scene.name, m_scene.entities().size(), vpath);
    return {};
}

Status GameWorld::saveScene(const std::string& vpath) {
    if (m_playing)
        return Status::failure("cannot save while playing: Play edits are discarded on stop, so saving now would store a play state. "
                               "Call stop, then save_scene (make the edits outside Play to keep them)");
    if (!m_vfs.writeText(vpath, m_scene.toJson().dump(2))) return Status::failure("cannot write " + vpath);
    m_scenePath = vpath;
    FLEDGE_LOG_INFO("world", "Saved scene to {}", vpath);
    return {};
}

void GameWorld::beginPlay() {
    if (m_playing) return;
    m_playing = true;
    m_modules.beginPlay(m_scene);
    // open world: stream chunks around the viewer from here on (before scripts start, so they see the start area)
    if (world::findPartitionSettings(m_scene)) {
        if (!m_jobs) m_jobs = std::make_unique<JobSystem>();
        buildHlods();  // before chunks are streamed out (needs the meshes of every chunk)
        if (m_cookedWorldIndex.is_object()) {
            auto src = std::make_shared<world::VfsChunkSource>(m_vfs, m_cookedWorldDir, m_cookedWorldIndex);
            m_streaming = std::make_unique<world::StreamingWorld>(m_scene, world::PartitionSettings::from(*world::findPartitionSettings(m_scene)), src, m_jobs.get());
        } else {
            m_streaming = world::StreamingWorld::fromLiveScene(m_scene, m_jobs.get());
        }
        if (m_streaming)
            if (auto v = streamingViewer()) m_streaming->loadAround(*v);
    }
    startPhysics();
    beginGameplay();
    for (const WorldSystemFactory& f : worldSystemFactories())  // module systems (world_system.h; none in Fledge)
        if (auto sys = f.create ? f.create() : nullptr) m_systems.emplace_back(f.name, std::move(sys));
    for (auto& [n, sys] : m_systems) sys->beginPlay(*this);
    m_script.beginPlay(m_scene, &m_vfs);
}

WorldSystem* GameWorld::system(std::string_view name) {
    for (auto& [n, sys] : m_systems)
        if (n == name) return sys.get();
    return nullptr;
}

void GameWorld::endPlay() {
    if (!m_playing) return;
    for (auto& d : m_decalPool) releaseDecal(d);  // 1.3.2: impact decals belong to the play session
    m_script.endPlay();
    for (auto it = m_systems.rbegin(); it != m_systems.rend(); ++it) it->second->endPlay(*this);
    m_systems.clear();
    endGameplay();
    m_modules.endPlay(m_scene);
    m_streaming.reset();
    m_prevViewer.reset();
    m_hlods.clear();
    m_anim.clear();
    stopPhysics();
    stopSceneAudio();
    m_playing = false;
}

void GameWorld::tick(float dt) {
    if (!m_playing) return;
    m_scene.each<RotatorComponent, TransformComponent>([dt](EntityId, RotatorComponent& r, TransformComponent& t) {
        Quat delta = Quat::fromAxisAngle(r.axis, radians(r.degreesPerSecond) * dt);
        t.rotation = (delta * t.rotation).normalized();
    });
    auto timed = [](double& acc, auto&& fn) {  // per-system CPU time (systemTimes(), --profile)
        auto t0 = std::chrono::steady_clock::now();
        fn();
        acc += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    timed(m_times.modules, [&] { m_modules.tick(m_scene, dt); });
    timed(m_times.scripts, [&] { m_script.updateEarly(dt); });  // fixed steps + OnUpdate
    timed(m_times.gameplay, [&] { updateGameplay(dt); });
    if (!m_systems.empty())
        timed(m_times.gameplay, [&] {
            for (auto& [n, sys] : m_systems) sys->prePhysics(*this, dt);
        });
    timed(m_times.physics, [&] { stepPhysics(dt); });
    timed(m_times.animation, [&] { animate(dt); });
    updateDecals(dt);  // 1.3.2: decal lifetimes (Fledge Advanced; nothing to do elsewhere)
    if (!m_systems.empty())
        timed(m_times.lateGameplay, [&] {
            for (auto& [n, sys] : m_systems) sys->postAnimation(*this, dt);
        });
    // OnLateUpdate after physics + animation: bone queries / bone sockets see this frame's pose (docs/ANIMATION.md)
    timed(m_times.scripts, [&] { m_script.updateLate(); });
    timed(m_times.lateGameplay, [&] { lateGameplay(dt); });
    timed(m_times.streaming, [&] { updateStreaming(dt); });
    timed(m_times.audio, [&] { updateAudio(dt); });
    ++m_times.ticks;
}

void GameWorld::setRenderer(ForwardRenderer* renderer) {
    if (renderer != m_renderer) {
        releaseGpu();
        m_loadedEnvironment.reset();
    }
    m_renderer = renderer;
}

void GameWorld::releaseGpu() {
    if (!m_renderer) return;
    for (auto mesh : m_clothFrameMeshes) m_renderer->device().destroyMesh(mesh);
    m_clothFrameMeshes.clear();
    clearDecals();  // 1.3.2: decal meshes / procedural decal textures belong to this renderer
    for (auto& t : m_decalTex)
        if (t) {
            m_renderer->device().destroyTexture(t);
            t = {};
        }
    if (m_uiFontTexture) {  // re-uploaded for the next renderer on first use
        m_renderer->setOverlayTexture({});
        m_renderer->device().destroyTexture(m_uiFontTexture);
        m_uiFontTexture = {};
    }
    for (auto& [k, t] : m_skinTextures)
        if (t) m_renderer->device().destroyTexture(t);
    m_skinTextures.clear();
    for (auto& [e, t] : m_terrains)
        if (t.albedo) m_renderer->device().destroyTexture(t.albedo);
    m_terrains.clear();
    for (auto& [ref, h] : m_procHash)
        if (auto it = m_meshCache.find(ref); it != m_meshCache.end() && it->second) {
            m_renderer->device().destroyMesh(it->second);
            m_meshCache.erase(it);
        }
    m_procHash.clear();
    for (auto& [k, m] : m_meshCache)
        if (!startsWith(k, "builtin:")) m_renderer->device().destroyMesh(m);
    for (auto& [k, t] : m_texCache) m_renderer->device().destroyTexture(t);
    m_meshCache.clear();
    m_texCache.clear();
}

const MeshData* GameWorld::cpuMesh(const std::string& ref) {
    if (auto it = m_cpuMeshes.find(ref); it != m_cpuMeshes.end()) return it->second.get();
    std::unique_ptr<MeshData> mesh;
    std::string path = m_registry.resolve(ref);
    if (startsWith(path, "builtin:")) {
        MeshData d = primitives::byName(path);
        if (!d.empty()) mesh = std::make_unique<MeshData>(std::move(d));
    } else if (!path.empty()) {
        std::string err;
        if (auto loaded = m_loader->loadMesh(path, &err)) {
            mesh = std::make_unique<MeshData>(std::move(*loaded));
            // 1.3.3: zero-length / NaN normals, once per mesh (this cache); rendering copes (face normal fallback)
            if (std::vector<uint32_t> zero = zeroNormalVertices(*mesh); !zero.empty())
                FLEDGE_LOG_WARN("world", "{}", zeroNormalWarning("mesh '" + ref + "'", mesh->vertices.size(), zero));
        } else {
            FLEDGE_LOG_WARN("world", "Mesh '{}': {}", ref, err);
        }
    } else {
        FLEDGE_LOG_WARN("world", "Mesh '{}' is not a registered asset", ref);
    }
    auto* raw = mesh.get();
    m_cpuMeshes[ref] = std::move(mesh);  // cache failures too (no log spam)
    return raw;
}

const Material* GameWorld::material(const std::string& ref) {
    if (ref.empty()) return nullptr;
    if (auto it = m_materials.find(ref); it != m_materials.end()) return it->second.get();
    std::unique_ptr<Material> mat;
    std::string path = m_registry.resolve(ref);
    if (auto text = path.empty() ? std::nullopt : m_vfs.readText(path)) {
        auto m = Material::fromJson(Json::parse(*text, nullptr, false));
        if (m) mat = std::make_unique<Material>(m.value());
        else FLEDGE_LOG_WARN("world", "Material '{}': {}", ref, m.error());
    } else {
        FLEDGE_LOG_WARN("world", "Material '{}' not found", ref);
    }
    auto* raw = mat.get();
    m_materials[ref] = std::move(mat);
    return raw;
}

rhi::TextureHandle GameWorld::uploadTexture(const TextureData& tex) {
    auto& dev = m_renderer->device();
    if (tex.format == TextureFormat::RGBA16F) {  // HDR data texture: upload as float
        HdrImage hdr = textureToHdr(tex);
        rhi::TextureDesc d;
        d.format = rhi::TextureFormat::RGBA16F;
        d.width = hdr.width;
        d.height = hdr.height;
        d.sampler = {rhi::Filter::Linear, false, rhi::Wrap::Repeat, false};
        rhi::TextureSubresource sub{0, 0, hdr.width, hdr.height, hdr.rgba.data(), hdr.rgba.size() * sizeof(float)};
        return dev.createTexture(d, std::span(&sub, 1));
    }
    rhi::TextureFormat fmt = tex.format == TextureFormat::BC1 ? rhi::TextureFormat::BC1
                           : tex.format == TextureFormat::BC3 ? rhi::TextureFormat::BC3
                                                              : rhi::TextureFormat::RGBA8;
    const TextureData* src = &tex;
    TextureData raw;
    if (!dev.supportsTextureFormat(fmt)) {  // fallback: decode blocks on the CPU
        raw = decompressToRgba8(tex);
        src = &raw;
        fmt = rhi::TextureFormat::RGBA8;
    }
    std::vector<rhi::TextureMipDesc> mips;
    for (auto& m : src->mips) mips.push_back({m.width, m.height, m.data.data(), m.data.size()});
    return dev.createTexture(fmt, mips, src->srgb);
}

void GameWorld::preloadSceneAssets(const std::function<void(int, int, const std::string&)>& progress) {
    std::vector<std::string> meshes, materials;
    m_scene.each<MeshRendererComponent, TransformComponent>([&](EntityId, MeshRendererComponent& mr, TransformComponent&) {
        if (!mr.mesh.empty()) meshes.push_back(mr.mesh);
        if (!mr.material.empty()) materials.push_back(mr.material);
    });
    std::sort(meshes.begin(), meshes.end());
    meshes.erase(std::unique(meshes.begin(), meshes.end()), meshes.end());
    std::sort(materials.begin(), materials.end());
    materials.erase(std::unique(materials.begin(), materials.end()), materials.end());
    int step = 0;
    int total = static_cast<int>(meshes.size() + materials.size());
    auto tick = [&](const std::string& label) {
        if (progress) progress(step, total, label);
        ++step;
    };
    for (const std::string& mesh : meshes) {
        tick(mesh);
        (void)meshFor(mesh);
    }
    for (const std::string& mat : materials) {
        tick(mat);
        if (const Material* m = material(mat)) {
            (void)textureFor(m->baseColorTexture);
            (void)textureFor(m->normalTexture);
            (void)textureFor(m->metallicRoughnessTexture);
            (void)textureFor(m->occlusionTexture);
            (void)textureFor(m->emissiveTexture);
        }
    }
}

rhi::MeshHandle GameWorld::meshFor(const std::string& ref) {
    if (auto it = m_meshCache.find(ref); it != m_meshCache.end()) return it->second;
    rhi::MeshHandle h;
    std::string path = m_registry.resolve(ref);
    if (startsWith(path, "builtin:")) h = m_renderer->builtinMesh(path);
    else if (const MeshData* mesh = cpuMesh(ref)) h = m_renderer->device().createMesh(*mesh);
    m_meshCache[ref] = h;
    return h;
}

rhi::TextureHandle GameWorld::textureFor(const std::string& ref) {
    if (ref.empty()) return {};
    if (auto it = m_texCache.find(ref); it != m_texCache.end()) return it->second;
    rhi::TextureHandle h;
    std::string err;
    std::string path = m_registry.resolve(ref);
    if (auto tex = path.empty() ? std::nullopt : m_loader->loadTexture(path, &err)) h = uploadTexture(*tex);
    else FLEDGE_LOG_WARN("world", "Texture '{}': {}", ref, err.empty() ? "not a registered asset" : err);
    m_texCache[ref] = h;
    return h;
}

void GameWorld::applyMaterial(DrawItem& item, const std::string& materialRef) {
    const Material* m = material(materialRef);
    if (!m) return;
    item.color = m->baseColor;
    item.texture = textureFor(m->baseColorTexture);
    item.unlit = m->shader == MaterialShader::Unlit;
    item.twoSided = m->twoSided;
    item.alphaMode = static_cast<int>(m->alphaMode);
    item.opacity = m->opacity;
    item.alphaCutoff = m->alphaCutoff;
    item.roughness = m->roughness;
    item.metallic = m->metallic;
    item.normalTexture = textureFor(m->normalTexture);
    item.normalScale = m->normalScale;
    item.metallicRoughnessTexture = textureFor(m->metallicRoughnessTexture);
    item.occlusionTexture = textureFor(m->occlusionTexture);
    item.occlusionStrength = m->occlusionStrength;
    item.emissiveTexture = textureFor(m->emissiveTexture);
    item.emissive = m->emissive * m->emissiveIntensity;
}

void GameWorld::updateEnvironment() {
    if (m_loadedEnvironment && *m_loadedEnvironment == renderSettings.environment) return;
    m_loadedEnvironment = renderSettings.environment;
    if (renderSettings.environment.empty()) {
        m_renderer->setProceduralSky();
        return;
    }
    std::string err;
    std::string path = m_registry.resolve(renderSettings.environment);
    auto tex = path.empty() ? std::nullopt : m_loader->loadTexture(path, &err);
    if (!tex) {
        FLEDGE_LOG_WARN("world", "Environment '{}': {} - using the procedural sky", renderSettings.environment, err.empty() ? "unknown asset" : err);
        m_renderer->setProceduralSky();
        return;
    }
    HdrImage hdr = textureToHdr(*tex);
    m_renderer->setEnvironmentEquirect(hdr.width, hdr.height, std::move(hdr.rgba));
}

void GameWorld::renderAssetPreview(rhi::RenderTargetHandle target, int size, const std::string& meshRef, const std::string& materialRef) {
    if (!m_renderer) return;
    const MeshData* cpu = cpuMesh(meshRef);
    auto mesh = meshFor(meshRef);
    Vec3 mn{-0.5f}, mx{0.5f};
    if (cpu) meshBounds(*cpu, mn, mx);
    Vec3 center = (mn + mx) * 0.5f;
    float radius = std::max(0.05f, length(mx - mn) * 0.5f);
    ViewItem view;
    view.position = center + normalize(Vec3{1.0f, 0.7f, 1.4f}) * (radius * 2.6f);
    view.view = Mat4::lookAt(view.position, center, {0, 1, 0});
    view.projection = Mat4::perspective(radians(40), 1.0f, radius * 0.1f, radius * 10.0f);
    std::vector<DrawItem> items;
    if (mesh) {
        DrawItem item{mesh, Mat4{}, {0.8f, 0.8f, 0.8f}, {}, false};
        applyMaterial(item, materialRef);
        items.push_back(item);
    }
    LightItem key;
    key.direction = normalize(Vec3{-0.5f, -1.0f, -0.6f});
    key.intensity = 1.2f;
    RenderOptions opts;
    opts.drawGrid = false;
    opts.clearColor = {0.16f, 0.17f, 0.2f, 1.0f};
    opts.ambient = {0.3f, 0.3f, 0.33f};
    opts.settings.shadows = false;
    opts.settings.bloom = false;
    opts.settings.fxaa = false;
    opts.settings.showSky = false;
    opts.settings.environmentIntensity = 0.8f;
    opts.temporal = false;  // one-off thumbnail: no TAA history
    opts.graphics.upscaler = UpscalerKind::Off;
    opts.graphics.rt.enabled = false;
    updateEnvironment();
    m_renderer->render(target, size, size, view, items, {key}, opts);
}

std::optional<ViewItem> GameWorld::primaryView(float aspect) {
    std::optional<ViewItem> out;
    m_scene.each<CameraComponent, TransformComponent>([&](EntityId e, CameraComponent& c, TransformComponent&) {
        if (out && !c.primary) return;
        Transform tr = m_scene.worldTransform(e);
        ViewItem v;
        v.position = tr.position;
        v.view = Mat4::lookAt(tr.position, tr.position + tr.forward(), tr.up());
        v.projection = Mat4::perspective(radians(c.fovDegrees), aspect, c.nearPlane, c.farPlane);
        v.camera = static_cast<uint32_t>(e);  // 1.3.1: shadowOnly / ownerCamera (docs/GRAPHICS.md "Per-mesh shadows")
        // 1.3.2 viewmodel pass (docs/GRAPHICS.md "Viewmodel"): MeshRenderer / Animator viewmodel items use this projection
        float vmFov = c.viewmodelFov > 0 ? c.viewmodelFov : c.fovDegrees;
        float vmNear = std::clamp(c.viewmodelNear > 0 ? c.viewmodelNear : 0.01f, 0.001f, 1.0f);
        v.viewmodelPass = true;
        v.viewmodelProjection = Mat4::perspective(radians(std::clamp(vmFov, 1.0f, 170.0f)), aspect, vmNear, std::max(vmNear * 2, c.viewmodelFar));
        out = v;
    });
    return out;
}

Json GameWorld::skinningInfo(EntityId e) {
    Json j = Json::object();
    const int gpuLimit = m_renderer ? m_renderer->gpuSkinBoneLimit() : 0;
    j["gpuLimit"] = gpuLimit;
    j["editionLimit"] = maxGpuSkinBones();
    j["edition"] = editionId(currentEdition());
    j["backend"] = m_renderer ? rhi::toString(m_renderer->device().backend()) : "none";
    j["note"] = m_renderer ? m_renderer->gpuSkinNote() : std::string("no renderer (headless MCP server without a GPU): nothing is drawn");
    auto* ac = m_scene.get<AnimatorComponent>(e);
    const anim::Character* ch = ac ? m_anim.character(ac->character) : nullptr;
    if (!ch) {
        j["path"] = "none";
        j["reason"] = ac ? "the character did not load" : "no Animator";
        return j;
    }
    const int bones = static_cast<int>(ch->skeleton.bones.size());
    j["bones"] = bones;
    if (ch->mesh.empty()) {
        j["path"] = "none";
        j["reason"] = "animation-only character (no mesh)";
        return j;
    }
    SkinningDecision d = skinningDecision(bones, gpuLimit, ac->crowd && !ac->footIK && ac->lookAt.empty());
    // 1.3.2: 8 skin influences need the 8-influence shader variants (all backends that skin on the GPU have them)
    const int influences = ch->skin.influences();
    j["influences"] = influences;
    const int gpuInfluences = m_renderer ? m_renderer->gpuSkinInfluences() : 0;
    if (d.gpu && influences > gpuInfluences) {
        d.gpu = false;
        d.reason = std::format("{} skin influences per vertex: the {} backend has no 8-influence skinning shader here, so it is skinned on the CPU",
                               influences, j["backend"].get<std::string>());
    } else if (d.gpu && influences > 4) {
        d.reason += ", 8 influences per vertex";
    }
    j["path"] = d.gpu ? "gpu" : "cpu";
    j["reason"] = d.reason;
    return j;
}

bool GameWorld::shadowOnlyInView(bool shadowOnly, const std::string& ownerCamera) const {
    if (!shadowOnly || !m_viewCamera) return false;  // the editor's free camera (camera 0) shows every mesh
    if (ownerCamera.empty()) return true;            // every camera view of the game
    EntityId owner = m_scene.findByName(ownerCamera);
    return owner == m_viewCamera;
}

void GameWorld::render(rhi::RenderTargetHandle target, int width, int height, const ViewItem& view,
                       const std::vector<EntityId>& selection, bool drawGrid) {
    if (!m_renderer) return;
    auto t0 = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    m_viewCamera = static_cast<EntityId>(view.camera);
    m_viewViewmodel = view.viewmodelPass;  // 1.3.2: viewmodel items are drawn in their own pass in this view
    auto world = m_scene.computeWorldMatrices();
    auto t1 = std::chrono::steady_clock::now();
    m_times.worldMatrices += ms(t0, t1);
    auto isSelected = [&](EntityId e) { return std::find(selection.begin(), selection.end(), e) != selection.end(); };
    std::vector<LightItem> lights;
    m_scene.each<LightComponent, TransformComponent>([&](EntityId e, LightComponent& l, TransformComponent&) {
        Transform wt = decompose(world[e]);
        LightItem li;
        li.kind = l.type == LightType::Point ? LightKind::Point : l.type == LightType::Spot ? LightKind::Spot : LightKind::Directional;
        li.direction = wt.forward();
        li.position = wt.position;
        li.color = l.color;
        li.intensity = l.intensity;
        li.range = l.range;
        li.innerCone = radians(std::min(l.innerConeDegrees, l.outerConeDegrees));
        li.outerCone = radians(l.outerConeDegrees);
        li.castShadows = l.castShadows;
        li.shadowBias = l.shadowBias;
        li.normalBias = l.normalBias;
        lights.push_back(li);
    });
    const std::vector<Mat4> shadowVolumes = shadowCasterVolumes(view, renderSettings, lights);
    auto hitsShadow = [&](Vec3 centre, float radius) {
        for (const Mat4& vp : shadowVolumes)
            if (sphereInClip(vp, centre, radius)) return true;
        return false;
    };
    std::vector<DrawItem> items;
    (void)isSelected;
    m_frameOccluders.clear();
    addOpenWorldItems(items, world, view.position);  // terrain tiles become occluders
    if (frustumCulling) {  // terrain tiles, foliage instances, roads with bounds
        world::Frustum fr = world::Frustum::fromViewProj(view.projection * view.view);
        std::vector<DrawItem> kept;
        kept.reserve(items.size());
        for (DrawItem& d : items) {
            bool out = d.boundsRadius > 0 && !fr.sphere(d.boundsCenter, d.boundsRadius);
            if (out) {
                ++m_owStats.culled;
                if (!d.castShadows || !hitsShadow(d.boundsCenter, d.boundsRadius)) continue;
                d.offCamera = true;  // hill / tree behind the camera still writes the shadow map
            }
            kept.push_back(std::move(d));
        }
        items.swap(kept);
    }
    auto ta = std::chrono::steady_clock::now();
    m_times.openWorldItems += ms(t1, ta);
    addMeshItems(items, world, view, selection, shadowVolumes);  // frustum / distance / occlusion culling + LOD selection
    auto tb = std::chrono::steady_clock::now();
    m_times.meshItems += ms(ta, tb);
    addAnimatedItems(items, world, view, selection, shadowVolumes);
    addHlodItems(items, view.position);
    addDecalItems(items, world);  // 1.3.2 projected decals (Fledge Advanced)
    addPhysicsDebug(selection, world);  // collider gizmos (selection) + physics debug view
    auto tc = std::chrono::steady_clock::now();
    m_times.animatedItems += ms(tb, tc);
    if (m_playing) addHud(width, height);
    m_times.hud += ms(tc, std::chrono::steady_clock::now());
    // Selection outline: world-space wire box around each selected mesh (or a small cross for empties).
    for (EntityId e : selection) {
        if (!m_scene.isValid(e)) continue;
        const Mat4& m = world[e];
        Vec3 mn{-0.1f, -0.1f, -0.1f}, mx{0.1f, 0.1f, 0.1f};
        if (auto* mr = m_scene.get<MeshRendererComponent>(e))
            if (const MeshData* md = cpuMesh(mr->mesh)) meshBounds(*md, mn, mx);
        Vec3 c[8];
        for (int k = 0; k < 8; ++k) c[k] = m.transformPoint({k & 1 ? mx.x : mn.x, k & 2 ? mx.y : mn.y, k & 4 ? mx.z : mn.z});
        const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (auto& ed : edges) m_renderer->addLine(c[ed[0]], c[ed[1]], {1.0f, 0.6f, 0.1f});
    }
    if (renderSettings.giProbes) {
        Vec3 mn{1e9f, 1e9f, 1e9f}, mx{-1e9f, -1e9f, -1e9f};
        std::vector<Vec3> centres;
        std::vector<float> radii;
        for (const DrawItem& it : items) {
            if (!it.staticCaster || it.boundsRadius <= 0) continue;
            Vec3 r{it.boundsRadius, it.boundsRadius, it.boundsRadius};
            mn.x = std::min(mn.x, it.boundsCenter.x - r.x);
            mn.y = std::min(mn.y, it.boundsCenter.y - r.y);
            mn.z = std::min(mn.z, it.boundsCenter.z - r.z);
            mx.x = std::max(mx.x, it.boundsCenter.x + r.x);
            mx.y = std::max(mx.y, it.boundsCenter.y + r.y);
            mx.z = std::max(mx.z, it.boundsCenter.z + r.z);
            if (centres.size() < 64) {
                centres.push_back(it.boundsCenter);
                radii.push_back(it.boundsRadius);
            }
        }
        uint64_t key = 14695981039346656037ull;
        auto mix = [&](uint64_t v) {
            key ^= v;
            key *= 1099511628211ull;
        };
        mix(centres.size());
        mix(static_cast<uint64_t>(std::llround(mn.x * 4)) ^ (static_cast<uint64_t>(std::llround(mx.x * 4)) << 20));
        mix(static_cast<uint64_t>(std::llround(mn.z * 4)) ^ (static_cast<uint64_t>(std::llround(mx.z * 4)) << 20));
        if (key != m_giKey) {
            m_giKey = key;
            if (centres.empty()) m_renderer->setGiBake({});
            else m_renderer->setGiBake(bakeGiProbes(mn, mx, centres, radii, {0.55f, 0.62f, 0.78f}));
        }
    } else if (m_giKey != 0) {
        m_giKey = 0;
        m_renderer->setGiBake({});
    }
    RenderOptions opts;
    opts.drawGrid = drawGrid;
    opts.settings = renderSettings;
    opts.graphics = graphicsSettings;
    opts.pathTraceReference = pathTraceReference;
    updateEnvironment();
    auto t2 = std::chrono::steady_clock::now();
    m_times.drawItems += ms(t1, t2);
    m_renderer->render(target, width, height, view, items, lights, opts);
    m_times.renderer += ms(t2, std::chrono::steady_clock::now());
    ++m_times.renders;
}

} // namespace fledge
