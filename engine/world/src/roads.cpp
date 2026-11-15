#include "fledge/world/roads.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace fledge::world {

std::vector<Vec3> splineControlPoints(const SplineComponent& s) {
    std::vector<Vec3> pts;
    if (!s.points.is_array()) return pts;
    for (auto& p : s.points)
        if (p.is_array() && p.size() >= 3) pts.push_back({p[0].get<float>(), p[1].get<float>(), p[2].get<float>()});
    return pts;
}

Vec3 catmullRom(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float t) {
    float t2 = t * t, t3 = t2 * t;
    return (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 + (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f;
}

std::vector<Vec3> sampleSpline(const std::vector<Vec3>& pts, bool closed, float spacing) {
    std::vector<Vec3> out;
    size_t n = pts.size();
    if (n < 2) return pts;
    auto P = [&](long i) {
        if (closed) return pts[static_cast<size_t>((i % static_cast<long>(n) + static_cast<long>(n)) % static_cast<long>(n))];
        return pts[static_cast<size_t>(std::clamp<long>(i, 0, static_cast<long>(n) - 1))];
    };
    size_t segs = closed ? n : n - 1;
    for (size_t s = 0; s < segs; ++s) {
        long i = static_cast<long>(s);
        float segLen = length(P(i + 1) - P(i));
        int steps = std::max(1, static_cast<int>(std::ceil(segLen / std::max(0.1f, spacing))));
        for (int k = 0; k < steps; ++k) out.push_back(catmullRom(P(i - 1), P(i), P(i + 1), P(i + 2), static_cast<float>(k) / steps));
    }
    out.push_back(closed ? pts[0] : pts.back());
    return out;
}

namespace {
// A strip between two offset curves (left/right offsets across the path), at height y offset.
void strip(MeshData& m, const std::vector<Vec3>& path, float offA, float offB, float yA, float yB, float vScale) {
    uint32_t base = static_cast<uint32_t>(m.vertices.size());
    float along = 0;
    for (size_t i = 0; i < path.size(); ++i) {
        Vec3 d = i + 1 < path.size() ? path[i + 1] - path[i] : path[i] - path[i - 1];
        if (i > 0) along += length(path[i] - path[i - 1]);
        Vec3 side = normalize(cross({0, 1, 0}, normalize(d)));
        Vec3 up = (yA == yB) ? Vec3{0, 1, 0} : normalize(side * (offA < offB ? -1.0f : 1.0f) * (yB > yA ? -1.0f : 1.0f));
        if (yA == yB) up = {0, 1, 0};
        Vertex a, b;
        a.position = path[i] + side * offA + Vec3{0, yA, 0};
        b.position = path[i] + side * offB + Vec3{0, yB, 0};
        a.normal = b.normal = yA == yB ? Vec3{0, 1, 0} : up;
        a.uv = {0, along * vScale};
        b.uv = {1, along * vScale};
        m.vertices.push_back(a);
        m.vertices.push_back(b);
    }
    for (uint32_t i = 0; i + 1 < path.size(); ++i) {
        uint32_t a = base + i * 2, b = a + 1, c = a + 2, d = a + 3;
        // keep the winding counter-clockwise seen from above for either offset order
        if (offA < offB) m.indices.insert(m.indices.end(), {a, b, c, b, d, c});
        else m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
    }
}
} // namespace

RoadMeshes buildRoadMeshes(const SplineComponent& s) {
    RoadMeshes r;
    auto path = sampleSpline(splineControlPoints(s), s.closed, 2.0f);
    if (path.size() < 2) return r;
    float hw = s.width * 0.5f;
    strip(r.road, path, hw, -hw, 0.03f, 0.03f, 0.1f);
    // sidewalkTrim keeps sidewalks and markings out of intersections at both ends (city grids)
    std::vector<Vec3> walk = path;
    if (s.sidewalkTrim > 0 && !s.closed) {
        std::vector<float> cum(path.size(), 0);
        for (size_t i = 1; i < path.size(); ++i) cum[i] = cum[i - 1] + length(path[i] - path[i - 1]);
        float total = cum.back(), a = s.sidewalkTrim, b = total - s.sidewalkTrim;
        walk.clear();
        if (b > a + 0.5f) {
            auto at = [&](float d) {
                size_t k = 1;
                while (k + 1 < path.size() && cum[k] < d) ++k;
                float t = (d - cum[k - 1]) / std::max(1e-4f, cum[k] - cum[k - 1]);
                return path[k - 1] + (path[k] - path[k - 1]) * std::clamp(t, 0.0f, 1.0f);
            };
            walk.push_back(at(a));
            for (size_t i = 0; i < path.size(); ++i)
                if (cum[i] > a && cum[i] < b) walk.push_back(path[i]);
            walk.push_back(at(b));
        }
    }
    if (s.sidewalk > 0 && walk.size() >= 2) {
        strip(r.sidewalks, walk, hw + s.sidewalk, hw, 0.18f, 0.18f, 0.2f);     // left sidewalk top
        strip(r.sidewalks, walk, -hw, -hw - s.sidewalk, 0.18f, 0.18f, 0.2f);   // right sidewalk top
        strip(r.sidewalks, walk, hw, hw, 0.18f, 0.03f, 1.0f);                  // left curb face
        strip(r.sidewalks, walk, -hw, -hw, 0.03f, 0.18f, 1.0f);                // right curb face
    }
    path = walk;  // markings follow the trimmed path too
    // dashed centre line: 3 m dash, 3 m gap
    float along = 0;
    std::vector<Vec3> dash;
    for (size_t i = 0; i < path.size(); ++i) {
        if (i > 0) along += length(path[i] - path[i - 1]);
        bool on = std::fmod(along, 6.0f) < 3.0f;
        if (on) dash.push_back(path[i]);
        if ((!on || i + 1 == path.size()) && dash.size() >= 2) {
            strip(r.markings, dash, 0.08f, -0.08f, 0.045f, 0.045f, 1.0f);
            dash.clear();
        } else if (!on) {
            dash.clear();
        }
    }
    return r;
}

RoadGraph RoadGraph::build(Scene& scene, float snap) {
    RoadGraph g;
    auto worlds = scene.computeWorldMatrices();
    auto nodeAt = [&](Vec3 p) {
        for (size_t i = 0; i < g.nodes.size(); ++i)
            if (length(g.nodes[i].position - p) <= snap) return static_cast<int>(i);
        g.nodes.push_back({p, {}});
        return static_cast<int>(g.nodes.size()) - 1;
    };
    struct Road {
        std::vector<Vec3> path;
        const SplineComponent* s;
        EntityId e;
    };
    std::vector<Road> roads;
    for (EntityId e : scene.entities()) {
        auto* s = scene.get<SplineComponent>(e);
        if (!s || s->kind != "road") continue;
        auto pts = splineControlPoints(*s);
        if (pts.size() < 2) continue;
        for (auto& p : pts) p = worlds[e].transformPoint(p);
        roads.push_back({sampleSpline(pts, s->closed, 2.0f), s, e});
    }
    // split points: endpoints of other roads lying on this road's interior (T-junctions)
    for (auto& r : roads) {
        std::vector<size_t> cuts{0};
        std::vector<float> cum(r.path.size(), 0);
        for (size_t i = 1; i < r.path.size(); ++i) cum[i] = cum[i - 1] + length(r.path[i] - r.path[i - 1]);
        for (auto& o : roads) {
            if (&o == &r) continue;
            for (Vec3 ep : {o.path.front(), o.path.back()})
                for (size_t i = 2; i + 2 < r.path.size(); ++i)
                    if (cum[i] > snap * 1.5f && cum.back() - cum[i] > snap * 1.5f && length(r.path[i] - ep) <= snap * 0.75f) {  // not near our own ends
                        cuts.push_back(i);
                        break;
                    }
        }
        cuts.push_back(r.path.size() - 1);
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        for (size_t k = 0; k + 1 < cuts.size(); ++k) {
            Edge ed;
            ed.polyline.assign(r.path.begin() + static_cast<long>(cuts[k]), r.path.begin() + static_cast<long>(cuts[k + 1]) + 1);
            if (ed.polyline.size() < 2) continue;
            for (size_t i = 1; i < ed.polyline.size(); ++i) ed.length += length(ed.polyline[i] - ed.polyline[i - 1]);
            ed.a = nodeAt(ed.polyline.front());
            ed.b = nodeAt(ed.polyline.back());
            ed.width = r.s->width;
            ed.lanes = r.s->lanes;
            ed.speedLimit = r.s->speedLimit;
            ed.sidewalk = r.s->sidewalk;
            ed.spline = r.e;
            int id = static_cast<int>(g.edges.size());
            g.edges.push_back(std::move(ed));
            g.nodes[static_cast<size_t>(g.edges.back().a)].edges.push_back(id);
            if (g.edges.back().b != g.edges.back().a) g.nodes[static_cast<size_t>(g.edges.back().b)].edges.push_back(id);
        }
    }
    return g;
}

int RoadGraph::nearestNode(Vec3 p) const {
    int best = -1;
    float bd = std::numeric_limits<float>::max();
    for (size_t i = 0; i < nodes.size(); ++i)
        if (float d = length(nodes[i].position - p); d < bd) {
            bd = d;
            best = static_cast<int>(i);
        }
    return best;
}

std::vector<int> RoadGraph::shortestPath(int from, int to) const {
    if (from < 0 || to < 0 || from >= static_cast<int>(nodes.size()) || to >= static_cast<int>(nodes.size())) return {};
    std::vector<float> dist(nodes.size(), std::numeric_limits<float>::max());
    std::vector<int> prev(nodes.size(), -1);
    using Q = std::pair<float, int>;
    std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
    dist[static_cast<size_t>(from)] = 0;
    q.push({0, from});
    while (!q.empty()) {
        auto [d, n] = q.top();
        q.pop();
        if (d > dist[static_cast<size_t>(n)]) continue;
        if (n == to) break;
        for (int ei : nodes[static_cast<size_t>(n)].edges) {
            const Edge& e = edges[static_cast<size_t>(ei)];
            int m = e.a == n ? e.b : e.a;
            float nd = d + e.length;
            if (nd < dist[static_cast<size_t>(m)]) {
                dist[static_cast<size_t>(m)] = nd;
                prev[static_cast<size_t>(m)] = n;
                q.push({nd, m});
            }
        }
    }
    if (dist[static_cast<size_t>(to)] == std::numeric_limits<float>::max()) return {};
    std::vector<int> path;
    for (int n = to; n != -1; n = prev[static_cast<size_t>(n)]) path.push_back(n);
    std::reverse(path.begin(), path.end());
    return path;
}

int RoadGraph::intersections() const {
    int n = 0;
    for (auto& nd : nodes) n += nd.edges.size() >= 3 ? 1 : 0;
    return n;
}

Vec3 RoadGraph::closestPoint(Vec3 p, int* edgeOut, float* alongOut) const {
    Vec3 best = p;
    float bd = std::numeric_limits<float>::max();
    for (size_t ei = 0; ei < edges.size(); ++ei) {
        float along = 0;
        const auto& pl = edges[ei].polyline;
        for (size_t i = 0; i + 1 < pl.size(); ++i) {
            Vec3 a = pl[i], b = pl[i + 1], ab = b - a;
            float l2 = dot(ab, ab);
            float t = l2 > 0 ? std::clamp(dot(p - a, ab) / l2, 0.0f, 1.0f) : 0;
            Vec3 q = a + ab * t;
            float d = length(q - p);
            if (d < bd) {
                bd = d;
                best = q;
                if (edgeOut) *edgeOut = static_cast<int>(ei);
                if (alongOut) *alongOut = along + std::sqrt(l2) * t;
            }
            along += std::sqrt(l2);
        }
    }
    return best;
}

} // namespace fledge::world
