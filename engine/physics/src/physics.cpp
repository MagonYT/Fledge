// Jolt Physics integration. Jolt headers must come first (Jolt.h configures the rest).
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Vehicle/MotorcycleController.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/RegisterTypes.h>

#include "fledge/physics/physics.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "fledge/core/json.h"

#include "fledge/anim/anim.h"
#include "fledge/core/log.h"

namespace fledge::physics {

namespace {
namespace Layers {
constexpr JPH::ObjectLayer NON_MOVING = 0, MOVING = 1, TRIGGER = 2, COUNT = 3;
}
namespace BPLayers {
constexpr JPH::BroadPhaseLayer NON_MOVING(0), MOVING(1);
constexpr unsigned COUNT = 2;
}
class BPLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    unsigned GetNumBroadPhaseLayers() const override { return BPLayers::COUNT; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return l == Layers::NON_MOVING ? BPLayers::NON_MOVING : BPLayers::MOVING; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override { return l == BPLayers::NON_MOVING ? "static" : "moving"; }
#endif
};
class ObjVsBP final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::BroadPhaseLayer b) const override { return a == Layers::NON_MOVING ? b == BPLayers::MOVING : true; }
};
class ObjPair final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (a == Layers::NON_MOVING) return b == Layers::MOVING;
        if (a == Layers::TRIGGER) return b == Layers::MOVING;
        return true;
    }
};

JPH::Vec3 J(Vec3 v) { return {v.x, v.y, v.z}; }
JPH::Quat J(Quat q) { return {q.x, q.y, q.z, q.w}; }
Vec3 F(JPH::Vec3 v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
Quat F(JPH::Quat q) { return {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}; }

uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull; }
uint64_t hashF(uint64_t h, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return mix(h, u);
}
uint64_t hashS(uint64_t h, std::string_view s) {
    for (unsigned char c : s) h = mix(h, c);
    return h;
}

struct GlobalJolt {
    GlobalJolt() {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
};
void ensureJolt() { static GlobalJolt g; }
} // namespace

namespace {
// 1.3.1 CharacterController filter (ignore / collideWithDynamic): bodies of the listed entities, and optionally every dynamic
// body, are invisible to the capsule's collision queries, so they neither block nor push it (and it does not push them).
class CharacterBodyFilter final : public JPH::BodyFilter {
public:
    CharacterBodyFilter(const std::vector<EntityId>& ignore, bool collideWithDynamic) : m_ignore(ignore), m_dynamic(collideWithDynamic) {}
    bool ShouldCollideLocked(const JPH::Body& b) const override {
        if (!m_dynamic && b.IsDynamic()) return false;
        auto e = static_cast<EntityId>(b.GetUserData());
        return std::find(m_ignore.begin(), m_ignore.end(), e) == m_ignore.end();
    }

private:
    const std::vector<EntityId>& m_ignore;
    bool m_dynamic;
};
} // namespace

// ============================================================================== impl ===
struct PhysicsWorld::Impl : public JPH::ContactListener {
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    BPLayerInterface bpl;
    ObjVsBP ovb;
    ObjPair opf;
    JPH::PhysicsSystem sys;

    struct Body {
        JPH::BodyID id;
        uint64_t hash = 0;
        MotionType motion = MotionType::Static;
        bool trigger = false;
        Vec3 lastPos;
        Quat lastRot;
        Vec3 lastVel, lastAngVel;
    };
    std::map<EntityId, Body> bodies;  // ordered: deterministic iteration
    std::unordered_map<uint32_t, EntityId> byBody;
    std::unordered_map<uint32_t, bool> sensorBody;
    struct Char {
        std::unique_ptr<JPH::CharacterVirtualSettings> settings;
        JPH::Ref<JPH::CharacterVirtual> c;
        uint64_t hash = 0;
        float vy = 0;
    };
    std::map<EntityId, Char> chars;
    struct Veh {
        JPH::Ref<JPH::VehicleConstraint> c;
        uint64_t hash = 0;
        JPH::BodyID body;
        bool bike = false;  // MotorcycleController (2 wheels, leans)
    };
    std::map<EntityId, Veh> vehicles;
    struct JointRec {
        JPH::Ref<JPH::TwoBodyConstraint> c;
        uint64_t hash = 0;
    };
    std::map<EntityId, JointRec> joints;
    struct Rag {
        std::vector<JPH::BodyID> bodies;
        std::vector<JPH::Ref<JPH::Constraint>> cons;
        std::vector<Mat4> offsets;  // body world^-1 * bone world at creation
        std::vector<Mat4> pose;     // model space (relative to the entity) after each step
        Mat4 entityWorld;
        float motor = 1, upright = 1, brace = 0;
        const char* mode = "off";
        int armL = -1, armR = -1;
        bool named = false;
    };
    std::map<EntityId, Rag> ragdolls;
    std::map<EntityId, ActiveRagdollDrive> drives;
    std::map<EntityId, float> hitSpeed;
    struct Cloth {
        JPH::BodyID body;
        uint64_t hash;
        MeshData rest;
    };
    std::map<EntityId, Cloth> cloths;
    std::mutex evMutex;
    std::vector<ContactEvent> events;
    std::map<int, JPH::RefConst<JPH::Shape>> sphereShapes;  // filtered sphere casts, by radius in 0.1 mm

    Impl() {
        ensureJolt();
        temp = std::make_unique<JPH::TempAllocatorImpl>(32 * 1024 * 1024);
        const unsigned logicalProcessors = std::thread::hardware_concurrency();
        const int workers = logicalProcessors > 1 ? static_cast<int>(logicalProcessors - 1) : 1;
        jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workers);
        sys.Init(32768, 0, 32768, 8192, bpl, ovb, opf);
        sys.SetContactListener(this);
    }
    ~Impl() override {
        for (const auto& [entity, cloth] : cloths) {
            sys.GetBodyInterface().RemoveBody(cloth.body);
            sys.GetBodyInterface().DestroyBody(cloth.body);
        }
        for (auto& [e, v] : vehicles) {
            sys.RemoveStepListener(v.c);
            sys.RemoveConstraint(v.c);
        }
        for (auto& [e, j] : joints) sys.RemoveConstraint(j.c);
        for (auto& [e, r] : ragdolls) {
            for (auto& c : r.cons) sys.RemoveConstraint(c);
            for (auto id : r.bodies)
                if (!id.IsInvalid()) {
                    sys.GetBodyInterface().RemoveBody(id);
                    sys.GetBodyInterface().DestroyBody(id);
                }
        }
        for (auto& [e, b] : bodies) {
            sys.GetBodyInterface().RemoveBody(b.id);
            sys.GetBodyInterface().DestroyBody(b.id);
        }
    }

    // ---- contacts (called from job threads)
    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m, JPH::ContactSettings&) override {
        auto e1 = static_cast<EntityId>(b1.GetUserData()), e2 = static_cast<EntityId>(b2.GetUserData());
        if (!e1 || !e2 || e1 == e2) return;
        bool trig = b1.IsSensor() || b2.IsSensor();
        JPH::Vec3 p = m.GetWorldSpaceContactPointOn1(0);
        float speed = std::fabs((b1.GetPointVelocity(p) - b2.GetPointVelocity(p)).Dot(m.mWorldSpaceNormal));
        std::lock_guard l(evMutex);
        events.push_back({e1, e2, trig ? ContactKind::TriggerEnter : ContactKind::Enter, F(p), speed});
        if (!trig) {
            hitSpeed[e1] = std::max(hitSpeed[e1], speed);
            hitSpeed[e2] = std::max(hitSpeed[e2], speed);
        }
    }
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        auto a = byBody.find(pair.GetBody1ID().GetIndexAndSequenceNumber()), b = byBody.find(pair.GetBody2ID().GetIndexAndSequenceNumber());
        if (a == byBody.end() || b == byBody.end() || a->second == b->second) return;
        bool trig = sensorBody[pair.GetBody1ID().GetIndexAndSequenceNumber()] || sensorBody[pair.GetBody2ID().GetIndexAndSequenceNumber()];
        std::lock_guard l(evMutex);
        events.push_back({a->second, b->second, trig ? ContactKind::TriggerExit : ContactKind::Exit, {}, 0});
    }

    void removeBody(EntityId e) {
        auto it = bodies.find(e);
        if (it == bodies.end()) return;
        if (auto v = vehicles.find(e); v != vehicles.end()) {
            sys.RemoveStepListener(v->second.c);
            sys.RemoveConstraint(v->second.c);
            vehicles.erase(v);
        }
        for (auto j = joints.begin(); j != joints.end();) {  // constraints touching this body
            if (j->second.c && (j->second.c->GetBody1()->GetID() == it->second.id || j->second.c->GetBody2()->GetID() == it->second.id)) {
                sys.RemoveConstraint(j->second.c);
                j = joints.erase(j);
            } else {
                ++j;
            }
        }
        byBody.erase(it->second.id.GetIndexAndSequenceNumber());
        sensorBody.erase(it->second.id.GetIndexAndSequenceNumber());
        sys.GetBodyInterface().RemoveBody(it->second.id);
        sys.GetBodyInterface().DestroyBody(it->second.id);
        bodies.erase(it);
    }
};

// ============================================================================== public ===
PhysicsWorld::PhysicsWorld() : m(std::make_unique<Impl>()) {}
PhysicsWorld::~PhysicsWorld() = default;

size_t PhysicsWorld::bodyCount() const { return m->bodies.size(); }
size_t PhysicsWorld::characterCount() const { return m->chars.size(); }
size_t PhysicsWorld::vehicleCount() const { return m->vehicles.size(); }

std::vector<ContactEvent> PhysicsWorld::takeEvents() {
    std::lock_guard l(m->evMutex);
    std::vector<ContactEvent> out;
    out.swap(m->events);
    return out;
}

namespace {
JPH::RefConst<JPH::Shape> meshShape(const MeshData& md, Vec3 scale, bool convex) {
    if (md.empty()) return nullptr;
    if (convex) {
        JPH::Array<JPH::Vec3> pts;
        for (auto& v : md.vertices) pts.push_back(J(v.position * scale));
        JPH::ConvexHullShapeSettings s(pts);
        auto r = s.Create();
        return r.HasError() ? nullptr : r.Get();
    }
    JPH::VertexList verts;
    for (auto& v : md.vertices) {
        Vec3 p = v.position * scale;
        verts.push_back(JPH::Float3(p.x, p.y, p.z));
    }
    JPH::IndexedTriangleList tris;
    for (size_t i = 0; i + 2 < md.indices.size(); i += 3) tris.push_back(JPH::IndexedTriangle(md.indices[i], md.indices[i + 1], md.indices[i + 2], 0));
    JPH::MeshShapeSettings s(verts, tris);
    auto r = s.Create();
    return r.HasError() ? nullptr : r.Get();
}
} // namespace

MeshData clothGrid(const ClothComponent& settings);

std::vector<int> ClothAsset::pinsFor(std::string_view group) const {
    if (group.empty()) return pins;
    auto it = groups.find(std::string(group));
    return it == groups.end() ? pins : it->second;
}

Result<ClothAsset> ClothAsset::parse(std::string_view text) {
    Json json = Json::parse(text, nullptr, false);
    if (!json.is_object() || json.value("format", "") != "fledgecloth" || json.value("version", 0) != 1)
        return Result<ClothAsset>::failure(".fcloth must be {\"format\":\"fledgecloth\",\"version\":1}");
    ClothAsset asset;
    auto takeFloat = [&](const char* key, float& out, bool& set, float lo, float hi) {
        if (!json.contains(key)) return true;
        if (!json[key].is_number()) return false;
        out = std::clamp(json[key].get<float>(), lo, hi);
        set = true;
        return std::isfinite(out);
    };
    if (!takeFloat("damping", asset.damping, asset.hasDamping, 0, 1) ||
        !takeFloat("gravityScale", asset.gravityScale, asset.hasGravity, -10, 10))
        return Result<ClothAsset>::failure(".fcloth damping or gravityScale is not a finite number");
    if (json.contains("iterations")) {
        if (!json["iterations"].is_number_integer()) return Result<ClothAsset>::failure(".fcloth iterations must be an integer");
        asset.iterations = std::clamp(json["iterations"].get<int>(), 1, 16);
        asset.hasIterations = true;
    }
    auto readPins = [&](const Json& list, std::vector<int>& out) {
        if (!list.is_array()) return false;
        for (const Json& n : list) {
            if (!n.is_number_integer()) return false;
            out.push_back(n.get<int>());
        }
        return true;
    };
    if (json.contains("pins") && !readPins(json["pins"], asset.pins)) return Result<ClothAsset>::failure(".fcloth pins must be an array of vertex indices");
    if (json.contains("groups")) {
        if (!json["groups"].is_object()) return Result<ClothAsset>::failure(".fcloth groups must be an object");
        for (auto& [name, list] : json["groups"].items()) {
            std::vector<int> pins;
            if (!readPins(list, pins)) return Result<ClothAsset>::failure(".fcloth group '" + name + "' must be an array of vertex indices");
            asset.groups.emplace(name, std::move(pins));
        }
    }
    if (json.contains("positions")) {
        if (!json["positions"].is_array() || !json.contains("indices") || !json["indices"].is_array())
            return Result<ClothAsset>::failure(".fcloth positions require an indices array");
        for (const Json& p : json["positions"]) {
            if (!p.is_array() || p.size() != 3 || !p[0].is_number() || !p[1].is_number() || !p[2].is_number())
                return Result<ClothAsset>::failure(".fcloth positions must be [x,y,z] arrays");
            Vec3 position{p[0].get<float>(), p[1].get<float>(), p[2].get<float>()};
            if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
                return Result<ClothAsset>::failure(".fcloth positions must be finite");
            asset.mesh.vertices.push_back({position, {0, 1, 0}, {}});
        }
        if (json["indices"].size() % 3 != 0) return Result<ClothAsset>::failure(".fcloth indices must be triangles");
        for (const Json& index : json["indices"]) {
            if (!index.is_number_integer()) return Result<ClothAsset>::failure(".fcloth indices must be integers");
            int i = index.get<int>();
            if (i < 0 || i >= static_cast<int>(asset.mesh.vertices.size())) return Result<ClothAsset>::failure(".fcloth index out of range");
            asset.mesh.indices.push_back(static_cast<uint32_t>(i));
        }
    } else {
        if (json.contains("columns") && !json["columns"].is_number_integer()) return Result<ClothAsset>::failure(".fcloth columns must be an integer");
        if (json.contains("rows") && !json["rows"].is_number_integer()) return Result<ClothAsset>::failure(".fcloth rows must be an integer");
        if (json.contains("spacing") && !json["spacing"].is_number()) return Result<ClothAsset>::failure(".fcloth spacing must be a number");
        ClothComponent grid;
        grid.columns = json.value("columns", 8);
        grid.rows = json.value("rows", 4);
        grid.spacing = json.value("spacing", 0.2f);
        asset.mesh = clothGrid(grid);
    }
    if (asset.mesh.vertices.empty() || asset.mesh.vertices.size() > 48u * 48u || asset.mesh.indices.empty())
        return Result<ClothAsset>::failure(".fcloth needs 1 to 2304 vertices and at least one triangle");
    auto checkPins = [&](const std::vector<int>& pins) {
        for (int pin : pins)
            if (pin < 0 || pin >= static_cast<int>(asset.mesh.vertices.size())) return false;
        return true;
    };
    if (!checkPins(asset.pins)) return Result<ClothAsset>::failure(".fcloth pin index out of range");
    for (const auto& [name, pins] : asset.groups)
        if (!checkPins(pins)) return Result<ClothAsset>::failure(".fcloth group '" + name + "' has an index out of range");
    return asset;
}

MeshData clothGrid(const ClothComponent& settings) {
    const int columns = std::clamp(settings.columns, 2, 48);
    const int rows = std::clamp(settings.rows, 2, 48);
    const float spacing = std::isfinite(settings.spacing) ? std::clamp(settings.spacing, 0.02f, 5.0f) : 0.2f;
    MeshData mesh;
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < columns; ++col) {
            mesh.vertices.push_back({{(col - (columns - 1) * 0.5f) * spacing, 0, row * spacing},
                                     {0, 1, 0},
                                     {static_cast<float>(col) / (columns - 1), static_cast<float>(row) / (rows - 1)}});
            if (row + 1 < rows && col + 1 < columns) {
                const auto a = static_cast<uint32_t>(row * columns + col);
                const auto down = a + static_cast<uint32_t>(columns);
                mesh.indices.insert(mesh.indices.end(), {a, down, down + 1, a, down + 1, a + 1});
            }
        }
    }
    return mesh;
}

MeshData PhysicsWorld::clothMesh(EntityId entity) const {
    auto it = m->cloths.find(entity);
    if (it == m->cloths.end()) return {};
    JPH::BodyLockRead lock(m->sys.GetBodyLockInterface(), it->second.body);
    if (!lock.Succeeded()) return {};
    const auto& body = lock.GetBody();
    const auto& vertices = static_cast<const JPH::SoftBodyMotionProperties*>(body.GetMotionProperties())->GetVertices();
    MeshData mesh = it->second.rest;
    if (vertices.size() != mesh.vertices.size()) return {};
    const auto transform = body.GetCenterOfMassTransform();
    for (size_t i = 0; i < vertices.size(); ++i) {
        mesh.vertices[i].position = F(transform * vertices[i].mPosition);
        mesh.vertices[i].normal = {};
    }
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        auto& a = mesh.vertices[mesh.indices[i]];
        auto& b = mesh.vertices[mesh.indices[i + 1]];
        auto& c = mesh.vertices[mesh.indices[i + 2]];
        const Vec3 n = cross(b.position - a.position, c.position - a.position);
        a.normal += n;
        b.normal += n;
        c.normal += n;
    }
    for (auto& vertex : mesh.vertices) {
        const float n = length(vertex.normal);
        vertex.normal = std::isfinite(n) && n > 1e-8f ? vertex.normal / n : Vec3{0, 1, 0};
    }
    return mesh;
}

void PhysicsWorld::sync(Scene& scene) {
    auto& bi = m->sys.GetBodyInterface();
    auto worlds = scene.computeWorldMatrices();
    std::erase_if(m->cloths, [&](const auto& entry) {
        const auto* c = scene.get<ClothComponent>(entry.first);
        if (c && c->enabled && worlds.contains(entry.first)) return false;
        bi.RemoveBody(entry.second.body);
        bi.DestroyBody(entry.second.body);
        return true;
    });
    scene.each<ClothComponent, TransformComponent>([&](EntityId e, ClothComponent& cloth, TransformComponent&) {
        if (!cloth.enabled) return;
        const auto& world = worlds.at(e);
        uint64_t hash = mix(mix(mix(1469598103934665603ull, cloth.columns), cloth.rows), cloth.iterations);
        for (float value : {cloth.spacing, cloth.damping, cloth.gravityScale}) hash = hashF(hash, value);
        hash = hashS(hash, cloth.asset);
        hash = hashS(hash, cloth.pinGroup);
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row) hash = hashF(hash, world.at(row, col));
        auto previous = m->cloths.find(e);
        if (previous != m->cloths.end()) {
            if (previous->second.hash == hash) return;
            bi.RemoveBody(previous->second.body);
            bi.DestroyBody(previous->second.body);
            m->cloths.erase(previous);
        }
        const auto origin = world.transformPoint({});
        const Vec3 x = world.transformPoint({1, 0, 0}) - origin;
        const Vec3 z = world.transformPoint({0, 0, 1}) - origin;
        const float area = length(cross(x, z));
        if (!std::isfinite(area) || area < 1e-5f) return;
        MeshData rest = clothGrid(cloth);
        float damping = cloth.damping, gravityScale = cloth.gravityScale;
        int iterations = cloth.iterations;
        std::vector<int> pins;
        const size_t lastPin = static_cast<size_t>(std::clamp(cloth.columns, 2, 48) - 1);
        pins = {0, static_cast<int>(lastPin)};
        if (!cloth.asset.empty()) {
            std::optional<std::string> text = providers.clothText ? providers.clothText(cloth.asset) : std::nullopt;
            if (!text) {
                FLEDGE_LOG_WARN("physics", "Cloth '{}' asset '{}' was not found", scene.displayName(e), cloth.asset);
                return;
            }
            auto parsed = ClothAsset::parse(*text);
            if (!parsed) {
                FLEDGE_LOG_WARN("physics", "Cloth '{}': {}", scene.displayName(e), parsed.error());
                return;
            }
            hash = hashS(hash, *text);
            rest = std::move(parsed->mesh);
            pins = parsed->pinsFor(cloth.pinGroup);
            if (parsed->hasDamping) damping = parsed->damping;
            if (parsed->hasGravity) gravityScale = parsed->gravityScale;
            if (parsed->hasIterations) iterations = parsed->iterations;
        }
        JPH::Ref<JPH::SoftBodySharedSettings> shared = new JPH::SoftBodySharedSettings;
        std::vector<char> pinned(rest.vertices.size(), 0);
        for (int pin : pins)
            if (pin >= 0 && pin < static_cast<int>(pinned.size())) pinned[static_cast<size_t>(pin)] = 1;
        for (size_t i = 0; i < rest.vertices.size(); ++i) {
            JPH::SoftBodySharedSettings::Vertex vertex;
            J(world.transformPoint(rest.vertices[i].position) - origin).StoreFloat3(&vertex.mPosition);
            vertex.mInvMass = pinned[i] ? 0.0f : 1.0f;
            shared->mVertices.push_back(vertex);
        }
        for (size_t i = 0; i < rest.indices.size(); i += 3) {
            JPH::SoftBodySharedSettings::Face face;
            for (size_t corner = 0; corner < 3; ++corner) face.mVertex[corner] = rest.indices[i + corner];
            shared->AddFace(face);
        }
        const JPH::SoftBodySharedSettings::VertexAttributes attributes;
        shared->CreateConstraints(&attributes, 1, JPH::SoftBodySharedSettings::EBendType::Distance);
        shared->Optimize();
        JPH::SoftBodyCreationSettings settings(shared, J(origin), JPH::Quat::sIdentity(), Layers::MOVING);
        settings.mUpdatePosition = false;
        settings.mNumIterations = static_cast<uint32_t>(std::clamp(iterations, 1, 16));
        settings.mLinearDamping = std::isfinite(damping) ? std::clamp(damping, 0.0f, 1.0f) : 0.1f;
        settings.mGravityFactor = std::isfinite(gravityScale) ? std::clamp(gravityScale, -10.0f, 10.0f) : 1.0f;
        const auto body = bi.CreateAndAddSoftBody(settings, JPH::EActivation::Activate);
        if (!body.IsInvalid()) m->cloths.emplace(e, Impl::Cloth{body, hash, std::move(rest)});
    });
    std::map<EntityId, bool> wanted;
    // ---- bodies: colliders, terrains, roads
    for (EntityId e : scene.entities()) {
        auto* col = scene.get<ColliderComponent>(e);
        auto* terr = scene.get<TerrainComponent>(e);
        auto* road = scene.get<SplineComponent>(e);
        bool isRoad = road && road->kind == "road" && providers.roadMesh;
        if (!col && !(terr && providers.terrainHeights) && !isRoad) continue;
        auto* rb = scene.get<RigidBodyComponent>(e);
        Transform wt = decompose(worlds[e]);
        MotionType motion = (col && rb) ? rb->motion : MotionType::Static;
        uint64_t h = 1469598103934665603ull;
        h = mix(h, static_cast<uint64_t>(motion));
        for (int k = 0; k < 3; ++k) h = hashF(h, std::round(wt.scale[k] * 1e4f));  // decomposed scale jitters with rotation
        std::string meshRef;
        if (col) {
            h = mix(h, static_cast<uint64_t>(col->shape));
            for (float f : {col->size.x, col->size.y, col->size.z, col->radius, col->height, col->offset.x, col->offset.y, col->offset.z}) h = hashF(h, f);
            h = mix(h, col->isTrigger);
            if (col->layer != 0) h = mix(h, 0x4c41594552ull + static_cast<uint64_t>(col->layer));  // 1.3.2 (layer 0 keeps the 1.3.1 hash)
            meshRef = col->mesh;
            if (meshRef.empty())
                if (auto* mr = scene.get<MeshRendererComponent>(e)) meshRef = mr->mesh;
            h = hashS(h, meshRef);
            if (auto* vc = scene.get<VehicleComponent>(e)) h = hashS(h, vc->preset);  // bikes lower the centre of mass
            if (rb) {
                for (float f : {rb->mass, rb->friction, rb->restitution, rb->linearDamping, rb->angularDamping, rb->gravityScale}) h = hashF(h, f);
                h = mix(h, rb->lockRotation);
                h = mix(h, rb->ccd);
            }
        } else if (terr) {
            h = hashS(h, std::to_string(std::hash<std::string>{}(terr->heights)));
            for (float f : {terr->size, static_cast<float>(terr->resolution), static_cast<float>(terr->seed), terr->maxHeight}) h = hashF(h, f);
        } else {
            h = hashS(h, road->points.dump());
            for (float f : {road->width, road->sidewalk}) h = hashF(h, f);
        }
        wanted[e] = true;
        auto it = m->bodies.find(e);
        if (it != m->bodies.end() && it->second.hash == h) {
            Impl::Body& b = it->second;
            bool moved = length(wt.position - b.lastPos) > 1e-4f || std::fabs(dot(Vec4(wt.rotation.x, wt.rotation.y, wt.rotation.z, wt.rotation.w),
                                                                             Vec4(b.lastRot.x, b.lastRot.y, b.lastRot.z, b.lastRot.w))) < 0.99999f;
            if (moved) {  // moved by the editor, a script or a rebase: teleport
                if (motion == MotionType::Kinematic) bi.MoveKinematic(b.id, J(wt.position), J(wt.rotation), 1.0f / 60);
                else bi.SetPositionAndRotation(b.id, J(wt.position), J(wt.rotation), JPH::EActivation::Activate);
                b.lastPos = wt.position;
                b.lastRot = wt.rotation;
            }
            continue;
        }
        if (it != m->bodies.end()) m->removeBody(e);
        // build the shape
        JPH::RefConst<JPH::Shape> shape;
        Vec3 sc = wt.scale;
        if (col) {
            switch (col->shape) {
                case ColliderShape::Box: {
                    Vec3 he = (col->size * sc) * 0.5f;
                    he = {std::max(he.x, 0.01f), std::max(he.y, 0.01f), std::max(he.z, 0.01f)};
                    shape = new JPH::BoxShape(J(he), std::min(0.05f, std::min({he.x, he.y, he.z}) * 0.5f));
                    break;
                }
                case ColliderShape::Sphere: shape = new JPH::SphereShape(std::max(0.01f, col->radius * std::max({sc.x, sc.y, sc.z}))); break;
                case ColliderShape::Capsule: {
                    float r = std::max(0.01f, col->radius * std::max(sc.x, sc.z));
                    shape = new JPH::CapsuleShape(std::max(0.01f, col->height * sc.y * 0.5f), r);
                    break;
                }
                case ColliderShape::ConvexHull:
                case ColliderShape::Mesh: {
                    const MeshData* md = providers.mesh ? providers.mesh(meshRef) : nullptr;
                    bool convex = col->shape == ColliderShape::ConvexHull || motion != MotionType::Static;  // dynamic meshes must be convex
                    if (md) shape = meshShape(*md, sc, convex);
                    if (!shape) shape = new JPH::BoxShape(J(Vec3{0.5f, 0.5f, 0.5f} * sc));
                    break;
                }
            }
            if (length(col->offset) > 1e-6f) {
                JPH::RotatedTranslatedShapeSettings rts(J(col->offset * sc), JPH::Quat::sIdentity(), shape);
                auto r = rts.Create();
                if (!r.HasError()) shape = r.Get();
            }
        } else if (terr) {
            std::vector<float> hs;
            int res = 0;
            float size = 0;
            if (providers.terrainHeights(e, hs, res, size) && res >= 2 && hs.size() == static_cast<size_t>(res) * static_cast<size_t>(res)) {
                // pad to a multiple of the block size with "no collision" samples so the grid matches the render mesh exactly
                constexpr int kBlock = 4;
                int n = std::max(2 * kBlock, (res + kBlock - 1) / kBlock * kBlock);
                std::vector<float> padded(static_cast<size_t>(n) * static_cast<size_t>(n), JPH::HeightFieldShapeConstants::cNoCollisionValue);
                for (int z = 0; z < res; ++z)
                    for (int x = 0; x < res; ++x) padded[static_cast<size_t>(z) * n + x] = hs[static_cast<size_t>(z) * res + x];
                float sp = size / static_cast<float>(res - 1);
                JPH::HeightFieldShapeSettings hf(padded.data(), JPH::Vec3(-size * 0.5f * sc.x, 0, -size * 0.5f * sc.z), JPH::Vec3(sp * sc.x, sc.y, sp * sc.z),
                                                 static_cast<JPH::uint32>(n));
                hf.mBlockSize = kBlock;
                auto r = hf.Create();
                if (!r.HasError()) shape = r.Get();
            }
        } else {
            MeshData md = providers.roadMesh(e);
            shape = meshShape(md, sc, false);
        }
        if (!shape) continue;
        if (auto* vc = scene.get<VehicleComponent>(e); vc && vc->preset == "bike" && col) {
            // bikes balance on two wheels: centre of mass at the bottom of the chassis box (rider + engine sit low)
            JPH::OffsetCenterOfMassShapeSettings com(JPH::Vec3(0, -col->size.y * sc.y * 0.5f, 0), shape);
            auto r = com.Create();
            if (!r.HasError()) shape = r.Get();
        }
        bool trigger = col && col->isTrigger;
        JPH::EMotionType mt = motion == MotionType::Dynamic ? JPH::EMotionType::Dynamic : motion == MotionType::Kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static;
        JPH::ObjectLayer layer = trigger ? Layers::TRIGGER : (mt == JPH::EMotionType::Static ? Layers::NON_MOVING : Layers::MOVING);
        JPH::BodyCreationSettings bs(shape, J(wt.position), J(wt.rotation), mt, layer);
        bs.mUserData = e;
        bs.mIsSensor = trigger;
        // 1.3.2 collision layer for query filters: stored as the group id (no group filter is set, so the simulation ignores it)
        if (col && col->layer > 0 && col->layer < 32) bs.mCollisionGroup.SetGroupID(static_cast<JPH::CollisionGroup::GroupID>(col->layer));
        bs.mFriction = col ? 0.6f : 0.9f;  // static colliders without a RigidBody; terrain / asphalt grip well
        if (rb && col) {
            bs.mFriction = rb->friction;
            bs.mRestitution = rb->restitution;
            bs.mLinearDamping = rb->linearDamping;
            bs.mAngularDamping = rb->angularDamping;
            bs.mGravityFactor = rb->gravityScale;
            bs.mLinearVelocity = J(rb->velocity);
            bs.mAngularVelocity = J(rb->angularVelocity);
            if (rb->ccd) bs.mMotionQuality = JPH::EMotionQuality::LinearCast;
            if (rb->lockRotation) bs.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
            if (rb->mass > 0 && mt == JPH::EMotionType::Dynamic) {
                bs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                bs.mMassPropertiesOverride.mMass = rb->mass;
            }
        }
        if (scene.get<VehicleComponent>(e)) bs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia,
                                            bs.mMassPropertiesOverride.mMass = rb && rb->mass > 0 ? rb->mass : 1500.0f;
        JPH::BodyID id = bi.CreateAndAddBody(bs, mt == JPH::EMotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        if (id.IsInvalid()) continue;
        Impl::Body b;
        b.id = id;
        b.hash = h;
        b.motion = motion;
        b.trigger = trigger;
        b.lastPos = wt.position;
        b.lastRot = wt.rotation;
        b.lastVel = rb ? rb->velocity : Vec3{};
        b.lastAngVel = rb ? rb->angularVelocity : Vec3{};
        m->bodies[e] = b;
        m->byBody[id.GetIndexAndSequenceNumber()] = e;
        m->sensorBody[id.GetIndexAndSequenceNumber()] = trigger;
    }
    std::vector<EntityId> gone;
    for (auto& [e, b] : m->bodies)
        if (!wanted.count(e)) gone.push_back(e);
    for (EntityId e : gone) m->removeBody(e);

    // ---- characters
    std::map<EntityId, bool> wantedChars;
    for (EntityId e : scene.entities()) {
        auto* cc = scene.get<CharacterControllerComponent>(e);
        if (!cc || !cc->enabled) continue;
        wantedChars[e] = true;
        uint64_t h = 1469598103934665603ull;
        for (float f : {cc->height, cc->radius, cc->maxSlope, cc->stepHeight}) h = hashF(h, f);
        Transform wt = decompose(worlds[e]);
        auto it = m->chars.find(e);
        if (it != m->chars.end() && it->second.hash == h) {
            JPH::RVec3 p = it->second.c->GetPosition();
            if (length(F(p) - wt.position) > 0.01f) it->second.c->SetPosition(J(wt.position));  // teleported
            continue;
        }
        Impl::Char ch;
        ch.settings = std::make_unique<JPH::CharacterVirtualSettings>();
        float r = std::max(0.05f, cc->radius), half = std::max(0.05f, cc->height * 0.5f - r);
        JPH::RotatedTranslatedShapeSettings rts(JPH::Vec3(0, half + r, 0), JPH::Quat::sIdentity(), new JPH::CapsuleShape(half, r));
        ch.settings->mShape = rts.Create().Get();
        ch.settings->mMaxSlopeAngle = radians(cc->maxSlope);
        ch.settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -r);
        ch.c = new JPH::CharacterVirtual(ch.settings.get(), J(wt.position), JPH::Quat::sIdentity(), e, &m->sys);
        ch.hash = h;
        m->chars[e] = std::move(ch);
    }
    for (auto it = m->chars.begin(); it != m->chars.end();) it = wantedChars.count(it->first) ? std::next(it) : m->chars.erase(it);

    // ---- vehicles
    for (EntityId e : scene.entities()) {
        auto* vc = scene.get<VehicleComponent>(e);
        auto bit = m->bodies.find(e);
        if (!vc || bit == m->bodies.end() || bit->second.motion != MotionType::Dynamic) {
            if (auto v = m->vehicles.find(e); v != m->vehicles.end()) {
                m->sys.RemoveStepListener(v->second.c);
                m->sys.RemoveConstraint(v->second.c);
                m->vehicles.erase(v);
            }
            continue;
        }
        uint64_t h = hashS(1469598103934665603ull, vc->preset);
        for (float f : {vc->maxEngineTorque, vc->maxRpm, vc->wheelRadius, vc->wheelWidth, vc->suspension}) h = hashF(h, f);
        h = mix(h, bit->second.id.GetIndexAndSequenceNumber());
        auto it = m->vehicles.find(e);
        if (it != m->vehicles.end() && it->second.hash == h) continue;
        if (it != m->vehicles.end()) {
            m->sys.RemoveStepListener(it->second.c);
            m->sys.RemoveConstraint(it->second.c);
        }
        auto* col = scene.get<ColliderComponent>(e);
        Vec3 sc = decompose(worlds[e]).scale;
        Vec3 half = col ? col->size * sc * 0.5f : Vec3{0.9f, 0.5f, 2.1f};
        bool truck = vc->preset == "truck";
        JPH::VehicleConstraintSettings vs;
        vs.mUp = JPH::Vec3::sAxisY();
        vs.mForward = JPH::Vec3(0, 0, -1);  // Fledge forward is -Z
        if (vc->preset == "bike") {
            // two wheels in line (front steers on a 30 degree raked fork, rear drives), Jolt's motorcycle controller
            // leans into turns and keeps the bike upright; based on Jolt's MotorcycleTest sample
            vs.mMaxPitchRollAngle = radians(60.0f);
            float bz = half.z + vc->wheelRadius * 0.8f, by = -0.9f * half.y;
            const float caster = radians(30.0f);
            auto* front = new JPH::WheelSettingsWV;
            front->mPosition = JPH::Vec3(0, by, -bz);
            front->mMaxSteerAngle = radians(30.0f);
            front->mSuspensionDirection = JPH::Vec3(0, -1, -std::tan(caster)).Normalized();  // rake towards the front (-Z)
            front->mSteeringAxis = -front->mSuspensionDirection;
            front->mSuspensionSpring.mFrequency = 1.5f;
            front->mMaxBrakeTorque = 500.0f;
            auto* back = new JPH::WheelSettingsWV;
            back->mPosition = JPH::Vec3(0, by, bz);
            back->mMaxSteerAngle = 0;
            back->mSuspensionSpring.mFrequency = 2.0f;
            back->mMaxBrakeTorque = 250.0f;
            back->mMaxHandBrakeTorque = 1500.0f;
            for (auto* w : {front, back}) {
                w->mRadius = vc->wheelRadius;
                w->mWidth = vc->wheelWidth;
                w->mSuspensionMinLength = 0.3f;
                w->mSuspensionMaxLength = 0.3f + vc->suspension;
                w->mWheelForward = JPH::Vec3(0, 0, -1);
                vs.mWheels.push_back(w);
            }
            auto* mc = new JPH::MotorcycleControllerSettings;
            mc->mEngine.mMaxTorque = vc->maxEngineTorque;
            mc->mEngine.mMaxRPM = std::max(vc->maxRpm, 1500.0f);
            mc->mEngine.mMinRPM = std::min(1000.0f, mc->mEngine.mMaxRPM * 0.2f);
            mc->mTransmission.mShiftUpRPM = mc->mEngine.mMaxRPM * 0.8f;
            mc->mTransmission.mShiftDownRPM = mc->mEngine.mMaxRPM * 0.2f;
            mc->mTransmission.mGearRatios = {2.27f, 1.63f, 1.3f, 1.09f, 0.96f, 0.88f};
            mc->mTransmission.mReverseGearRatios = {-4.0f};
            mc->mTransmission.mClutchStrength = 2.0f;
            mc->mDifferentials.resize(1);  // rear wheel only (no left wheel)
            mc->mDifferentials[0].mLeftWheel = -1;
            mc->mDifferentials[0].mRightWheel = 1;
            mc->mDifferentials[0].mDifferentialRatio = 1.93f * 40.0f / 16.0f;
            vs.mController = mc;
            JPH::BodyLockWrite lock(m->sys.GetBodyLockInterface(), bit->second.id);
            if (!lock.Succeeded()) continue;
            Impl::Veh v;
            v.c = new JPH::VehicleConstraint(lock.GetBody(), vs);
            v.c->SetVehicleCollisionTester(new JPH::VehicleCollisionTesterCastCylinder(Layers::MOVING, 1.0f));  // rounded tyres
            v.hash = h;
            v.body = bit->second.id;
            v.bike = true;
            m->sys.AddConstraint(v.c);
            m->sys.AddStepListener(v.c);
            m->vehicles[e] = v;
            continue;
        }
        float wy = -half.y + 0.05f, wx = half.x - vc->wheelWidth * 0.5f, wz = half.z - vc->wheelRadius * 1.2f;
        const Vec3 pos[4] = {{-wx, wy, -wz}, {wx, wy, -wz}, {-wx, wy, wz}, {wx, wy, wz}};  // FL FR RL RR
        for (size_t i = 0; i < 4; ++i) {
            auto* w = new JPH::WheelSettingsWV;
            w->mPosition = J(pos[i]);
            w->mRadius = vc->wheelRadius;
            w->mWidth = vc->wheelWidth;
            w->mSuspensionMinLength = 0.05f;
            w->mSuspensionMaxLength = 0.05f + vc->suspension;
            w->mWheelForward = JPH::Vec3(0, 0, -1);
            bool front = i < 2;
            w->mMaxSteerAngle = front ? radians(truck ? 30.0f : 35.0f) : 0.0f;
            w->mMaxHandBrakeTorque = front ? 0.0f : 4000.0f;
            vs.mWheels.push_back(w);
        }
        auto* cs = new JPH::WheeledVehicleControllerSettings;
        cs->mEngine.mMaxTorque = vc->maxEngineTorque;
        cs->mEngine.mMaxRPM = std::max(vc->maxRpm, 1500.0f);
        cs->mEngine.mMinRPM = std::min(1000.0f, cs->mEngine.mMaxRPM * 0.2f);
        cs->mTransmission.mShiftUpRPM = cs->mEngine.mMaxRPM * 0.67f;
        cs->mTransmission.mShiftDownRPM = cs->mEngine.mMaxRPM * 0.3f;
        {
            // four-wheel drive (40 / 60 front / rear for cars, 50 / 50 for trucks): no wheel spin at full throttle,
            // so the automatic gearbox can shift up
            cs->mDifferentials.resize(2);
            cs->mDifferentials[0].mLeftWheel = 0;
            cs->mDifferentials[0].mRightWheel = 1;
            cs->mDifferentials[1].mLeftWheel = 2;
            cs->mDifferentials[1].mRightWheel = 3;
            cs->mDifferentials[0].mEngineTorqueRatio = truck ? 0.5f : 0.4f;
            cs->mDifferentials[1].mEngineTorqueRatio = truck ? 0.5f : 0.6f;
        }
        vs.mController = cs;
        JPH::BodyLockWrite lock(m->sys.GetBodyLockInterface(), bit->second.id);
        if (!lock.Succeeded()) continue;
        Impl::Veh v;
        v.c = new JPH::VehicleConstraint(lock.GetBody(), vs);
        v.c->SetVehicleCollisionTester(new JPH::VehicleCollisionTesterCastCylinder(Layers::MOVING));
        v.hash = h;
        v.body = bit->second.id;
        m->sys.AddConstraint(v.c);
        m->sys.AddStepListener(v.c);
        m->vehicles[e] = v;
    }

    // ---- joints
    for (EntityId e : scene.entities()) {
        auto* jc = scene.get<JointComponent>(e);
        auto bit = m->bodies.find(e);
        if (!jc || bit == m->bodies.end()) {
            if (auto j = m->joints.find(e); j != m->joints.end()) {
                m->sys.RemoveConstraint(j->second.c);
                m->joints.erase(j);
            }
            continue;
        }
        EntityId other = jc->target.empty() ? kNullEntity : scene.findByName(jc->target);
        auto oit = other ? m->bodies.find(other) : m->bodies.end();
        uint64_t h = mix(mix(1469598103934665603ull, static_cast<uint64_t>(jc->type)), other);
        for (float f : {jc->anchor.x, jc->anchor.y, jc->anchor.z, jc->axis.x, jc->axis.y, jc->axis.z, jc->minDistance, jc->maxDistance}) h = hashF(h, f);
        h = mix(h, bit->second.id.GetIndexAndSequenceNumber());
        if (oit != m->bodies.end()) h = mix(h, oit->second.id.GetIndexAndSequenceNumber());
        auto it = m->joints.find(e);
        if (it != m->joints.end() && it->second.hash == h) continue;
        if (it != m->joints.end()) m->sys.RemoveConstraint(it->second.c);
        Mat4 wm = worlds[e];
        Vec3 anchorW = wm.transformPoint(jc->anchor);
        Vec3 axisW = normalize((wm * Vec4(jc->axis, 0)).xyz());
        JPH::BodyID b1 = bit->second.id, b2 = oit != m->bodies.end() ? oit->second.id : JPH::BodyID();
        JPH::Ref<JPH::TwoBodyConstraintSettings> st;
        switch (jc->type) {
            case JointType::Fixed: {
                auto* s = new JPH::FixedConstraintSettings;
                s->mAutoDetectPoint = true;
                st = s;
                break;
            }
            case JointType::Hinge: {
                auto* s = new JPH::HingeConstraintSettings;
                s->mPoint1 = s->mPoint2 = J(anchorW);
                s->mHingeAxis1 = s->mHingeAxis2 = J(axisW);
                Vec3 n = std::fabs(axisW.y) < 0.9f ? normalize(cross(axisW, {0, 1, 0})) : normalize(cross(axisW, {1, 0, 0}));
                s->mNormalAxis1 = s->mNormalAxis2 = J(n);
                st = s;
                break;
            }
            case JointType::Distance: {
                auto* s = new JPH::DistanceConstraintSettings;
                s->mPoint1 = J(anchorW);
                s->mPoint2 = oit != m->bodies.end() ? J(worlds[other].c[3].xyz()) : J(anchorW);
                s->mMinDistance = jc->minDistance;
                s->mMaxDistance = jc->maxDistance;
                st = s;
                break;
            }
            case JointType::Point: {
                auto* s = new JPH::PointConstraintSettings;
                s->mPoint1 = s->mPoint2 = J(anchorW);
                st = s;
                break;
            }
        }
        Impl::JointRec jr;
        jr.c = m->sys.GetBodyInterface().CreateConstraint(st, b1, b2);
        if (!jr.c) continue;
        jr.hash = h;
        m->sys.AddConstraint(jr.c);
        m->joints[e] = jr;
        jc->broken = false;
    }

    // ---- ragdolls
    for (EntityId e : scene.entities()) {
        auto* ac = scene.get<AnimatorComponent>(e);
        bool want = ac && ac->ragdoll && providers.skeleton && providers.bonePose;
        auto it = m->ragdolls.find(e);
        if (!want) {
            if (it != m->ragdolls.end()) {
                for (auto& c : it->second.cons) m->sys.RemoveConstraint(c);
                for (auto id : it->second.bodies)
                    if (!id.IsInvalid()) {
                        m->byBody.erase(id.GetIndexAndSequenceNumber());
                        bi.RemoveBody(id);
                        bi.DestroyBody(id);
                    }
                m->ragdolls.erase(it);
            }
            continue;
        }
        if (it != m->ragdolls.end()) continue;
        auto* skel = static_cast<const anim::Skeleton*>(providers.skeleton(e));
        const std::vector<Mat4>* pose = providers.bonePose(e);
        if (!skel || !pose || pose->size() != skel->bones.size()) continue;
        Impl::Rag rg;
        rg.entityWorld = worlds[e];
        rg.bodies.assign(skel->bones.size(), JPH::BodyID());
        rg.offsets.assign(skel->bones.size(), Mat4{});
        rg.pose = *pose;
        std::vector<Mat4> boneW(skel->bones.size());
        for (size_t b = 0; b < skel->bones.size(); ++b) boneW[b] = rg.entityWorld * (*pose)[b];
        for (size_t b = 0; b < skel->bones.size(); ++b) {
            int child = -1;
            for (size_t c = 0; c < skel->bones.size(); ++c)
                if (skel->bones[c].parent == static_cast<int>(b)) {
                    child = static_cast<int>(c);
                    break;
                }
            Vec3 a = boneW[b].c[3].xyz();
            Vec3 bpos = child >= 0 ? boneW[static_cast<size_t>(child)].c[3].xyz() : a + normalize((boneW[b] * Vec4(0, 1, 0, 0)).xyz()) * 0.1f;
            Vec3 seg = bpos - a;
            float len = std::max(0.05f, length(seg));
            float r = std::clamp(len * 0.22f, 0.04f, 0.12f);
            Vec3 dir = normalize(seg);
            // capsule along +Y rotated onto the bone direction
            Vec3 axis = cross({0, 1, 0}, dir);
            float ang = std::acos(std::clamp(dot(Vec3{0, 1, 0}, dir), -1.0f, 1.0f));
            Quat q = length(axis) > 1e-5f ? Quat::fromAxisAngle(normalize(axis), ang) : (dir.y < 0 ? Quat::fromAxisAngle({1, 0, 0}, 3.14159265f) : Quat{});
            Vec3 mid = (a + bpos) * 0.5f;
            JPH::BodyCreationSettings bs(new JPH::CapsuleShape(std::max(0.01f, len * 0.5f - r), r), J(mid), J(q), JPH::EMotionType::Dynamic, Layers::MOVING);
            bs.mUserData = e;
            bs.mLinearDamping = 0.1f;
            bs.mAngularDamping = 0.6f;
            JPH::BodyID id = bi.CreateAndAddBody(bs, JPH::EActivation::Activate);
            rg.bodies[b] = id;
            Mat4 bodyW = Mat4::translation(mid) * Mat4::rotation(q);
            rg.offsets[b] = bodyW.inverse() * boneW[b];
            if (skel->bones[b].parent >= 0 && !rg.bodies[static_cast<size_t>(skel->bones[b].parent)].IsInvalid()) {
                JPH::SwingTwistConstraintSettings st;
                st.mPosition1 = st.mPosition2 = J(a);
                st.mTwistAxis1 = st.mTwistAxis2 = J(dir);
                Vec3 pl = std::fabs(dir.y) < 0.9f ? normalize(cross(dir, {0, 1, 0})) : normalize(cross(dir, {1, 0, 0}));
                st.mPlaneAxis1 = st.mPlaneAxis2 = J(pl);
                st.mNormalHalfConeAngle = radians(45);
                st.mPlaneHalfConeAngle = radians(45);
                st.mTwistMinAngle = radians(-30);
                st.mTwistMaxAngle = radians(30);
                JPH::Ref<JPH::TwoBodyConstraint> c = bi.CreateConstraint(&st, rg.bodies[static_cast<size_t>(skel->bones[b].parent)], id);
                if (c) {
                    m->sys.AddConstraint(c);
                    rg.cons.push_back(JPH::Ref<JPH::Constraint>(c.GetPtr()));
                }
            }
        }
        m->ragdolls[e] = std::move(rg);
    }
}

namespace {
Vec3 axisAngle(Quat q) {
    q = q.normalized();
    if (q.w < 0) {
        q.x = -q.x;
        q.y = -q.y;
        q.z = -q.z;
        q.w = -q.w;
    }
    Vec3 v{q.x, q.y, q.z};
    float s = length(v);
    if (s < 1e-8f) return {};
    return v * (2.0f * std::atan2(s, q.w) / s);
}
} // namespace

void PhysicsWorld::setActiveRagdoll(EntityId entity, ActiveRagdollDrive drive) {
    if (!drive.enabled) {
        m->drives.erase(entity);
        return;
    }
    drive.strength = std::clamp(drive.strength, 0.0f, 1.0f);
    drive.stiffness = std::clamp(drive.stiffness, 0.0f, 400.0f);
    drive.damping = std::clamp(drive.damping, 0.0f, 80.0f);
    drive.balance = std::clamp(drive.balance, 0.0f, 2.0f);
    drive.stumbleSpeed = std::max(0.0f, drive.stumbleSpeed);
    drive.recovery = std::max(0.05f, drive.recovery);
    m->drives[entity] = std::move(drive);
}

ActiveRagdollStatus PhysicsWorld::activeRagdollStatus(EntityId entity) const {
    auto rag = m->ragdolls.find(entity);
    if (rag == m->ragdolls.end() || !m->drives.count(entity)) return {};
    return {rag->second.motor, rag->second.upright, rag->second.brace, rag->second.mode};
}

void PhysicsWorld::step(Scene& scene, float dt) {
    auto& bi = m->sys.GetBodyInterface();
    m->sys.SetGravity(J(gravity));
    // ---- inputs: rigid bodies
    for (auto& [e, b] : m->bodies) {
        auto* rb = scene.get<RigidBodyComponent>(e);
        if (!rb || b.motion != MotionType::Dynamic) continue;
        if (length(rb->velocity - b.lastVel) > 1e-5f) bi.SetLinearVelocity(b.id, J(rb->velocity));
        if (length(rb->angularVelocity - b.lastAngVel) > 1e-5f) bi.SetAngularVelocity(b.id, J(rb->angularVelocity));
        if (length(rb->force) > 0) {
            bi.AddForce(b.id, J(rb->force));
            rb->force = {0, 0, 0};
        }
        if (length(rb->impulse) > 0) {
            bi.AddImpulse(b.id, J(rb->impulse));
            rb->impulse = {0, 0, 0};
        }
    }
    // ---- vehicles: driver input
    for (auto& [e, v] : m->vehicles) {
        auto* vc = scene.get<VehicleComponent>(e);
        if (!vc) continue;
        auto* ctl = static_cast<JPH::WheeledVehicleController*>(v.c->GetController());
        float thr = std::clamp(vc->throttle, -1.0f, 1.0f), brake = std::clamp(vc->brake, 0.0f, 1.0f);
        if (v.bike && brake > 0) {  // full brakes while leaned over would wash out the front: scale by the lean
            JPH::Quat r = bi.GetRotation(v.body);
            float sinLean = std::fabs(JPH::Vec3::sAxisY().Cross(r * JPH::Vec3::sAxisY()).Dot(r * JPH::Vec3(0, 0, -1)));
            brake *= (1.0f - sinLean) * (1.0f - sinLean);
        }
        ctl->SetDriverInput(thr, std::clamp(vc->steer, -1.0f, 1.0f), brake, std::clamp(vc->handbrake, 0.0f, 1.0f));
        if (thr != 0 || vc->steer != 0 || brake != 0) bi.ActivateBody(v.body);
    }
    // ---- characters (kinematic, before the simulation step)
    JPH::CharacterVirtual::ExtendedUpdateSettings eus;
    for (auto& [e, ch] : m->chars) {
        auto* cc = scene.get<CharacterControllerComponent>(e);
        if (!cc) continue;
        eus.mWalkStairsStepUp = JPH::Vec3(0, cc->stepHeight, 0);
        eus.mStickToFloorStepDown = JPH::Vec3(0, -cc->stepHeight, 0);
        ch.c->UpdateGroundVelocity();
        bool grounded = ch.c->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        Vec3 v{cc->move.x, 0, cc->move.z};
        if (grounded) {
            v += F(ch.c->GetGroundVelocity());
            ch.vy = 0;
            if (cc->jump) {
                ch.vy = cc->jumpSpeed;
                cc->jump = false;
            }
        } else {
            cc->jump = false;
            ch.vy += gravity.y * dt;
        }
        v.y = ch.vy + (grounded ? F(ch.c->GetGroundVelocity()).y : 0);
        ch.c->SetLinearVelocity(J(v));
        const bool filtered = !cc->collideWithDynamic || (cc->ignore.is_array() && !cc->ignore.empty());
        if (!filtered) {  // default: collide with every body (unchanged)
            ch.c->ExtendedUpdate(dt, J(gravity), eus, m->sys.GetDefaultBroadPhaseLayerFilter(Layers::MOVING), m->sys.GetDefaultLayerFilter(Layers::MOVING),
                                 JPH::BodyFilter(), JPH::ShapeFilter(), *m->temp);
        } else {
            std::vector<EntityId> ignoreIds;
            if (cc->ignore.is_array())
                for (const Json& n : cc->ignore)
                    if (n.is_string())
                        if (EntityId t = scene.findByName(n.get<std::string>())) {
                            ignoreIds.push_back(t);
                            for (EntityId d : scene.descendants(t)) ignoreIds.push_back(d);
                        }
            CharacterBodyFilter filter(ignoreIds, cc->collideWithDynamic);
            ch.c->ExtendedUpdate(dt, J(gravity), eus, m->sys.GetDefaultBroadPhaseLayerFilter(Layers::MOVING), m->sys.GetDefaultLayerFilter(Layers::MOVING),
                                 filter, JPH::ShapeFilter(), *m->temp);
        }
        Vec3 p = F(ch.c->GetPosition());
        auto* tc = scene.get<TransformComponent>(e);
        EntityId parent = scene.parentOf(e);
        tc->position = parent ? scene.parentFrame(e).inverse().transformPoint(p) : p;
        cc->grounded = ch.c->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        cc->velocity = F(ch.c->GetLinearVelocity());
        if (cc->driveAnimator)
            if (auto* an = scene.get<AnimatorComponent>(e)) {
                if (!an->params.is_object()) an->params = Json::object();
                an->params["speed"] = std::sqrt(cc->move.x * cc->move.x + cc->move.z * cc->move.z);
                if (ch.vy > 0 && !cc->grounded && v.y == cc->jumpSpeed) an->params["jump"] = true;
                an->rootMotion = false;  // the controller moves the entity
            }
    }
    // ---- active ragdolls (1.3.5): PD motors toward the animated pose, then the simulation
    std::map<EntityId, float> hits;
    {
        std::lock_guard lock(m->evMutex);
        hits.swap(m->hitSpeed);
    }
    for (auto& [e, drive] : m->drives) {
        auto rag = m->ragdolls.find(e);
        if (rag == m->ragdolls.end()) continue;
        auto& rg = rag->second;
        if (!rg.named)
            if (const void* sk = providers.skeleton ? providers.skeleton(e) : nullptr) {
                const auto* skeleton = static_cast<const anim::Skeleton*>(sk);
                rg.armL = skeleton->find("UpperArm.L");
                rg.armR = skeleton->find("UpperArm.R");
                rg.named = true;
            }
        float hit = 0;
        if (auto h = hits.find(e); h != hits.end()) hit = h->second;
        int hips = 0;
        if (!rg.bodies.empty() && !rg.bodies[static_cast<size_t>(hips)].IsInvalid()) {
            Quat hipsRot = F(bi.GetRotation(rg.bodies[static_cast<size_t>(hips)]));
            rg.upright = std::clamp(dot(hipsRot.rotate({0, 1, 0}), Vec3{0, 1, 0}), -1.0f, 1.0f);
        }
        if (hit >= drive.stumbleSpeed) {
            rg.motor = std::min(rg.motor, 0.15f);
            rg.brace = std::max(rg.brace, 0.45f);
        }
        rg.motor = std::min(drive.strength, rg.motor + dt / drive.recovery);
        rg.brace = std::max(0.0f, rg.brace - dt);
        rg.mode = rg.brace > 0.05f ? "brace" : rg.motor < drive.strength * 0.5f && rg.upright < 0.45f ? "stumble"
                                                                                                     : rg.motor < drive.strength - 0.12f ? "recover" : "track";
        if (drive.target.size() != rg.bodies.size() || rg.motor <= 0.001f) continue;
        const float follow = std::clamp(drive.stiffness / 40.0f, 0.25f, 4.0f) * rg.motor;
        for (size_t b = 0; b < rg.bodies.size(); ++b) {
            if (rg.bodies[b].IsInvalid()) continue;
            Mat4 desiredBone = rg.entityWorld * drive.target[b];
            Quat desired = decompose(desiredBone * rg.offsets[b].inverse()).rotation;
            Quat current = F(bi.GetRotation(rg.bodies[b]));
            Vec3 turn = axisAngle(desired * current.conjugate()) * (8.0f * follow);
            if ((static_cast<int>(b) == rg.armL || static_cast<int>(b) == rg.armR) && rg.brace > 0) {
                Vec3 along = current.rotate({0, 1, 0});
                Vec3 lift = cross(along, Vec3{0, 1, 0});
                if (length(lift) > 1e-4f) turn += normalize(lift) * rg.brace * 6.0f;
            }
            if (static_cast<int>(b) == hips && drive.balance > 0) {
                Vec3 up = current.rotate({0, 1, 0});
                Vec3 back = cross(up, Vec3{0, 1, 0});
                if (length(back) > 1e-4f) turn += normalize(back) * (1.0f - std::max(rg.upright, 0.0f)) * drive.balance * 6.0f;
                Vec3 target = desiredBone.c[3].xyz();
                Vec3 velocity = (target - F(bi.GetCenterOfMassPosition(rg.bodies[b]))) * (6.0f * follow);
                if (length(velocity) > 8.0f) velocity = normalize(velocity) * 8.0f;
                bi.SetLinearVelocity(rg.bodies[b], J(velocity));
            }
            if (length(turn) > 12.0f) turn = normalize(turn) * 12.0f;
            bi.ActivateBody(rg.bodies[b]);
            bi.SetAngularVelocity(rg.bodies[b], J(turn));
        }
    }
    // ---- simulate
    m->sys.Update(dt, 1, m->temp.get(), m->jobs.get());
    // ---- write back
    auto setWorld = [&](EntityId e, Vec3 p, Quat r) {
        auto* tc = scene.get<TransformComponent>(e);
        if (!tc) return;
        EntityId parent = scene.parentOf(e);
        if (parent) {
            Transform lt = decompose(scene.parentFrame(e).inverse() * (Mat4::translation(p) * Mat4::rotation(r)));
            tc->position = lt.position;
            tc->rotation = lt.rotation;
        } else {
            tc->position = p;
            tc->rotation = r;
        }
    };
    for (auto& [e, b] : m->bodies) {
        if (b.motion != MotionType::Dynamic) continue;
        JPH::RVec3 p;
        JPH::Quat r;
        bi.GetPositionAndRotation(b.id, p, r);
        setWorld(e, F(p), F(r));
        b.lastPos = F(p);
        b.lastRot = F(r);
        if (auto* rb = scene.get<RigidBodyComponent>(e)) {
            rb->velocity = b.lastVel = F(bi.GetLinearVelocity(b.id));
            rb->angularVelocity = b.lastAngVel = F(bi.GetAngularVelocity(b.id));
        }
    }
    for (auto& [e, v] : m->vehicles) {
        auto* vc = scene.get<VehicleComponent>(e);
        if (!vc) continue;
        auto* ctl = static_cast<JPH::WheeledVehicleController*>(v.c->GetController());
        JPH::Vec3 vel = bi.GetLinearVelocity(v.body);
        JPH::Quat rot = bi.GetRotation(v.body);
        JPH::Vec3 fwd = rot * JPH::Vec3(0, 0, -1);
        vc->speedKmh = vel.Dot(fwd) * 3.6f;
        vc->rpm = ctl->GetEngine().GetCurrentRPM();
        vc->gear = ctl->GetTransmission().GetCurrentGear();
        // wheel visuals: children named Wheel0..3 follow the simulated wheels
        for (uint32_t i = 0; i < v.c->GetWheels().size(); ++i) {
            EntityId w = kNullEntity;
            for (EntityId c : scene.childrenOf(e))
                if (scene.displayName(c) == "Wheel" + std::to_string(i)) w = c;
            if (!w) continue;
            JPH::Mat44 lm = v.c->GetWheelLocalTransform(i, JPH::Vec3::sAxisX(), JPH::Vec3::sAxisY());
            if (auto* tc = scene.get<TransformComponent>(w)) {
                tc->position = F(lm.GetTranslation());
                tc->rotation = F(lm.GetQuaternion());
            }
        }
    }
    for (auto& [e, j] : m->joints) {
        auto* jc = scene.get<JointComponent>(e);
        if (!jc || jc->breakForce <= 0 || jc->broken || !j.c->GetEnabled()) continue;
        float lambda = 0;
        switch (jc->type) {
            case JointType::Fixed: lambda = static_cast<JPH::FixedConstraint*>(j.c.GetPtr())->GetTotalLambdaPosition().Length(); break;
            case JointType::Hinge: lambda = static_cast<JPH::HingeConstraint*>(j.c.GetPtr())->GetTotalLambdaPosition().Length(); break;
            case JointType::Distance: lambda = std::fabs(static_cast<JPH::DistanceConstraint*>(j.c.GetPtr())->GetTotalLambdaPosition()); break;
            case JointType::Point: lambda = static_cast<JPH::PointConstraint*>(j.c.GetPtr())->GetTotalLambdaPosition().Length(); break;
        }
        if (lambda / std::max(1e-6f, dt) > jc->breakForce) {
            j.c->SetEnabled(false);
            jc->broken = true;
        }
    }
    for (auto& [e, rg] : m->ragdolls) {
        Mat4 inv = rg.entityWorld.inverse();
        for (size_t b = 0; b < rg.bodies.size(); ++b) {
            if (rg.bodies[b].IsInvalid()) continue;
            JPH::RVec3 p;
            JPH::Quat r;
            bi.GetPositionAndRotation(rg.bodies[b], p, r);
            rg.pose[b] = inv * (Mat4::translation(F(p)) * Mat4::rotation(F(r)) * rg.offsets[b]);
        }
    }
    // damage-lite: hard hits hurt vehicles
    std::lock_guard l(m->evMutex);
    for (auto& ev : m->events)
        if (ev.kind == ContactKind::Enter && ev.speed > 5)
            for (EntityId x : {ev.a, ev.b})
                if (auto* vc = scene.get<VehicleComponent>(x)) vc->health = std::max(0.0f, vc->health - (ev.speed - 5) * 4);
}

const std::vector<Mat4>* PhysicsWorld::ragdollPose(EntityId e) const {
    auto it = m->ragdolls.find(e);
    return it == m->ragdolls.end() ? nullptr : &it->second.pose;
}

std::optional<RayHit> PhysicsWorld::raycast(Vec3 origin, Vec3 dir, float maxDistance, EntityId ignore) const {
    float len = length(dir);
    if (len < 1e-6f) return std::nullopt;
    dir = dir / len;
    JPH::RRayCast ray{J(origin), J(dir * maxDistance)};
    JPH::RayCastResult res;
    JPH::IgnoreSingleBodyFilter ignoreBody(ignore && m->bodies.count(ignore) ? m->bodies.at(ignore).id : JPH::BodyID());
    JPH::BodyFilter all;
    if (!m->sys.GetNarrowPhaseQuery().CastRay(ray, res, {}, {}, ignore ? static_cast<const JPH::BodyFilter&>(ignoreBody) : all)) return std::nullopt;
    RayHit h;
    JPH::RVec3 p = ray.GetPointOnRay(res.mFraction);
    h.point = F(p);
    h.distance = res.mFraction * maxDistance;
    JPH::BodyLockRead lock(m->sys.GetBodyLockInterface(), res.mBodyID);
    if (lock.Succeeded()) {
        h.entity = static_cast<EntityId>(lock.GetBody().GetUserData());
        h.normal = F(lock.GetBody().GetWorldSpaceSurfaceNormal(res.mSubShapeID2, p));
    }
    return h;
}

std::optional<RayHit> PhysicsWorld::sphereCast(Vec3 origin, float radius, Vec3 dir, float maxDistance, EntityId ignore) const {
    float len = length(dir);
    if (len < 1e-6f) return std::nullopt;
    dir = dir / len;
    JPH::RefConst<JPH::Shape> sphere = new JPH::SphereShape(std::max(0.001f, radius));
    JPH::RShapeCast cast(sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(J(origin)), J(dir * maxDistance));
    JPH::ShapeCastSettings settings;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    JPH::IgnoreSingleBodyFilter ignoreBody(ignore && m->bodies.count(ignore) ? m->bodies.at(ignore).id : JPH::BodyID());
    JPH::BodyFilter all;
    m->sys.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector, {}, {}, ignore ? static_cast<const JPH::BodyFilter&>(ignoreBody) : all);
    if (!collector.HadHit()) return std::nullopt;
    RayHit h;
    h.distance = collector.mHit.mFraction * maxDistance;
    h.point = F(collector.mHit.mContactPointOn2);
    h.normal = F(-collector.mHit.mPenetrationAxis.Normalized());
    JPH::BodyLockRead lock(m->sys.GetBodyLockInterface(), collector.mHit.mBodyID2);
    if (lock.Succeeded()) h.entity = static_cast<EntityId>(lock.GetBody().GetUserData());
    return h;
}

namespace {
// Skips sensors (triggers) and the bodies of a list of entities (the ballistics owner and its hitboxes, ...).
// 1.3.2: and bodies whose collision layer (the group id, 0 when unset) is not in the mask.
int layerOfBody(const JPH::Body& b) {
    JPH::CollisionGroup::GroupID g = b.GetCollisionGroup().GetGroupID();
    return g < 32 ? static_cast<int>(g) : 0;
}
class IgnoreEntitiesFilter final : public JPH::BodyFilter {
public:
    IgnoreEntitiesFilter(const EntityId* ignore, int count, uint32_t layerMask = 0xFFFFFFFFu)
        : m_ignore(ignore), m_count(ignore ? count : 0), m_mask(layerMask) {}
    bool ShouldCollideLocked(const JPH::Body& b) const override {
        if (b.IsSensor()) return false;
        if (m_mask != 0xFFFFFFFFu && !(m_mask & (1u << layerOfBody(b)))) return false;
        auto e = static_cast<EntityId>(b.GetUserData());
        for (int i = 0; i < m_count; ++i)
            if (m_ignore[i] == e) return false;
        return true;
    }

private:
    const EntityId* m_ignore;
    int m_count;
    uint32_t m_mask;
};
// Closest hit per body, into a fixed buffer (no allocation).
class AllRayHits final : public JPH::CastRayCollector {
public:
    static constexpr int kMax = 64;
    JPH::RayCastResult hits[kMax];
    int count = 0, capacity = kMax;
    void AddHit(const JPH::RayCastResult& r) override {
        for (int i = 0; i < count; ++i)
            if (hits[i].mBodyID == r.mBodyID) {
                if (r.mFraction < hits[i].mFraction) hits[i] = r;
                return;
            }
        if (count < capacity) {
            hits[count++] = r;
            return;
        }
        int worst = 0;  // full: keep the closest ones
        for (int i = 1; i < count; ++i)
            if (hits[i].mFraction > hits[worst].mFraction) worst = i;
        if (r.mFraction < hits[worst].mFraction) hits[worst] = r;
    }
};
} // namespace

std::optional<RayHit> PhysicsWorld::raycastIgnoring(Vec3 origin, Vec3 dir, float maxDistance, const EntityId* ignore, int ignoreCount) const {
    return raycastFiltered(origin, dir, maxDistance, ignore, ignoreCount, kAllLayers);
}

int PhysicsWorld::bodyLayer(EntityId e) const {
    auto it = m->bodies.find(e);
    if (it == m->bodies.end()) return 0;
    JPH::BodyLockRead lock(m->sys.GetBodyLockInterface(), it->second.id);
    return lock.Succeeded() ? layerOfBody(lock.GetBody()) : 0;
}

std::optional<RayHit> PhysicsWorld::raycastFiltered(Vec3 origin, Vec3 dir, float maxDistance, const EntityId* ignore, int ignoreCount,
                                                    uint32_t layerMask) const {
    float len = length(dir);
    if (len < 1e-6f || maxDistance <= 0 || !std::isfinite(maxDistance) || layerMask == 0) return std::nullopt;
    dir = dir / len;
    JPH::RRayCast ray{J(origin), J(dir * maxDistance)};
    JPH::RayCastResult res;
    IgnoreEntitiesFilter filter(ignore, ignoreCount, layerMask);
    if (!m->sys.GetNarrowPhaseQuery().CastRay(ray, res, {}, {}, filter)) return std::nullopt;
    RayHit h;
    JPH::RVec3 p = ray.GetPointOnRay(res.mFraction);
    h.point = F(p);
    h.distance = res.mFraction * maxDistance;
    JPH::BodyLockRead lock(m->sys.GetBodyLockInterface(), res.mBodyID);
    if (lock.Succeeded()) {
        h.entity = static_cast<EntityId>(lock.GetBody().GetUserData());
        h.normal = F(lock.GetBody().GetWorldSpaceSurfaceNormal(res.mSubShapeID2, p));
    }
    return h;
}

std::optional<RayHit> PhysicsWorld::sphereCastIgnoring(Vec3 origin, float radius, Vec3 dir, float maxDistance, const EntityId* ignore,
                                                       int ignoreCount) const {
    return sphereCastFiltered(origin, radius, dir, maxDistance, ignore, ignoreCount, kAllLayers);
}

std::optional<RayHit> PhysicsWorld::sphereCastFiltered(Vec3 origin, float radius, Vec3 dir, float maxDistance, const EntityId* ignore,
                                                       int ignoreCount, uint32_t layerMask) const {
    float len = length(dir);
    if (len < 1e-6f || maxDistance <= 0 || !std::isfinite(maxDistance) || !std::isfinite(radius) || layerMask == 0) return std::nullopt;
    dir = dir / len;
    int key = static_cast<int>(std::lround(std::clamp(radius, 0.001f, 1000.0f) * 1e4f));  // 1.3.2: bounded (script / MCP input)
    auto it = m->sphereShapes.find(key);
    if (it == m->sphereShapes.end()) it = m->sphereShapes.emplace(key, new JPH::SphereShape(static_cast<float>(key) * 1e-4f)).first;
    JPH::RShapeCast cast(it->second, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(J(origin)), J(dir * maxDistance));
    JPH::ShapeCastSettings settings;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    IgnoreEntitiesFilter filter(ignore, ignoreCount, layerMask);
    m->sys.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector, {}, {}, filter);
    if (!collector.HadHit()) return std::nullopt;
    RayHit h;
    h.distance = collector.mHit.mFraction * maxDistance;
    h.point = F(collector.mHit.mContactPointOn2);
    h.normal = F(-collector.mHit.mPenetrationAxis.Normalized());
    JPH::BodyLockRead lock(m->sys.GetBodyLockInterface(), collector.mHit.mBodyID2);
    if (lock.Succeeded()) h.entity = static_cast<EntityId>(lock.GetBody().GetUserData());
    return h;
}

int PhysicsWorld::raycastAll(Vec3 origin, Vec3 dir, float maxDistance, RayHit* out, int capacity, const EntityId* ignore, int ignoreCount) const {
    float len = length(dir);
    if (len < 1e-6f || maxDistance <= 0 || capacity <= 0 || !out) return 0;
    dir = dir / len;
    JPH::RRayCast ray{J(origin), J(dir * maxDistance)};
    AllRayHits hits;
    hits.capacity = std::min(capacity, AllRayHits::kMax);
    IgnoreEntitiesFilter filter(ignore, ignoreCount);
    m->sys.GetNarrowPhaseQuery().CastRay(ray, JPH::RayCastSettings(), hits, {}, {}, filter);
    for (int i = 1; i < hits.count; ++i)  // insertion sort by distance (tiny n)
        for (int k = i; k > 0 && hits.hits[k].mFraction < hits.hits[k - 1].mFraction; --k) std::swap(hits.hits[k], hits.hits[k - 1]);
    for (int i = 0; i < hits.count; ++i) {
        const JPH::RayCastResult& r = hits.hits[i];
        RayHit& h = out[i];
        JPH::RVec3 p = ray.GetPointOnRay(r.mFraction);
        h = {};
        h.point = F(p);
        h.distance = r.mFraction * maxDistance;
        JPH::BodyLockRead lock(m->sys.GetBodyLockInterface(), r.mBodyID);
        if (lock.Succeeded()) {
            h.entity = static_cast<EntityId>(lock.GetBody().GetUserData());
            h.normal = F(lock.GetBody().GetWorldSpaceSurfaceNormal(r.mSubShapeID2, p));
        }
    }
    return hits.count;
}

Bytes PhysicsWorld::saveState() const {
    JPH::StateRecorderImpl rec;
    m->sys.SaveState(rec);
    for (auto& [e, ch] : m->chars) ch.c->SaveState(rec);
    std::string data = rec.GetData();
    return Bytes(data.begin(), data.end());
}

bool PhysicsWorld::restoreState(const Bytes& state) {
    JPH::StateRecorderImpl rec;
    rec.WriteBytes(state.data(), state.size());
    if (!m->sys.RestoreState(rec)) return false;
    for (auto& [e, ch] : m->chars) ch.c->RestoreState(rec);
    // keep the bookkeeping in step so sync() does not see a "teleport"
    auto& bi = m->sys.GetBodyInterface();
    for (auto& [e, b] : m->bodies) {
        JPH::RVec3 p;
        JPH::Quat r;
        bi.GetPositionAndRotation(b.id, p, r);
        b.lastPos = F(p);
        b.lastRot = F(r);
        b.lastVel = F(bi.GetLinearVelocity(b.id));
        b.lastAngVel = F(bi.GetAngularVelocity(b.id));
    }
    return true;
}

void PhysicsWorld::debugLines(std::vector<std::pair<Vec3, Vec3>>& out) const {
    auto& bi = m->sys.GetBodyLockInterface();
    auto box = [&](JPH::AABox b) {
        Vec3 mn = F(b.mMin), mx = F(b.mMax), c[8];
        for (int k = 0; k < 8; ++k) c[k] = {k & 1 ? mx.x : mn.x, k & 2 ? mx.y : mn.y, k & 4 ? mx.z : mn.z};
        const int ed[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (auto& e : ed) out.push_back({c[e[0]], c[e[1]]});
    };
    for (auto& [e, b] : m->bodies) {
        JPH::BodyLockRead lock(bi, b.id);
        if (lock.Succeeded()) box(lock.GetBody().GetWorldSpaceBounds());
    }
    for (auto& [e, ch] : m->chars) box(ch.c->GetShape()->GetWorldSpaceBounds(JPH::RMat44::sRotationTranslation(ch.c->GetRotation(), ch.c->GetPosition()), JPH::Vec3::sReplicate(1)));
}

Json fitPhysics(const MeshData* mesh, const std::string& meshRef, Vec3 worldScale) {
    Vec3 mn{-0.5f, -0.5f, -0.5f}, mx{0.5f, 0.5f, 0.5f};
    if (mesh && !mesh->empty()) meshBounds(*mesh, mn, mx);
    Vec3 size = mx - mn, centre = (mn + mx) * 0.5f;
    Vec3 world = size * worldScale;
    bool flat = world.y < 0.2f * std::max(world.x, world.z) && std::max(world.x, world.z) > 4;
    Json col;
    if (meshRef == "builtin:sphere") col = {{"shape", "Sphere"}, {"radius", std::max({size.x, size.y, size.z}) * 0.5f}, {"offset", centre}};
    else col = {{"shape", "Box"}, {"size", Vec3{std::max(size.x, 0.02f), std::max(size.y, 0.02f), std::max(size.z, 0.02f)}}, {"offset", centre}};
    Json rb = {{"motion", flat ? "Static" : "Dynamic"}};
    return {{"Collider", col}, {"RigidBody", rb}};
}

} // namespace fledge::physics
