#pragma once
// Physics (docs/PHYSICS.md): Jolt Physics 5.2 behind a small engine API. Components drive it:
// RigidBody + Collider (box / sphere / capsule / convex hull / mesh), CharacterController (Jolt CharacterVirtual:
// slopes, steps, moving platforms), Vehicle (wheeled: engine, gearbox, suspension, steering, brakes; presets
// car / truck / bike), Joint (fixed / hinge / distance / point), Animator.ragdoll (capsule per bone + swing-twist
// joints). Terrains and roads get static colliders automatically. Deterministic fixed steps; the whole state
// can be saved / restored (Time Machine). Bodies follow entities: streamed-in entities get bodies, removed
// entities lose them.
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fledge/core/math.h"
#include "fledge/core/mesh_data.h"
#include "fledge/core/utility.h"
#include "fledge/scene/components.h"
#include "fledge/scene/scene.h"

namespace fledge::physics {

struct RayHit {
    EntityId entity = kNullEntity;
    Vec3 point, normal;
    float distance = 0;
};

enum class ContactKind { Enter = 0, Exit = 1, TriggerEnter = 2, TriggerExit = 3 };
struct ContactEvent {
    EntityId a = kNullEntity, b = kNullEntity;
    ContactKind kind = ContactKind::Enter;
    Vec3 point;
    float speed = 0;  // relative approach speed (m/s) at enter
};

struct Providers {
    std::function<const MeshData*(const std::string& ref)> mesh;           // MeshRenderer / Collider meshes
    // Terrain heightfield (local space, res x res samples over size x size m, centred): Jolt HeightFieldShape.
    std::function<bool(EntityId terrain, std::vector<float>& heights, int& res, float& size)> terrainHeights;
    std::function<MeshData(EntityId spline)> roadMesh;                     // road + sidewalks (local space), empty = none
    std::function<const std::vector<Mat4>*(EntityId)> bonePose;            // ragdoll: model-space bones of an Animator
    std::function<const void*(EntityId)> skeleton;                         // ragdoll: const anim::Skeleton*
    // .fcloth file text for ClothComponent::asset. Empty optional = the file is missing.
    std::function<std::optional<std::string>(const std::string& asset)> clothText;
};

// A .fcloth document: a grid or an explicit triangle surface, plus named pin groups.
// Vertex count is capped at 48 x 48. Pinned vertices stay fixed (inverse mass 0).
struct ClothAsset {
    MeshData mesh;
    std::vector<int> pins;                                  // default group
    std::map<std::string, std::vector<int>> groups;        // extra named groups
    float damping = 0.1f;
    float gravityScale = 1;
    int iterations = 5;
    bool hasDamping = false, hasGravity = false, hasIterations = false;
    std::vector<int> pinsFor(std::string_view group) const;
    static Result<ClothAsset> parse(std::string_view text);
};

// 1.3.5 Euphoria-style active ragdoll. The motors track `target` (model-space bone matrices, usually
// AnimationSystem::animatedPose). strength 0 is limp. A contact faster than stumbleSpeed drops the motors and
// raises the arms for a moment; balance torques the hips back toward upright while they recover.
struct ActiveRagdollDrive {
    bool enabled = false;
    float strength = 1;
    float stiffness = 40;
    float damping = 6;
    float balance = 1;
    float stumbleSpeed = 6;
    float recovery = 1.2f;
    std::vector<Mat4> target;
};
struct ActiveRagdollStatus {
    float motor = 0;
    float upright = 1;
    float brace = 0;
    const char* mode = "off";
};

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    Providers providers;
    Vec3 gravity{0, -9.81f, 0};

    // Creates / removes / updates bodies to match the scene (call every step; cheap when nothing changed).
    void sync(Scene& scene);
    // One fixed step: inputs from components (forces, impulses, character moves, vehicle inputs), simulation,
    // write-back (transforms, velocities, grounded, speed / rpm / gear, ragdoll bone transforms).
    void step(Scene& scene, float dt);

    std::optional<RayHit> raycast(Vec3 origin, Vec3 direction, float maxDistance, EntityId ignore = kNullEntity) const;
    // Sphere sweep (shape cast); returns the first hit along the path.
    std::optional<RayHit> sphereCast(Vec3 origin, float radius, Vec3 direction, float maxDistance, EntityId ignore = kNullEntity) const;
    std::vector<ContactEvent> takeEvents();

    // ---- filtered queries (1.3.0; used by the Fledge Advanced ballistics / movement modules, docs/ADVANCED_GAMEPLAY.md)
    // Triggers are never hit; bodies of the `ignore` entities (owner, its hitboxes) are skipped. They allocate nothing per
    // call once a sphere radius has been used (shapes are cached by radius). CharacterController capsules are not bodies
    // and are never hit by any query.
    std::optional<RayHit> raycastIgnoring(Vec3 origin, Vec3 direction, float maxDistance, const EntityId* ignore, int ignoreCount) const;
    std::optional<RayHit> sphereCastIgnoring(Vec3 origin, float radius, Vec3 direction, float maxDistance, const EntityId* ignore,
                                             int ignoreCount) const;
    // Every body along the ray (closest hit per body), sorted by distance, at most `capacity`; returns the count written.
    int raycastAll(Vec3 origin, Vec3 direction, float maxDistance, RayHit* out, int capacity, const EntityId* ignore = nullptr,
                   int ignoreCount = 0) const;
    // ---- collision layers (1.3.2, docs/PHYSICS.md "Collision layers and query filters"): Collider.layer 0..31 (default 0;
    // terrains, roads and ragdoll bones are layer 0). Queries only: layers never change what collides with what in the
    // simulation. As the *Ignoring queries (triggers are never hit, the `ignore` bodies are skipped) plus a layer mask
    // (bit n = layer n; kAllLayers = every layer).
    static constexpr uint32_t kAllLayers = 0xFFFFFFFFu;
    std::optional<RayHit> raycastFiltered(Vec3 origin, Vec3 direction, float maxDistance, const EntityId* ignore, int ignoreCount,
                                          uint32_t layerMask) const;
    std::optional<RayHit> sphereCastFiltered(Vec3 origin, float radius, Vec3 direction, float maxDistance, const EntityId* ignore, int ignoreCount,
                                             uint32_t layerMask) const;
    // The collision layer of an entity's body (0 when it has none).
    int bodyLayer(EntityId e) const;

    // Exact simulation state (positions, velocities, contacts, constraints) for replays.
    Bytes saveState() const;
    bool restoreState(const Bytes& state);

    size_t bodyCount() const;
    size_t characterCount() const;
    size_t vehicleCount() const;
    // Simulated surface in world coordinates. Empty until the body exists.
    MeshData clothMesh(EntityId entity) const;
    // Wireframe of every collider (world space) for the physics debug view.
    void debugLines(std::vector<std::pair<Vec3, Vec3>>& out) const;
    // Ragdoll bone world matrices (after step) for an Animator entity; empty if not ragdolled.
    const std::vector<Mat4>* ragdollPose(EntityId e) const;
    void setActiveRagdoll(EntityId entity, ActiveRagdollDrive drive);
    ActiveRagdollStatus activeRagdollStatus(EntityId entity) const;

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

// Authored surface in entity coordinates, also used for the editor preview.
MeshData clothGrid(const ClothComponent& settings);

// One-click "Add physics": a Collider fitted to the entity's mesh bounds (sphere for builtin:sphere, box otherwise)
// plus a RigidBody (static when the object is large and flat, e.g. a ground plane). Returns the component JSON.
Json fitPhysics(const MeshData* mesh, const std::string& meshRef, Vec3 worldScale);

} // namespace fledge::physics
