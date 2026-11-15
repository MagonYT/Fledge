#include "fledge/core/mesh_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>

namespace fledge {

namespace {
bool usableNormal(Vec3 n) {
    float l2 = dot(n, n);
    return std::isfinite(l2) && l2 >= 1e-12f;
}
} // namespace

std::vector<uint32_t> zeroNormalVertices(const MeshData& m) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < m.vertices.size(); ++i)
        if (!usableNormal(m.vertices[i].normal)) out.push_back(static_cast<uint32_t>(i));
    return out;
}

int fixZeroNormals(MeshData& m) {
    std::vector<uint32_t> zero = zeroNormalVertices(m);
    if (zero.empty()) return 0;
    const size_t n = m.vertices.size();
    std::vector<Vec3> acc(n, Vec3{0, 0, 0});
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        uint32_t a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
        if (a >= n || b >= n || c >= n) continue;  // hostile index buffers: skip, never read outside
        Vec3 f = cross(m.vertices[b].position - m.vertices[a].position, m.vertices[c].position - m.vertices[a].position);
        if (!std::isfinite(f.x) || !std::isfinite(f.y) || !std::isfinite(f.z)) continue;
        for (uint32_t v : {a, b, c}) acc[v] += f;  // |f| = 2 x area: area-weighted
    }
    // second chance: vertices whose own triangles are all degenerate take the sum over every vertex at the same position
    Vec3 mn, mx;
    meshBounds(m, mn, mx);
    const float cell = std::max(1e-6f * std::max({mx.x - mn.x, mx.y - mn.y, mx.z - mn.z}), 1e-9f);
    auto key = [&](Vec3 p) {
        auto q = [&](float x, float o) { return static_cast<int64_t>(std::llround((x - o) / cell)); };
        return std::array<int64_t, 3>{q(p.x, mn.x), q(p.y, mn.y), q(p.z, mn.z)};
    };
    std::map<std::array<int64_t, 3>, Vec3> byPos;
    bool needPos = false;
    for (uint32_t v : zero) needPos |= !usableNormal(acc[v]);
    if (needPos)
        for (size_t v = 0; v < n; ++v)
            if (std::isfinite(m.vertices[v].position.x) && std::isfinite(m.vertices[v].position.y) && std::isfinite(m.vertices[v].position.z))
                byPos[key(m.vertices[v].position)] += acc[v];
    int fixed = 0;
    for (uint32_t v : zero) {
        Vec3 s = acc[v];
        if (!usableNormal(s) && needPos) {
            auto it = byPos.find(key(m.vertices[v].position));
            if (it != byPos.end()) s = it->second;
        }
        if (usableNormal(s)) {
            m.vertices[v].normal = normalize(s);
            ++fixed;
        } else {
            m.vertices[v].normal = Vec3{0, 0, 0};  // NaN / Inf never stays
        }
    }
    if (!m.tangents.empty() && fixed > 0) generateTangents(m);
    return fixed;
}

std::string zeroNormalWarning(const std::string& what, size_t vertexCount, const std::vector<uint32_t>& zero, int fixed) {
    if (zero.empty()) return {};
    std::string ex;
    for (size_t i = 0; i < zero.size() && i < 8; ++i) ex += (i ? ", " : "") + std::to_string(zero[i]);
    if (zero.size() > 8) ex += ", ...";
    if (fixed >= 0)
        return std::format("{}: {} of {} vertices had zero-length (or NaN) normals (e.g. vertex {}); fixZeroNormals regenerated {} of them from "
                           "the geometry",
                           what, zero.size(), vertexCount, ex, fixed);
    return std::format("{}: {} of {} vertices have zero-length (or NaN) normals (e.g. vertex {}). Fledge 1.3.3 shades them with the face "
                       "normal; older versions and other engines can show black pixels and a bloom halo there. {} (docs/ANIMATION.md "
                       "\"Zero-length normals\")",
                       what, zero.size(), vertexCount, ex,
                       fixed == -2 ? "Re-export with valid normals, or without normals (they are then generated)"
                                   : "Re-export with normals, or re-import with fixZeroNormals (regenerates them from the geometry)");
}

void generateTangents(MeshData& m) {
    std::vector<Vec3> tan(m.vertices.size()), bit(m.vertices.size());
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        uint32_t a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
        const Vertex &va = m.vertices[a], &vb = m.vertices[b], &vc = m.vertices[c];
        Vec3 e1 = vb.position - va.position, e2 = vc.position - va.position;
        Vec2 d1 = vb.uv - va.uv, d2 = vc.uv - va.uv;
        float det = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(det) < 1e-12f) continue;
        float rr = 1.0f / det;
        Vec3 t = (e1 * d2.y - e2 * d1.y) * rr;
        Vec3 bt = (e2 * d1.x - e1 * d2.x) * rr;
        for (uint32_t v : {a, b, c}) {
            tan[v] += t;
            bit[v] += bt;
        }
    }
    m.tangents.resize(m.vertices.size());
    for (size_t i = 0; i < m.vertices.size(); ++i) {
        Vec3 n = m.vertices[i].normal;
        Vec3 t = normalize(tan[i] - n * dot(n, tan[i]));
        if (length(t) < 0.5f) t = normalize(cross(std::fabs(n.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}, n));
        float w = dot(cross(n, t), bit[i]) < 0 ? -1.0f : 1.0f;
        m.tangents[i] = {t, w};
    }
}


} // namespace fledge

namespace fledge::primitives {

MeshData cube(float size) {
    MeshData m;
    float h = size * 0.5f;
    struct Face { Vec3 n, u, v; };
    const Face faces[6] = {
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
    };
    for (const auto& f : faces) {
        auto base = static_cast<uint32_t>(m.vertices.size());
        const float c[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (auto& cc : c) {
            Vec3 p = (f.n + f.u * cc[0] + f.v * cc[1]) * h;
            m.vertices.push_back({p, f.n, {(cc[0] + 1) * 0.5f, (cc[1] + 1) * 0.5f}});
        }
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return m;
}

MeshData plane(float size, float uvRepeat) {
    MeshData m;
    float h = size * 0.5f;
    m.vertices = {{{-h, 0, h}, {0, 1, 0}, {0, 0}},
                  {{h, 0, h}, {0, 1, 0}, {uvRepeat, 0}},
                  {{h, 0, -h}, {0, 1, 0}, {uvRepeat, uvRepeat}},
                  {{-h, 0, -h}, {0, 1, 0}, {0, uvRepeat}}};
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}

MeshData sphere(float radius, int segments, int rings) {
    MeshData m;
    for (int r = 0; r <= rings; ++r) {
        float v = static_cast<float>(r) / static_cast<float>(rings);
        float phi = v * kPi;
        for (int s = 0; s <= segments; ++s) {
            float u = static_cast<float>(s) / static_cast<float>(segments);
            float theta = u * 2.0f * kPi;
            Vec3 n{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            m.vertices.push_back({n * radius, n, {u, 1.0f - v}});
        }
    }
    auto stride = static_cast<uint32_t>(segments + 1);
    for (uint32_t r = 0; r < static_cast<uint32_t>(rings); ++r)
        for (uint32_t s = 0; s < static_cast<uint32_t>(segments); ++s) {
            uint32_t a = r * stride + s, b = a + stride;
            m.indices.insert(m.indices.end(), {a, a + 1, b, b, a + 1, b + 1});
        }
    return m;
}

MeshData byName(const std::string& name) {
    if (name == "builtin:cube") return cube();
    if (name == "builtin:plane") return plane();
    if (name == "builtin:sphere") return sphere();
    return {};
}

} // namespace fledge::primitives
