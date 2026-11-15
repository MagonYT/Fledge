#include "fledge/world/terrain.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

#include "fledge/core/utility.h"

namespace fledge::world {

Heightfield::Heightfield(int resolution, float size) : m_res(std::max(2, resolution)), m_size(std::max(1.0f, size)) {
    m_h.assign(static_cast<size_t>(m_res) * m_res, 0.0f);
    m_splat.assign(static_cast<size_t>(m_res) * m_res * 4, 0);
    for (size_t i = 0; i < m_splat.size(); i += 4) m_splat[i] = 255;  // all layer 0
}

Heightfield Heightfield::fromComponent(const TerrainComponent& t) {
    Heightfield h(t.resolution, t.size);
    size_t n = static_cast<size_t>(h.m_res) * h.m_res;
    Bytes hb = t.heights.empty() ? Bytes{} : base64Decode(t.heights);
    if (hb.size() == n * sizeof(float)) std::memcpy(h.m_h.data(), hb.data(), hb.size());
    else if (t.seed > 0) h.generateHills(t.seed, t.maxHeight);
    Bytes sb = t.splat.empty() ? Bytes{} : base64Decode(t.splat);
    if (sb.size() == n * 4) std::memcpy(h.m_splat.data(), sb.data(), sb.size());
    return h;
}

void Heightfield::toComponent(TerrainComponent& t) const {
    t.resolution = m_res;
    t.size = m_size;
    t.heights = base64Encode(std::span(reinterpret_cast<const uint8_t*>(m_h.data()), m_h.size() * sizeof(float)));
    t.splat = base64Encode(m_splat);
}

void Heightfield::generateHills(int seed, float maxHeight) {
    std::mt19937 rng(static_cast<uint32_t>(seed));
    std::uniform_real_distribution<float> u(0, 1);
    struct Wave {
        float fx, fz, phase, amp;
    };
    std::vector<Wave> waves;
    for (int i = 0; i < 6; ++i) waves.push_back({0.5f + u(rng) * 3.0f * (i + 1), 0.5f + u(rng) * 3.0f * (i + 1), u(rng) * 6.28f, 1.0f / (1 + i)});
    float total = 0;
    for (auto& w : waves) total += w.amp;
    for (int z = 0; z < m_res; ++z)
        for (int x = 0; x < m_res; ++x) {
            float fx = static_cast<float>(x) / (m_res - 1), fz = static_cast<float>(z) / (m_res - 1);
            float v = 0;
            for (auto& w : waves) v += w.amp * (0.5f + 0.5f * std::sin(fx * w.fx * 6.28f + w.phase) * std::cos(fz * w.fz * 6.28f + w.phase * 0.7f));
            at(x, z) = v / total * maxHeight;
            uint8_t* s = splatAt(x, z);  // rock on the heights, sand in the lows
            float hn = v / total;
            s[0] = static_cast<uint8_t>(255 * std::clamp(1.0f - std::fabs(hn - 0.45f) * 3, 0.0f, 1.0f));
            s[2] = static_cast<uint8_t>(255 * std::clamp((hn - 0.65f) * 4, 0.0f, 1.0f));
            s[3] = static_cast<uint8_t>(255 * std::clamp((0.25f - hn) * 4, 0.0f, 1.0f));
            s[1] = static_cast<uint8_t>(std::max(0, 255 - s[0] - s[2] - s[3]));
        }
}

bool Heightfield::contains(float x, float z) const { return std::fabs(x) <= m_size * 0.5f && std::fabs(z) <= m_size * 0.5f; }

float Heightfield::heightAt(float x, float z) const {
    float gx = std::clamp((x + m_size * 0.5f) / spacing(), 0.0f, static_cast<float>(m_res - 1));
    float gz = std::clamp((z + m_size * 0.5f) / spacing(), 0.0f, static_cast<float>(m_res - 1));
    int x0 = std::min(static_cast<int>(gx), m_res - 2), z0 = std::min(static_cast<int>(gz), m_res - 2);
    float fx = gx - x0, fz = gz - z0;
    float a = at(x0, z0) * (1 - fx) + at(x0 + 1, z0) * fx;
    float b = at(x0, z0 + 1) * (1 - fx) + at(x0 + 1, z0 + 1) * fx;
    return a * (1 - fz) + b * fz;
}

Vec3 Heightfield::normalAt(float x, float z) const {
    float d = spacing();
    float hx = heightAt(x + d, z) - heightAt(x - d, z), hz = heightAt(x, z + d) - heightAt(x, z - d);
    return normalize({-hx, 2 * d, -hz});
}

std::optional<Vec3> Heightfield::raycast(Vec3 o, Vec3 dir, float maxDistance) const {
    float len = length(dir);
    if (len < 1e-6f) return std::nullopt;
    dir = dir / len;
    float step = spacing() * 0.5f;
    float prevT = 0;
    bool prevAbove = o.y >= heightAt(o.x, o.z);
    for (float t = step; t <= maxDistance + step; t += step) {
        float tc = std::min(t, maxDistance);
        Vec3 p = o + dir * tc;
        if (!contains(p.x, p.z)) {
            prevT = tc;
            prevAbove = true;
            continue;
        }
        bool above = p.y >= heightAt(p.x, p.z);
        if (prevAbove && !above) {  // bisection between prevT and tc
            float a = prevT, b = tc;
            for (int i = 0; i < 20; ++i) {
                float m = 0.5f * (a + b);
                Vec3 q = o + dir * m;
                if (q.y >= heightAt(q.x, q.z)) a = m;
                else b = m;
            }
            Vec3 hit = o + dir * b;
            hit.y = heightAt(hit.x, hit.z);
            return hit;
        }
        prevAbove = above;
        prevT = tc;
        if (tc >= maxDistance) break;
    }
    return std::nullopt;
}

void Heightfield::brush(Vec3 c, float radius, float strength, BrushMode mode, int layer, float flattenHeight) {
    float sp = spacing();
    int x0 = std::max(0, static_cast<int>((c.x - radius + m_size * 0.5f) / sp)), x1 = std::min(m_res - 1, static_cast<int>((c.x + radius + m_size * 0.5f) / sp) + 1);
    int z0 = std::max(0, static_cast<int>((c.z - radius + m_size * 0.5f) / sp)), z1 = std::min(m_res - 1, static_cast<int>((c.z + radius + m_size * 0.5f) / sp) + 1);
    std::vector<float> src = m_h;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            float px = x * sp - m_size * 0.5f, pz = z * sp - m_size * 0.5f;
            float d = std::sqrt((px - c.x) * (px - c.x) + (pz - c.z) * (pz - c.z));
            if (d > radius) continue;
            float w = 0.5f + 0.5f * std::cos(d / radius * 3.14159265f);  // smooth falloff
            float k = std::clamp(strength * w, 0.0f, 1.0f);
            switch (mode) {
                case BrushMode::Raise: at(x, z) += strength * w; break;
                case BrushMode::Lower: at(x, z) -= strength * w; break;
                case BrushMode::Flatten: at(x, z) += (flattenHeight - at(x, z)) * k; break;
                case BrushMode::Smooth: {
                    float sum = 0;
                    int n = 0;
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dx = -1; dx <= 1; ++dx) {
                            int sx = std::clamp(x + dx, 0, m_res - 1), sz = std::clamp(z + dz, 0, m_res - 1);
                            sum += src[static_cast<size_t>(sz) * m_res + sx];
                            ++n;
                        }
                    at(x, z) += (sum / n - at(x, z)) * k;
                    break;
                }
                case BrushMode::Paint: {
                    uint8_t* s = splatAt(x, z);
                    int l = std::clamp(layer, 0, 3);
                    for (int i = 0; i < 4; ++i) {
                        float v = s[i] / 255.0f;
                        v = i == l ? v + (1 - v) * k : v * (1 - k);
                        s[i] = static_cast<uint8_t>(std::lround(v * 255));
                    }
                    break;
                }
            }
        }
}

MeshData Heightfield::tileMesh(int tx, int tz, int lod, int tileQuads) const {
    MeshData m;
    int stride = 1 << std::clamp(lod, 0, 6);
    int gx0 = tx * tileQuads, gz0 = tz * tileQuads;
    int gx1 = std::min(gx0 + tileQuads, m_res - 1), gz1 = std::min(gz0 + tileQuads, m_res - 1);
    if (gx0 >= gx1 || gz0 >= gz1) return m;
    std::vector<int> xs, zs;
    for (int g = gx0; g < gx1; g += stride) xs.push_back(g);
    xs.push_back(gx1);
    for (int g = gz0; g < gz1; g += stride) zs.push_back(g);
    zs.push_back(gz1);
    float sp = spacing(), half = m_size * 0.5f;
    auto vert = [&](int gx, int gz, float drop) {
        float x = gx * sp - half, z = gz * sp - half;
        Vertex v;
        v.position = {x, at(gx, gz) - drop, z};
        v.normal = normalAt(x, z);
        v.uv = {static_cast<float>(gx) / (m_res - 1), static_cast<float>(gz) / (m_res - 1)};
        return v;
    };
    int w = static_cast<int>(xs.size()), h = static_cast<int>(zs.size());
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) m.vertices.push_back(vert(xs[static_cast<size_t>(i)], zs[static_cast<size_t>(j)], 0));
    for (int j = 0; j + 1 < h; ++j)
        for (int i = 0; i + 1 < w; ++i) {
            uint32_t a = static_cast<uint32_t>(j * w + i), b = a + 1, c = a + static_cast<uint32_t>(w), d = c + 1;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    // skirts: a strip hanging down around the border hides cracks between tiles of different LOD
    float drop = sp * stride * 2;
    auto skirt = [&](std::vector<std::pair<int, int>> edge, bool flip) {
        for (size_t k = 0; k + 1 < edge.size(); ++k) {
            uint32_t base = static_cast<uint32_t>(m.vertices.size());
            m.vertices.push_back(vert(edge[k].first, edge[k].second, 0));
            m.vertices.push_back(vert(edge[k + 1].first, edge[k + 1].second, 0));
            m.vertices.push_back(vert(edge[k].first, edge[k].second, drop));
            m.vertices.push_back(vert(edge[k + 1].first, edge[k + 1].second, drop));
            if (flip) m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base + 1, base + 3, base + 2});
            else m.indices.insert(m.indices.end(), {base, base + 2, base + 1, base + 1, base + 2, base + 3});
        }
    };
    std::vector<std::pair<int, int>> top, bottom, left, right;
    for (int x : xs) {
        top.push_back({x, gz0});
        bottom.push_back({x, gz1});
    }
    for (int z : zs) {
        left.push_back({gx0, z});
        right.push_back({gx1, z});
    }
    skirt(top, false);
    skirt(bottom, true);
    skirt(left, true);
    skirt(right, false);
    return m;
}

std::vector<uint8_t> Heightfield::bakeAlbedo(int texRes, const Vec3 layers[4]) const {
    std::vector<uint8_t> out(static_cast<size_t>(texRes) * texRes * 4);
    auto enc = [](float lin) {
        lin = std::clamp(lin, 0.0f, 1.0f);
        float s = lin <= 0.0031308f ? lin * 12.92f : 1.055f * std::pow(lin, 1 / 2.4f) - 0.055f;
        return static_cast<uint8_t>(std::lround(s * 255));
    };
    for (int y = 0; y < texRes; ++y)
        for (int x = 0; x < texRes; ++x) {
            int sx = x * (m_res - 1) / std::max(1, texRes - 1), sz = y * (m_res - 1) / std::max(1, texRes - 1);
            const uint8_t* s = &m_splat[(static_cast<size_t>(sz) * m_res + sx) * 4];
            float sum = static_cast<float>(s[0] + s[1] + s[2] + s[3]);
            Vec3 c{0, 0, 0};
            if (sum <= 0) c = layers[0];
            else
                for (int i = 0; i < 4; ++i) c += layers[i] * (s[i] / sum);
            // cheap detail: slight variation so large areas don't look flat
            float n = 0.92f + 0.08f * static_cast<float>(((sx * 73856093) ^ (sz * 19349663)) & 255) / 255.0f;
            c = c * n;
            uint8_t* o = &out[(static_cast<size_t>(y) * texRes + x) * 4];
            o[0] = enc(c.x);
            o[1] = enc(c.y);
            o[2] = enc(c.z);
            o[3] = 255;
        }
    return out;
}

int terrainLod(float distance, float tileWorldSize, int previous, int maxLod) {
    auto lodFor = [&](float d) {
        float r = d / (tileWorldSize * 1.5f);
        return r < 1 ? 0 : std::clamp(static_cast<int>(std::floor(std::log2(r))) + 1, 0, maxLod);
    };
    int target = lodFor(distance);
    if (previous < 0 || target == previous) return target;
    // hysteresis: only switch once the distance is 15 % past the boundary
    int finer = lodFor(distance * 1.15f), coarser = lodFor(distance / 1.15f);
    if (target > previous) return coarser > previous ? target : previous;
    return finer < previous ? target : previous;
}

std::vector<Mat4> scatterFoliage(const FoliageComponent& f, const Mat4& world, const Heightfield* terrain, const Mat4* terrainWorld) {
    std::vector<Mat4> out;
    double count = static_cast<double>(f.area) * f.area / 100.0 * f.density;
    size_t n = static_cast<size_t>(std::min(count, 200000.0));
    std::mt19937 rng(static_cast<uint32_t>(f.seed));
    std::uniform_real_distribution<float> u(-0.5f, 0.5f), s(f.minScale, std::max(f.minScale, f.maxScale)), yaw(0, 6.2831853f);
    Mat4 invTerrain = terrainWorld ? terrainWorld->inverse() : Mat4{};
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Vec3 local{u(rng) * f.area, 0, u(rng) * f.area};
        float sc = s(rng), r = yaw(rng);
        Vec3 wp = world.transformPoint(local);
        if (terrain && terrainWorld && f.alignToTerrain) {
            Vec3 tl = invTerrain.transformPoint(wp);
            if (terrain->contains(tl.x, tl.z)) {
                tl.y = terrain->heightAt(tl.x, tl.z);
                wp = terrainWorld->transformPoint(tl);
            }
        }
        out.push_back(Mat4::translation(wp) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, r)) * Mat4::scale(Vec3(sc)));
    }
    return out;
}

} // namespace fledge::world
