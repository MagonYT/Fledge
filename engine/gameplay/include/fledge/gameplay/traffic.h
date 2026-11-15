#pragma once
// Ambient traffic and pedestrians on the road graph (docs/GAMEPLAY.md "Traffic"). Deterministic for a seed.
// Cars keep to the right-hand lane of each road and follow the car ahead with the Intelligent Driver Model.
// At an intersection a car first checks that its next lane has room, then reserves the junction; only the
// holder may cross. That rules out junction collisions and spill-back gridlock. Pedestrians walk the sidewalks.
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

#include "fledge/core/math.h"
#include "fledge/world/roads.h"

namespace fledge::gameplay {

struct TrafficSettings {
    int cars = 40, pedestrians = 60;
    float radius = 0;        // keep agents within this distance of the focus (m), 0 = the whole graph
    uint32_t seed = 1;
    float carLength = 4.5f, minGap = 2.0f, timeHeadway = 1.2f;
    float maxAccel = 2.5f, comfortDecel = 3.0f;
    float pedestrianSpeed = 1.4f;
    float stuckSeconds = 30;  // a car waiting longer is respawned elsewhere (counted in stats.respawned)
};

struct TrafficCar {
    int id = 0;
    int edge = -1;
    bool forward = true;   // a -> b
    float s = 0;           // distance along the edge in the travel direction
    float speed = 0, desiredSpeed = 10;
    int nextEdge = -1;
    bool nextForward = true;
    bool inJunction = false;
    float js = 0, jlen = 0;  // junction transit
    Vec3 jFrom, jCtrl, jTo;
    float waited = 0;
    double arrival = -1;  // when it reached the head of its junction queue (first come, first served)
    uint32_t rng = 1;
    int colorIndex = 0;
    Vec3 position;
    float yaw = 0;  // radians, 0 = facing -Z
};

struct Pedestrian {
    int id = 0;
    int edge = -1;
    bool forward = true;
    float s = 0, side = 1, speed = 1.4f;
    uint32_t rng = 1;
    bool down = false;  // knocked down (ragdoll-lite); gets up after a while
    float downTime = 0;
    Vec3 position;
    float yaw = 0;
};

struct TrafficStats {
    float minGap = std::numeric_limits<float>::max();  // smallest bumper-to-bumper gap in a lane (m)
    int overlaps = 0;                                   // lane gaps below 0.5 m
    int junctionConflicts = 0;                          // two cars inside one junction (must stay 0)
    int respawned = 0;
    int junctionCrossings = 0;
    double distance = 0;  // metres driven by all cars
    float avgSpeed = 0;   // current mean speed (m/s)
    int waiting = 0;      // cars currently stopped
};

class TrafficSim {
public:
    // obstacle(position, direction, range): distance to something blocking a car (player, props), or a big number.
    using ObstacleFn = std::function<float(Vec3, Vec3, float)>;
    TrafficSim(const world::RoadGraph& graph, const TrafficSettings& settings);

    void step(float dt, Vec3 focus, const ObstacleFn& obstacle = nullptr);
    const std::vector<TrafficCar>& cars() const { return m_cars; }
    const std::vector<Pedestrian>& pedestrians() const { return m_peds; }
    const TrafficStats& stats() const { return m_stats; }
    const TrafficSettings& settings() const { return m_set; }
    // Takes a car out of the traffic (the player carjacked it): it reappears elsewhere, out of the way.
    void respawnCar(int id, Vec3 focus);
    // Pedestrian nearest to a ray (shots): index or -1. maxOffset = how far off the ray still counts.
    int pedestrianOnRay(Vec3 origin, Vec3 dir, float maxDistance, float maxOffset = 0.5f) const;
    // Knocks down pedestrians within radius of p (vehicle hits); returns how many.
    int knockDown(Vec3 p, float radius);
    bool knockDownIndex(int index);
    double time() const { return m_time; }

private:
    struct EdgeGeo {
        std::vector<Vec3> pts;
        std::vector<float> cum;  // cumulative length
        float length = 0, width = 8, sidewalk = 2, speed = 13.9f;
        float startCut = 0, endCut = 0;  // distance kept clear around intersections (a end, b end)
    };
    Vec3 pointAt(int edge, float distFromA, Vec3* tangent = nullptr) const;
    Vec3 lanePoint(int edge, bool forward, float s, float offset, Vec3* dir = nullptr) const;
    float laneStart(int edge, bool forward) const;
    float laneEnd(int edge, bool forward) const;
    int endNode(int edge, bool forward) const { return forward ? m_graph.edges[static_cast<size_t>(edge)].b : m_graph.edges[static_cast<size_t>(edge)].a; }
    bool junctionControlled(int node) const;
    void chooseNext(TrafficCar& c);
    void placeCar(TrafficCar& c, Vec3 focus, bool initial);
    void placePed(Pedestrian& p, Vec3 focus);
    float laneRoomAtStart(int edge, bool forward, int ignoreId) const;
    uint32_t rnd(uint32_t& s) const;
    float rndf(uint32_t& s) const { return static_cast<float>(rnd(s) >> 8) / 16777216.0f; }

    world::RoadGraph m_graph;
    TrafficSettings m_set;
    std::vector<EdgeGeo> m_geo;
    std::vector<int> m_reservation;  // per node: car id holding the junction (0 = free)
    std::vector<TrafficCar> m_cars;
    std::vector<Pedestrian> m_peds;
    TrafficStats m_stats;
    uint32_t m_rng;
    double m_time = 0;
};

} // namespace fledge::gameplay
