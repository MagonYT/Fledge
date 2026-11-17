#pragma once
// Private: GameWorld's gameplay runtime state (game_world_gameplay.cpp).
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fledge/framework/game_world.h"
#include "fledge/gameplay/camera.h"
#include "fledge/gameplay/environment.h"
#include "fledge/gameplay/mission.h"
#include "fledge/gameplay/nav.h"
#include "fledge/gameplay/particles.h"
#include "fledge/gameplay/police.h"
#include "fledge/gameplay/widgets.h"
#include "fledge/gameplay/rpg.h"
#include "fledge/gameplay/traffic.h"
#include "fledge/world/roads.h"

namespace fledge {

struct GameWorld::GameplayState {
    world::RoadGraph roads;
    bool roadsBuilt = false;
    std::unique_ptr<gameplay::TrafficSim> traffic;
    std::vector<EntityId> trafficCars, trafficPeds;
    std::unique_ptr<gameplay::NavMesh> nav;
    bool navTried = false;
    std::string navError;
    gameplay::WantedState wanted;
    gameplay::Factions factions = gameplay::Factions::defaults();
    std::map<EntityId, gameplay::MissionRunner> missions;
    float camYaw = 0, camPitch = -0.2f;
    bool camInit = false;
    gameplay::CameraPose camPose;
    std::map<EntityId, gameplay::CameraPose> rigPoses;
    std::map<EntityId, float> seqTime;
    struct Agent {
        std::vector<Vec3> path;
        size_t next = 0;
        float repath = 0, arrestTimer = 0;
        uint32_t rng = 1;
        gameplay::ShooterState shooter;  // police (3+ stars)
        bool shooterSeeded = false;
    };
    std::map<EntityId, Agent> agents;
    std::vector<EntityId> police;  // officers on foot (spawned + deployed from cars)
    float policeSpawnCooldown = 0;
    struct Cruiser {
        gameplay::PursuitState pursuit;
        std::vector<Vec3> route;
        float repath = 0;
        bool deployed = false;  // officers got out; the car stays parked (and can be stolen)
    };
    std::vector<EntityId> policeCars;
    std::map<EntityId, Cruiser> cruisers;
    float policeCarCooldown = 0;
    int policeSerial = 0, policeCarSerial = 0;
    int shotsAtPlayer = 0, hitsOnPlayer = 0;
    Vec3 lastPlayerPos{0, 0, 0};
    bool hasLastPlayerPos = false;
    float playerSpeed = 0;  // m/s (aiming penalty)
    float time = 0, lastShotCrime = -100, lastShot = -100;
    std::string bigMessage;
    float bigMessageLeft = 0;
    Vec4 bigColor{1, 1, 1, 1};
    std::string toast;
    float toastLeft = 0;
    float deadTimer = -1;
    int weatherPreset = -1;
    gameplay::Weather weatherFrom, weatherTo, weatherNow;
    float weatherT = 1;
    audio::VoiceId rainVoice = 0;
    bool envSaved = false;
    float baseEnvIntensity = 1, baseExposure = 0;
    RenderSettings baseFog;  // the project's fog fields, restored at the end (weather drives fog while playing)
    float skyBrightness = 1;  // sky light scale this frame (weather particles)
    gameplay::HydrologyConfig hydrologyConfig;
    gameplay::HydrologyState hydrology;  // transient rain storage; reset each play session
    gameplay::TerrainWater terrainWater;
    EntityId waterTerrain = kNullEntity;
    bool waterTerrainTried = false;
    Mat4 waterTerrainMatrix;
    double weatherRemainder = 0;         // fixed 60 Hz weather/hydrology clock
    std::map<EntityId, gameplay::ParticleEmitter> emitters;
    std::map<std::string, std::optional<gameplay::UiDocument>> uiDocs;  // UiScreen documents by ref (nullopt = failed)
    bool pointerWasDown = false;
    float uiAnimAt = -1;
    gameplay::Precipitation rain;
    std::map<EntityId, float> pickupRespawn;  // hidden pickups -> seconds until they return
    std::map<EntityId, Vec3> pickupHome;
    float speedKmh = 0;
    double ms = 0;
};

} // namespace fledge
