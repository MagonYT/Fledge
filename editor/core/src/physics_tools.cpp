// Physics authoring (docs/PHYSICS.md): one-click "Add physics", vehicle presets, and the MCP tools. Mutating MCP
// tools run in one AI-tagged undo transaction (the registry's callWrapper).
#include <cmath>
#include <format>

#include "fledge/editor/editor_session.h"
#include "fledge/framework/game_world.h"
#include "fledge/gameplay/city.h"
#include "fledge/mcp/mcp_server.h"
#include "fledge/physics/physics.h"

namespace fledge {
using mcp::ToolResult;

namespace {
Json sch(std::initializer_list<std::pair<const std::string, Json>> props, std::vector<std::string> required = {}) {
    Json p = Json::object();
    for (auto& [k, v] : props) p[k] = v;
    Json s = {{"type", "object"}, {"properties", p}, {"additionalProperties", false}};
    if (!required.empty()) s["required"] = required;
    return s;
}
EntityId ref(EditorSession& s, const Json& r) {
    if (r.is_number_integer()) return s.scene().isValid(r.get<EntityId>()) ? r.get<EntityId>() : kNullEntity;
    if (r.is_string()) return s.scene().findByName(r.get<std::string>());
    return kNullEntity;
}
Vec3 vec3(const Json& j, Vec3 def = {}) {
    if (!j.is_array() || j.size() < 3) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
struct Preset {
    Vec3 size;
    float mass, torque, rpm, wheelRadius, wheelWidth, suspension;
    Vec3 color;
};
Preset presetOf(const std::string& name) {
    if (name == "truck") return {{2.4f, 1.4f, 7.0f}, 6000, 2500, 3500, 0.55f, 0.4f, 0.3f, {0.2f, 0.35f, 0.6f}};
    return {{1.8f, 0.7f, 4.2f}, 1400, 500, 6000, 0.38f, 0.25f, 0.25f, {0.75f, 0.12f, 0.1f}};  // car
}
} // namespace

const std::vector<std::string>& vehiclePresets() {
    static const std::vector<std::string> p = {"car", "truck", "bike"};
    return p;
}

EntityId createTree(EditorSession& s, const Json& node, EntityId parent);  // gameplay_tools.cpp

// Collider fitted to the mesh bounds + RigidBody, for each entity without a Collider. One undo step.
Result<int> addPhysicsToSelection(EditorSession& s, const std::vector<EntityId>& entities, const std::string& motion) {
    s.beginEdit("Add physics", EditSource::User);
    int n = 0;
    for (EntityId e : entities) {
        if (!s.scene().isValid(e) || s.scene().get<ColliderComponent>(e)) continue;
        std::string meshRef;
        if (auto* mr = s.scene().get<MeshRendererComponent>(e)) meshRef = mr->mesh;
        if (meshRef.empty() && !s.scene().get<TerrainComponent>(e)) continue;  // nothing to fit
        if (s.scene().get<TerrainComponent>(e)) continue;                        // terrains collide automatically
        Vec3 scale = decompose(s.scene().worldMatrix(e)).scale;
        Json fit = physics::fitPhysics(s.world().cpuMesh(meshRef), meshRef, scale);
        if (!motion.empty()) fit["RigidBody"]["motion"] = motion;
        if (auto st = s.applyComponents(e, fit, "Add physics"); !st) {
            s.cancelEdit();
            return Result<int>::failure(st.error);
        }
        ++n;
    }
    s.endEdit();
    return n;
}

// A drivable vehicle: chassis (Collider box + RigidBody + Vehicle), a body mesh and Wheel0..3 (posed by physics).
EntityId createVehicleFromPreset(EditorSession& s, const std::string& preset, Vec3 position, const std::string& name) {
    if (preset == "bike") {  // two wheels: the shared gameplay::vehicleNode layout (Wheel0 front, Wheel1 rear)
        s.beginEdit("Create vehicle", EditSource::User);
        EntityId root = createTree(s, gameplay::vehicleNode("bike", position, 0, name, {0.1f, 0.3f, 0.75f}), kNullEntity);
        if (!root) {
            s.cancelEdit();
            return kNullEntity;
        }
        s.endEdit();
        return root;
    }
    Preset p = presetOf(preset);
    s.beginEdit("Create vehicle", EditSource::User);
    Json comps = {{"Transform", {{"position", Vec3{position.x, position.y + p.wheelRadius + p.size.y * 0.5f + 0.3f, position.z}}}},
                  {"Collider", {{"shape", "Box"}, {"size", p.size}}},
                  {"RigidBody", {{"motion", "Dynamic"}, {"mass", p.mass}, {"friction", 0.5f}}},
                  {"Vehicle",
                   {{"preset", preset == "truck" ? "truck" : "car"},
                    {"maxEngineTorque", p.torque},
                    {"maxRpm", p.rpm},
                    {"wheelRadius", p.wheelRadius},
                    {"wheelWidth", p.wheelWidth},
                    {"suspension", p.suspension}}},
                  {"Tags", {{"tags", "vehicle"}}}};
    EntityId root = s.createEntity(name.empty() ? (preset == "truck" ? "Truck" : "Car") : name, kNullEntity, comps);
    if (!root) {
        s.cancelEdit();
        return kNullEntity;
    }
    s.createEntity("Body", root,
                   {{"Transform", {{"scale", p.size}}}, {"MeshRenderer", {{"mesh", "builtin:cube"}, {"color", p.color}}}});
    Vec3 cabin{p.size.x * 0.85f, p.size.y * 0.8f, p.size.z * (preset == "truck" ? 0.25f : 0.45f)};
    float cabinZ = preset == "truck" ? -p.size.z * 0.35f : p.size.z * 0.05f;
    s.createEntity("Cabin", root,
                   {{"Transform", {{"position", Vec3{0, p.size.y * 0.5f + cabin.y * 0.5f, cabinZ}}, {"scale", cabin}}},
                    {"MeshRenderer", {{"mesh", "builtin:cube"}, {"color", Vec3{0.15f, 0.18f, 0.22f}}}}});
    float wx = p.size.x * 0.5f - p.wheelWidth * 0.5f, wz = p.size.z * 0.5f - p.wheelRadius * 1.2f, wy = -p.size.y * 0.5f;
    const Vec3 wheelPos[4] = {{-wx, wy, -wz}, {wx, wy, -wz}, {-wx, wy, wz}, {wx, wy, wz}};
    for (int i = 0; i < 4; ++i)
        s.createEntity(std::format("Wheel{}", i), root,
                       {{"Transform", {{"position", wheelPos[i]}, {"scale", Vec3{p.wheelWidth, p.wheelRadius * 2, p.wheelRadius * 2}}}},
                        {"MeshRenderer", {{"mesh", "builtin:sphere"}, {"color", Vec3{0.08f, 0.08f, 0.08f}}}}});
    s.endEdit();
    return root;
}

void registerPhysicsTools(mcp::ToolRegistry& reg, EditorSession& s) {
    reg.add({"add_physics", "Add physics",
             "Adds a Collider fitted to each entity's mesh bounds plus a RigidBody (Dynamic; Static for large flat objects such as floors). "
             "motion overrides it (Static | Dynamic | Kinematic). Terrains and roads collide automatically. One undo step.",
             sch({{"entities", {{"type", "array"}, {"description", "entity ids or names"}}}, {"motion", {{"type", "string"}}}}, {"entities"}), false, false,
             [&s](const Json& a) {
                 std::vector<EntityId> es;
                 for (auto& r : a["entities"])
                     if (EntityId e = ref(s, r)) es.push_back(e);
                 if (es.empty()) return ToolResult::error("no valid entities");
                 auto n = addPhysicsToSelection(s, es, a.value("motion", std::string()));
                 if (!n) return ToolResult::error(n.error());
                 return ToolResult::json({{"added", *n}});
             }});
    reg.add({"create_vehicle_from_preset", "Create vehicle",
             "Creates a drivable wheeled vehicle (engine, automatic gearbox, suspension, steering, brakes, damage-lite health). presets: car | "
             "truck | bike (two wheels, leans into turns). Drive it with set_component Vehicle {throttle, steer, brake, handbrake} or Fledge Script "
             "Vehicle::SetInput.",
             sch({{"preset", {{"type", "string"}, {"enum", {"car", "truck", "bike"}}}}, {"position", {{"type", "array"}}}, {"name", {{"type", "string"}}}}),
             false, false, [&s](const Json& a) {
                 std::string preset = a.value("preset", std::string("car"));
                 if (preset != "car" && preset != "truck" && preset != "bike") return ToolResult::error("unknown preset '" + preset + "' (car | truck | bike)");
                 EntityId e = createVehicleFromPreset(s, preset, vec3(a.value("position", Json()), {0, 0, 0}), a.value("name", std::string()));
                 if (!e) return ToolResult::error("create_vehicle_from_preset failed");
                 return ToolResult::json({{"id", e}, {"name", s.scene().displayName(e)}});
             }});
    reg.add({"raycast", "Raycast",
             "Casts a ray against the exact physics colliders (and terrains / roads). While playing it uses the live simulation. Returns the "
             "hit entity, point, normal and distance, or hit:false. 1.3.2 filters (any of them switches to the filtered query, which never "
             "hits triggers): ignore (entity id / name; with ignoreHierarchy, default true, also its children), layerMask (integer bit mask, or "
             "an array of Collider.layer numbers 0..31; meshes / terrains without a collider are layer 0), radius > 0 = sphere cast.",
             sch({{"origin", {{"type", "array"}}}, {"direction", {{"type", "array"}}}, {"maxDistance", {{"type", "number"}}},
                  {"ignore", {{"description", "entity id or name that is never hit"}}}, {"ignoreHierarchy", {{"type", "boolean"}}},
                  {"layerMask", {{"description", "integer bit mask or array of layer numbers"}}}, {"radius", {{"type", "number"}}}},
                 {"origin", "direction"}),
             true, false, [&s](const Json& a) {
                 Vec3 o = vec3(a["origin"]), d = vec3(a["direction"]);
                 float maxD = a.value("maxDistance", 1000.0f);
                 std::optional<physics::RayHit> h;
                 if (a.contains("ignore") || a.contains("layerMask") || a.contains("radius") || a.contains("ignoreHierarchy")) {
                     GameWorld::QueryFilter f;
                     if (a.contains("ignore") && !a["ignore"].is_null()) {
                         f.ignore = ref(s, a["ignore"]);
                         if (!f.ignore) return ToolResult::error("raycast: ignore entity not found: " + a["ignore"].dump());
                     }
                     f.ignoreHierarchy = a.value("ignoreHierarchy", true);
                     if (a.contains("layerMask")) {
                         const Json& m = a["layerMask"];
                         if (m.is_number_integer() && m.get<int64_t>() >= 0 && m.get<int64_t>() <= 0xFFFFFFFFll) {
                             f.layerMask = static_cast<uint32_t>(m.get<int64_t>());
                         } else if (m.is_array()) {
                             f.layerMask = 0;
                             for (auto& l : m) {
                                 if (!l.is_number_integer() || l.get<int>() < 0 || l.get<int>() > 31) return ToolResult::error("raycast: layers are 0..31");
                                 f.layerMask |= 1u << l.get<int>();
                             }
                         } else {
                             return ToolResult::error("raycast: layerMask must be an integer bit mask or an array of layer numbers 0..31");
                         }
                     }
                     float radius = a.value("radius", 0.0f);
                     if (!std::isfinite(radius) || radius < 0 || radius > 100) return ToolResult::error("raycast: radius must be 0..100 m");
                     std::optional<script::RaycastHit> r;
                     auto run = [&](GameWorld& w) { r = radius > 0 ? w.sphereCast(o, radius, d, maxD, f) : w.raycast(o, d, maxD, f); };
                     if (s.world().physics()) {
                         run(s.world());
                     } else {  // edit mode: a temporary simulation of the current scene answers the collider part
                         physics::PhysicsWorld tmp;
                         tmp.providers.mesh = [&s](const std::string& m) { return s.world().cpuMesh(m); };
                         tmp.sync(s.scene());
                         std::vector<EntityId> ig;
                         if (f.ignore) {
                             ig.push_back(f.ignore);
                             if (f.ignoreHierarchy)
                                 for (EntityId c : s.scene().descendants(f.ignore)) ig.push_back(c);
                         }
                         auto ph = radius > 0 ? tmp.sphereCastFiltered(o, radius, d, maxD, ig.data(), static_cast<int>(ig.size()), f.layerMask)
                                              : tmp.raycastFiltered(o, d, maxD, ig.data(), static_cast<int>(ig.size()), f.layerMask);
                         if (ph) r = script::RaycastHit{ph->entity, ph->point, ph->normal, ph->distance};
                         if (radius <= 0)  // mesh bounds + terrains of entities without a collider
                             if (auto mb = s.world().raycast(o, d, maxD, f); mb && (!r || mb->distance < r->distance)) r = mb;
                     }
                     if (r) h = physics::RayHit{r->entity, r->point, r->normal, r->distance};
                 } else if (auto* pw = s.world().physics()) {
                     h = pw->raycast(o, d, maxD);
                 } else {  // edit mode: a temporary world from the current scene
                     auto r = s.world().raycast(o, d, maxD);  // meshes + terrains (bounds)
                     physics::PhysicsWorld tmp;
                     tmp.providers.mesh = [&s](const std::string& m) { return s.world().cpuMesh(m); };
                     tmp.sync(s.scene());
                     h = tmp.raycast(o, d, maxD);
                     if (r && (!h || r->distance < h->distance)) h = physics::RayHit{r->entity, r->point, r->normal, r->distance};
                 }
                 if (!h) return ToolResult::json({{"hit", false}});
                 return ToolResult::json({{"hit", true},
                                          {"entity", h->entity},
                                          {"name", s.scene().isValid(h->entity) ? s.scene().displayName(h->entity) : ""},
                                          {"point", h->point},
                                          {"normal", h->normal},
                                          {"distance", h->distance}});
             }});
    reg.add({"get_physics_status", "Physics status",
             "Whether physics runs (it does while playing when the scene has colliders, characters, vehicles or ragdolls), body / character / "
             "vehicle counts, and live vehicle telemetry (speed km/h, rpm, gear, health).",
             sch({}), true, false, [&s](const Json&) {
                 auto* pw = s.world().physics();
                 Json j = {{"running", pw != nullptr}, {"fixedStep", s.world().physicsStep}};
                 if (pw) {
                     j["bodies"] = pw->bodyCount();
                     j["characters"] = pw->characterCount();
                     j["vehicles"] = pw->vehicleCount();
                 }
                 Json v = Json::array();
                 for (EntityId e : s.scene().entities())
                     if (auto* vc = s.scene().get<VehicleComponent>(e))
                         v.push_back({{"entity", e}, {"name", s.scene().displayName(e)}, {"speedKmh", vc->speedKmh}, {"rpm", vc->rpm}, {"gear", vc->gear},
                                      {"health", vc->health}, {"driver", vc->driver}});
                 j["vehicleTelemetry"] = v;
                 return ToolResult::json(j);
             }});
}

} // namespace fledge
