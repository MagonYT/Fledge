// Recast solo-mesh build + Detour queries (the standard pipeline from RecastDemo's Sample_SoloMesh).
#include "fledge/gameplay/nav.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <chrono>
#include <cmath>
#include <cstring>

namespace fledge::gameplay {

struct NavMesh::Impl {
    dtNavMesh* mesh = nullptr;
    dtNavMeshQuery* query = nullptr;
    ~Impl() {
        if (query) dtFreeNavMeshQuery(query);
        if (mesh) dtFreeNavMesh(mesh);
    }
};

NavMesh::NavMesh() : m(std::make_unique<Impl>()) {}
NavMesh::~NavMesh() = default;
bool NavMesh::valid() const { return m->mesh && m->query; }

namespace {
struct RecastScratch {
    rcHeightfield* hf = nullptr;
    rcCompactHeightfield* chf = nullptr;
    rcContourSet* cset = nullptr;
    rcPolyMesh* pmesh = nullptr;
    rcPolyMeshDetail* dmesh = nullptr;
    ~RecastScratch() {
        rcFreeHeightField(hf);
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
    }
};
} // namespace

bool NavMesh::build(const std::vector<Vec3>& verts, const std::vector<uint32_t>& idx, const NavSettings& s, std::string* error) {
    auto t0 = std::chrono::steady_clock::now();
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    m = std::make_unique<Impl>();
    m_stats = {};
    if (verts.empty() || idx.size() < 3) return fail("no geometry");
    rcContext ctx(false);
    rcConfig cfg{};
    cfg.cs = s.cellSize;
    cfg.ch = s.cellHeight;
    cfg.walkableSlopeAngle = s.agentMaxSlope;
    cfg.walkableHeight = static_cast<int>(std::ceil(s.agentHeight / cfg.ch));
    cfg.walkableClimb = static_cast<int>(std::floor(s.agentMaxClimb / cfg.ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(s.agentRadius / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(s.maxEdgeLength / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = static_cast<int>(s.regionMinSize * s.regionMinSize);
    cfg.mergeRegionArea = static_cast<int>(s.regionMergeSize * s.regionMergeSize);
    cfg.maxVertsPerPoly = 6;
    cfg.detailSampleDist = cfg.cs * 6;
    cfg.detailSampleMaxError = cfg.ch * 1;
    std::vector<float> fv(verts.size() * 3);
    for (size_t i = 0; i < verts.size(); ++i) {
        fv[i * 3] = verts[i].x;
        fv[i * 3 + 1] = verts[i].y;
        fv[i * 3 + 2] = verts[i].z;
    }
    std::vector<int> tris(idx.begin(), idx.end());
    int nverts = static_cast<int>(verts.size()), ntris = static_cast<int>(idx.size() / 3);
    m_stats.inputTriangles = ntris;
    rcCalcBounds(fv.data(), nverts, cfg.bmin, cfg.bmax);
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
    if (static_cast<long long>(cfg.width) * cfg.height > 6000ll * 6000ll) return fail("navmesh area too large for one tile (max ~1.8 km at 0.3 m cells)");
    RecastScratch r;
    r.hf = rcAllocHeightfield();
    if (!r.hf || !rcCreateHeightfield(&ctx, *r.hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) return fail("heightfield");
    std::vector<unsigned char> areas(static_cast<size_t>(ntris), 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, fv.data(), nverts, tris.data(), ntris, areas.data());
    if (!rcRasterizeTriangles(&ctx, fv.data(), nverts, tris.data(), areas.data(), ntris, *r.hf, cfg.walkableClimb)) return fail("rasterize");
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *r.hf);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *r.hf);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *r.hf);
    r.chf = rcAllocCompactHeightfield();
    if (!r.chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *r.hf, *r.chf)) return fail("compact heightfield");
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *r.chf)) return fail("erode");
    if (!rcBuildDistanceField(&ctx, *r.chf)) return fail("distance field");
    if (!rcBuildRegions(&ctx, *r.chf, 0, cfg.minRegionArea, cfg.mergeRegionArea)) return fail("regions");
    r.cset = rcAllocContourSet();
    if (!r.cset || !rcBuildContours(&ctx, *r.chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *r.cset)) return fail("contours");
    r.pmesh = rcAllocPolyMesh();
    if (!r.pmesh || !rcBuildPolyMesh(&ctx, *r.cset, cfg.maxVertsPerPoly, *r.pmesh)) return fail("poly mesh");
    r.dmesh = rcAllocPolyMeshDetail();
    if (!r.dmesh || !rcBuildPolyMeshDetail(&ctx, *r.pmesh, *r.chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *r.dmesh)) return fail("detail mesh");
    if (r.pmesh->npolys == 0) return fail("no walkable area");
    for (int i = 0; i < r.pmesh->npolys; ++i) {
        if (r.pmesh->areas[i] == RC_WALKABLE_AREA) r.pmesh->areas[i] = 0;
        r.pmesh->flags[i] = 1;
    }
    dtNavMeshCreateParams p{};
    p.verts = r.pmesh->verts;
    p.vertCount = r.pmesh->nverts;
    p.polys = r.pmesh->polys;
    p.polyAreas = r.pmesh->areas;
    p.polyFlags = r.pmesh->flags;
    p.polyCount = r.pmesh->npolys;
    p.nvp = r.pmesh->nvp;
    p.detailMeshes = r.dmesh->meshes;
    p.detailVerts = r.dmesh->verts;
    p.detailVertsCount = r.dmesh->nverts;
    p.detailTris = r.dmesh->tris;
    p.detailTriCount = r.dmesh->ntris;
    p.walkableHeight = s.agentHeight;
    p.walkableRadius = s.agentRadius;
    p.walkableClimb = s.agentMaxClimb;
    std::memcpy(p.bmin, r.pmesh->bmin, sizeof(p.bmin));
    std::memcpy(p.bmax, r.pmesh->bmax, sizeof(p.bmax));
    p.cs = cfg.cs;
    p.ch = cfg.ch;
    p.buildBvTree = true;
    unsigned char* data = nullptr;
    int dataSize = 0;
    if (!dtCreateNavMeshData(&p, &data, &dataSize)) return fail("detour data (too many vertices per tile?)");
    m->mesh = dtAllocNavMesh();
    if (!m->mesh || dtStatusFailed(m->mesh->init(data, dataSize, DT_TILE_FREE_DATA))) {
        dtFree(data);
        return fail("detour init");
    }
    m->query = dtAllocNavMeshQuery();
    if (!m->query || dtStatusFailed(m->query->init(m->mesh, 4096))) return fail("detour query");
    m_stats.polygons = r.pmesh->npolys;
    m_stats.vertices = r.pmesh->nverts;
    m_stats.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

std::vector<Vec3> NavMesh::findPath(Vec3 a, Vec3 b, Vec3 extents) const {
    std::vector<Vec3> out;
    if (!valid()) return out;
    dtQueryFilter filter;
    float ext[3] = {extents.x, extents.y, extents.z}, pa[3] = {a.x, a.y, a.z}, pb[3] = {b.x, b.y, b.z}, na[3], nb[3];
    dtPolyRef ra = 0, rb = 0;
    m->query->findNearestPoly(pa, ext, &filter, &ra, na);
    m->query->findNearestPoly(pb, ext, &filter, &rb, nb);
    if (!ra || !rb) return out;
    dtPolyRef polys[512];
    int npolys = 0;
    m->query->findPath(ra, rb, na, nb, &filter, polys, &npolys, 512);
    if (npolys == 0) return out;
    float end[3] = {nb[0], nb[1], nb[2]};
    if (polys[npolys - 1] != rb) m->query->closestPointOnPoly(polys[npolys - 1], nb, end, nullptr);  // partial path
    float straight[256 * 3];
    int nstraight = 0;
    m->query->findStraightPath(na, end, polys, npolys, straight, nullptr, nullptr, &nstraight, 256);
    for (int i = 0; i < nstraight; ++i) out.push_back({straight[i * 3], straight[i * 3 + 1], straight[i * 3 + 2]});
    return out;
}

std::optional<Vec3> NavMesh::nearestPoint(Vec3 p, Vec3 extents) const {
    if (!valid()) return std::nullopt;
    dtQueryFilter filter;
    float ext[3] = {extents.x, extents.y, extents.z}, pp[3] = {p.x, p.y, p.z}, np[3];
    dtPolyRef r = 0;
    m->query->findNearestPoly(pp, ext, &filter, &r, np);
    if (!r) return std::nullopt;
    return Vec3{np[0], np[1], np[2]};
}

namespace {
uint32_t g_seed = 1;
float frand() {
    g_seed = g_seed * 1664525u + 1013904223u;
    return static_cast<float>(g_seed >> 8) / 16777216.0f;
}
} // namespace

std::optional<Vec3> NavMesh::randomPoint(uint32_t seed) const {
    if (!valid()) return std::nullopt;
    g_seed = seed * 2654435761u + 1;
    dtQueryFilter filter;
    dtPolyRef r = 0;
    float p[3];
    if (dtStatusFailed(m->query->findRandomPoint(&filter, frand, &r, p))) return std::nullopt;
    return Vec3{p[0], p[1], p[2]};
}

void NavMesh::debugTriangles(std::vector<Vec3>& out) const {
    if (!m->mesh) return;
    const dtNavMesh& nm = *m->mesh;
    for (int t = 0; t < nm.getMaxTiles(); ++t) {
        const dtMeshTile* tile = nm.getTile(t);
        if (!tile || !tile->header) continue;
        for (int i = 0; i < tile->header->polyCount; ++i) {
            const dtPoly& poly = tile->polys[i];
            const dtPolyDetail& pd = tile->detailMeshes[i];
            for (int j = 0; j < pd.triCount; ++j) {
                const unsigned char* tri = &tile->detailTris[(pd.triBase + j) * 4];
                for (int k = 0; k < 3; ++k) {
                    const float* v = tri[k] < poly.vertCount ? &tile->verts[poly.verts[tri[k]] * 3] : &tile->detailVerts[(pd.vertBase + tri[k] - poly.vertCount) * 3];
                    out.push_back({v[0], v[1], v[2]});
                }
            }
        }
    }
}

} // namespace fledge::gameplay
