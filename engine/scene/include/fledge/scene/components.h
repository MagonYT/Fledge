#pragma once
// Built-in components. Each is a plain struct; reflection is registered in
// components.cpp. Component type names (used in .fscene and MCP) are the
// strings in quotes below.
#include <string>

#include "fledge/core/json.h"
#include "fledge/core/math.h"

namespace fledge {

// "Name"
struct NameComponent {
    std::string name = "Entity";
};

// "Transform"
struct TransformComponent {
    Vec3 position{0, 0, 0};
    Quat rotation{};
    Vec3 scale{1, 1, 1};
    // Bone socket (1.2.1, docs/ANIMATION.md "Bone sockets"): when set, the entity follows this bone of its PARENT's Animator.
    // position / rotation / scale are then an offset in the bone's space: world = parentWorld * boneModel * local.
    // Unknown bone or a parent without an Animator: warned once, the parent transform is used. Saved only when set.
    std::string attachBone;
    Transform toTransform() const { return {position, rotation, scale}; }
    Mat4 matrix() const { return toTransform().matrix(); }
};

// "MeshRenderer"  mesh: "builtin:cube|plane|sphere", a content path or an asset GUID.
// material: .fmat GUID/path. When set it defines the look; otherwise the
// legacy color/texture fields are used (scenes written before materials).
struct MeshRendererComponent {
    std::string mesh = "builtin:cube";
    Vec3 color{0.8f, 0.8f, 0.8f};
    std::string texture;  // legacy: texture GUID/path used when material is empty
    bool visible = true;
    std::string material;
    float lodBias = 0;           // > 0 switches to coarser LODs earlier (per asset/instance)
    int forceLod = -1;           // >= 0 pins a LOD (debug / art direction)
    float maxDrawDistance = 0;   // distance culling (m), 0 = unlimited
    // 1.3.1 per-mesh shadows (docs/GRAPHICS.md "Per-mesh shadows"); the defaults render exactly as before.
    bool castShadows = true;     // false: left out of every shadow map (first-person viewmodels)
    bool shadowOnly = false;     // true: drawn into the shadow maps only, not into the camera view (see ownerCamera)
    std::string ownerCamera;     // shadowOnly applies to the view of this Camera entity (name); "" = every game camera
    // 1.3.2: first-person viewmodel mesh (arms, weapon): drawn in the game camera's viewmodel pass (own FOV, never clips into
    // walls), casts no world shadow. The editor's free camera draws it as an ordinary mesh.
    bool viewmodel = false;
    bool isStatic = true;  // false: a mover. Static meshes can stay in a cached shadow cascade until they move.
};

// A cloth surface. With no asset, a rectangular grid is pinned at the two corners of its first row.
// A .fcloth asset supplies the surface and named pin groups (1.3.5).
struct ClothComponent {
    bool enabled = true;
    int columns = 12, rows = 12;
    float spacing = 0.2f;
    float damping = 0.1f;
    float gravityScale = 1.0f;
    int iterations = 5;
    std::string asset;     // Content-relative .fcloth; empty = the grid above
    std::string pinGroup;  // named group in the asset; empty = the asset's default pins
};

// "Camera"
struct CameraComponent {
    float fovDegrees = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 500.0f;
    bool primary = true;
    // 1.3.2 viewmodel pass (docs/GRAPHICS.md "Viewmodel"): MeshRenderer / Animator with viewmodel = true are drawn in this
    // camera's view with their own projection, in front of the world. 0 = the camera's fovDegrees (they still never clip).
    float viewmodelFov = 0;
    float viewmodelNear = 0.01f;
    float viewmodelFar = 20.0f;
};

enum class LightType : int { Directional = 0, Point = 1, Spot = 2 };

// "Light"  directional and spot lights shine along the entity's forward (-Z) axis.
// Shadows: directional (cascaded) and spot lights; point-light shadows are not implemented (stub).
struct LightComponent {
    LightType type = LightType::Directional;
    Vec3 color{1, 1, 1};
    float intensity = 1.0f;
    float range = 10.0f;
    float innerConeDegrees = 20.0f;  // spot (half angle)
    float outerConeDegrees = 30.0f;  // spot (half angle)
    bool castShadows = false;
    float shadowBias = 0.0005f;  // constant depth bias
    float normalBias = 1.0f;     // receiver normal offset in shadow texels
};

// "Rotator"  built-in behaviour: spins the entity at a constant rate.
struct RotatorComponent {
    Vec3 axis{0, 1, 0};
    float degreesPerSecond = 45.0f;
};

// "Script"  binds the entity to a gameplay module (C-ABI DLL) by name.
struct ScriptComponent {
    std::string module;
    std::string behaviour;  // module-defined behaviour name
    float param = 1.0f;     // free parameter passed to the behaviour
};

// "Behaviours"  Fledge Script behaviours attached to the entity (see docs/FLEDGE_SCRIPT.md):
// [{"class": "Spinner", "enabled": true, "properties": {"degreesPerSecond": 90}}, ...]
// The scene file is the source of truth in edit mode; while playing, the engine
// creates one script object per entry and applies "properties" over the class defaults.
struct BehavioursComponent {
    Json list = Json::array();
};

// "Tags"  space-separated tags, queried by scripts (Scene::FindAllWithTag).
struct TagsComponent {
    std::string tags;
};


// ---------------------------------------------------------------- open world (v1.0, see docs/WORLD_STREAMING.md)
// "WorldPartition"  put it on one entity (e.g. "World Settings") to make the scene an open world: it is stored /
// cooked as a grid of chunks and streamed around the camera while playing.
struct WorldPartitionComponent {
    float cellSize = 128.0f;         // chunk edge (m)
    float loadRadius = 384.0f;       // chunks whose bounds come within this distance load
    float unloadRadius = 512.0f;     // ... and unload beyond this one (hysteresis)
    float memoryBudgetMB = 256.0f;   // loaded chunk data cap (least recently needed chunks are evicted)
    float frameBudgetMs = 2.0f;      // main-thread time per frame for applying loaded chunks
    float rebaseDistance = 2048.0f;  // floating origin: re-centre when the camera is this far from the origin
    std::string activeLayers;        // space-separated data layers that are on (e.g. "day mission1")
    float hlodRadius = 0;            // draw merged proxy meshes (HLOD) of unloaded chunks up to here; 0 = 3 x unloadRadius
};

// "Streaming"  per-entity streaming policy (entities without it are streamed by position).
struct StreamingComponent {
    bool alwaysLoaded = false;  // never streamed out (e.g. skybox, global managers)
    bool persistent = false;    // player, mission objects: survive chunk unloads and are saved
    std::string layer;          // data layer: loaded only when the layer is active ("" = always)
};

// "Terrain"  heightmap terrain centred on the entity, tiled into LOD tiles, splat-painted in 4 layers.
struct TerrainComponent {
    float size = 256.0f;      // edge length (m)
    int resolution = 129;     // height samples per edge
    float maxHeight = 40.0f;  // generator / UI scale
    int seed = 0;             // > 0: procedural hills while heights is empty
    std::string heights;      // [data] base64 float32 heights (resolution^2), edited by the terrain tools
    std::string splat;        // [data] base64 RGBA8 layer weights (resolution^2)
    Vec3 layer0{0.30f, 0.45f, 0.18f}, layer1{0.45f, 0.38f, 0.28f}, layer2{0.50f, 0.50f, 0.50f}, layer3{0.85f, 0.80f, 0.65f};
    int tileSize = 32;        // quads per tile at LOD 0
};

// "Spline"  a Catmull-Rom spline; kind "road" generates a road + sidewalks mesh and joins the road graph.
struct SplineComponent {
    Json points = Json::array();  // [[x,y,z], ...] local space
    bool closed = false;
    std::string kind = "road";    // "road" | "path" | "rail" (only roads generate meshes)
    float width = 8.0f;           // carriageway width (m)
    float sidewalk = 2.0f;        // each side (m), 0 = none
    int lanes = 2;
    float speedLimit = 13.9f;     // m/s (50 km/h), used by traffic
    float sidewalkTrim = 0;       // m kept free of sidewalks / markings at both ends (intersections)
    bool operator==(const SplineComponent&) const = default;  // change detection (road mesh cache)
};

// "Foliage"  deterministic scatter of instanced meshes over an area (streamed with its chunk).
struct FoliageComponent {
    std::string mesh = "builtin:sphere";
    std::string material;
    Vec3 color{0.2f, 0.5f, 0.2f};
    float area = 128.0f;     // square edge centred on the entity (m)
    float density = 2.0f;    // instances per 100 m2
    float minScale = 0.6f, maxScale = 1.4f;
    int seed = 1;
    bool alignToTerrain = true;
    bool operator==(const FoliageComponent&) const = default;  // change detection (scatter cache)
};


// "Animator"  an animated character: skinned mesh + state machine (docs/ANIMATION.md). Scripts drive it through
// params / play (Fledge Script: fledge::script::Animator); state and events are written back by the engine every update.
struct AnimatorComponent {
    std::string character = "builtin:mannequin";  // .fchar asset (imported glTF / BVH) or the built-in mannequin
    Json graph = Json::object();                  // state machine; empty = default locomotion (Idle / Move / Jump / Wave)
    Json params = Json::object();                 // {"speed": 1.4, "jump": true} - triggers are consumed
    std::string play;                             // request: jump straight to this state (consumed)
    float crossFade = 0.2f;
    float speed = 1.0f;
    bool rootMotion = true;                       // move the entity by the clip's root motion
    bool footIK = false;                          // plant the feet on terrain / ground
    std::string lookAt;                           // entity name the head turns to ("" = off)
    Vec3 color{0.78f, 0.74f, 0.70f};
    std::string material;
    bool crowd = false;                           // share poses with other crowd agents (instanced)
    bool preview = false;                         // editor: animate while not playing
    bool ragdoll = false;                         // physics takes over the skeleton (capsules + joints)
    bool visible = true;
    bool castShadows = true;                      // 1.3.1: false = no shadow (first-person arms / weapon)
    bool shadowOnly = false;                      // 1.3.1: shadow maps only, hidden in the camera view (full-body player)
    std::string ownerCamera;                      // 1.3.1: shadowOnly only in this Camera's view ("" = every game camera)
    bool viewmodel = false;                       // 1.3.2: first-person viewmodel (see MeshRendererComponent::viewmodel)
    std::string state;                            // [out] current state
    Json events = Json::array();                  // [out] events fired during the last update
    // 1.3.2 layers at run time (docs/ANIMATION.md "Layers at run time"); the defaults change nothing.
    // layerPlay: requests, consumed by the next update: [{"layer": "UpperBody" | 1, "state": "Reload", "fade": 0.1, "speed": 1.3}]
    //   (fade default crossFade; speed > 0 scales that layer's clock while it stays in the state).
    // layerSpeeds / layerWeights: {"UpperBody": 1.5} by layer name or index; speeds multiply `speed` for that layer only
    // (1 = normal, any layer incl. the base), weights replace the graph weight of a layer > 0 (-1 or a removed key = graph).
    Json layerPlay = Json::array();
    Json layerSpeeds = Json::object();
    Json layerWeights = Json::object();
    Json layers = Json::array();                  // [out] [{"index", "name", "state", "time", "weight", "speed", "inTransition"}]
    std::string layerError;                       // [out] why the last layer request / name was refused ("" = none)
    // 1.3.2 Fledge Advanced (docs/ANIMATION.md "Leader pose", "Multi-material characters"); Fledge loads and ignores them.
    // leaderPose: name of another Animator entity whose evaluated pose this one uses (bones mapped by name); this Animator then
    // runs no state machine of its own (a character split into many skinned parts costs one evaluation).
    std::string leaderPose;
    std::string leaderStatus;                     // [out] "" off, else "following <leader>: N of M bones" or why not
    // materials: one per material slot of a multi-material .fchar ("" = the slot's own material from the file); empty =
    // every slot uses its file material, or `material` when the file has none.
    Json materials = Json::array();
};


// ---------------------------------------------------------------- physics (v1.0, see docs/PHYSICS.md)
enum class MotionType : int { Static = 0, Dynamic = 1, Kinematic = 2 };
enum class ColliderShape : int { Box = 0, Sphere = 1, Capsule = 2, ConvexHull = 3, Mesh = 4 };
enum class JointType : int { Fixed = 0, Hinge = 1, Distance = 2, Point = 3 };

// "RigidBody"  simulated body (needs a Collider). force / impulse are consumed each step; velocity is in/out.
struct RigidBodyComponent {
    MotionType motion = MotionType::Dynamic;
    float mass = 0;  // kg, 0 = from shape volume (density 1000)
    float friction = 0.6f, restitution = 0.1f;
    float linearDamping = 0.05f, angularDamping = 0.05f, gravityScale = 1.0f;
    Vec3 velocity{0, 0, 0}, angularVelocity{0, 0, 0};
    Vec3 force{0, 0, 0}, impulse{0, 0, 0};
    bool lockRotation = false;
    bool ccd = false;  // continuous collision detection (fast objects)
};

// "Collider"  shape; with no RigidBody it is a static collider. Triggers report enter/exit without collision response.
struct ColliderComponent {
    ColliderShape shape = ColliderShape::Box;
    Vec3 size{1, 1, 1};    // box full extents (entity scale applies)
    float radius = 0.5f;   // sphere / capsule
    float height = 1.0f;   // capsule cylinder height
    Vec3 offset{0, 0, 0};
    bool isTrigger = false;
    std::string mesh;      // ConvexHull / Mesh: asset ref ("" = the entity's MeshRenderer mesh)
    // 1.3.2 collision layer 0..31 for query filters (Fledge Script Physics::Raycast / SphereCast layer masks, MCP raycast
    // layerMask, docs/PHYSICS.md "Collision layers and query filters"). Queries only: the simulation ignores it.
    int layer = 0;
};

// "CharacterController"  kinematic capsule (slopes, steps, moving platforms). move = desired horizontal velocity.
struct CharacterControllerComponent {
    float height = 1.8f, radius = 0.3f;
    float maxSlope = 45.0f, stepHeight = 0.35f;
    float jumpSpeed = 5.0f;
    Vec3 move{0, 0, 0};    // in (world m/s, horizontal)
    bool jump = false;     // in (consumed)
    bool grounded = false; // out
    Vec3 velocity{0, 0, 0};  // out
    bool driveAnimator = true;  // write the horizontal speed into the Animator's "speed" param (and "jump")
    bool enabled = true;        // false: no capsule in the simulation (e.g. the player sits in a car)
    // 1.3.1 collision filter (docs/PHYSICS.md "Character controller filters"); the defaults collide with everything as before.
    Json ignore = Json::array();     // entity names whose bodies (and their children's) the capsule passes through
    bool collideWithDynamic = true;  // false: dynamic RigidBodies neither block nor push the capsule (props, debris)
};

// "Vehicle"  wheeled vehicle on a RigidBody chassis (Collider box = chassis). Inputs in, telemetry out.
struct VehicleComponent {
    std::string preset = "car";  // car | truck (4 wheels, 4WD, automatic gearbox) | bike (2 wheels, rear drive, leans)
    float throttle = 0, steer = 0, brake = 0, handbrake = 0;  // in: -1..1 / 0..1
    float maxEngineTorque = 500, maxRpm = 6000;
    float wheelRadius = 0.38f, wheelWidth = 0.25f, suspension = 0.25f;
    float speedKmh = 0, rpm = 0;  // out
    int gear = 0;                 // out
    float health = 100;           // damage-lite: hard impacts reduce it
    std::string driver;           // entity name of the driver ("" = empty)
    bool engineSound = true;      // procedural engine sound (pitch follows rpm) while playing
};

// "Joint"  constraint to another entity (target "" = the world).
struct JointComponent {
    JointType type = JointType::Hinge;
    std::string target;
    Vec3 anchor{0, 0, 0};  // local to this entity
    Vec3 axis{0, 1, 0};    // hinge axis (local)
    float minDistance = 0, maxDistance = 1;
    float breakForce = 0;  // N, 0 = unbreakable
    bool broken = false;   // out
};


// ---------------------------------------------------------------- audio (v1.0, see docs/AUDIO.md)
enum class AudioBus : int { Music = 0, Sfx = 1, Ambient = 2, Voice = 3, UI = 4 };  // audio::Bus = value + 1

// "AudioSource"  plays a clip (WAV / MP3 / FLAC / Ogg asset or builtin:beep|click|noise|wind). 3D when spatial.
struct AudioSourceComponent {
    std::string clip;
    AudioBus bus = AudioBus::Sfx;
    float volume = 1, pitch = 1;
    bool loop = false;
    bool playOnStart = true;
    bool spatial = true;
    float minDistance = 2, maxDistance = 60;
    bool stream = false;   // decode while playing (long music / ambience) instead of loading the whole clip
    bool play = false;     // in: start (consumed)
    bool stop = false;     // in: stop (consumed)
    bool playing = false;  // out
    bool oneShot = false;  // destroy the entity when the sound ends (Audio::PlayOneShot)
    float reverb = 1;      // send into the reverb of the zone the listener is in (0..1, spatial sources)
    bool occlusion = true; // muffled behind walls (physics rays between listener and source)
    float occluded = 0;    // [out] current occlusion 0..1
};

enum class ReverbPreset : int { Room = 0, Hall = 1, Cave = 2, Tunnel = 3, Garage = 4 };
// "ReverbZone"  box (entity transform, `size` = full extents) where spatial sounds get reverb while the listener is
// inside; the effect fades in over `fade` meters from the walls. The strongest zone wins.
struct ReverbZoneComponent {
    ReverbPreset preset = ReverbPreset::Room;
    Vec3 size{10, 4, 10};
    float fade = 2;
    float mix = 1;  // scales the preset's wet level (0..2)
};

// "AudioListener"  the ears (default: the primary camera, or the editor camera).
struct AudioListenerComponent {
    bool active = true;
};

// "AudioMixer"  bus volumes and ducking (one per scene, usually on World Settings).
struct AudioMixerComponent {
    float master = 1, music = 0.8f, sfx = 1, ambient = 0.8f, voice = 1, ui = 1;
    bool duckMusicUnderVoice = true;
    float duckDb = -12;
};


// ---------------------------------------------------------------- gameplay (v1.0, see docs/GAMEPLAY.md)
// "PlayerController"  built-in third-person player: walks / runs (CharacterController), drives vehicles
// (Interact = enter / exit, also takes traffic cars), shoots (Fire, needs a weapon), drives the camera.
struct PlayerControllerComponent {
    float walkSpeed = 2.2f, runSpeed = 6.5f;
    std::string camera;             // camera entity name ("" = the primary camera)
    float cameraDistance = 5.5f, cameraHeight = 2.2f;
    float mouseSensitivity = 0.0035f;
    float enterRadius = 4.0f;
    bool defaultHud = true;         // minimap, health, money, wanted stars, objective, speedometer, crosshair
    std::string vehicle;            // [out] vehicle being driven ("" = on foot)
    Vec3 spawnPoint{0, 0, 0};       // respawn after death / arrest (set from the start position)
};

// "Health"  hit points and faction (player, civilian, police, gang ...).
struct HealthComponent {
    float max = 100, current = 100;
    std::string faction = "civilian";
    bool dead = false;  // [out]
};

// "Inventory"  money, items {"name": count}, weapon ammo {"pistol": 24} and the equipped weapon.
struct InventoryComponent {
    int money = 0;
    Json items = Json::object();
    Json ammo = Json::object();
    std::string weapon;
};

// "Pickup"  collected when the player walks / drives through it (money, item, weapon + ammo).
struct PickupComponent {
    int money = 0;
    std::string item;
    int count = 1;
    std::string weapon;
    int ammo = 0;
    float radius = 1.5f;
    float respawnSeconds = 0;  // 0 = once
};

// "Wanted"  on the player: crimes raise stars, police come; lose them by staying out of sight.
struct WantedComponent {
    bool enabled = true;
    int maxPolice = 8;
    int level = 0;      // [out] stars 0..5
    float heat = 0;     // [out]
    int setLevel = -1;  // in: force the stars (scripts, consumed)
};

// "Traffic"  ambient cars + pedestrians on the scene's road splines (one per scene, e.g. on World Settings).
struct TrafficComponent {
    bool enabled = true;
    int cars = 30, pedestrians = 50;
    float radius = 0;   // keep agents near the player (m), 0 = the whole road network
    int seed = 1;
};

// "Mission"  a mission graph (docs/GAMEPLAY.md); edited in the Missions panel.
struct MissionComponent {
    Json graph = Json::object();
    bool autoStart = true;
    bool start = false;          // in: (re)start (consumed)
    std::string state = "Idle";  // [out] Idle | Running | Succeeded | Failed
    std::string objective;       // [out]
};

// "TimeOfDay"  sun / moon, light colour, sky brightness from the hour.
struct TimeOfDayComponent {
    float hour = 10;
    float dayMinutes = 24;  // real minutes per game day, 0 = time stands still
    float latitude = 40;
    std::string sun;        // directional light entity name ("" = the first directional light)
};

enum class WeatherPreset : int { Clear = 0, Cloudy = 1, Overcast = 2, Rain = 3, Storm = 4, Fog = 5 };
// "Weather"  clouds dim the light, haze / rain add height fog, rain falls (particles) and plays rain sound;
// changes blend over `transition` seconds.
struct WeatherComponent {
    WeatherPreset preset = WeatherPreset::Clear;
    float transition = 10;
};

enum class ParticlePreset : int { Custom = 0, Smoke = 1, Fire = 2, Sparks = 3, Dust = 4, Steam = 5, Rain = 6, Snow = 7 };
enum class ParticleShape : int { Point = 0, Sphere = 1, Box = 2 };
// "ParticleEmitter"  CPU particles drawn as camera-facing sprites (docs/GAMEPLAY.md "Particles"). A preset other
// than Custom supplies every setting below except enabled / burst; Custom uses the fields. Runs while playing.
struct ParticleEmitterComponent {
    bool enabled = true;
    ParticlePreset preset = ParticlePreset::Smoke;
    float rate = 20;          // particles / s
    int maxParticles = 500;
    float lifetime = 2;       // s
    Vec3 direction{0, 1, 0};  // local emission axis
    float speed = 2, spread = 20;  // m/s, cone half-angle (deg)
    ParticleShape shape = ParticleShape::Point;
    Vec3 extent{0.5f, 0.5f, 0.5f};  // sphere radius (x) / box half size
    float gravity = 0, drag = 0;    // x 9.81 m/s^2 down (negative rises), 1/s towards the wind
    float startSize = 0.2f, endSize = 0.6f;
    Vec3 startColor{1, 1, 1}, endColor{1, 1, 1};
    float startOpacity = 1, endOpacity = 0;
    float intensity = 1, stretch = 0;
    bool additive = false, worldSpace = true;
    int burst = 0;       // in: spawn this many now (consumed)
    int count = 0;       // [out] alive particles
};

enum class HudType : int { Text = 0, Panel = 1, Bar = 2, Minimap = 3, Objective = 4, Wanted = 5, Money = 6, Health = 7, Speedometer = 8, Crosshair = 9, Clock = 10, Subtitle = 11 };
enum class HudAnchor : int { TopLeft = 0, Top = 1, TopRight = 2, Left = 3, Center = 4, Right = 5, BottomLeft = 6, Bottom = 7, BottomRight = 8 };
// 1.3.2: which edge / centre of an element sits on its anchor point (anchor + offset). Auto = the anchor's own rule (left
// anchors: left edge, centre anchors: centre, right anchors: right edge; the 1.3.1 placement).
enum class HudAlign : int { Auto = 0, Left = 1, Center = 2, Right = 3 };
enum class HudVAlign : int { Auto = 0, Top = 1, Middle = 2, Bottom = 3 };
// "HudElement"  an in-game UI element. Text accepts {money} {health} {wanted} {objective} {speed} {hour} {ammo} {weapon}.
struct HudElementComponent {
    HudType type = HudType::Text;
    HudAnchor anchor = HudAnchor::TopLeft;
    Vec3 offset{20, 20, 0};   // pixels from the anchor (x, y)
    Vec3 size{200, 24, 0};    // pixels (bars, panels, minimap)
    std::string text;
    Vec3 color{1, 1, 1};
    float opacity = 1, textSize = 2;
    std::string binding;      // Bar: "Entity/Component.field" over "Entity/Component.max field" e.g. "Player/Health.current/max"
    bool visible = true;
    // 1.3.2 alignment on the anchor point: align Center + verticalAlign Middle centres a text marker on offset (e.g. a world
    // position projected to the screen with anchor TopLeft). Auto keeps the 1.3.1 placement.
    HudAlign align = HudAlign::Auto;
    HudVAlign verticalAlign = HudVAlign::Auto;
};

// "Decal"  1.3.2 Fledge Advanced projected decal (docs/GRAPHICS.md "Decals"): `texture` / `material` projected along the
// entity's -Z onto opaque MeshRenderer geometry inside a box of `size` (width x, height y, depth z) centred on the entity.
// The receivers keep the decal when they move (it is built in their space). Fledge (Standard) loads it and draws nothing.
struct DecalComponent {
    std::string material;            // .fmat (base colour texture with alpha, colour, roughness, metallic); wins over texture
    std::string texture;             // texture GUID / path (alpha = coverage), or builtin:decal_bullet | builtin:decal_blood
    Vec3 color{1, 1, 1};
    float opacity = 1;
    Vec3 size{0.25f, 0.25f, 0.2f};   // m
    float angleFade = 70;            // degrees between surface normal and projection axis where the decal has faded out
    float lifetime = 0;              // s while playing, 0 = forever
    float fadeOut = 0.5f;            // s at the end of the lifetime
    int sortOrder = 0;               // higher draws over lower
    int layerMask = -1;              // receivers: bit n = the receiving entity's Collider.layer n (no collider = layer 0)
    bool visible = true;
    float age = 0;                   // [out] s since Play (with a lifetime)
};

// "UiScreen"  a retained UI document (.fui JSON under Content/: panels, stacks, text, buttons, bars) drawn over the
// game while playing. Text "@key" is localised; {money} etc. are HUD bindings. Buttons react to the mouse and to
// UiUp / UiDown / UiAccept (arrows, Enter, d-pad, A); the last activated button id lands in `clicked`.
struct UiScreenComponent {
    std::string document;      // e.g. "UI/pause.fui"
    bool visible = true;
    bool interactive = true;   // mouse + focus navigation
    std::string clicked;       // [out] last activated button id (scripts may clear it)
    int clicks = 0;            // [out] activations so far
    std::string focus;         // [out] focused button id
};

enum class CameraRigMode : int { Follow = 0, Orbit = 1, FirstPerson = 2, Fixed = 3, TopDown = 4 };
// "CameraRig"  on a camera: follows `target` (when no PlayerController drives the camera).
struct CameraRigComponent {
    CameraRigMode mode = CameraRigMode::Follow;
    std::string target;
    float distance = 6, height = 2.5f, lookHeight = 1.5f, smoothing = 8;
    bool collision = true;
};

// "Sequence"  keyframed camera shots (cutscene) for a camera entity.
struct SequenceComponent {
    Json keys = Json::array();  // [{"time", "position", "target" | "rotation", "fov"}]
    bool playOnStart = false, loop = false;
    bool play = false;          // in (consumed)
    bool playing = false;       // [out]
    float time = 0;             // [out]
};

// "NavAgent"  walks the navigation mesh to `target` (entity) or `destination`; police chase the player.
struct NavAgentComponent {
    float speed = 3.5f, arriveRadius = 1.5f;
    std::string target;
    Vec3 destination{0, 0, 0};
    bool hasDestination = false;
    bool wander = false;
    bool arrived = false;  // [out]
};

// "NetIdentity"  marks an entity as replicated by a fledge_net server (docs/NETWORKING.md). The server
// entity id is the network id. Only properties registered with .replicated() are sent.
struct NetIdentityComponent {
    int owner = 0;               // client id that controls it (0 = the server)
    float relevancyRadius = 0;   // > 0: only sent to clients whose player is within this distance (m); 0 = always
};

// "NetPlayer"  a client-controlled character: moved by the client's input commands on the server
// (authoritative) and predicted on the owning client (net::defaultPlayerSim).
struct NetPlayerComponent {
    Vec3 velocity{0, 0, 0};
    float moveSpeed = 5.0f;
    float jumpSpeed = 6.0f;
    float groundHeight = 0.5f;   // lowest y the entity position can reach (feet on a y = 0 floor)
};

class ComponentRegistry;
void registerBuiltinComponents(ComponentRegistry& registry);  // called by the registry constructor

} // namespace fledge
