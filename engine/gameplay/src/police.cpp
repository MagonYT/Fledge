#include "fledge/gameplay/police.h"

#include <algorithm>
#include <cmath>

namespace fledge::gameplay {

namespace {
float flatDist(Vec3 a, Vec3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }
} // namespace

PoliceResponse policeResponse(int stars, int maxPolice) {
    static const PoliceResponse kByStars[6] = {{0, 0, false}, {2, 0, false}, {2, 1, false}, {3, 2, true}, {4, 3, true}, {4, 4, true}};
    PoliceResponse r = kByStars[std::clamp(stars, 0, 5)];
    int cap = std::max(maxPolice, 0);
    r.onFoot = std::min(r.onFoot, cap);
    r.cars = std::min(r.cars, cap - r.onFoot);
    return r;
}

std::vector<Vec3> roadRoute(const world::RoadGraph& roads, Vec3 from, Vec3 to) {
    if (roads.nodes.empty() || roads.edges.empty()) return {from, to};
    int ea = -1, eb = -1;
    Vec3 pa = roads.closestPoint(from, &ea), pb = roads.closestPoint(to, &eb);
    std::vector<Vec3> route{pa};
    if (ea >= 0 && eb >= 0 && ea != eb) {
        const auto& A = roads.edges[static_cast<size_t>(ea)];
        const auto& B = roads.edges[static_cast<size_t>(eb)];
        auto pathLen = [&](const std::vector<int>& path) {
            float l = 0;
            for (size_t i = 1; i < path.size(); ++i)
                l += length(roads.nodes[static_cast<size_t>(path[i])].position - roads.nodes[static_cast<size_t>(path[i - 1])].position);
            return l;
        };
        std::vector<int> best;
        float bestLen = 1e30f;
        for (int s : {A.a, A.b})
            for (int e : {B.a, B.b}) {
                if (s < 0 || e < 0) continue;
                std::vector<int> path = s == e ? std::vector<int>{s} : roads.shortestPath(s, e);
                if (path.empty()) continue;
                float l = length(roads.nodes[static_cast<size_t>(s)].position - pa) + pathLen(path) + length(roads.nodes[static_cast<size_t>(e)].position - pb);
                if (l < bestLen) {
                    bestLen = l;
                    best = std::move(path);
                }
            }
        for (size_t i = 0; i < best.size(); ++i) {
            if (i == 0) {
                route.push_back(roads.nodes[static_cast<size_t>(best[0])].position);
                continue;
            }
            int u = best[i - 1], v = best[i];
            const world::RoadGraph::Edge* edge = nullptr;
            for (int ei : roads.nodes[static_cast<size_t>(u)].edges) {
                const auto& ed = roads.edges[static_cast<size_t>(ei)];
                if ((ed.a == u && ed.b == v) || (ed.a == v && ed.b == u)) {
                    edge = &ed;
                    break;
                }
            }
            if (edge && edge->polyline.size() >= 2) {
                std::vector<Vec3> poly = edge->polyline;
                if (edge->a != u) std::reverse(poly.begin(), poly.end());
                route.insert(route.end(), poly.begin() + 1, poly.end());
            } else {
                route.push_back(roads.nodes[static_cast<size_t>(v)].position);
            }
        }
    }
    route.push_back(pb);
    route.push_back(to);
    return route;
}

DriveCommand pursuitDrive(PursuitState& st, const PursuitParams& p, const std::vector<Vec3>& route, Vec3 carPos, float carYaw, float speed,
                          Vec3 target, float dt) {
    DriveCommand c;
    Vec3 fwd{-std::sin(carYaw), 0, -std::cos(carYaw)}, right{-fwd.z, 0, fwd.x};
    auto angleTo = [&](Vec3 aim) {
        Vec3 d = aim - carPos;
        return std::atan2(dot(d, right), dot(d, fwd));
    };
    float dTarget = flatDist(target, carPos);
    // waypoint progress + look-ahead point along the route
    Vec3 aim = target;
    if (dTarget > 25 && route.size() >= 2) {
        float look = p.lookAhead + std::max(speed, 0.0f) * 0.5f;
        while (st.next + 1 < route.size() && flatDist(route[st.next], carPos) < look) ++st.next;
        st.next = std::min(st.next, route.size() - 1);
        aim = route[st.next];
    }
    float ang = angleTo(aim);
    if (st.reverseLeft > 0) {  // backing out of a wall / car: steer the other way so the nose swings to the aim
        st.reverseLeft -= dt;
        c.throttle = -0.7f;
        c.steer = ang > 0 ? -1.0f : 1.0f;
        return c;
    }
    c.steer = std::clamp(ang / p.maxSteerRad, -1.0f, 1.0f);
    float vCorner = p.maxSpeed * std::clamp(1.0f - std::fabs(ang) / 1.2f, 0.25f, 1.0f);
    float vArrive = std::max(0.0f, (dTarget - p.stopDistance) * 0.8f);
    float want = std::min({vCorner, vArrive, p.maxSpeed});
    float err = want - speed;
    if (err > 0) c.throttle = std::clamp(err * 0.25f + 0.2f, 0.0f, 1.0f);
    else c.brake = std::clamp(-err * 0.3f, 0.0f, 1.0f);
    if (dTarget <= p.stopDistance) {
        c.throttle = 0;
        c.brake = 1;
    }
    // stuck: pushing but not moving for 1.5 s -> reverse for 1.2 s
    if (c.throttle > 0.3f && std::fabs(speed) < 0.5f) st.stuckFor += dt;
    else st.stuckFor = std::max(0.0f, st.stuckFor - dt);
    if (st.stuckFor > 1.5f) {
        st.stuckFor = 0;
        st.reverseLeft = 1.2f;
    }
    return c;
}

float hitChance(const ShooterParams& p, float distance, float targetSpeed) {
    float t = std::clamp(distance / std::max(p.range, 0.1f), 0.0f, 1.0f);
    float byDistance = 1.0f + (0.15f - 1.0f) * t;
    return std::clamp(p.accuracy * byDistance / (1.0f + std::max(targetSpeed, 0.0f) / 10.0f), 0.0f, 1.0f);
}

ShotResult updateShooter(ShooterState& st, const ShooterParams& p, float dt, bool lineOfSight, float distance, float targetSpeed) {
    ShotResult r;
    st.cooldown = std::max(0.0f, st.cooldown - dt);
    if (!lineOfSight || distance > p.range) {
        st.seenFor = 0;
        st.shotsLeft = 0;
        return r;
    }
    st.seenFor += dt;
    if (st.seenFor < p.reaction || st.cooldown > 0) return r;
    if (st.shotsLeft <= 0) st.shotsLeft = std::max(p.burst, 1);
    --st.shotsLeft;
    st.cooldown = st.shotsLeft > 0 ? p.shotInterval : p.burstPause;
    r.fired = true;
    if (st.rng == 0) st.rng = 1;
    st.rng ^= st.rng << 13;
    st.rng ^= st.rng >> 17;
    st.rng ^= st.rng << 5;
    float roll = static_cast<float>(st.rng >> 8) * (1.0f / 16777216.0f);
    r.hit = roll < hitChance(p, distance, targetSpeed);
    r.damage = r.hit ? p.damage : 0;
    return r;
}

} // namespace fledge::gameplay
