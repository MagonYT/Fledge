#include "fledge/assets/lod.h"

#include <algorithm>
#include <cmath>

#include <meshoptimizer.h>

namespace fledge {

void generateLods(MeshData& mesh, const LodSettings& s) {
    mesh.lods.clear();
    if (mesh.indices.size() / 3 < s.minTriangles || mesh.vertices.empty()) return;
    std::vector<float> pos(mesh.vertices.size() * 3);
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        pos[i * 3] = mesh.vertices[i].position.x;
        pos[i * 3 + 1] = mesh.vertices[i].position.y;
        pos[i * 3 + 2] = mesh.vertices[i].position.z;
    }
    const std::vector<uint32_t>* src = &mesh.indices;
    for (int l = 0; l < s.levels; ++l) {
        size_t target = static_cast<size_t>(static_cast<double>(src->size() / 3) * s.ratio) * 3;
        if (target < 3 * 4) break;
        std::vector<uint32_t> out(src->size());
        float err = 0;
        size_t n = meshopt_simplify(out.data(), src->data(), src->size(), pos.data(), mesh.vertices.size(), sizeof(float) * 3, target,
                                    s.maxError * static_cast<float>(l + 1), 0, &err);
        if (n == 0 || n >= src->size() * 9 / 10) {  // locked by the error cap: try the sloppy simplifier once
            n = meshopt_simplifySloppy(out.data(), src->data(), src->size(), pos.data(), mesh.vertices.size(), sizeof(float) * 3, target,
                                       s.maxError * static_cast<float>(l + 2), &err);
            if (n == 0 || n >= src->size() * 9 / 10) break;
        }
        out.resize(n);
        meshopt_optimizeVertexCache(out.data(), out.data(), n, mesh.vertices.size());
        mesh.lods.push_back({std::move(out), err});
        src = &mesh.lods.back().indices;
    }
}

size_t triangleCount(const MeshData& m, int lod) {
    if (lod <= 0 || m.lods.empty()) return m.indices.size() / 3;
    return m.lods[static_cast<size_t>(std::min<int>(lod, static_cast<int>(m.lods.size())) - 1)].indices.size() / 3;
}

int selectLod(float screenSize, int lodCount, float bias, int previous, int forced) {
    if (lodCount <= 0) return 0;
    if (forced >= 0) return std::min(forced, lodCount);
    static const float kThresholds[] = {0.25f, 0.10f, 0.04f, 0.015f, 0.006f};
    float scale = std::pow(2.0f, bias);
    auto pick = [&](float s) {
        int l = 0;
        while (l < lodCount && l < 5 && s < kThresholds[l] * scale) ++l;
        return l;
    };
    int target = pick(screenSize);
    if (previous < 0 || previous == target) return target;
    // hysteresis: only move when the size is 10 % past the boundary
    int moved = target > previous ? pick(screenSize * 1.1f) : pick(screenSize / 1.1f);
    return moved == previous ? previous : target;
}

MeshData buildProxyMesh(const std::vector<ProxyPart>& parts, size_t targetTriangles) {
    MeshData m;
    for (auto& p : parts) {
        if (!p.mesh) continue;
        uint32_t base = static_cast<uint32_t>(m.vertices.size());
        // use the coarsest LOD available: the proxy is for far away anyway
        const std::vector<uint32_t>& idx = p.mesh->lods.empty() ? p.mesh->indices : p.mesh->lods.back().indices;
        Mat4 nm = p.world;
        for (const Vertex& v : p.mesh->vertices) {
            Vertex w = v;
            w.position = p.world.transformPoint(v.position);
            w.normal = normalize((nm * Vec4(v.normal, 0)).xyz());
            m.vertices.push_back(w);
        }
        for (uint32_t i : idx) m.indices.push_back(base + i);
    }
    if (m.indices.size() / 3 > targetTriangles && !m.vertices.empty()) {
        std::vector<float> pos(m.vertices.size() * 3);
        for (size_t i = 0; i < m.vertices.size(); ++i) {
            pos[i * 3] = m.vertices[i].position.x;
            pos[i * 3 + 1] = m.vertices[i].position.y;
            pos[i * 3 + 2] = m.vertices[i].position.z;
        }
        std::vector<uint32_t> out(m.indices.size());
        size_t n = meshopt_simplifySloppy(out.data(), m.indices.data(), m.indices.size(), pos.data(), m.vertices.size(), sizeof(float) * 3,
                                          targetTriangles * 3, 0.2f, nullptr);
        if (n > 0) {
            out.resize(n);
            m.indices = std::move(out);
        }
    }
    // drop unreferenced vertices
    std::vector<uint32_t> remap(m.vertices.size());
    size_t unique = meshopt_optimizeVertexFetchRemap(remap.data(), m.indices.data(), m.indices.size(), m.vertices.size());
    std::vector<Vertex> verts(unique);
    for (size_t i = 0; i < m.vertices.size(); ++i)
        if (remap[i] != ~0u) verts[remap[i]] = m.vertices[i];
    for (auto& i : m.indices) i = remap[i];
    m.vertices = std::move(verts);
    return m;
}

} // namespace fledge
