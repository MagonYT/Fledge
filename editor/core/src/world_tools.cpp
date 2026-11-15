// MCP tools for the open world (docs/WORLD_STREAMING.md, docs/MCP.md). Mutating tools run inside one
// AI-tagged undo transaction (EditorSession + the registry's callWrapper).
#include <format>

#include "fledge/editor/editor_session.h"
#include "fledge/mcp/mcp_server.h"
#include "fledge/world/partition.h"
#include "fledge/world/roads.h"
#include "fledge/world/terrain.h"

namespace fledge {
using mcp::ToolResult;

namespace {
Json schema(std::initializer_list<std::pair<const std::string, Json>> props, std::vector<std::string> required = {}) {
    Json p = Json::object();
    for (auto& [k, v] : props) p[k] = v;
    Json s = {{"type", "object"}, {"properties", p}, {"additionalProperties", false}};
    if (!required.empty()) s["required"] = required;
    return s;
}
const Json kNum = {{"type", "number"}};
const Json kVec3 = {{"type", "array"}, {"items", {{"type", "number"}}}, {"description", "[x, y, z]"}};
EntityId entityRef(EditorSession& s, const Json& ref) {
    if (ref.is_number_integer()) return s.scene().isValid(ref.get<EntityId>()) ? ref.get<EntityId>() : kNullEntity;
    if (ref.is_string()) return s.scene().findByName(ref.get<std::string>());
    return kNullEntity;
}
Vec3 vec3(const Json& j, Vec3 def = {}) {
    if (!j.is_array() || j.size() < 3) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
} // namespace

// Undoable "Convert scene to open world" (used by the editor and MCP).
Status convertSceneToOpenWorld(EditorSession& s, float cellSize, size_t* chunks) {
    s.beginEdit("Convert scene to open world", EditSource::User);
    if (!world::findPartitionSettings(s.scene())) {
        Json wp = {{"cellSize", cellSize}, {"loadRadius", cellSize * 3}, {"unloadRadius", cellSize * 4}};
        if (!s.createEntity("World Settings", kNullEntity, {{"WorldPartition", wp}, {"Streaming", {{"persistent", true}}}})) {
            s.cancelEdit();
            return Status::failure("cannot create World Settings");
        }
    }
    for (EntityId e : std::vector<EntityId>(s.scene().entities())) {
        auto* l = s.scene().get<LightComponent>(e);
        bool keep = s.scene().get<CameraComponent>(e) || (l && l->type == LightType::Directional) || s.scene().displayName(e) == "Player";
        if (keep && !s.scene().get<StreamingComponent>(e)) (void)s.applyComponents(e, {{"Streaming", {{"persistent", true}}}}, "Persistent");
    }
    s.endEdit();
    if (chunks) {
        auto* wp = world::findPartitionSettings(s.scene());
        *chunks = world::buildPartition(s.scene(), world::PartitionSettings::from(*wp)).chunks.size();
    }
    return {};
}

void registerWorldTools(mcp::ToolRegistry& reg, EditorSession& s) {
    reg.add({"convert_to_open_world", "Convert scene to open world",
             "Adds a World Settings entity with WorldPartition (chunked storage + streaming while playing) and marks cameras, "
             "the sun and the Player persistent. Returns the chunk count.",
             schema({{"cellSize", {{"type", "number"}, {"description", "chunk edge in metres (default 128)"}}}}), false, false, [&s](const Json& a) {
                 size_t n = 0;
                 if (auto st = convertSceneToOpenWorld(s, a.value("cellSize", 128.0f), &n); !st) return ToolResult::error(st.error);
                 return ToolResult::json({{"chunks", n}});
             }});
    reg.add({"get_streaming_status", "Streaming status",
             "World partition settings, chunk count and (while playing) loaded/loading chunks, memory and the floating origin.", schema({}), true,
             false, [&s](const Json&) {
                 const WorldPartitionComponent* wp = world::findPartitionSettings(s.scene());
                 if (!wp) return ToolResult::json({{"openWorld", false}});
                 Json j = {{"openWorld", true}, {"cellSize", wp->cellSize}, {"loadRadius", wp->loadRadius}, {"unloadRadius", wp->unloadRadius},
                           {"memoryBudgetMB", wp->memoryBudgetMB}};
                 if (auto* sw = s.world().streaming()) {
                     const auto& st = sw->stats();
                     j["playing"] = {{"known", st.known}, {"loaded", st.loaded}, {"loading", st.loading}, {"residentKB", st.residentBytes / 1024},
                                     {"applyMsMax", st.applyMsMax}, {"rebases", st.rebases}, {"origin", {st.origin.x, st.origin.y, st.origin.z}}};
                 } else {
                     j["chunks"] = world::buildPartition(s.scene(), world::PartitionSettings::from(*wp)).chunks.size();
                 }
                 return ToolResult::json(j);
             }});
    reg.add({"create_terrain", "Create terrain",
             "Creates a heightmap terrain (procedural hills from a seed, 4 splat layers). {\"size\":512,\"resolution\":257,\"seed\":3,\"maxHeight\":40}",
             schema({{"name", {{"type", "string"}}}, {"size", kNum}, {"resolution", {{"type", "integer"}}}, {"seed", {{"type", "integer"}}},
                     {"maxHeight", kNum}, {"position", kVec3}}),
             false, false, [&s](const Json& a) {
                 TerrainComponent t;
                 t.size = a.value("size", 256.0f);
                 t.resolution = std::clamp(a.value("resolution", 129), 9, 1025);
                 t.maxHeight = a.value("maxHeight", 30.0f);
                 t.seed = a.value("seed", 1);
                 world::Heightfield h = world::Heightfield::fromComponent(t);
                 h.toComponent(t);
                 Json comp = ComponentRegistry::get().find("Terrain")->type.toJson(&t);
                 Json comps = {{"Terrain", comp}};
                 if (a.contains("position")) comps["Transform"] = {{"position", a["position"]}};
                 EntityId e = s.createEntity(a.value("name", std::string("Terrain")), kNullEntity, comps);
                 if (!e) return ToolResult::error("create_terrain failed");
                 return ToolResult::json({{"id", e}, {"size", t.size}, {"resolution", t.resolution}});
             }});
    reg.add({"sculpt_terrain", "Sculpt / paint terrain",
             "Applies a brush to a terrain: mode raise|lower|smooth|flatten|paint, center [x,y,z] (world), radius, strength "
             "(metres for raise/lower, 0..1 otherwise), layer 0-3 for paint, height for flatten. One undo step.",
             schema({{"terrain", {{"description", "entity id or name"}}}, {"mode", {{"type", "string"}}}, {"center", kVec3}, {"radius", kNum},
                     {"strength", kNum}, {"layer", {{"type", "integer"}}}, {"height", kNum}},
                    {"terrain", "mode", "center"}),
             false, false, [&s](const Json& a) {
                 EntityId e = entityRef(s, a["terrain"]);
                 auto* tc = e ? s.scene().get<TerrainComponent>(e) : nullptr;
                 if (!tc) return ToolResult::error("not a terrain: " + a["terrain"].dump());
                 std::string m = a.value("mode", "raise");
                 world::BrushMode mode = m == "lower" ? world::BrushMode::Lower : m == "smooth" ? world::BrushMode::Smooth
                                         : m == "flatten" ? world::BrushMode::Flatten : m == "paint" ? world::BrushMode::Paint : world::BrushMode::Raise;
                 world::Heightfield h = world::Heightfield::fromComponent(*tc);
                 Vec3 local = s.scene().worldMatrix(e).inverse().transformPoint(vec3(a["center"]));
                 h.brush(local, a.value("radius", 10.0f), a.value("strength", 1.0f), mode, a.value("layer", 0), a.value("height", 0.0f));
                 TerrainComponent copy = *tc;
                 h.toComponent(copy);
                 if (auto st = s.applyComponents(e, {{"Terrain", {{"heights", copy.heights}, {"splat", copy.splat}}}}, "Sculpt terrain"); !st)
                     return ToolResult::error(st.error);
                 return ToolResult::json({{"terrain", e}, {"heightAtCenter", h.heightAt(local.x, local.z)}});
             }});
    reg.add({"create_road", "Create road",
             "Creates a road spline (road + sidewalks + markings mesh, joins the road graph). points: [[x,y,z],...] world, width, sidewalk, "
             "lanes, speedLimit (m/s).",
             schema({{"name", {{"type", "string"}}}, {"points", {{"type", "array"}}}, {"width", kNum}, {"sidewalk", kNum},
                     {"lanes", {{"type", "integer"}}}, {"speedLimit", kNum}},
                    {"points"}),
             false, false, [&s](const Json& a) {
                 Json spline = {{"points", a["points"]}, {"kind", "road"}, {"width", a.value("width", 8.0f)}, {"sidewalk", a.value("sidewalk", 2.0f)},
                                {"lanes", a.value("lanes", 2)}, {"speedLimit", a.value("speedLimit", 13.9f)}};
                 EntityId e = s.createEntity(a.value("name", std::string("Road")), kNullEntity, {{"Spline", spline}});
                 if (!e) return ToolResult::error("create_road failed (points must be [[x,y,z],...])");
                 auto g = world::RoadGraph::build(s.scene());
                 return ToolResult::json({{"id", e}, {"graphNodes", g.nodes.size()}, {"graphEdges", g.edges.size()}, {"intersections", g.intersections()}});
             }});
    reg.add({"get_road_graph", "Road graph", "Nodes (intersections / ends) and edges (road segments with length, lanes, speed) of all roads.",
             schema({}), true, false, [&s](const Json&) {
                 auto g = world::RoadGraph::build(s.scene());
                 Json nodes = Json::array(), edges = Json::array();
                 for (auto& n : g.nodes) nodes.push_back({{"position", n.position}, {"degree", n.edges.size()}});
                 for (auto& e : g.edges)
                     edges.push_back({{"a", e.a}, {"b", e.b}, {"length", e.length}, {"lanes", e.lanes}, {"speedLimit", e.speedLimit}, {"spline", e.spline}});
                 return ToolResult::json({{"nodes", nodes}, {"edges", edges}, {"intersections", g.intersections()}});
             }});
}

} // namespace fledge
