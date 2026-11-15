#include "fledge/world/culling.h"

#include <algorithm>
#include <cmath>

namespace fledge::world {

Frustum Frustum::fromViewProj(const Mat4& m) {
    Frustum f;
    auto row = [&](int r) { return Vec4{m.at(r, 0), m.at(r, 1), m.at(r, 2), m.at(r, 3)}; };
    Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    auto add = [](Vec4 a, Vec4 b) { return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; };
    auto sub = [](Vec4 a, Vec4 b) { return Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; };
    f.planes = {add(r3, r0), sub(r3, r0), add(r3, r1), sub(r3, r1), add(r3, r2), sub(r3, r2)};
    for (auto& p : f.planes) {
        float l = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (l > 1e-12f) p = p * (1.0f / l);
    }
    return f;
}

bool Frustum::sphere(Vec3 c, float r) const {
    for (auto& p : planes)
        if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r) return false;
    return true;
}

bool Frustum::box(Vec3 mn, Vec3 mx) const {
    for (auto& p : planes) {
        Vec3 v{p.x >= 0 ? mx.x : mn.x, p.y >= 0 ? mx.y : mn.y, p.z >= 0 ? mx.z : mn.z};  // most positive corner
        if (p.x * v.x + p.y * v.y + p.z * v.z + p.w < 0) return false;
    }
    return true;
}

namespace {
uint64_t key(int x, int y, int z) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x) & 0x1FFFFF) << 42) | (static_cast<uint64_t>(static_cast<uint32_t>(y) & 0x1FFFFF) << 21) |
           (static_cast<uint64_t>(static_cast<uint32_t>(z) & 0x1FFFFF));
}
} // namespace

void SpatialGrid::insert(uint32_t item, Vec3 c, float r) {
    int x = static_cast<int>(std::floor(c.x / m_cell)), y = static_cast<int>(std::floor(c.y / m_cell)), z = static_cast<int>(std::floor(c.z / m_cell));
    Cell& cell = m_cells[key(x, y, z)];
    cell.x = x;
    cell.y = y;
    cell.z = z;
    cell.items.push_back(item);
    cell.maxRadius = std::max(cell.maxRadius, r);
}

void SpatialGrid::query(const Frustum& f, const std::vector<Vec3>& centres, const std::vector<float>& radii, std::vector<uint32_t>& out) const {
    for (auto& [k, cell] : m_cells) {
        Vec3 mn{cell.x * m_cell - cell.maxRadius, cell.y * m_cell - cell.maxRadius, cell.z * m_cell - cell.maxRadius};
        Vec3 mx{(cell.x + 1) * m_cell + cell.maxRadius, (cell.y + 1) * m_cell + cell.maxRadius, (cell.z + 1) * m_cell + cell.maxRadius};
        if (!f.box(mn, mx)) continue;
        for (uint32_t i : cell.items)
            if (f.sphere(centres[i], radii[i])) out.push_back(i);
    }
}

// ============================================================================== occlusion ===
OcclusionBuffer::OcclusionBuffer(int w, int h) : m_w(std::max(8, w)), m_h(std::max(8, h)) { m_depth.assign(static_cast<size_t>(m_w) * m_h, 1.0f); }

void OcclusionBuffer::begin(const Mat4& vp) {
    m_vp = vp;
    std::fill(m_depth.begin(), m_depth.end(), 1.0f);
    m_mips.clear();
    m_sizes.clear();
    m_tris = 0;
}

void OcclusionBuffer::addOccluder(const MeshData& mesh, const Mat4& model) {
    Mat4 mvp = m_vp * model;
    m_clip.resize(mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i) m_clip[i] = mvp * Vec4(mesh.vertices[i].position, 1.0f);
    rasterIndexed(mesh.indices);
}

void OcclusionBuffer::addOccluder(const std::vector<Vec3>& pos, const std::vector<uint32_t>& idx, const Mat4& model) {
    Mat4 mvp = m_vp * model;
    m_clip.resize(pos.size());
    for (size_t i = 0; i < pos.size(); ++i) m_clip[i] = mvp * Vec4(pos[i], 1.0f);
    rasterIndexed(idx);
}

void OcclusionBuffer::rasterIndexed(const std::vector<uint32_t>& idx) {
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        if (idx[t] >= m_clip.size() || idx[t + 1] >= m_clip.size() || idx[t + 2] >= m_clip.size()) continue;
        Vec4 a = m_clip[idx[t]], b = m_clip[idx[t + 1]], c = m_clip[idx[t + 2]];
        if (a.w < 0.05f || b.w < 0.05f || c.w < 0.05f) continue;  // near-plane crossing: skip (conservative)
        auto toScreen = [&](Vec4 p) {
            return Vec3{(p.x / p.w * 0.5f + 0.5f) * m_w, (0.5f - p.y / p.w * 0.5f) * m_h, std::clamp(p.z / p.w * 0.5f + 0.5f, 0.0f, 1.0f)};
        };
        raster(toScreen(a), toScreen(b), toScreen(c));
        ++m_tris;
    }
}

void OcclusionBuffer::raster(Vec3 a, Vec3 b, Vec3 c) {
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-6f) return;
    int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x})))), x1 = std::min(m_w - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y})))), y1 = std::min(m_h - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    // depth written = the triangle's FARTHEST depth: a pixel only counts as covered by depth <= that, so the
    // buffer never claims an occluder is closer than it really is (conservative without interpolation error)
    float zmax = std::max({a.z, b.z, c.z});
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
            float w1 = (c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x);
            float w2 = (a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x);
            bool inside = area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (!inside) continue;
            float& d = m_depth[static_cast<size_t>(y) * m_w + x];
            d = std::min(d, zmax);
        }
}

void OcclusionBuffer::finish() {
    m_mips.clear();
    m_sizes.clear();
    std::vector<float> cur = m_depth;
    int w = m_w, h = m_h;
    m_mips.push_back(cur);
    m_sizes.push_back({w, h});
    while (w > 1 || h > 1) {
        int nw = std::max(1, (w + 1) / 2), nh = std::max(1, (h + 1) / 2);
        std::vector<float> next(static_cast<size_t>(nw) * nh, 0.0f);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                float m = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        int sx = std::min(w - 1, x * 2 + dx), sy = std::min(h - 1, y * 2 + dy);
                        m = std::max(m, cur[static_cast<size_t>(sy) * w + sx]);
                    }
                next[static_cast<size_t>(y) * nw + x] = m;
            }
        cur = std::move(next);
        w = nw;
        h = nh;
        m_mips.push_back(cur);
        m_sizes.push_back({w, h});
    }
}

bool OcclusionBuffer::visible(Vec3 mn, Vec3 mx) const {
    if (m_mips.empty()) return true;
    float sx0 = 1e30f, sy0 = 1e30f, sx1 = -1e30f, sy1 = -1e30f, zmin = 1e30f;
    for (int i = 0; i < 8; ++i) {
        Vec4 p = m_vp * Vec4{(i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z, 1.0f};
        if (p.w < 0.05f) return true;  // crosses the near plane / behind the camera: assume visible
        float x = (p.x / p.w * 0.5f + 0.5f) * m_w, y = (0.5f - p.y / p.w * 0.5f) * m_h, z = p.z / p.w * 0.5f + 0.5f;
        sx0 = std::min(sx0, x);
        sx1 = std::max(sx1, x);
        sy0 = std::min(sy0, y);
        sy1 = std::max(sy1, y);
        zmin = std::min(zmin, z);
    }
    if (sx1 < 0 || sy1 < 0 || sx0 >= m_w || sy0 >= m_h) return false;  // off screen (the frustum test normally catches this)
    // one pixel of margin: occluder coverage is sampled at pixel centres
    int x0 = std::max(0, static_cast<int>(std::floor(sx0)) - 1), x1 = std::min(m_w - 1, static_cast<int>(std::floor(sx1)) + 1);
    int y0 = std::max(0, static_cast<int>(std::floor(sy0)) - 1), y1 = std::min(m_h - 1, static_cast<int>(std::floor(sy1)) + 1);
    // pick the mip where the rectangle spans at most ~4 texels per axis
    int level = 0;
    int span = std::max(x1 - x0, y1 - y0);
    while (span > 4 && level + 1 < static_cast<int>(m_mips.size())) {
        span /= 2;
        ++level;
    }
    auto [w, h] = m_sizes[static_cast<size_t>(level)];
    const auto& mip = m_mips[static_cast<size_t>(level)];
    int lx0 = x0 >> level, lx1 = std::min(w - 1, x1 >> level), ly0 = y0 >> level, ly1 = std::min(h - 1, y1 >> level);
    for (int y = ly0; y <= ly1; ++y)
        for (int x = lx0; x <= lx1; ++x)
            if (mip[static_cast<size_t>(y) * w + x] >= zmin) return true;  // some occluder pixel is not in front
    return false;
}

} // namespace fledge::world
