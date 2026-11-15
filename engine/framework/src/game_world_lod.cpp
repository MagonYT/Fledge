// GameWorld: LOD selection, culling (frustum grid, distance, software occlusion) and HLOD proxies.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

#include "fledge/assets/cooked.h"
#include "fledge/assets/lod.h"
#include "fledge/core/log.h"
#include "fledge/framework/game_world.h"
#include "fledge/physics/physics.h"
#include "fledge/world/culling.h"

namespace fledge {

MeshData* GameWorld::lodChain(const std::string& ref) {
    if (!cpuMesh(ref)) return nullptr;
    MeshData* m = m_cpuMeshes[ref].get();
    static const std::string kTried = "#lods-tried";
    if (m->lods.empty() && !m_procHash.count(ref + kTried)) {  // runtime fallback for meshes cooked without LODs / builtins
        generateLods(*m);
        m_procHash[ref + kTried] = 1;
    }
    return m;
}

rhi::MeshHandle GameWorld::lodMesh(const std::string& ref, int lod) {
    if (lod <= 0) return meshFor(ref);
    std::string key = std::format("lod:{}#{}", ref, lod);
    if (auto it = m_meshCache.find(key); it != m_meshCache.end()) return it->second;
    MeshData* src = lodChain(ref);
    rhi::MeshHandle h;
    if (src && lod <= static_cast<int>(src->lods.size())) {
        MeshData d;
        d.vertices = src->vertices;
        d.tangents = src->tangents;
        d.indices = src->lods[static_cast<size_t>(lod) - 1].indices;
        if (d.tangents.size() != d.vertices.size()) generateTangents(d);
        h = m_renderer->device().createMesh(d);
    }
    m_meshCache[key] = h;
    return h;
}

void GameWorld::addMeshItems(std::vector<DrawItem>& items, std::unordered_map<EntityId, Mat4>& worlds, const ViewItem& view,
                             const std::vector<EntityId>& selection, const std::vector<Mat4>& shadowVolumes) {
    auto t0 = std::chrono::steady_clock::now();
    m_rstats = {};
    for (auto mesh : m_clothFrameMeshes) m_renderer->device().destroyMesh(mesh);
    m_clothFrameMeshes.clear();
    m_scene.each<ClothComponent, TransformComponent>([&](EntityId e, ClothComponent& cloth, TransformComponent&) {
        if (!cloth.enabled) return;
        const auto* appearance = m_scene.get<MeshRendererComponent>(e);
        if (appearance && !appearance->visible) return;
        MeshData surface = m_physics ? m_physics->clothMesh(e) : MeshData{};
        DrawItem draw{};
        if (surface.empty()) {
            surface = physics::clothGrid(cloth);
            draw.model = worlds.at(e);
        }
        generateTangents(surface);
        draw.mesh = m_renderer->device().createMesh(surface);
        if (!draw.mesh) return;
        m_clothFrameMeshes.push_back(draw.mesh);
        draw.objectId = e;
        draw.twoSided = true;
        draw.instanceable = false;
        draw.selected = std::find(selection.begin(), selection.end(), e) != selection.end();
        if (appearance) {
            draw.color = appearance->color;
            draw.castShadows = appearance->castShadows;
            draw.texture = textureFor(appearance->texture);
            applyMaterial(draw, appearance->material);
            draw.twoSided = true;
        }
        items.push_back(draw);
    });
    struct Cand {
        EntityId e;
        MeshRendererComponent* mr;
        Vec3 mn, mx, centre;
        float radius;
        MeshData* mesh;
    };
    std::vector<Cand> cands;
    std::vector<std::pair<EntityId, MeshRendererComponent*>> viewmodelMeshes;  // 1.3.2: never culled, LOD 0, own pass
    m_scene.each<MeshRendererComponent, TransformComponent>([&](EntityId e, MeshRendererComponent& mr, TransformComponent&) {
        if (!mr.visible) return;
        if (auto* cloth = m_scene.get<ClothComponent>(e); cloth && cloth->enabled) return;
        if (mr.viewmodel && m_viewViewmodel) {
            viewmodelMeshes.push_back({e, &mr});
            return;
        }
        MeshData* md = lodChain(mr.mesh);
        if (!md) return;
        Vec3 lmn, lmx;
        meshBounds(*md, lmn, lmx);
        const Mat4& m = worlds[e];
        Vec3 mn{1e30f}, mx{-1e30f};
        for (int i = 0; i < 8; ++i) {
            Vec3 p = m.transformPoint({(i & 1) ? lmx.x : lmn.x, (i & 2) ? lmx.y : lmn.y, (i & 4) ? lmx.z : lmn.z});
            for (int k = 0; k < 3; ++k) {
                mn[k] = std::min(mn[k], p[k]);
                mx[k] = std::max(mx[k], p[k]);
            }
        }
        Vec3 c = (mn + mx) * 0.5f;
        cands.push_back({e, &mr, mn, mx, c, length(mx - c), md});
    });
    m_rstats.candidates = static_cast<int>(cands.size());
    Mat4 vp = view.projection * view.view;
    world::Frustum fr = world::Frustum::fromViewProj(vp);
    // distance + frustum (spatial grid)
    std::vector<uint32_t> visible;
    {
        world::SpatialGrid grid(64);
        std::vector<Vec3> centres;
        std::vector<float> radii;
        for (uint32_t i = 0; i < cands.size(); ++i) {
            const Cand& c = cands[i];
            float d = length(c.centre - view.position) - c.radius;
            float limit = c.mr->maxDrawDistance > 0 ? c.mr->maxDrawDistance : drawDistance;
            if (limit > 0 && d > limit) {
                ++m_rstats.distanceCulled;
                centres.push_back(c.centre);
                radii.push_back(c.radius);
                continue;
            }
            centres.push_back(c.centre);
            radii.push_back(c.radius);
            grid.insert(i, c.centre, c.radius);
        }
        if (frustumCulling) {
            grid.query(fr, centres, radii, visible);
            std::sort(visible.begin(), visible.end());
            m_rstats.frustumCulled = static_cast<int>(cands.size()) - m_rstats.distanceCulled - static_cast<int>(visible.size());
        } else {
            for (uint32_t i = 0; i < cands.size(); ++i)
                if (radii[i] >= 0 && !(drawDistance > 0 && length(cands[i].centre - view.position) - cands[i].radius > drawDistance)) visible.push_back(i);
        }
    }
    // software occlusion: big, simple meshes + terrain tiles are occluders
    // Edit mode skips the software raster. It walks every visible mesh on the CPU each frame, which is what makes a
    // large project stutter while the camera moves. Play still occludes. The GPU (frustum cull stays on) can draw the extras.
    if (occlusionCulling && m_playing && !visible.empty()) {
        world::OcclusionBuffer ob(256, 128);
        ob.begin(vp);
        auto add = [&](const MeshData* md, const Mat4& m) {
            ob.addOccluder(*md, m);
            ++m_rstats.occluders;
        };
        for (auto& [md, m] : m_frameOccluders) add(md, m);
        for (uint32_t i : visible) {
            const Cand& c = cands[i];
            if (c.radius >= 2.0f && c.mesh->indices.size() / 3 <= 1000 && std::find(selection.begin(), selection.end(), c.e) == selection.end() &&
                !shadowOnlyInView(c.mr->shadowOnly, c.mr->ownerCamera))  // a mesh hidden in this view occludes nothing
                add(c.mesh, worlds[c.e]);
        }
        ob.finish();
        std::vector<uint32_t> kept;
        for (uint32_t i : visible) {
            if (ob.visible(cands[i].mn, cands[i].mx)) kept.push_back(i);
            else ++m_rstats.occlusionCulled;
        }
        visible.swap(kept);
    }
    // LOD + draw items
    float projY = view.projection.at(1, 1);
    for (uint32_t i : visible) {
        const Cand& c = cands[i];
        int lodCount = static_cast<int>(c.mesh->lods.size());
        float dist = length(c.centre - view.position);
        float size = screenSizeOf(c.radius, dist, projY);
        auto it = m_lodState.find(c.e);
        int lod = selectLod(size, lodCount, c.mr->lodBias + lodBias, it == m_lodState.end() ? -1 : it->second, c.mr->forceLod);
        m_lodState[c.e] = lod;
        auto mesh = lodMesh(c.mr->mesh, lod);
        if (!mesh) continue;
        bool sel = std::find(selection.begin(), selection.end(), c.e) != selection.end();
        DrawItem item{mesh, worlds[c.e], c.mr->color, textureFor(c.mr->texture), sel};
        applyMaterial(item, c.mr->material);  // material wins over the legacy color/texture fields
        item.objectId = c.e;
        item.boundsCenter = c.centre;
        item.boundsRadius = c.radius;
        item.castShadows = c.mr->castShadows;
        item.staticCaster = c.mr->isStatic;
        item.shadowOnly = shadowOnlyInView(c.mr->shadowOnly, c.mr->ownerCamera);
        items.push_back(item);
        ++m_rstats.lodHistogram[std::min(lod, 4)];
        ++m_rstats.drawn;
    }
    // Frustum and occlusion cull the camera view only. A mesh behind the player still casts into the shadow maps
    // (and ray-traced shadows) when it overlaps a cascade, spot, or point volume.
    auto inShadow = [&](Vec3 centre, float radius) {
        for (const Mat4& vp : shadowVolumes)
            if (sphereInClip(vp, centre, radius)) return true;
        return false;
    };
    std::vector<char> shown(cands.size(), 0);
    for (uint32_t i : visible) shown[i] = 1;
    for (uint32_t i = 0; i < cands.size(); ++i) {
        if (shown[i]) continue;
        const Cand& c = cands[i];
        if (!c.mr->castShadows) continue;
        if (!inShadow(c.centre, c.radius)) continue;
        int lodCount = static_cast<int>(c.mesh->lods.size());
        float dist = length(c.centre - view.position);
        float size = screenSizeOf(c.radius, dist, projY);
        auto it = m_lodState.find(c.e);
        int lod = selectLod(size, lodCount, c.mr->lodBias + lodBias, it == m_lodState.end() ? -1 : it->second, c.mr->forceLod);
        m_lodState[c.e] = lod;
        auto mesh = lodMesh(c.mr->mesh, lod);
        if (!mesh) continue;
        DrawItem item{mesh, worlds[c.e], c.mr->color, textureFor(c.mr->texture), false};
        applyMaterial(item, c.mr->material);
        item.objectId = c.e;
        item.boundsCenter = c.centre;
        item.boundsRadius = c.radius;
        item.castShadows = true;
        item.staticCaster = c.mr->isStatic;
        item.shadowOnly = shadowOnlyInView(c.mr->shadowOnly, c.mr->ownerCamera);
        item.offCamera = true;
        items.push_back(item);
    }
    // 1.3.2 viewmodel meshes: drawn with their own projection (often a different FOV and a closer near plane than the
    // world camera), so the world frustum / distance / occlusion tests do not apply to them; they occlude nothing either
    for (auto& [e, mr] : viewmodelMeshes) {
        auto mesh = lodMesh(mr->mesh, 0);
        if (!mesh) continue;
        bool sel = std::find(selection.begin(), selection.end(), e) != selection.end();
        DrawItem item{mesh, worlds[e], mr->color, textureFor(mr->texture), sel};
        applyMaterial(item, mr->material);
        item.objectId = e;
        item.castShadows = false;
        item.viewmodel = true;
        items.push_back(item);
        ++m_rstats.drawn;
    }
    m_rstats.cullMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------- animation ---
void GameWorld::syncAnimationGpuPath() {
    if (!m_renderer) {
        m_anim.gpuSkinning = false;
        return;
    }
    m_anim.gpuSkinning = m_renderer->gpuSkinning();
    m_anim.gpuSkinBones = m_renderer->gpuSkinBoneLimit();
    m_anim.gpuSkinInfluences = m_renderer->gpuSkinInfluences();
}

void GameWorld::animate(float dt) {
    syncAnimationGpuPath();
    Vec3 viewer = viewerOverride.value_or(Vec3{0, 0, 0});
    if (auto v = streamingViewer()) viewer = *v;
    m_anim.update(m_scene, dt, m_playing, viewer, [this](float x, float z) { return terrainHeight(x, z); });
}

void GameWorld::catchUpAnimation() {
    if (!m_playing) return;
    Vec3 viewer = viewerOverride.value_or(Vec3{0, 0, 0});
    if (auto v = streamingViewer()) viewer = *v;
    m_anim.catchUp(m_scene, viewer, [this](float x, float z) { return terrainHeight(x, z); });
}

namespace {
// Joint / weight texture of the GPU path (RGBA32F, 2 texels per vertex; 4 with 8 influences) for the vertices `verts` of a
// character (all vertices when verts is null, the 1.3.1 layout).
rhi::TextureHandle makeSkinTexture(rhi::RenderDevice& dev, const anim::Character& ch, const std::vector<uint32_t>* verts) {
    const int texels = ch.skin.influences() > 4 ? 4 : 2;  // 1.3.2: 8 influences = 4 texels per vertex
    const size_t count = verts ? verts->size() : ch.mesh.vertices.size();
    int n = static_cast<int>(count) * texels, W = 1024, H = std::max(1, (n + W - 1) / W);
    std::vector<float> data(static_cast<size_t>(W) * H * 4, 0.0f);
    // fromJson guarantees one joint/weight entry per vertex; stay inside the texture regardless
    size_t nv = std::min({ch.mesh.vertices.size(), ch.skin.joints.size(), ch.skin.weights.size()});
    float maxJoint = static_cast<float>(std::max<size_t>(1, ch.skeleton.bones.size()) - 1);
    const size_t stride = static_cast<size_t>(texels) * 4;
    for (size_t i = 0; i < count; ++i) {
        size_t v = verts ? (*verts)[i] : i;
        if (v >= nv) continue;
        for (int k = 0; k < 4; ++k) {
            data[i * stride + static_cast<size_t>(k)] = std::min(maxJoint, static_cast<float>(ch.skin.joints[v][static_cast<size_t>(k)]));
            data[i * stride + 4 + static_cast<size_t>(k)] = ch.skin.weights[v][k];
        }
        if (texels == 4 && v < ch.skin.joints1.size() && v < ch.skin.weights1.size())
            for (int k = 0; k < 4; ++k) {
                data[i * stride + 8 + static_cast<size_t>(k)] = std::min(maxJoint, static_cast<float>(ch.skin.joints1[v][static_cast<size_t>(k)]));
                data[i * stride + 12 + static_cast<size_t>(k)] = ch.skin.weights1[v][k];
            }
    }
    rhi::TextureDesc d;
    d.format = rhi::TextureFormat::RGBA32F;
    d.width = W;
    d.height = H;
    d.sampler = {rhi::Filter::Nearest, false, rhi::Wrap::Clamp};
    rhi::TextureSubresource sub{0, 0, W, H, data.data(), data.size() * sizeof(float)};
    return dev.createTexture(d, std::span(&sub, 1));
}
// One material slot's part of a mesh: its own vertices (compacted, in the order of `verts`) and remapped indices.
MeshData slotMesh(const MeshData& src, const std::vector<uint32_t>& verts, const std::vector<uint32_t>& indices) {
    MeshData m;
    m.vertices.reserve(verts.size());
    for (uint32_t v : verts) m.vertices.push_back(v < src.vertices.size() ? src.vertices[v] : Vertex{});
    m.indices = indices;
    return m;
}
} // namespace

const GameWorld::CharSlots* GameWorld::characterSlots(const anim::Character& ch) {
    if (ch.submeshes.size() < 2 || !advancedRenderFeatures()) return nullptr;
    auto it = m_charSlots.find(&ch);
    if (it != m_charSlots.end()) return &it->second;
    CharSlots cs;
    std::vector<uint32_t> local(ch.mesh.vertices.size());
    for (const auto& sm : ch.submeshes) {
        std::vector<uint32_t> verts, idx;
        std::fill(local.begin(), local.end(), UINT32_MAX);
        size_t end = std::min<size_t>(ch.mesh.indices.size(), static_cast<size_t>(sm.firstIndex) + sm.indexCount);
        for (size_t i = sm.firstIndex; i < end; ++i) {
            uint32_t v = ch.mesh.indices[i];
            if (v >= local.size()) continue;
            if (local[v] == UINT32_MAX) {
                local[v] = static_cast<uint32_t>(verts.size());
                verts.push_back(v);
            }
            idx.push_back(local[v]);
        }
        cs.verts.push_back(std::move(verts));
        cs.indices.push_back(std::move(idx));
    }
    return &m_charSlots.emplace(&ch, std::move(cs)).first->second;
}

void GameWorld::addAnimatedItems(std::vector<DrawItem>& items, std::unordered_map<EntityId, Mat4>& worlds, const ViewItem& view,
                                const std::vector<EntityId>& selection, const std::vector<Mat4>& shadowVolumes) {
    if (!m_renderer) return;
    m_rstats.shadowProxies = 0;
    world::Frustum fr = world::Frustum::fromViewProj(view.projection * view.view);
    syncAnimationGpuPath();
    // 1.3.2 fix: a reloaded character (reimport, file changed on disk) gets a new bind mesh and skin texture; the 1.3.1 cache
    // was keyed by path only and kept the first upload forever
    if (m_skinGeneration != m_anim.characterGeneration()) {
        for (auto& [k, t] : m_skinTextures)
            if (t) m_renderer->device().destroyTexture(t);
        m_skinTextures.clear();
        m_charSlots.clear();
        m_skinGeneration = m_anim.characterGeneration();
    }
    m_scene.each<AnimatorComponent, TransformComponent>([&](EntityId e, AnimatorComponent& ac, TransformComponent&) {
        if (!ac.visible) return;
        // missing / malformed (logged once by the animation system) or animation-only (no mesh): nothing to draw
        const anim::Character* loaded = m_anim.character(ac.character);
        if (!loaded || loaded->mesh.empty()) return;
        auto sk = m_anim.skinned(e);
        if (sk.character && sk.character != loaded) {  // Animator.character changed since the last animation update
            if (m_playing) return;                     // the next update rebinds it (one frame without this character)
            m_anim.update(m_scene, 0, false, {}, nullptr);  // edit mode: rebind now (same as the first-frame bind pose below)
            sk = m_anim.skinned(e);
        }
        // 1.3.2 multi-material characters (Fledge Advanced): one draw item per material slot, all skinned by the one pose
        const CharSlots* slots = characterSlots(*loaded);
        if (!slots && loaded->submeshes.size() >= 2 && m_multiMaterialWarned.insert(ac.character).second)
            FLEDGE_LOG_WARN("anim", "Character '{}' has {} material slots: {}; drawn with one material", ac.character, loaded->submeshes.size(),
                            advancedRenderNote("Multi-material characters"));
        const bool sel = std::find(selection.begin(), selection.end(), e) != selection.end();
        const bool viewmodelDraw = ac.viewmodel && m_viewViewmodel;
        Vec3 centre = worlds[e].transformPoint({0, 0.9f, 0});
        constexpr float kRadius = 1.2f;
        // Off camera: a box in the shadow map, not the skinned mesh. In view, or selected, the real mesh.
        if (!viewmodelDraw && !sel && !fr.sphere(centre, kRadius)) {
            if (!ac.castShadows) return;
            bool hit = false;
            for (const Mat4& vp : shadowVolumes)
                if (sphereInClip(vp, centre, kRadius)) hit = true;
            if (!hit) return;
            auto mesh = m_renderer->builtinMesh("builtin:cube");
            if (!mesh) return;
            DrawItem it{mesh, worlds[e] * Mat4::translation({0, 0.9f, 0}) * Mat4::scale({0.45f, 0.9f, 0.25f}), ac.color, {}, false};
            it.objectId = e;
            it.boundsCenter = centre;
            it.boundsRadius = kRadius;
            it.castShadows = true;
            it.offCamera = true;
            it.staticCaster = true;
            it.roughness = 1.0f;
            items.push_back(it);
            ++m_rstats.shadowProxies;
            return;
        }
        // the material of slot k: Animator.materials[k], else the slot's own (from the file), else Animator.material
        auto slotMaterial = [&](size_t k) -> std::string {
            if (ac.materials.is_array() && k < ac.materials.size() && ac.materials[k].is_string() && !ac.materials[k].get<std::string>().empty())
                return ac.materials[k].get<std::string>();
            if (k < loaded->submeshes.size() && !loaded->submeshes[k].material.empty()) return loaded->submeshes[k].material;
            return ac.material;
        };
        auto common = [&](DrawItem& it, const std::string& material) {
            applyMaterial(it, material);
            if (material == ac.material) it.roughness = 0.7f;  // the 1.0 look of a character without its own material
            it.objectId = e;
            it.boundsCenter = worlds[e].transformPoint({0, 0.9f, 0});
            it.boundsRadius = 1.2f;
            it.castShadows = ac.castShadows;
            it.shadowOnly = shadowOnlyInView(ac.shadowOnly, ac.ownerCamera);
            it.viewmodel = ac.viewmodel && m_viewViewmodel;
        };
        if (sk.bones && sk.character) {  // GPU skinning: static bind mesh + per-character joint/weight texture + bone matrices
            const anim::Character* ch = sk.character;
            if (ch->mesh.empty()) return;
            const int infl = ch->skin.influences() > 4 ? 8 : 4;
            float projY = view.projection.at(1, 1);
            float dist = length(centre - view.position);
            float size = screenSizeOf(kRadius, dist, projY);
            int lodCount = static_cast<int>(ch->mesh.lods.size());
            int lod = viewmodelDraw ? 0 : selectLod(size, lodCount, lodBias, -1, -1);
            auto gpuItem = [&](rhi::MeshHandle mesh, rhi::TextureHandle tex, const std::string& material) {
                if (!mesh || !tex) return;
                DrawItem it{mesh, worlds[e], ac.color, {}, sel};
                common(it, material);
                it.skinTexture = tex;
                it.bones = sk.bones->data();
                it.boneCount = static_cast<int>(sk.bones->size());
                it.skinInfluences = infl;
                it.instanceable = false;
                items.push_back(it);
            };
            if (slots) {
                for (size_t k = 0; k < slots->verts.size(); ++k) {
                    if (slots->indices[k].empty()) continue;
                    std::string key = std::format("{}#{}", ac.character, k);
                    auto mesh = procMesh("proc:charbind/" + key, m_skinGeneration + 1, [&] { return slotMesh(ch->mesh, slots->verts[k], slots->indices[k]); });
                    rhi::TextureHandle& tex = m_skinTextures[key];
                    if (!tex) tex = makeSkinTexture(m_renderer->device(), *ch, &slots->verts[k]);
                    gpuItem(mesh, tex, slotMaterial(k));
                }
                ++m_rstats.lodHistogram[0];
                return;
            }
            auto mesh = procMesh(lod == 0 ? std::format("proc:charbind/{}", ac.character)
                                         : std::format("proc:charbind/{}#lod{}", ac.character, lod),
                                m_skinGeneration + 1, [ch, lod] {
                                    MeshData m = ch->mesh;
                                    if (lod > 0 && lod <= static_cast<int>(m.lods.size()))
                                        m.indices = m.lods[static_cast<size_t>(lod) - 1].indices;
                                    return m;
                                });
            rhi::TextureHandle& tex = m_skinTextures[ac.character];
            if (!tex) tex = makeSkinTexture(m_renderer->device(), *ch, nullptr);
            gpuItem(mesh, tex, ac.material);
            ++m_rstats.lodHistogram[std::min(lod, 4)];
            return;
        }
        if (sk.key.empty() || !sk.mesh) {
            // 1.3.3: never during Play. This is a whole edit-mode update(playing = false): it reset EVERY Animator's pose,
            // model-space bones (sockets) and CPU mesh to the Idle clip's first frame for the rest of the frame, so items
            // drawn after it showed the bind / idle pose and GPU-skinned Animators fell back to that CPU mesh (shadow pass
            // included). In Play the next animation update skins it (one frame undrawn).
            if (m_playing) return;
            m_anim.update(m_scene, 0, false, {}, nullptr);  // first frame: produce the bind/idle pose
            sk = m_anim.skinned(e);
            if (sk.key.empty() || !sk.mesh) return;
        }
        const MeshData* md = sk.mesh;
        if (slots && md->vertices.size() == loaded->mesh.vertices.size()) {  // CPU-skinned slots (crowds included)
            for (size_t k = 0; k < slots->verts.size(); ++k) {
                if (slots->indices[k].empty()) continue;
                auto mesh = procMesh(std::format("proc:{}#{}", sk.key, k), sk.version, [&] { return slotMesh(*md, slots->verts[k], slots->indices[k]); });
                if (!mesh) continue;
                DrawItem it{mesh, worlds[e], ac.color, {}, sel};
                common(it, slotMaterial(k));
                items.push_back(it);
            }
            return;
        }
        auto mesh = procMesh("proc:" + sk.key, sk.version, [md] { return *md; });
        if (!mesh) return;
        DrawItem it{mesh, worlds[e], ac.color, {}, sel};
        common(it, ac.material);
        items.push_back(it);
    });
}
// ---------------------------------------------------------------- HLOD ---
void GameWorld::buildHlods() {
    m_hlods.clear();
    const WorldPartitionComponent* wp = world::findPartitionSettings(m_scene);
    if (!wp) return;
    if (m_cookedWorldIndex.is_object()) {  // cooked: proxies were built by the packager
        for (auto& c : m_cookedWorldIndex.value("chunks", Json::array())) {
            if (!c.value("hlod", false)) continue;
            world::ChunkCoord cc{c.value("x", 0), c.value("z", 0)};
            auto bytes = m_vfs.read(m_cookedWorldDir + "/" + cc.name() + ".hlod");
            if (!bytes) continue;
            auto md = readCookedMesh(*bytes);
            if (!md || md->empty()) continue;
            Hlod h;
            h.mesh = std::move(*md);
            auto col = c.value("hlodColor", Json::array({0.6, 0.6, 0.6}));
            h.color = {col[0].get<float>(), col[1].get<float>(), col[2].get<float>()};
            Vec3 mn, mx;
            meshBounds(h.mesh, mn, mx);
            h.centre = (mn + mx) * 0.5f;
            h.radius = length(mx - h.centre);
            m_hlods[cc] = std::move(h);
        }
        return;
    }
    world::Partition part = world::buildPartition(m_scene, world::PartitionSettings::from(*wp));
    auto worlds = m_scene.computeWorldMatrices();
    for (auto& [cc, ch] : part.chunks) {
        std::vector<ProxyPart> parts;
        Vec3 colour{0, 0, 0};
        int n = 0;
        for (EntityId r : ch.roots) {
            std::vector<EntityId> all = m_scene.descendants(r);
            all.push_back(r);
            for (EntityId e : all)
                if (auto* mr = m_scene.get<MeshRendererComponent>(e); mr && mr->visible)
                    if (MeshData* md = lodChain(mr->mesh)) {
                        parts.push_back({md, worlds[e]});
                        const Material* mat = material(mr->material);
                        colour += mat ? mat->baseColor : mr->color;
                        ++n;
                    }
        }
        if (parts.empty()) continue;
        Hlod h;
        h.mesh = buildProxyMesh(parts, 400);
        if (h.mesh.empty()) continue;
        h.color = colour / static_cast<float>(n);
        Vec3 mn, mx;
        meshBounds(h.mesh, mn, mx);
        h.centre = (mn + mx) * 0.5f;
        h.radius = length(mx - h.centre);
        m_hlods[cc] = std::move(h);
    }
    if (!m_hlods.empty()) FLEDGE_LOG_INFO("world", "HLOD: {} chunk proxies", m_hlods.size());
}

void GameWorld::addHlodItems(std::vector<DrawItem>& items, Vec3 viewPos) {
    if (!m_streaming || m_hlods.empty()) return;
    const auto& s = m_streaming->settings();
    double radius = 0;
    if (auto* wp = world::findPartitionSettings(m_scene)) radius = wp->hlodRadius;
    if (radius <= 0) radius = s.unloadRadius * 3;
    world::DVec3 o = m_streaming->origin();
    Vec3 originOffset = o.toVec3();
    world::DVec3 vw = m_streaming->toWorld(viewPos);
    for (auto& [cc, h] : m_hlods) {
        if (m_streaming->state(cc) == world::ChunkState::Loaded) continue;
        double d = world::distanceToChunk(cc, s.cellSize, vw.x, vw.z);
        if (d > radius) continue;
        auto mesh = procMesh(std::format("proc:hlod/{}", cc.name()), 1, [&] { return h.mesh; });
        if (!mesh) continue;
        DrawItem it{mesh, Mat4::translation(-originOffset), h.color};  // proxies are in absolute world space
        it.roughness = 0.9f;
        it.boundsCenter = h.centre - originOffset;
        it.boundsRadius = h.radius;
        items.push_back(it);
        ++m_rstats.hlodDrawn;
    }
}

} // namespace fledge
