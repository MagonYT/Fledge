#pragma once
// Camera system (docs/GAMEPLAY.md "Cameras"): rig modes that follow a target with smoothing and collision,
// and a sequencer that plays keyframed camera shots (cutscenes).
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/math.h"

namespace fledge::gameplay {

enum class RigMode { Follow = 0, Orbit = 1, FirstPerson = 2, Fixed = 3, TopDown = 4 };
const char* toString(RigMode m);

struct RigParams {
    RigMode mode = RigMode::Follow;
    float distance = 6, height = 2, lookHeight = 1.5f;
    float yaw = 0, pitch = -0.25f;  // radians (orbit / follow input), pitch < 0 looks down
    float smoothing = 8;            // 1/s, 0 = snap
    float minDistance = 1.0f;
};
struct CameraPose {
    Vec3 position;
    Quat rotation;
    float fovDegrees = 60;
};
// Ray from the look-at point back to the camera: returns the free distance (collision), or nullopt if clear.
using CameraCollision = std::function<std::optional<float>(Vec3 from, Vec3 dir, float maxDistance)>;
// Desired pose (before smoothing). targetYaw = the target's facing (radians, 0 = -Z); Follow sits behind it.
CameraPose rigPose(const RigParams& p, Vec3 target, float targetYaw, const CameraCollision& collide = nullptr);
// Frame-rate independent smoothing towards a desired pose.
CameraPose smoothPose(const CameraPose& current, const CameraPose& desired, float smoothing, float dt);
Quat lookRotation(Vec3 from, Vec3 to);

// Sequencer: keyframed shots. {"keys": [{"time": 0, "position": [..], "target": [..] | "rotation": [pitch, yaw, roll deg], "fov": 60}, ...]}
struct SequenceKey {
    float time = 0;
    Vec3 position;
    Quat rotation;
    float fov = 60;
};
class Sequence {
public:
    static Sequence fromJson(const Json& j);
    Json toJson() const;
    float duration() const { return m_keys.empty() ? 0 : m_keys.back().time; }
    bool empty() const { return m_keys.empty(); }
    // Catmull-Rom position, slerped rotation, linear fov.
    CameraPose sample(float t) const;
    const std::vector<SequenceKey>& keys() const { return m_keys; }

private:
    std::vector<SequenceKey> m_keys;
};

} // namespace fledge::gameplay
