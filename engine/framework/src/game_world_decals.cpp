// GameWorld: 1.3.2 projected decals (Fledge Advanced; docs/GRAPHICS.md "Decals").
//
// A decal is a box (width x height x depth) projecting a texture along its -Z. When it is placed (Decal component moved,
// or spawnDecal for a ballistic impact) every opaque MeshRenderer whose bounds meet the box is clipped against it on the
// CPU (Sutherland-Hodgman, 6 planes), and the pieces are uploaded once as meshes in the receivers' own space (a decal on a
// crate rides with the crate). tangent.w carries the per-vertex angle fade; the renderer's decal shader multiplies the
// coverage by it and biases depth towards the camera. Drawing is a walk over the live decals: no allocation per frame.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>

#include "fledge/core/log.h"
#include "fledge/framework/game_world.h"

namespace fledge {

namespace {
struct ClipVert {
    Vec3 p;  // decal space (metres, box centred on the origin, projecting along -Z)
    Vec3 n;  // receiver-local normal (interpolated)
    Vec3 nd; // decal-space normal (fade)
};
constexpr int kMaxClip = 16;

ClipVert lerpV(const ClipVert& a, const ClipVert& b, float t) {
    return {a.p + (b.p - a.p) * t, a.n + (b.n - a.n) * t, a.nd + (b.nd - a.nd) * t};
}
// Keeps the part of the polygon where sign * p[axis] <= limit. Fixed-size buffers.
int clipPlane(const std::array<ClipVert, kMaxClip>& in, int n, std::array<ClipVert, kMaxClip>& out, int axis, float sign, float limit) {
    int m = 0;
    for (int i = 0; i < n && m < kMaxClip - 1; ++i) {
        const ClipVert& a = in[static_cast<size_t>(i)];
        const ClipVert& b = in[static_cast<size_t>((i + 1) % n)];
        float da = sign * a.p[axis] - limit, db = sign * b.p[axis] - limit;
        if (da <= 0) out[static_cast<size_t>(m++)] = a;
        if ((da <= 0) != (db <= 0) && m < kMaxClip) out[static_cast<size_t>(m++)] = lerpV(a, b, da / (da - db));
    }
    return m;
}
float smooth01(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / std::max(1e-6f, e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
uint64_t hashBytes(uint64_t h, const void* p, size_t n) {
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
    return h;
}
// The decal box of a spawn: centred on the surface point, +Z along the surface normal (it projects along -Z).
Mat4 decalWorldFromSpawn(const GameWorld::DecalSpawn& s) {
    Vec3 z = normalize(length(s.normal) > 1e-6f ? s.normal : Vec3{0, 1, 0});
    Vec3 up = std::fabs(z.y) > 0.95f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    Vec3 x = normalize(cross(up, z));
    Vec3 y = cross(z, x);
    float c = std::cos(s.rotation), sn = std::sin(s.rotation);
    Vec3 xr = x * c + y * sn, yr = y * c - x * sn;
    Mat4 m;
    m.c[0] = Vec4(xr, 0);
    m.c[1] = Vec4(yr, 0);
    m.c[2] = Vec4(z, 0);
    m.c[3] = Vec4(s.position, 1);
    return m;
}
} // namespace

bool GameWorld::decalsAvailable() const { return advancedRenderFeatures() && m_renderer && m_renderer->decalsSupported(); }

void GameWorld::releaseDecal(DecalInst& d) {
    if (m_renderer)
        for (auto& p : d.pieces)
            if (p.mesh) m_renderer->device().destroyMesh(p.mesh);
    d.pieces.clear();
    d.active = false;
}

void GameWorld::clearDecals() {
    for (auto& d : m_decalPool) releaseDecal(d);
    for (auto& [e, d] : m_decalComponents) releaseDecal(d);
    m_decalComponents.clear();
    m_decalNext = 0;
}

rhi::TextureHandle GameWorld::decalTexture(const std::string& ref) {
    const int kind = ref == "builtin:decal_bullet" ? 0 : ref == "builtin:decal_blood" ? 1 : -1;
    if (kind < 0) return textureFor(ref);
    if (!m_decalTex[kind] && m_renderer) {  // procedural 64x64 RGBA (no assets): a dark hole with a soft rim / a splat
        const int N = 64;
        std::vector<uint8_t> px(static_cast<size_t>(N) * N * 4);
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                float u = (static_cast<float>(x) + 0.5f) / N * 2 - 1, v = (static_cast<float>(y) + 0.5f) / N * 2 - 1;
                float r = std::sqrt(u * u + v * v);
                float a, l;
                if (kind == 0) {
                    a = r < 0.35f ? 1.0f : r < 1.0f ? std::pow(1 - (r - 0.35f) / 0.65f, 2.0f) * 0.85f : 0.0f;  // core + scorch
                    l = r < 0.3f ? 0.02f : 0.12f;
                } else {
                    float wob = 0.75f + 0.2f * std::sin(std::atan2(v, u) * 7) * std::cos(std::atan2(v, u) * 3);
                    a = r < wob ? std::min(1.0f, (wob - r) * 6) : 0.0f;
                    l = 0.25f;
                }
                uint8_t* p = &px[(static_cast<size_t>(y) * N + x) * 4];
                p[0] = static_cast<uint8_t>(std::lround(std::clamp(l * (kind == 1 ? 1.0f : 1.0f), 0.0f, 1.0f) * 255));
                p[1] = static_cast<uint8_t>(std::lround(std::clamp(l * (kind == 1 ? 0.05f : 1.0f), 0.0f, 1.0f) * 255));
                p[2] = static_cast<uint8_t>(std::lround(std::clamp(l * (kind == 1 ? 0.05f : 1.0f), 0.0f, 1.0f) * 255));
                p[3] = static_cast<uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255));
            }
        m_decalTex[kind] = m_renderer->device().createTexture2D(N, N, px.data());
    }
    return m_decalTex[kind];
}

void GameWorld::clipDecalMesh(const MeshData& receiver, const Mat4& toDecal, Vec3 h, float angleFade, MeshData& out) {
    out.vertices.clear();
    out.indices.clear();
    out.tangents.clear();
    out.lods.clear();
    if (!(h.x > 0 && h.y > 0 && h.z > 0) || !std::isfinite(h.x + h.y + h.z)) return;
    const float fadeEnd = radians(std::clamp(std::isfinite(angleFade) ? angleFade : 70.0f, 1.0f, 90.0f)), fadeStart = std::max(0.0f, fadeEnd - radians(20.0f));
    const float cosEnd = std::cos(fadeEnd), cosStart = std::cos(fadeStart);
    const Mat4 toLocal = toDecal.inverse();
    // decal-space normals: the inverse transpose of toDecal (3x3)
    const Mat4 nrm = toLocal.transposed();
    const MeshData* md = &receiver;
    std::array<ClipVert, kMaxClip> a{}, b{};
    for (size_t t = 0; t + 2 < md->indices.size(); t += 3) {
        const uint32_t i0 = md->indices[t], i1 = md->indices[t + 1], i2 = md->indices[t + 2];
        if (i0 >= md->vertices.size() || i1 >= md->vertices.size() || i2 >= md->vertices.size()) continue;
        const Vertex* vs[3] = {&md->vertices[i0], &md->vertices[i1], &md->vertices[i2]};
        for (int k = 0; k < 3; ++k) {
            a[static_cast<size_t>(k)].p = toDecal.transformPoint(vs[k]->position);
            a[static_cast<size_t>(k)].n = vs[k]->normal;
            a[static_cast<size_t>(k)].nd = normalize((nrm * Vec4(vs[k]->normal, 0)).xyz());
        }
        // quick reject: all three outside one plane
        bool outside = false;
        for (int axis = 0; axis < 3 && !outside; ++axis)
            for (float sgn : {1.0f, -1.0f}) {
                float lim = h[axis];
                if (sgn * a[0].p[axis] > lim && sgn * a[1].p[axis] > lim && sgn * a[2].p[axis] > lim) outside = true;
            }
        if (outside) continue;
        // faces turned away from the projection (or steeper than angleFade) receive nothing
        Vec3 fn = cross(a[1].p - a[0].p, a[2].p - a[0].p);
        if (!(dot(fn, fn) > 1e-20f) || normalize(fn).z < cosEnd) continue;
        int n = 3;
        n = clipPlane(a, n, b, 0, 1, h.x);
        n = clipPlane(b, n, a, 0, -1, h.x);
        n = clipPlane(a, n, b, 1, 1, h.y);
        n = clipPlane(b, n, a, 1, -1, h.y);
        n = clipPlane(a, n, b, 2, 1, h.z);
        n = clipPlane(b, n, a, 2, -1, h.z);
        if (n < 3) continue;
        const uint32_t base = static_cast<uint32_t>(out.vertices.size());
        const Vec3 fnd = normalize(fn);
        for (int k = 0; k < n; ++k) {
            const ClipVert& v = a[static_cast<size_t>(k)];
            Vec3 nd = length(v.nd) > 1e-6f ? normalize(v.nd) : fnd;
            Vec3 pd = v.p + fnd * 0.0015f;  // 1.5 mm off the surface (plus the shader's depth bias)
            Vertex vx;
            vx.position = toLocal.transformPoint(pd);
            vx.normal = length(v.n) > 1e-6f ? normalize(v.n) : Vec3{0, 0, 1};
            vx.uv = {v.p.x / (2 * h.x) + 0.5f, 0.5f - v.p.y / (2 * h.y)};
            out.vertices.push_back(vx);
            out.tangents.push_back(Vec4(0, 0, 0, smooth01(cosEnd, cosStart, nd.z)));
        }
        for (int k = 1; k + 1 < n; ++k) out.indices.insert(out.indices.end(), {base, base + static_cast<uint32_t>(k), base + static_cast<uint32_t>(k + 1)});
    }
}

bool GameWorld::buildDecal(DecalInst& d, const Mat4& decalWorld, Vec3 size, float angleFade, uint32_t layerMask, EntityId only,
                           const std::unordered_map<EntityId, Mat4>& worlds) {
    for (auto& p : d.pieces)
        if (p.mesh && m_renderer) m_renderer->device().destroyMesh(p.mesh);
    d.pieces.clear();
    if (!m_renderer) return false;
    const Vec3 h{std::max(0.001f, std::fabs(size.x) * 0.5f), std::max(0.001f, std::fabs(size.y) * 0.5f), std::max(0.001f, std::fabs(size.z) * 0.5f)};
    const Mat4 decalInv = decalWorld.inverse();
    // world bounds of the box (receiver pre-test)
    Vec3 bmn{1e30f}, bmx{-1e30f};
    for (int i = 0; i < 8; ++i) {
        Vec3 c = decalWorld.transformPoint({(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z});
        for (int k = 0; k < 3; ++k) {
            bmn[k] = std::min(bmn[k], c[k]);
            bmx[k] = std::max(bmx[k], c[k]);
        }
    }
    m_scene.each<MeshRendererComponent, TransformComponent>([&](EntityId e, MeshRendererComponent& mr, TransformComponent&) {
        if (!mr.visible || mr.viewmodel || (only && e != only) || m_scene.get<DecalComponent>(e)) return;
        auto* col = m_scene.get<ColliderComponent>(e);
        int layer = col && col->layer > 0 && col->layer < 32 ? col->layer : 0;
        if (!(layerMask & (1u << layer))) return;
        if (col && col->isTrigger) return;
        if (const Material* mat = material(mr.material); mat && mat->alphaMode == AlphaMode::Blend) return;  // opaque receivers only
        const MeshData* md = cpuMesh(mr.mesh);
        if (!md || md->empty()) return;
        auto wit = worlds.find(e);
        const Mat4 rw = wit != worlds.end() ? wit->second : m_scene.worldMatrix(e);
        Vec3 lmn, lmx;
        meshBounds(*md, lmn, lmx);
        Vec3 wmn{1e30f}, wmx{-1e30f};
        for (int i = 0; i < 8; ++i) {
            Vec3 p = rw.transformPoint({(i & 1) ? lmx.x : lmn.x, (i & 2) ? lmx.y : lmn.y, (i & 4) ? lmx.z : lmn.z});
            for (int k = 0; k < 3; ++k) {
                wmn[k] = std::min(wmn[k], p[k]);
                wmx[k] = std::max(wmx[k], p[k]);
            }
        }
        for (int k = 0; k < 3; ++k)
            if (wmx[k] < bmn[k] || wmn[k] > bmx[k]) return;
        MeshData& out = m_decalScratch;
        clipDecalMesh(*md, decalInv * rw, h, angleFade, out);
        if (out.indices.empty()) return;
        DecalPiece piece;
        piece.receiver = e;
        piece.triangles = static_cast<int>(out.indices.size() / 3);
        piece.mesh = m_renderer->device().createMesh(out);
        if (piece.mesh) d.pieces.push_back(piece);
    });
    return !d.pieces.empty();
}

int GameWorld::spawnDecal(const DecalSpawn& s) {
    if (!decalsAvailable()) return -1;
    const size_t cap = static_cast<size_t>(std::clamp(maxDecals, 1, 4096));
    if (m_decalPool.size() != cap) {
        for (size_t i = cap; i < m_decalPool.size(); ++i) releaseDecal(m_decalPool[i]);
        m_decalPool.resize(cap);
        if (m_decalNext >= cap) m_decalNext = 0;
    }
    // built first: a spawn that finds nothing to project on leaves the pool as it was
    DecalInst built;
    auto worlds = m_scene.computeWorldMatrices();
    if (!buildDecal(built, decalWorldFromSpawn(s), s.size, s.angleFade, s.layerMask, s.only, worlds)) return -1;
    // the oldest slot (ring): recycled when the pool is full
    DecalInst& d = m_decalPool[m_decalNext];
    if (d.active) ++m_decalStats.recycled;
    releaseDecal(d);
    d.pieces = std::move(built.pieces);
    d.active = true;
    d.seq = ++m_decalSeq;
    d.entity = kNullEntity;
    d.material = s.material;
    d.texture = s.texture;
    d.color = s.color;
    d.opacity = std::clamp(s.opacity, 0.0f, 1.0f);
    d.age = 0;
    d.lifetime = std::max(0.0f, s.lifetime);
    d.fadeOut = std::max(0.0f, s.fadeOut);
    d.sortOrder = s.sortOrder;
    const int slot = static_cast<int>(m_decalNext);
    m_decalNext = (m_decalNext + 1) % cap;
    ++m_decalStats.spawned;
    return slot;
}

void GameWorld::updateDecals(float dt) {
    for (auto& d : m_decalPool) {
        if (!d.active) continue;
        d.age += dt;
        if (d.lifetime > 0 && d.age >= d.lifetime) releaseDecal(d);  // expired: its meshes go now
    }
    m_scene.each<DecalComponent, TransformComponent>([&](EntityId, DecalComponent& dc, TransformComponent&) {
        if (dc.lifetime > 0) dc.age += dt;
    });
}

void GameWorld::addDecalItems(std::vector<DrawItem>& items, std::unordered_map<EntityId, Mat4>& worlds) {
    m_decalStats.active = m_decalStats.components = m_decalStats.pieces = m_decalStats.triangles = 0;
    if (!decalsAvailable()) {
        if (!m_decalWarned && m_scene.pool<DecalComponent>().size() > 0) {
            m_decalWarned = true;
            FLEDGE_LOG_WARN("render", "Decal components are not drawn here: {}",
                            advancedRenderFeatures() ? std::string("this renderer has no decal shader (Direct3D 9 / no renderer)")
                                                     : advancedRenderNote("Projected decals"));
        }
        return;
    }
    // Decal components: (re)built when the box, its placement or the receivers' layer mask change
    for (auto it = m_decalComponents.begin(); it != m_decalComponents.end();) {
        if (!m_scene.isValid(it->first) || !m_scene.get<DecalComponent>(it->first)) {
            releaseDecal(it->second);
            it = m_decalComponents.erase(it);
        } else {
            ++it;
        }
    }
    m_scene.each<DecalComponent, TransformComponent>([&](EntityId e, DecalComponent& dc, TransformComponent&) {
        DecalInst& d = m_decalComponents[e];
        const Mat4& w = worlds.count(e) ? worlds[e] : (worlds[e] = m_scene.worldMatrix(e));
        Transform t = decompose(w);  // the entity's scale is not part of the box (size is)
        Mat4 place = Transform{t.position, t.rotation, {1, 1, 1}}.matrix();
        uint64_t h = 1469598103934665603ull;
        h = hashBytes(h, &place, sizeof(place));
        h = hashBytes(h, &dc.size, sizeof(dc.size));
        h = hashBytes(h, &dc.angleFade, sizeof(dc.angleFade));
        h = hashBytes(h, &dc.layerMask, sizeof(dc.layerMask));
        if (h != d.hash) {
            d.hash = h;
            d.entity = e;
            d.seq = ++m_decalSeq;
            buildDecal(d, place, dc.size, dc.angleFade, static_cast<uint32_t>(dc.layerMask), kNullEntity, worlds);
            ++m_decalStats.rebuilt;
        }
        d.active = dc.visible;
        d.material = dc.material;
        d.texture = dc.texture;
        d.color = dc.color;
        d.opacity = std::clamp(dc.opacity, 0.0f, 1.0f);
        d.age = dc.age;
        d.lifetime = dc.lifetime;
        d.fadeOut = dc.fadeOut;
        d.sortOrder = dc.sortOrder;
    });
    auto emit = [&](DecalInst& d, bool component) {
        if (!d.active || d.pieces.empty()) return;
        float fade = 1;
        if (d.lifetime > 0) {
            if (d.age >= d.lifetime) return;
            if (d.fadeOut > 0) fade = std::clamp((d.lifetime - d.age) / d.fadeOut, 0.0f, 1.0f);
        }
        if (d.opacity * fade <= 0.002f) return;
        component ? ++m_decalStats.components : ++m_decalStats.active;
        for (const DecalPiece& p : d.pieces) {
            if (!m_scene.isValid(p.receiver)) continue;
            auto wit = worlds.find(p.receiver);
            DrawItem it{p.mesh, wit != worlds.end() ? wit->second : m_scene.worldMatrix(p.receiver), d.color, {}};
            if (!d.material.empty()) applyMaterial(it, d.material);
            if (d.material.empty() || !it.texture) it.texture = decalTexture(d.texture);
            if (!d.material.empty()) it.color = it.color * d.color;
            it.alphaMode = 2;
            it.opacity = (d.material.empty() ? 1.0f : it.opacity) * d.opacity * fade;
            it.castShadows = false;
            it.instanceable = false;
            it.decal = true;
            it.decalOrder = (static_cast<int64_t>(d.sortOrder) << 40) + static_cast<int64_t>(d.seq & 0xFFFFFFFFFFull);
            it.objectId = p.receiver;
            items.push_back(it);
            ++m_decalStats.pieces;
            m_decalStats.triangles += p.triangles;
        }
    };
    for (auto& [e, d] : m_decalComponents) emit(d, true);
    for (auto& d : m_decalPool) emit(d, false);
}

} // namespace fledge
