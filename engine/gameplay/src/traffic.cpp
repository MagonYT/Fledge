#include "fledge/gameplay/traffic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace fledge::gameplay {

namespace {
constexpr Vec3 kUp{0, 1, 0};
float yawOf(Vec3 d) { return std::atan2(-d.x, -d.z); }
} // namespace

uint32_t TrafficSim::rnd(uint32_t& s) const {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

TrafficSim::TrafficSim(const world::RoadGraph& graph, const TrafficSettings& settings) : m_graph(graph), m_set(settings), m_rng(settings.seed * 2654435761u | 1u) {
    m_geo.resize(m_graph.edges.size());
    auto cut = [&](int node, float width) {
        size_t deg = m_graph.nodes[static_cast<size_t>(node)].edges.size();
        return deg >= 3 ? width * 0.75f + 1.0f : 0.0f;
    };
    for (size_t i = 0; i < m_graph.edges.size(); ++i) {
        auto& e = m_graph.edges[i];
        EdgeGeo& g = m_geo[i];
        g.pts = e.polyline;
        if (g.pts.size() < 2) g.pts = {m_graph.nodes[static_cast<size_t>(e.a)].position, m_graph.nodes[static_cast<size_t>(e.b)].position};
        g.cum.assign(g.pts.size(), 0);
        for (size_t k = 1; k < g.pts.size(); ++k) g.cum[k] = g.cum[k - 1] + length(g.pts[k] - g.pts[k - 1]);
        g.length = g.cum.back();
        g.width = e.width;
        g.sidewalk = e.sidewalk;
        g.speed = e.speedLimit > 0 ? e.speedLimit : 13.9f;
        g.startCut = cut(e.a, e.width);
        g.endCut = cut(e.b, e.width);
        float total = g.startCut + g.endCut;
        if (total > g.length - 2) {
            float k = std::max(0.0f, g.length - 2) / std::max(total, 1e-3f);
            g.startCut *= k;
            g.endCut *= k;
        }
    }
    m_reservation.assign(m_graph.nodes.size(), 0);
    if (m_graph.edges.empty()) return;
    m_cars.resize(static_cast<size_t>(std::max(0, m_set.cars)));
    for (size_t i = 0; i < m_cars.size(); ++i) {
        TrafficCar& c = m_cars[i];
        c.id = static_cast<int>(i) + 1;
        c.rng = (m_set.seed + 1) * 7919u + static_cast<uint32_t>(i) * 104729u + 1u;
        c.colorIndex = static_cast<int>(rnd(c.rng) % 8);
        placeCar(c, {0, 0, 0}, true);
    }
    m_peds.resize(static_cast<size_t>(std::max(0, m_set.pedestrians)));
    for (size_t i = 0; i < m_peds.size(); ++i) {
        Pedestrian& p = m_peds[i];
        p.id = static_cast<int>(i) + 1;
        p.rng = (m_set.seed + 3) * 15485863u + static_cast<uint32_t>(i) * 32452843u + 1u;
        placePed(p, {0, 0, 0});
    }
}

Vec3 TrafficSim::pointAt(int edge, float d, Vec3* tangent) const {
    const EdgeGeo& g = m_geo[static_cast<size_t>(edge)];
    d = std::clamp(d, 0.0f, g.length);
    auto it = std::upper_bound(g.cum.begin(), g.cum.end(), d);
    size_t k = static_cast<size_t>(std::clamp<std::ptrdiff_t>(it - g.cum.begin(), 1, static_cast<std::ptrdiff_t>(g.pts.size()) - 1));
    float segLen = std::max(1e-4f, g.cum[k] - g.cum[k - 1]);
    float t = (d - g.cum[k - 1]) / segLen;
    if (tangent) *tangent = normalize(g.pts[k] - g.pts[k - 1]);
    return g.pts[k - 1] + (g.pts[k] - g.pts[k - 1]) * t;
}

float TrafficSim::laneStart(int edge, bool forward) const {
    const EdgeGeo& g = m_geo[static_cast<size_t>(edge)];
    return forward ? g.startCut : g.endCut;
}
float TrafficSim::laneEnd(int edge, bool forward) const {
    const EdgeGeo& g = m_geo[static_cast<size_t>(edge)];
    return g.length - (forward ? g.endCut : g.startCut);
}

Vec3 TrafficSim::lanePoint(int edge, bool forward, float s, float offset, Vec3* dir) const {
    const EdgeGeo& g = m_geo[static_cast<size_t>(edge)];
    Vec3 t;
    Vec3 p = pointAt(edge, forward ? s : g.length - s, &t);
    if (!forward) t = t * -1.0f;
    Vec3 right = normalize(cross(t, kUp));
    if (dir) *dir = t;
    return p + right * offset;
}

bool TrafficSim::junctionControlled(int node) const { return m_graph.nodes[static_cast<size_t>(node)].edges.size() != 2; }

void TrafficSim::chooseNext(TrafficCar& c) {
    int node = endNode(c.edge, c.forward);
    const auto& es = m_graph.nodes[static_cast<size_t>(node)].edges;
    std::vector<int> options;
    for (int e : es)
        if (e != c.edge && m_graph.edges[static_cast<size_t>(e)].a != m_graph.edges[static_cast<size_t>(e)].b) options.push_back(e);
    if (options.empty()) options.push_back(c.edge);  // dead end: U-turn
    c.nextEdge = options[rnd(c.rng) % options.size()];
    c.nextForward = m_graph.edges[static_cast<size_t>(c.nextEdge)].a == node;
    if (c.nextEdge == c.edge) c.nextForward = !c.forward;
}

float TrafficSim::laneRoomAtStart(int edge, bool forward, int ignoreId) const {
    float start = laneStart(edge, forward), room = laneEnd(edge, forward) - start + 100.0f;
    for (const TrafficCar& o : m_cars) {
        if (o.id == ignoreId) continue;
        if (!o.inJunction && o.edge == edge && o.forward == forward) room = std::min(room, o.s - start);
        if (o.inJunction && o.nextEdge == edge && o.nextForward == forward) room = std::min(room, 0.0f);  // about to enter
    }
    return room;
}

void TrafficSim::placeCar(TrafficCar& c, Vec3 focus, bool initial) {
    for (int attempt = 0; attempt < 60; ++attempt) {
        int e = static_cast<int>(rnd(m_rng) % m_graph.edges.size());
        bool fwd = (rnd(m_rng) & 1) != 0;
        float a = laneStart(e, fwd), b = laneEnd(e, fwd);
        if (b - a < m_set.carLength + 2) continue;
        if (b - a < m_set.carLength + 26) continue;  // keep spawns away from both junction queues
        float s = a + 8 + rndf(m_rng) * (b - a - 24);
        Vec3 p = lanePoint(e, fwd, s, m_geo[static_cast<size_t>(e)].width * 0.25f);
        if (m_set.radius > 0) {
            float d = length(p - focus);
            if (d > m_set.radius || (!initial && d < m_set.radius * 0.4f)) continue;
        }
        bool free = true;
        for (const TrafficCar& o : m_cars)
            if (o.id != c.id && o.edge == e && o.forward == fwd && !o.inJunction && std::fabs(o.s - s) < m_set.carLength + m_set.minGap + 1) free = false;
        if (!free) continue;
        if (c.inJunction) {
            int node = endNode(c.edge, c.forward);
            if (m_reservation[static_cast<size_t>(node)] == c.id) m_reservation[static_cast<size_t>(node)] = 0;
        }
        for (int& r : m_reservation)
            if (r == c.id) r = 0;
        c.edge = e;
        c.forward = fwd;
        c.s = s;
        c.inJunction = false;
        c.waited = 0;
        c.desiredSpeed = m_geo[static_cast<size_t>(e)].speed * (0.85f + 0.3f * rndf(c.rng));
        c.speed = initial ? c.desiredSpeed * 0.5f : 0;
        Vec3 dir;
        c.position = lanePoint(e, fwd, s, m_geo[static_cast<size_t>(e)].width * 0.25f, &dir);
        c.yaw = yawOf(dir);
        chooseNext(c);
        return;
    }
}

void TrafficSim::placePed(Pedestrian& p, Vec3 focus) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        int e = static_cast<int>(rnd(m_rng) % m_graph.edges.size());
        const EdgeGeo& g = m_geo[static_cast<size_t>(e)];
        float s = rndf(m_rng) * g.length;
        p.edge = e;
        p.forward = (rnd(p.rng) & 1) != 0;
        p.side = (rnd(p.rng) & 1) ? 1.0f : -1.0f;
        p.s = s;
        p.speed = m_set.pedestrianSpeed * (0.8f + 0.4f * rndf(p.rng));
        Vec3 dir;
        p.position = lanePoint(e, p.forward, s, p.side * (g.width * 0.5f + g.sidewalk * 0.5f), &dir);
        p.yaw = yawOf(dir);
        p.down = false;
        if (m_set.radius > 0 && length(p.position - focus) > m_set.radius) continue;
        return;
    }
}

void TrafficSim::respawnCar(int id, Vec3 focus) {
    for (TrafficCar& c : m_cars)
        if (c.id == id) placeCar(c, focus, false);
}

int TrafficSim::pedestrianOnRay(Vec3 o, Vec3 d, float maxD, float maxOff) const {
    int best = -1;
    float bestT = maxD;
    for (size_t i = 0; i < m_peds.size(); ++i) {
        const Pedestrian& p = m_peds[i];
        if (p.down) continue;
        Vec3 c = p.position + Vec3{0, 0.9f, 0};
        float t = dot(c - o, d);
        if (t < 0 || t > bestT) continue;
        Vec3 closest = o + d * t;
        Vec3 off = c - closest;
        if (std::fabs(off.y) < 0.95f && std::sqrt(off.x * off.x + off.z * off.z) < maxOff) {
            bestT = t;
            best = static_cast<int>(i);
        }
    }
    return best;
}

bool TrafficSim::knockDownIndex(int i) {
    if (i < 0 || i >= static_cast<int>(m_peds.size()) || m_peds[static_cast<size_t>(i)].down) return false;
    m_peds[static_cast<size_t>(i)].down = true;
    m_peds[static_cast<size_t>(i)].downTime = 0;
    return true;
}

int TrafficSim::knockDown(Vec3 pos, float radius) {
    int n = 0;
    for (Pedestrian& p : m_peds)
        if (!p.down && length(p.position - pos) < radius) {
            p.down = true;
            p.downTime = 0;
            ++n;
        }
    return n;
}

void TrafficSim::step(float dt, Vec3 focus, const ObstacleFn& obstacle) {
    if (m_graph.edges.empty() || dt <= 0) return;
    m_time += dt;
    const float L = m_set.carLength;
    // lane occupancy (previous positions): leaders
    std::map<std::pair<int, bool>, std::vector<const TrafficCar*>> lanes;
    for (const TrafficCar& c : m_cars)
        if (!c.inJunction) lanes[{c.edge, c.forward}].push_back(&c);
    std::vector<std::pair<int, float>> leaderGap(m_cars.size(), {0, 1e9f});
    std::vector<float> leaderSpeed(m_cars.size(), 0);
    for (size_t i = 0; i < m_cars.size(); ++i) {
        const TrafficCar& c = m_cars[i];
        if (c.inJunction) continue;
        for (const TrafficCar* o : lanes[{c.edge, c.forward}])
            if (o->id != c.id && o->s > c.s) {
                float gap = o->s - c.s - L;
                if (gap < leaderGap[i].second) {
                    leaderGap[i] = {o->id, gap};
                    leaderSpeed[i] = o->speed;
                }
            }
    }
    // junction queues: the head car that arrived first (and has room on its next lane) gets the junction next
    std::map<int, std::pair<double, int>> firstInQueue;  // node -> (arrival, car id)
    for (size_t i = 0; i < m_cars.size(); ++i) {
        TrafficCar& c = m_cars[i];
        if (c.edge < 0 || c.inJunction || leaderGap[i].first != 0) continue;
        int node = endNode(c.edge, c.forward);
        if (!junctionControlled(node) || laneEnd(c.edge, c.forward) - c.s >= 12) continue;
        if (c.arrival < 0) c.arrival = m_time;
        if (laneRoomAtStart(c.nextEdge, c.nextForward, c.id) < m_set.carLength + m_set.minGap) continue;
        auto it = firstInQueue.find(node);
        if (it == firstInQueue.end() || c.arrival < it->second.first || (c.arrival == it->second.first && c.id < it->second.second))
            firstInQueue[node] = {c.arrival, c.id};
    }
    double speedSum = 0;
    m_stats.waiting = 0;
    for (size_t i = 0; i < m_cars.size(); ++i) {
        TrafficCar& c = m_cars[i];
        if (c.edge < 0) continue;
        float v0 = c.desiredSpeed;
        if (c.inJunction) {
            float vj = std::min(v0, 7.0f);
            c.speed = std::min(vj, c.speed + m_set.maxAccel * dt);
            if (c.speed < 2) c.speed = 2;  // never stall inside a junction
            c.js += c.speed * dt;
            m_stats.distance += c.speed * dt;
            float t = std::clamp(c.js / std::max(c.jlen, 1e-3f), 0.0f, 1.0f);
            Vec3 p = c.jFrom * ((1 - t) * (1 - t)) + c.jCtrl * (2 * (1 - t) * t) + c.jTo * (t * t);
            Vec3 d = (c.jCtrl - c.jFrom) * (2 * (1 - t)) + (c.jTo - c.jCtrl) * (2 * t);
            c.position = p;
            if (length(d) > 1e-4f) c.yaw = yawOf(normalize(d));
            if (c.js >= c.jlen) {
                int node = endNode(c.edge, c.forward);
                if (m_reservation[static_cast<size_t>(node)] == c.id) m_reservation[static_cast<size_t>(node)] = 0;
                c.edge = c.nextEdge;
                c.forward = c.nextForward;
                c.s = laneStart(c.edge, c.forward);
                c.inJunction = false;
                c.desiredSpeed = m_geo[static_cast<size_t>(c.edge)].speed * (0.85f + 0.3f * rndf(c.rng));
                chooseNext(c);
            }
            speedSum += c.speed;
            continue;
        }
        float end = laneEnd(c.edge, c.forward);
        float dEnd = end - c.s;
        int node = endNode(c.edge, c.forward);
        bool controlled = junctionControlled(node);
        bool roomNext = laneRoomAtStart(c.nextEdge, c.nextForward, c.id) >= L + m_set.minGap;
        bool reserved = !controlled || m_reservation[static_cast<size_t>(node)] == c.id;
        bool firstInLane = leaderGap[i].first == 0;  // only the car at the head of the queue may reserve the junction
        if (controlled && m_reservation[static_cast<size_t>(node)] == c.id && (!firstInLane || !roomNext)) {
            m_reservation[static_cast<size_t>(node)] = 0;  // cannot use it right now: let someone else cross
            reserved = false;
        }
        bool myTurn = firstInQueue.count(node) && firstInQueue[node].second == c.id;
        if (controlled && !reserved && roomNext && firstInLane && myTurn && dEnd < 12 && m_reservation[static_cast<size_t>(node)] == 0) {
            m_reservation[static_cast<size_t>(node)] = c.id;
            reserved = true;
        }
        bool canGo = roomNext && reserved;
        float gap = leaderGap[i].second, vLead = leaderSpeed[i];
        if (!canGo && dEnd - 0.5f < gap) {
            gap = std::max(0.0f, dEnd - 0.5f);
            vLead = 0;
        }
        if (obstacle) {
            Vec3 dir;
            Vec3 p = lanePoint(c.edge, c.forward, c.s, m_geo[static_cast<size_t>(c.edge)].width * 0.25f, &dir);
            float od = obstacle(p, dir, 30.0f) - L * 0.5f - 1.0f;
            if (od < gap) {
                gap = std::max(0.0f, od);
                vLead = 0;
            }
        }
        // Intelligent Driver Model
        float v = c.speed, b = m_set.comfortDecel, a = m_set.maxAccel;
        float sStar = m_set.minGap + v * m_set.timeHeadway + v * (v - vLead) / (2 * std::sqrt(a * b));
        float acc = a * (1 - std::pow(v / std::max(v0, 0.1f), 4.0f) - (sStar / std::max(gap, 0.1f)) * (sStar / std::max(gap, 0.1f)));
        acc = std::max(acc, -9.0f);
        v = std::max(0.0f, v + acc * dt);
        float ds = v * dt;
        float limit = gap < 1e8f ? std::max(0.0f, gap - 0.5f) : 1e9f;
        if (ds > limit) {
            ds = limit;
            v = std::min(v, limit / dt);
        }
        c.speed = v;
        c.s += ds;
        m_stats.distance += ds;
        if (c.s >= end) {
            if (canGo) {
                Vec3 dir;
                c.jFrom = lanePoint(c.edge, c.forward, end, m_geo[static_cast<size_t>(c.edge)].width * 0.25f, &dir);
                c.jTo = lanePoint(c.nextEdge, c.nextForward, laneStart(c.nextEdge, c.nextForward), m_geo[static_cast<size_t>(c.nextEdge)].width * 0.25f);
                Vec3 np = m_graph.nodes[static_cast<size_t>(node)].position;
                c.jCtrl = (c.nextEdge == c.edge) ? np + dir * 4.0f : (c.jFrom + c.jTo) * 0.25f + np * 0.5f;
                c.jlen = (length(c.jCtrl - c.jFrom) + length(c.jTo - c.jCtrl) + length(c.jTo - c.jFrom)) * 0.5f;
                c.js = 0;
                c.inJunction = true;
                c.arrival = -1;
                if (controlled) ++m_stats.junctionCrossings;
            } else {
                c.s = end;
                c.speed = 0;
            }
        }
        if (!c.inJunction) {
            Vec3 dir;
            c.position = lanePoint(c.edge, c.forward, c.s, m_geo[static_cast<size_t>(c.edge)].width * 0.25f, &dir);
            c.yaw = yawOf(dir);
        }
        speedSum += c.speed;
        if (c.speed < 0.1f) {
            ++m_stats.waiting;
            c.waited += dt;
        } else {
            c.waited = 0;
        }
        bool tooFar = m_set.radius > 0 && length(c.position - focus) > m_set.radius * 1.2f;
        if (c.waited > m_set.stuckSeconds || tooFar) {
            if (!tooFar) {
                ++m_stats.respawned;
                if (std::getenv("FLEDGE_TRAFFIC_DEBUG"))
                    std::fprintf(stderr, "stuck car %d t=%.1f edge %d fwd %d s %.1f/%.1f node %d deg %zu res %d head %d leaderGap %.1f roomNext %.1f next %d/%d\n", c.id,
                                 m_time, c.edge, c.forward, c.s, end, node, m_graph.nodes[static_cast<size_t>(node)].edges.size(),
                                 m_reservation[static_cast<size_t>(node)], leaderGap[i].first == 0, leaderGap[i].second,
                                 laneRoomAtStart(c.nextEdge, c.nextForward, c.id), c.nextEdge, c.nextForward);
                if (std::getenv("FLEDGE_TRAFFIC_DEBUG"))
                    for (const TrafficCar& h : m_cars)
                        if (h.id == m_reservation[static_cast<size_t>(node)])
                            std::fprintf(stderr, "  holder %d edge %d fwd %d s %.1f/%.1f inJ %d js %.1f/%.1f speed %.2f waited %.1f next %d/%d endNode %d\n", h.id, h.edge, h.forward, h.s,
                                         laneEnd(h.edge, h.forward), h.inJunction, h.js, h.jlen, h.speed, h.waited, h.nextEdge, h.nextForward, endNode(h.edge, h.forward));
            }
            placeCar(c, focus, false);
        }
    }
    m_stats.avgSpeed = m_cars.empty() ? 0 : static_cast<float>(speedSum / static_cast<double>(m_cars.size()));
    // safety statistics (after the step)
    std::map<std::pair<int, bool>, std::vector<float>> occ;
    std::map<int, int> inJ;
    for (const TrafficCar& c : m_cars) {
        if (c.inJunction) {
            int node = endNode(c.edge, c.forward);
            if (junctionControlled(node)) ++inJ[node];
        } else {
            occ[{c.edge, c.forward}].push_back(c.s);
        }
    }
    for (auto& [k, n] : inJ)
        if (n > 1) ++m_stats.junctionConflicts;
    for (auto& [k, v] : occ) {
        std::sort(v.begin(), v.end());
        for (size_t j = 1; j < v.size(); ++j) {
            float g = v[j] - v[j - 1] - L;
            m_stats.minGap = std::min(m_stats.minGap, g);
            if (g < 0.5f) ++m_stats.overlaps;
        }
    }
    // pedestrians
    for (Pedestrian& p : m_peds) {
        if (p.edge < 0) continue;
        if (p.down) {
            p.downTime += dt;
            if (p.downTime > 6) p.down = false;
            continue;
        }
        const EdgeGeo& g = m_geo[static_cast<size_t>(p.edge)];
        p.s += p.speed * dt;
        if (p.s >= g.length) {
            int node = endNode(p.edge, p.forward);
            const auto& es = m_graph.nodes[static_cast<size_t>(node)].edges;
            int next = es[rnd(p.rng) % es.size()];
            p.forward = m_graph.edges[static_cast<size_t>(next)].a == node;
            if (next == p.edge && es.size() == 1) p.forward = !p.forward;
            p.edge = next;
            p.s = 0;
        }
        const EdgeGeo& g2 = m_geo[static_cast<size_t>(p.edge)];
        Vec3 dir;
        p.position = lanePoint(p.edge, p.forward, p.s, p.side * (g2.width * 0.5f + g2.sidewalk * 0.5f), &dir);
        p.yaw = yawOf(dir);
        if (m_set.radius > 0 && length(p.position - focus) > m_set.radius * 1.2f) placePed(p, focus);
    }
}

} // namespace fledge::gameplay
