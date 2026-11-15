#pragma once
// Police response (docs/GAMEPLAY.md "Police"): how many officers / cruisers each wanted level sends, road routes
// and a pursuit driver for police cars, line-of-sight shooting with reaction time, bursts and distance-based
// accuracy. Plain logic, no scene access (GameWorld applies it); deterministic for a given seed.
#include <cstdint>
#include <vector>

#include "fledge/core/math.h"
#include "fledge/world/roads.h"

namespace fledge::gameplay {

// What a wanted level sends (capped by Wanted.maxPolice, cars count as one unit each).
struct PoliceResponse {
    int onFoot = 0;   // officers on the navmesh
    int cars = 0;     // cruisers on the road graph (two officers each, deployed when they reach you)
    bool armed = false;  // officers shoot (3+ stars)
};
PoliceResponse policeResponse(int stars, int maxPolice);

// Waypoints from `from` to `to` along the road graph: snap onto the nearest road, follow the shortest node path
// (edge polylines in travel direction), leave the road at the end. Straight line when there are no roads.
std::vector<Vec3> roadRoute(const world::RoadGraph& roads, Vec3 from, Vec3 to);

// Pure-pursuit driving for a vehicle following `route` towards `target`: look-ahead steering, cornering and
// arrival speed control, brake to stop `stopDistance` short of the target, and back up when stuck.
struct DriveCommand {
    float throttle = 0, steer = 0, brake = 0, handbrake = 0;  // VehicleComponent inputs (steer +1 = right)
};
struct PursuitParams {
    float maxSpeed = 22;       // m/s (about 80 km/h)
    float stopDistance = 9;    // m short of the target
    float maxSteerRad = 0.6f;  // steering angle at steer = 1
    float lookAhead = 7;       // m (+ 0.5 s of travel)
};
struct PursuitState {
    float stuckFor = 0, reverseLeft = 0;
    size_t next = 0;  // route waypoint being approached
};
// carYaw: radians, 0 = facing -Z (GameWorld convention); speed in m/s (signed, + forwards).
DriveCommand pursuitDrive(PursuitState& st, const PursuitParams& p, const std::vector<Vec3>& route, Vec3 carPos, float carYaw, float speed,
                          Vec3 target, float dt);

// Shooting: an armed officer needs line of sight for `reaction` seconds, then fires bursts. The chance to hit
// falls linearly from `accuracy` at point blank to 15 % of it at `range`, and halves against a target moving
// at 10 m/s (cars, sprinting).
struct ShooterParams {
    float range = 35;          // m
    float reaction = 0.7f;     // s of sight before the first shot
    float shotInterval = 0.35f;
    int burst = 3;
    float burstPause = 1.4f;
    float accuracy = 0.7f;     // at 0 m against a standing target
    float damage = 9;          // per hit
};
struct ShooterState {
    float seenFor = 0, cooldown = 0;
    int shotsLeft = 0;
    uint32_t rng = 1;
};
struct ShotResult {
    bool fired = false, hit = false;
    float damage = 0;
};
float hitChance(const ShooterParams& p, float distance, float targetSpeed);
ShotResult updateShooter(ShooterState& st, const ShooterParams& p, float dt, bool lineOfSight, float distance, float targetSpeed);

} // namespace fledge::gameplay
