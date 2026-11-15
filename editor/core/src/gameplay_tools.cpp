// Gameplay authoring (docs/GAMEPLAY.md): undoable creation of generated hierarchies (cities, vehicles), one-click
// player / traffic / day-night setup, and the MCP tools. Mutating MCP tools run in one AI-tagged undo transaction.
#include <format>

#include "fledge/editor/editor_session.h"
#include "fledge/gameplay/city.h"
#include "fledge/gameplay/mission.h"
#include "fledge/gameplay/traffic.h"
#include "fledge/mcp/mcp_server.h"

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
EntityId settingsEntity(EditorSession& s) {
    for (EntityId e : s.scene().entities())
        if (s.scene().get<TrafficComponent>(e) || s.scene().get<TimeOfDayComponent>(e) || s.scene().get<WorldPartitionComponent>(e)) return e;
    EntityId e = s.scene().findByName("World Settings");
    return e ? e : s.createEntity("World Settings");
}
} // namespace

// Creates a {"name", "components", "children"} tree as undoable entities (inside the caller's edit, if any).
EntityId createTree(EditorSession& s, const Json& node, EntityId parent) {
    EntityId e = s.createEntity(node.value("name", std::string("Entity")), parent, node.value("components", Json::object()));
    if (!e) return kNullEntity;
    if (node.contains("children"))
        for (auto& ch : node["children"]) createTree(s, ch, e);
    return e;
}

Result<EntityId> addCityDistrict(EditorSession& s, const Json& params) {
    gameplay::CityParams p = gameplay::cityParamsFromJson(params);
    gameplay::CityResult r = gameplay::generateCity(p);
    s.beginEdit("Generate city district");
    EntityId root = createTree(s, r.root, kNullEntity);
    s.endEdit();
    if (!root) return Result<EntityId>::failure("could not create the district");
    return root;
}

// One click: the selection (or a new mannequin) becomes the player with health, inventory and a wanted level.
EntityId makePlayer(EditorSession& s, EntityId e, Vec3 at) {
    s.beginEdit("Make player");
    if (!e) e = s.createEntity("Player", kNullEntity, {{"Transform", {{"position", {at.x, at.y + 0.3f, at.z}}}}});
    Scene& sc = s.scene();
    Json c = Json::object();
    if (!sc.get<AnimatorComponent>(e) && !sc.get<MeshRendererComponent>(e)) c["Animator"] = {{"rootMotion", false}};
    if (!sc.get<CharacterControllerComponent>(e)) c["CharacterController"] = Json::object();
    if (!sc.get<PlayerControllerComponent>(e)) c["PlayerController"] = Json::object();
    if (!sc.get<HealthComponent>(e)) c["Health"] = {{"faction", "player"}};
    if (!sc.get<InventoryComponent>(e)) c["Inventory"] = Json::object();
    if (!sc.get<WantedComponent>(e)) c["Wanted"] = Json::object();
    if (!c.empty()) (void)s.applyComponents(e, c, "Make player");
    if (s.scene().displayName(e) != "Player") (void)s.rename(e, "Player");
    s.endEdit();
    return e;
}

void registerGameplayTools(mcp::ToolRegistry& reg, EditorSession& s) {
    reg.add({"generate_city_district", "Generate city district",
             "Builds a city district: a grid of road segments (real intersections for traffic), buildings with colliders, parks with "
             "trees and a ground slab. Params: blocksX, blocksZ (1..32), blockSize (m), roadWidth, sidewalk, minHeight, maxHeight, "
             "parkChance, seed, origin [x,y,z] (corner), name. One [AI] undo step. Add traffic with set_traffic.",
             sch({{"blocksX", {{"type", "integer"}}}, {"blocksZ", {{"type", "integer"}}}, {"blockSize", {{"type", "number"}}}, {"roadWidth", {{"type", "number"}}},
                  {"sidewalk", {{"type", "number"}}}, {"minHeight", {{"type", "number"}}}, {"maxHeight", {{"type", "number"}}}, {"parkChance", {{"type", "number"}}},
                  {"seed", {{"type", "integer"}}}, {"origin", {{"type", "array"}}}, {"name", {{"type", "string"}}}}),
             false, false, [&s](const Json& a) {
                 auto r = addCityDistrict(s, a);
                 if (!r) return ToolResult::error(r.error());
                 int roads = 0, buildings = 0;
                 for (EntityId e : s.scene().descendants(*r)) {
                     roads += s.scene().get<SplineComponent>(e) ? 1 : 0;
                     buildings += s.scene().displayName(e).rfind("Building", 0) == 0 ? 1 : 0;
                 }
                 return ToolResult::json({{"id", *r}, {"name", s.scene().displayName(*r)}, {"roads", roads}, {"buildings", buildings}});
             }});
    reg.add({"set_traffic", "Set traffic",
             "Ambient traffic and pedestrians on the scene's roads (World Settings). cars, pedestrians, radius (0 = whole map), seed, "
             "enabled. One [AI] undo step.",
             sch({{"cars", {{"type", "integer"}}}, {"pedestrians", {{"type", "integer"}}}, {"radius", {{"type", "number"}}}, {"seed", {{"type", "integer"}}},
                  {"enabled", {{"type", "boolean"}}}}),
             false, false, [&s](const Json& a) {
                 EntityId e = settingsEntity(s);
                 if (auto st = s.applyComponents(e, {{"Traffic", a.is_object() ? a : Json::object()}}, "Set traffic"); !st) return ToolResult::error(st.error);
                 return ToolResult::json(s.scene().entityToJson(e)["components"]["Traffic"]);
             }});
    reg.add({"set_time_of_day", "Set time of day",
             "Sets the hour (0..24) and how many real minutes a game day lasts (0 = frozen) on World Settings; the sun, moon, light colour "
             "and sky follow. One [AI] undo step.",
             sch({{"hour", {{"type", "number"}}}, {"dayMinutes", {{"type", "number"}}}, {"latitude", {{"type", "number"}}}}), false, false, [&s](const Json& a) {
                 EntityId e = settingsEntity(s);
                 if (auto st = s.applyComponents(e, {{"TimeOfDay", a.is_object() ? a : Json::object()}}, "Set time of day"); !st) return ToolResult::error(st.error);
                 return ToolResult::json(s.scene().entityToJson(e)["components"]["TimeOfDay"]);
             }});
    reg.add({"set_weather", "Set weather",
             "Weather preset: Clear | Cloudy | Overcast | Rain | Storm | Fog (clouds dim the light; rain plays rain sound); transition "
             "seconds. One [AI] undo step.",
             sch({{"preset", {{"type", "string"}, {"enum", {"Clear", "Cloudy", "Overcast", "Rain", "Storm", "Fog"}}}}, {"transition", {{"type", "number"}}}},
                 {"preset"}),
             false, false, [&s](const Json& a) {
                 EntityId e = settingsEntity(s);
                 if (auto st = s.applyComponents(e, {{"Weather", a}}, "Set weather"); !st) return ToolResult::error(st.error);
                 return ToolResult::json(s.scene().entityToJson(e)["components"]["Weather"]);
             }});
    reg.add({"write_mission", "Write mission",
             "Creates or replaces a mission graph (an entity with a Mission component). graph = {name, nodes: [{id, type, ...params}], "
             "links: [{from, to, on?: \"fail\"}]}. Node types: Start, GoTo{target|position, radius, text}, EnterVehicle{target}, "
             "DriveTo{target|position, radius}, Collect{item, count}, Eliminate{target}, Wait{seconds}, Dialogue{text, seconds}, "
             "SetWanted{level}, LoseWanted, Timer{seconds; fail link}, Reward{money, item, count}, End{success, text, reward}. The graph "
             "is validated first (errors are returned, nothing changes). One [AI] undo step.",
             sch({{"name", {{"type", "string"}}}, {"graph", {{"type", "object"}}}, {"autoStart", {{"type", "boolean"}}}}, {"graph"}), false, false,
             [&s](const Json& a) {
                 gameplay::MissionGraph g = gameplay::MissionGraph::fromJson(a["graph"]);
                 auto problems = g.validate();
                 if (!problems.empty()) {
                     std::string msg = "mission graph has problems:";
                     for (auto& p : problems) msg += "\n- " + p;
                     return ToolResult::error(msg);
                 }
                 std::string name = a.value("name", g.name.empty() ? std::string("Mission") : g.name);
                 EntityId e = s.scene().findByName(name);
                 Json comp = {{"graph", g.toJson()}, {"autoStart", a.value("autoStart", true)}};
                 if (e && s.scene().get<MissionComponent>(e)) {
                     if (auto st = s.applyComponents(e, {{"Mission", comp}}, "Write mission"); !st) return ToolResult::error(st.error);
                 } else {
                     e = s.createEntity(name, kNullEntity, {{"Mission", comp}});
                 }
                 if (!e) return ToolResult::error("could not create the mission");
                 return ToolResult::json({{"id", e}, {"name", name}, {"nodes", g.nodes.size()}, {"links", g.links.size()}});
             }});
    reg.add({"make_player", "Make player",
             "Turns an entity (or a new mannequin at position) into the player: PlayerController (walk, run, jump, enter/exit and "
             "carjack vehicles, shoot), CharacterController, Health, Inventory, Wanted. One [AI] undo step.",
             sch({{"entity", {{"description", "entity id or name (optional)"}}}, {"position", {{"type", "array"}}}}), false, false, [&s](const Json& a) {
                 EntityId e = a.contains("entity") ? ref(s, a["entity"]) : kNullEntity;
                 Vec3 at{0, 0, 0};
                 if (a.contains("position") && a["position"].is_array() && a["position"].size() == 3)
                     at = {a["position"][0].get<float>(), a["position"][1].get<float>(), a["position"][2].get<float>()};
                 EntityId p = makePlayer(s, e, at);
                 return ToolResult::json({{"id", p}, {"name", s.scene().displayName(p)}});
             }});
    reg.add({"get_gameplay_status", "Gameplay status",
             "While playing: traffic cars / pedestrians and their safety stats, police units, wanted level, missions (state, objective, "
             "current node), speed, hour, navmesh. In edit mode: what the scene is set up with.",
             sch({}), true, false, [&s](const Json&) {
                 auto st = s.world().gameplayStats();
                 Json missions = Json::array();
                 for (EntityId e : s.scene().entities())
                     if (auto* mc = s.scene().get<MissionComponent>(e))
                         missions.push_back({{"entity", s.scene().displayName(e)}, {"state", mc->state}, {"objective", mc->objective},
                                             {"node", s.world().missionNode(e)},
                                             {"problems", gameplay::MissionGraph::fromJson(mc->graph).validate()}});
                 Json j = {{"playing", s.playing()},
                           {"trafficCars", st.trafficCars},
                           {"pedestrians", st.pedestrians},
                           {"police", st.police},
                           {"wanted", st.wanted},
                           {"speedKmh", st.speedKmh},
                           {"navMesh", st.navMesh},
                           {"gameplayMs", st.ms},
                           {"missions", missions},
                           {"player", s.world().player() ? s.scene().displayName(s.world().player()) : ""}};
                 if (auto* t = s.world().traffic())
                     j["trafficStats"] = {{"junctionCrossings", t->stats().junctionCrossings}, {"overlaps", t->stats().overlaps},
                                          {"junctionConflicts", t->stats().junctionConflicts}, {"respawnedStuck", t->stats().respawned},
                                          {"meanSpeed", t->stats().avgSpeed}, {"kmDriven", t->stats().distance / 1000}};
                 return ToolResult::json(j);
             }});
}

} // namespace fledge
