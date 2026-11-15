// AI authoring layer (docs/AI_HANDBOOK.md): higher-level MCP tools on top of the per-system tools.
//   place_road_network  roads from polylines (crossings become intersections), returns the road graph
//   populate_area       trees, street lights, benches, parked cars around roads in an area
//   build_and_playtest  (build scripts if allowed) + play + playtest bot + stop -> one structured report
//   explain_failure     rule-based diagnosis of build errors, log errors, playtest issues and scene problems
//   benchmark_scene     plays N frames headless and reports frame-time percentiles + per-system costs
//   list_capabilities   everything the engine can do, the tools for it and the docs to read
// Mutating tools run in one AI-tagged undo transaction (the registry's callWrapper).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <map>

#include "fledge/core/log.h"
#include "fledge/editor/editor_session.h"
#include "fledge/editor/perf_advisor.h"
#include "fledge/editor/playtest_bot.h"
#include "fledge/gameplay/city.h"
#include "fledge/gameplay/mission.h"
#include "fledge/gameplay/traffic.h"
#include "fledge/mcp/mcp_server.h"
#include "fledge/world/roads.h"

namespace fledge {
using mcp::ToolResult;

EntityId createTree(EditorSession& s, const Json& node, EntityId parent);

namespace {
Json sch(std::initializer_list<std::pair<const std::string, Json>> props, std::vector<std::string> required = {}) {
    Json p = Json::object();
    for (auto& [k, v] : props) p[k] = v;
    Json s = {{"type", "object"}, {"properties", p}, {"additionalProperties", false}};
    if (!required.empty()) s["required"] = required;
    return s;
}
Vec3 v3(const Json& j, Vec3 def = {}) {
    if (!j.is_array() || j.size() < 3) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
Json j3(Vec3 v) { return Json::array({v.x, v.y, v.z}); }

// The last playtest report (explain_failure reads it).
Json& lastPlaytest() {
    static Json j;
    return j;
}

// 2D segment intersection on XZ (strict interior), returns t along a.
bool segCross(Vec3 a0, Vec3 a1, Vec3 b0, Vec3 b1, float& ta, float& tb) {
    float dx = a1.x - a0.x, dz = a1.z - a0.z, ex = b1.x - b0.x, ez = b1.z - b0.z;
    float den = dx * ez - dz * ex;
    if (std::fabs(den) < 1e-6f) return false;
    float fx = b0.x - a0.x, fz = b0.z - a0.z;
    ta = (fx * ez - fz * ex) / den;
    tb = (fx * dz - fz * dx) / den;
    return ta > 0.01f && ta < 0.99f && tb > 0.01f && tb < 0.99f;
}
} // namespace

// Splits polylines where they cross so the road graph gets real intersections. Each output piece is one road.
std::vector<std::vector<Vec3>> splitRoadsAtCrossings(const std::vector<std::vector<Vec3>>& lines) {
    // collect cut parameters per (line, segment)
    std::vector<std::vector<std::vector<float>>> cuts(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) cuts[i].resize(lines[i].size() > 0 ? lines[i].size() - 1 : 0);
    for (size_t i = 0; i < lines.size(); ++i)
        for (size_t j = i + 1; j < lines.size(); ++j)
            for (size_t a = 0; a + 1 < lines[i].size(); ++a)
                for (size_t b = 0; b + 1 < lines[j].size(); ++b) {
                    float ta, tb;
                    if (segCross(lines[i][a], lines[i][a + 1], lines[j][b], lines[j][b + 1], ta, tb)) {
                        cuts[i][a].push_back(ta);
                        cuts[j][b].push_back(tb);
                    }
                }
    std::vector<std::vector<Vec3>> out;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::vector<Vec3> cur;
        if (lines[i].size() < 2) continue;
        cur.push_back(lines[i][0]);
        for (size_t a = 0; a + 1 < lines[i].size(); ++a) {
            auto ts = cuts[i][a];
            std::sort(ts.begin(), ts.end());
            for (float t : ts) {
                Vec3 p = lines[i][a] + (lines[i][a + 1] - lines[i][a]) * t;
                cur.push_back(p);
                out.push_back(cur);
                cur = {p};
            }
            cur.push_back(lines[i][a + 1]);
        }
        if (cur.size() >= 2) out.push_back(cur);
    }
    return out;
}

void registerAuthoringTools(mcp::ToolRegistry& reg, EditorSession& s) {
    reg.add({"place_road_network", "Place road network",
             "Creates roads from polylines (world XZ points, y = height): lines = [[[x,y,z], ...], ...]. Lines are split where they "
             "cross, so every crossing is a real intersection for traffic. Optional width (m, default 12), sidewalk (m, 3), "
             "speedLimit (m/s, 13.9), name. Returns the road graph (nodes, intersections, connected parts). One [AI] undo step.",
             sch({{"lines", {{"type", "array"}}}, {"width", {{"type", "number"}}}, {"sidewalk", {{"type", "number"}}}, {"speedLimit", {{"type", "number"}}},
                  {"name", {{"type", "string"}}}},
                 {"lines"}),
             false, false, [&s](const Json& a) {
                 std::vector<std::vector<Vec3>> lines;
                 for (auto& l : a["lines"]) {
                     std::vector<Vec3> pts;
                     if (l.is_array())
                         for (auto& p : l) pts.push_back(v3(p));
                     if (pts.size() >= 2) lines.push_back(pts);
                 }
                 if (lines.empty()) return ToolResult::error("lines: give at least one polyline with 2+ points");
                 auto pieces = splitRoadsAtCrossings(lines);
                 float width = a.value("width", 12.0f), sidewalk = a.value("sidewalk", 3.0f);
                 EntityId root = s.createEntity(a.value("name", std::string("Road Network")), kNullEntity, {{"Tags", {{"tags", "roads"}}}});
                 int n = 0;
                 for (auto& piece : pieces) {
                     Json pts = Json::array();
                     for (auto& p : piece) pts.push_back(j3(p));
                     Json spline = {{"points", pts}, {"kind", "road"}, {"width", width}, {"sidewalk", sidewalk}, {"speedLimit", a.value("speedLimit", 13.9f)},
                                    {"sidewalkTrim", width * 0.5f + sidewalk}};
                     if (!s.createEntity(std::format("Road {}", ++n), root, {{"Spline", spline}})) return ToolResult::error("could not create a road");
                 }
                 auto g = world::RoadGraph::build(s.scene());
                 // connected parts
                 std::vector<int> comp(g.nodes.size(), -1);
                 int parts = 0;
                 for (size_t i = 0; i < g.nodes.size(); ++i) {
                     if (comp[i] >= 0) continue;
                     std::vector<size_t> st{i};
                     comp[i] = parts;
                     while (!st.empty()) {
                         size_t c = st.back();
                         st.pop_back();
                         for (int e : g.nodes[c].edges)
                             for (int o : {g.edges[static_cast<size_t>(e)].a, g.edges[static_cast<size_t>(e)].b})
                                 if (comp[static_cast<size_t>(o)] < 0) {
                                     comp[static_cast<size_t>(o)] = parts;
                                     st.push_back(static_cast<size_t>(o));
                                 }
                     }
                     ++parts;
                 }
                 return ToolResult::json({{"id", root}, {"roads", n}, {"graphNodes", g.nodes.size()}, {"graphEdges", g.edges.size()},
                                          {"intersections", g.intersections()}, {"connectedParts", parts}});
             }});
    reg.add({"populate_area", "Populate area",
             "Dresses an area: kinds = any of trees, street_lights, benches, parked_cars. center [x,y,z], size (m, square), density "
             "(0..1), seed. Street lights, benches and parked cars follow the roads in the area; trees avoid roads. One [AI] undo step.",
             sch({{"center", {{"type", "array"}}}, {"size", {{"type", "number"}}}, {"kinds", {{"type", "array"}}}, {"density", {{"type", "number"}}},
                  {"seed", {{"type", "integer"}}}},
                 {"center", "size"}),
             false, false, [&s](const Json& a) {
                 Vec3 c = v3(a["center"]);
                 float size = std::clamp(a.value("size", 100.0f), 5.0f, 2000.0f), density = std::clamp(a.value("density", 0.5f), 0.0f, 1.0f);
                 uint32_t seed = a.value("seed", 1u) * 2654435761u | 1u;
                 auto rnd = [&] {
                     seed ^= seed << 13;
                     seed ^= seed >> 17;
                     seed ^= seed << 5;
                     return static_cast<float>(seed >> 8) / 16777216.0f;
                 };
                 std::vector<std::string> kinds;
                 if (a.contains("kinds") && a["kinds"].is_array())
                     for (auto& k : a["kinds"])
                         if (k.is_string()) kinds.push_back(k.get<std::string>());
                 if (kinds.empty()) kinds = {"trees", "street_lights", "parked_cars"};
                 auto has = [&](const char* k) { return std::find(kinds.begin(), kinds.end(), k) != kinds.end(); };
                 auto inside = [&](Vec3 p) { return std::fabs(p.x - c.x) <= size * 0.5f && std::fabs(p.z - c.z) <= size * 0.5f; };
                 auto g = world::RoadGraph::build(s.scene());
                 EntityId root = s.createEntity("Props", kNullEntity, {{"Transform", {{"position", j3({0, 0, 0})}}}, {"Tags", {{"tags", "props"}}}});
                 int lights = 0, benches = 0, cars = 0, trees = 0;
                 for (auto& e : g.edges) {
                     float along = 8;
                     for (size_t i = 1; i < e.polyline.size(); ++i) {
                         Vec3 a0 = e.polyline[i - 1], a1 = e.polyline[i];
                         float seg = length(a1 - a0);
                         Vec3 dir = seg > 1e-3f ? (a1 - a0) / seg : Vec3{1, 0, 0};
                         Vec3 right = normalize(cross(dir, {0, 1, 0}));
                         for (; along < seg; along += 24.0f / std::max(0.2f, density)) {
                             Vec3 p = a0 + dir * along;
                             if (!inside(p)) continue;
                             float side = (lights + benches) % 2 ? 1.0f : -1.0f;
                             Vec3 kerb = p + right * side * (e.width * 0.5f + 0.6f);
                             if (has("street_lights")) {
                                 Json pole = {{"name", std::format("Street Light {}", ++lights)},
                                              {"components", {{"Transform", {{"position", j3(kerb + Vec3{0, 3, 0})}, {"scale", j3({0.15f, 6, 0.15f})}}},
                                                              {"MeshRenderer", {{"mesh", "builtin:cube"}, {"color", j3({0.2f, 0.2f, 0.22f})}}},
                                                              {"Collider", {{"shape", "Box"}, {"size", j3({1, 1, 1})}}}}},
                                              {"children", Json::array({{{"name", "Lamp"},
                                                                         {"components", {{"Transform", {{"position", j3({0, 0.52f, 0})}, {"scale", j3({3, 0.03f, 3})}}},
                                                                                         {"Light", {{"type", "Point"}, {"color", j3({1, 0.85f, 0.6f})}, {"intensity", 6}, {"range", 14}}}}}}})}};
                                 createTree(s, pole, root);
                             }
                             if (has("benches") && rnd() < 0.5f) {
                                 Vec3 bp = p + right * -side * (e.width * 0.5f + 1.2f);
                                 createTree(s, {{"name", std::format("Bench {}", ++benches)},
                                                {"components", {{"Transform", {{"position", j3(bp + Vec3{0, 0.45f, 0})}, {"scale", j3({1.8f, 0.5f, 0.5f})}}},
                                                                {"MeshRenderer", {{"mesh", "builtin:cube"}, {"color", j3({0.45f, 0.3f, 0.18f})}}},
                                                                {"Collider", {{"shape", "Box"}, {"size", j3({1, 1, 1})}}}}}},
                                            root);
                             }
                             if (has("parked_cars") && rnd() < 0.35f * density + 0.1f) {
                                 Vec3 cp = p + right * (e.width * 0.5f - 0.9f);  // right-hand kerb, facing the travel direction
                                 float yaw = std::atan2(-dir.x, -dir.z);
                                 static const Vec3 cols[] = {{0.7f, 0.1f, 0.1f}, {0.1f, 0.3f, 0.7f}, {0.85f, 0.85f, 0.82f}, {0.1f, 0.1f, 0.1f}, {0.2f, 0.5f, 0.3f}};
                                 ++cars;
                                 createTree(s, gameplay::vehicleNode("car", cp, yaw, std::format("Parked Car {}", cars), cols[cars % 5]), root);
                             }
                         }
                         along -= seg;
                     }
                 }
                 if (has("trees")) {
                     int want = static_cast<int>(size * size / 400.0f * density);
                     for (int i = 0, tries = 0; i < want && tries < want * 20; ++tries) {
                         Vec3 p{c.x + (rnd() - 0.5f) * size, c.y, c.z + (rnd() - 0.5f) * size};
                         float d = g.edges.empty() ? 1e9f : length(g.closestPoint(p) - Vec3{p.x, g.closestPoint(p).y, p.z});
                         if (d < 12) continue;  // keep roads and sidewalks clear
                         // not inside buildings
                         bool blocked = false;
                         for (EntityId e : s.scene().entities())
                             if (auto* col = s.scene().get<ColliderComponent>(e); col && !s.scene().get<RigidBodyComponent>(e)) {
                                 Transform t = s.scene().worldTransform(e);
                                 Vec3 half = col->size * t.scale * 0.5f;
                                 if (half.y > 2 && std::fabs(p.x - t.position.x) < half.x + 1.5f && std::fabs(p.z - t.position.z) < half.z + 1.5f) blocked = true;
                             }
                         if (blocked) continue;
                         float h = 3 + rnd() * 3;
                         createTree(s, {{"name", std::format("Tree {}", ++trees)},
                                        {"components", {{"Transform", {{"position", j3(p + Vec3{0, h * 0.3f, 0})}, {"scale", j3({0.35f, h * 0.6f, 0.35f})}}},
                                                        {"MeshRenderer", {{"mesh", "builtin:cube"}, {"color", j3({0.35f, 0.25f, 0.15f})}}},
                                                        {"Collider", {{"shape", "Box"}, {"size", j3({1, 1, 1})}}}}},
                                        {"children", Json::array({{{"name", "Crown"},
                                                                   {"components", {{"Transform", {{"position", j3({0, 0.7f, 0})}, {"scale", j3({8, 0.6f, 8})}}},
                                                                                   {"MeshRenderer", {{"mesh", "builtin:sphere"}, {"color", j3({0.2f, 0.45f, 0.18f})}}}}}}})}},
                                    root);
                         ++i;
                     }
                 }
                 return ToolResult::json({{"id", root}, {"streetLights", lights}, {"benches", benches}, {"parkedCars", cars}, {"trees", trees}});
             }});
    reg.add({"build_and_playtest", "Build and playtest",
             "One step to check a game: builds the scripts (only if the user allows AI script builds; otherwise the current module is "
             "used), plays, runs the playtest bot for `seconds` (simulated, headless, through the Time Machine), stops, and reports: bot "
             "issues (script errors, softlocks, falls, perf spikes), distance travelled, missions (state, objective), traffic safety "
             "stats, physics / audio counts, log errors during the run, and a verdict. Then call explain_failure if anything is wrong.",
             sch({{"seconds", {{"type", "number"}}}, {"seed", {{"type", "integer"}}}, {"goal", {{"type", "string"}}}, {"buildScripts", {{"type", "boolean"}}}}),
             false, false, [&s](const Json& a) {
                 if (!s.hasProject()) return ToolResult::error(kNoProjectHelp);
                 Json rep = Json::object();
                 if (s.playing()) (void)s.stop();
                 bool wantBuild = a.value("buildScripts", true) && s.project().hasScripts();
                 if (wantBuild) {
                     if (!s.ai.categoryAllowed(AiCategory::ScriptBuild)) {
                         rep["build"] = {{"skipped", "AI script builds are not allowed by the user (Settings > AI control > Script builds)"}};
                     } else {
                         auto br = s.buildScripts();
                         rep["build"] = br.toJson(20);
                         if (!br.ok) {
                             rep["verdict"] = "build failed";
                             lastPlaytest() = rep;
                             return ToolResult::json(rep);
                         }
                     }
                 }
                 size_t logMark = Logger::get().recent(100000).size();
                 if (auto st = s.play(); !st) return ToolResult::error(st.error);
                 BotOptions bo;
                 bo.seconds = std::clamp(a.value("seconds", 20.0), 1.0, 600.0);
                 bo.seed = a.value("seed", 1u);
                 bo.goal = a.value("goal", std::string());
                 BotReport br = runPlaytestBot(s, bo);
                 GameWorld& w = s.world();
                 auto gs = w.gameplayStats();
                 Json missions = Json::array();
                 for (EntityId e : s.scene().entities())
                     if (auto* mc = s.scene().get<MissionComponent>(e))
                         missions.push_back({{"entity", s.scene().displayName(e)}, {"state", mc->state}, {"objective", mc->objective}});
                 Json issues = Json::array();
                 for (auto& i : br.issues) issues.push_back({{"kind", i.kind}, {"step", i.step}, {"detail", i.detail}});
                 rep["bot"] = {{"ran", br.ran}, {"error", br.error}, {"watched", br.subject}, {"distance", br.distance}, {"cellsVisited", br.cellsVisited},
                               {"wallSeconds", br.wallSeconds}, {"issues", issues}, {"notes", br.notes}};
                 rep["missions"] = missions;
                 rep["gameplay"] = {{"trafficCars", gs.trafficCars}, {"pedestrians", gs.pedestrians}, {"police", gs.police}, {"wanted", gs.wanted}, {"gameplayMs", gs.ms}};
                 if (auto* t = w.traffic())
                     rep["traffic"] = {{"junctionConflicts", t->stats().junctionConflicts}, {"overlaps", t->stats().overlaps}, {"stuck", t->stats().respawned},
                                       {"kmDriven", t->stats().distance / 1000}};
                 rep["physicsBodies"] = w.physics() ? w.physics()->bodyCount() : 0;
                 rep["audioVoices"] = w.audioStats().voices;
                 auto log = Logger::get().recent(100000);
                 Json errors = Json::array();
                 for (size_t i = std::min(logMark, log.size()); i < log.size() && errors.size() < 30; ++i)
                     if (log[i].level >= LogLevel::Warn) errors.push_back(std::format("[{}][{}] {}", toString(log[i].level), log[i].category, log[i].message));
                 rep["logWarnings"] = errors;
                 (void)s.stop();
                 bool scriptErr = std::any_of(br.issues.begin(), br.issues.end(), [](const BotIssue& i) { return i.kind == "script error"; });
                 bool bad = !br.ran || scriptErr || std::any_of(br.issues.begin(), br.issues.end(), [](const BotIssue& i) { return i.kind == "softlock" || i.kind == "fall"; });
                 rep["verdict"] = bad ? "issues" : br.issues.empty() ? "ok" : "ok (minor issues)";
                 lastPlaytest() = rep;
                 return ToolResult::json(rep);
             }});
    reg.add({"explain_failure", "Explain failure",
             "Diagnoses what is wrong and how to fix it, from: the last script build (errors with file:line and hints), engine log "
             "errors / warnings, the last build_and_playtest report, and scene checks (camera, light, player, ground under the player, "
             "mission graph problems, traffic without roads, vehicles without bodies). Rule-based (no LLM). Each finding has severity, "
             "problem, likely cause, fix, and the tool to use.",
             sch({{"question", {{"type", "string"}}}}), true, false, [&s](const Json& a) {
                 Json out = Json::array();
                 auto add = [&](const char* sev, const std::string& area, const std::string& problem, const std::string& cause, const std::string& fix,
                                const std::string& tool) {
                     out.push_back({{"severity", sev}, {"area", area}, {"problem", problem}, {"likelyCause", cause}, {"fix", fix}, {"tool", tool}});
                 };
                 // script build
                 if (s.hasScriptBuild() && !s.lastScriptBuild().ok)
                     for (auto& d : s.lastScriptBuild().diagnostics) {
                         if (d.severity != Diagnostic::Severity::Error || out.size() > 12) continue;
                         std::string cause = "a C++ compile error in a Fledge Script";
                         std::string m = toLower(d.message);
                         if (m.find("undefined reference") != std::string::npos) cause = "a declared function or behaviour has no definition (link error)";
                         else if (m.find("has no member") != std::string::npos || m.find("is not a member") != std::string::npos)
                             cause = "the script uses an API that does not exist; read fledge://script/api";
                         else if (m.find("was not declared") != std::string::npos) cause = "a missing #include or a typo in a name";
                         else if (m.find("expected") != std::string::npos) cause = "a syntax error (missing ; or brace) at or just before this line";
                         add("error", "scripts", std::format("{}:{}: {}", d.file, d.line, d.message), cause, "edit the script at that line, then build again",
                             "read_script / edit_script / build_scripts");
                     }
                 // log
                 auto log = Logger::get().recent(400);
                 std::map<std::string, int> seen;
                 for (auto& e : log) {
                     if (e.level < LogLevel::Warn) continue;
                     std::string key = e.category + ": " + e.message.substr(0, 120);
                     if (seen[key]++) continue;
                     std::string cause = "see the message", fix = "check the referenced asset / component", tool = "read_log";
                     if (e.message.find("not a registered asset") != std::string::npos || e.message.find("file not found") != std::string::npos) {
                         cause = "a component references an asset that is not in Content/ (renamed, deleted or a typo)";
                         fix = "point the component at an existing asset (list_assets) or import it";
                         tool = "list_assets / set_component_property";
                     } else if (e.category == "audio") {
                         cause = "an AudioSource clip cannot be decoded or found";
                         fix = "use a WAV / MP3 / FLAC / Ogg asset or a builtin: clip";
                         tool = "add_audio_source";
                     } else if (e.category == "gameplay" && e.message.find("Navmesh not built") != std::string::npos) {
                         cause = "no static geometry to walk on (police / NavAgents need ground colliders, terrain or roads)";
                         fix = "add a ground with a Collider (add_physics) or roads";
                         tool = "add_physics / place_road_network";
                     } else if (e.category == "script" || e.category == "script") {
                         cause = "a Fledge Script reported an error or threw";
                         fix = "read the script named in the message";
                         tool = "read_script";
                     }
                     if (out.size() < 30) add(e.level == LogLevel::Error ? "error" : "warning", "log/" + e.category, e.message, cause, fix, tool);
                 }
                 // last playtest
                 const Json& pt = lastPlaytest();
                 if (pt.is_object() && pt.contains("bot"))
                     for (auto& i : pt["bot"]["issues"]) {
                         std::string k = i.value("kind", std::string());
                         if (k == "fall")
                             add("error", "playtest", i.value("detail", std::string()), "the player (or camera target) fell: there is no collider under it",
                                 "add a ground with a Collider under the start position (Add Physics) or move the player", "add_physics / set_component_property");
                         else if (k == "softlock")
                             add("warning", "playtest", i.value("detail", std::string()), "the bot could not move for a long time (stuck in geometry or input does nothing)",
                                 "check the player's CharacterController and that the start is not inside a wall", "get_entity / raycast");
                         else if (k == "script error")
                             add("error", "playtest", i.value("detail", std::string()), "a script threw or logged an error while playing", "read the log and the named script",
                                 "get_runtime_log / read_script");
                         else if (k == "perf spike")
                             add("info", "playtest", i.value("detail", std::string()), "one frame took much longer than usual (snapshot, streaming, navmesh build)",
                                 "usually harmless; if frequent, run benchmark_scene", "benchmark_scene");
                     }
                 if (pt.is_object() && pt.contains("traffic") && pt["traffic"].value("stuck", 0) > 0)
                     add("warning", "traffic", std::format("{} traffic cars got stuck", pt["traffic"].value("stuck", 0)),
                         "something blocks a lane permanently (a parked vehicle or prop in the road)", "move props / parked cars to the kerb", "get_road_graph");
                 // scene checks
                 Scene& sc = s.scene();
                 bool camera = false, light = false;
                 for (EntityId e : sc.entities()) {
                     if (auto* c = sc.get<CameraComponent>(e); c && c->primary) camera = true;
                     if (sc.get<LightComponent>(e)) light = true;
                 }
                 if (!camera) add("error", "scene", "no primary camera", "the game has nothing to render from", "add a Camera with primary = true", "create_entity");
                 if (!light) add("warning", "scene", "no light", "the scene is lit by the sky only and looks flat", "add a directional light", "create_entity");
                 EntityId player = s.world().player();
                 if (player) {
                     physics::PhysicsWorld tmp;
                     tmp.providers.mesh = [&s](const std::string& m) { return s.world().cpuMesh(m); };
                     tmp.sync(sc);
                     Vec3 pp = sc.worldMatrix(player).c[3].xyz();
                     if (!tmp.raycast(pp + Vec3{0, 0.5f, 0}, {0, -1, 0}, 50))
                         add("error", "scene", "nothing to stand on under the player", "the player will fall forever when Play starts",
                             "add a ground with a Collider (or roads / terrain) under the player", "add_physics");
                 }
                 bool roads = false;
                 for (EntityId e : sc.entities())
                     if (auto* sp = sc.get<SplineComponent>(e); sp && sp->kind == "road") roads = true;
                 for (EntityId e : sc.entities()) {
                     if (auto* tc = sc.get<TrafficComponent>(e); tc && tc->enabled && !roads)
                         add("warning", "traffic", "Traffic is enabled but the scene has no roads", "traffic only drives on road splines",
                             "add roads (generate_city_district / place_road_network)", "place_road_network");
                     if (auto* mc = sc.get<MissionComponent>(e))
                         for (auto& p : gameplay::MissionGraph::fromJson(mc->graph).validate())
                             add("error", "mission", sc.displayName(e) + ": " + p, "the mission graph is incomplete", "fix the node / link", "write_mission");
                     if (sc.get<VehicleComponent>(e) && !sc.get<RigidBodyComponent>(e))
                         add("error", "physics", sc.displayName(e) + " has a Vehicle but no RigidBody", "vehicles need a dynamic body + box Collider",
                             "use create_vehicle_from_preset", "create_vehicle_from_preset");
                 }
                 if (out.empty()) add("info", "all", "no problems found", "the last build, log, playtest and scene checks are clean", "-", "build_and_playtest");
                 return ToolResult::json({{"findings", out}, {"question", a.value("question", std::string())}});
             }});
    reg.add({"benchmark_scene", "Benchmark scene",
             "Plays the scene headless for `frames` fixed steps (after `warmup`), measures the game loop per frame (scripts, gameplay, "
             "physics, animation, streaming, audio) and reports p50 / p95 / max ms, the gameplay share, entity / body / voice / "
             "traffic counts, and the static performance advisor's suggestions. Rendering is not included (headless). Stops afterwards.",
             sch({{"frames", {{"type", "integer"}}}, {"warmup", {{"type", "integer"}}}}), false, false, [&s](const Json& a) {
                 if (!s.hasProject()) return ToolResult::error(kNoProjectHelp);
                 int frames = std::clamp(a.value("frames", 300), 10, 20000), warmup = std::clamp(a.value("warmup", 30), 0, 2000);
                 if (s.playing()) (void)s.stop();
                 if (auto st = s.play(); !st) return ToolResult::error(st.error);
                 s.simulateFrames(warmup);
                 std::vector<double> ms;
                 double gameplay = 0;
                 for (int i = 0; i < frames; ++i) {
                     auto t0 = std::chrono::steady_clock::now();
                     s.simulateFrames(1);
                     ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                     gameplay += s.world().gameplayStats().ms;
                 }
                 std::vector<double> sorted = ms;
                 std::sort(sorted.begin(), sorted.end());
                 auto pct = [&](double p) { return sorted[std::min(sorted.size() - 1, static_cast<size_t>(p * static_cast<double>(sorted.size())))]; };
                 double mean = 0;
                 for (double v : ms) mean += v;
                 mean /= static_cast<double>(ms.size());
                 GameWorld& w = s.world();
                 Json rep = {{"frames", frames},
                             {"meanMs", mean},
                             {"p50Ms", pct(0.5)},
                             {"p95Ms", pct(0.95)},
                             {"maxMs", sorted.back()},
                             {"gameplayMeanMs", gameplay / frames},
                             {"animationMs", w.animation().stats().ms},
                             {"entities", s.scene().entities().size()},
                             {"physicsBodies", w.physics() ? w.physics()->bodyCount() : 0},
                             {"audioVoices", w.audioStats().voices},
                             {"trafficCars", w.gameplayStats().trafficCars},
                             {"pedestrians", w.gameplayStats().pedestrians},
                             {"budget60fps", mean < 16.6 ? "fits 16.6 ms" : "over 16.6 ms"},
                             {"note", "headless simulation only; GPU / render time is measured in the editor's stats overlay or a packaged smoke run"}};
                 (void)s.stop();
                 PerfReport pr = analyzePerformance(s, s.world().viewerOverride.value_or(Vec3{0, 2, 8}));
                 Json sugg = Json::array();
                 for (auto& sg : pr.suggestions) sugg.push_back({{"kind", sg.kind}, {"text", sg.text}, {"oneClickFix", sg.fixable}});
                 rep["advisor"] = {{"meshes", pr.meshes}, {"triangles", pr.triangles}, {"lights", pr.lights}, {"suggestions", sugg}};
                 return ToolResult::json(rep);
             }});
    reg.add({"list_capabilities", "List capabilities",
             "What Fledge can do and how: each engine system with its components, its MCP tools and the docs resource to read, plus "
             "the AI master switch / control level / allowed categories, and the project templates. Start here, then read fledge://handbook.",
             sch({}), true, false, [&s, &reg](const Json&) {
                 Json systems = Json::array();
                 auto sys = [&](const char* name, std::vector<std::string> comps, std::vector<std::string> tools, const char* docs, const char* summary) {
                     systems.push_back({{"system", name}, {"components", comps}, {"tools", tools}, {"docs", docs}, {"summary", summary}});
                 };
                 sys("Scene & assets", {"Name", "Transform", "MeshRenderer", "Light", "Camera", "Tags"},
                     {"create_entity", "set_components", "get_hierarchy", "import_asset", "instantiate_asset", "create_material"}, "fledge://scene",
                     "entities, components, hierarchy, assets, materials, undo/redo");
                 sys("Open world", {"WorldPartition", "Streaming", "Terrain", "Spline", "Foliage"},
                     {"convert_to_open_world", "create_terrain", "sculpt_terrain", "create_road", "get_road_graph", "get_streaming_status"},
                     "fledge://docs/world_streaming", "chunk streaming, floating origin, terrain, roads, foliage, LOD + culling");
                 sys("Animation", {"Animator"}, {"create_character", "set_animator", "set_anim_graph", "get_animation_info"}, "fledge://docs/animation",
                     "skeletal animation, state machines, IK, root motion, retargeting, crowds");
                 sys("Physics", {"RigidBody", "Collider", "CharacterController", "Vehicle", "Joint"},
                     {"add_physics", "create_vehicle_from_preset", "raycast", "get_physics_status"}, "fledge://docs/physics",
                     "Jolt: bodies, colliders, characters, vehicles, joints, ragdolls, deterministic replays");
                 sys("Audio", {"AudioSource", "AudioListener", "AudioMixer"}, {"add_audio_source", "set_mixer", "get_audio_status"}, "fledge://docs/audio",
                     "3D sounds, buses + ducking, streaming, engine sounds");
                 sys("Gameplay", {"PlayerController", "Health", "Inventory", "Pickup", "Wanted", "Traffic", "Mission", "TimeOfDay", "Weather", "HudElement", "CameraRig", "Sequence", "NavAgent"},
                     {"generate_city_district", "place_road_network", "populate_area", "make_player", "set_traffic", "write_mission", "set_time_of_day", "set_weather",
                      "get_gameplay_status"},
                     "fledge://docs/gameplay", "player controller, driving, traffic, police, missions, HUD, day/night, weather, save/load");
                 sys("Networking", {"NetIdentity", "NetPlayer"}, {"create_entity", "set_components"}, "fledge://docs/networking",
                     "client/server: replicated properties, delta snapshots, prediction (fledge_runtime --server / --connect)");
                 sys("Scripting (Fledge Script C++)", {"Behaviours"}, {"create_script", "write_script", "edit_script", "build_scripts", "attach_script", "get_build_log"},
                     "fledge://script/api", "hot-reloaded C++ behaviours with properties, coroutines, events");
                 sys("Blueprint conversion (Unreal -> Fledge Script)", {}, {"convert_blueprint", "build_scripts", "attach_script"}, "fledge://docs/blueprint_converter",
                     "Unreal Blueprint T3D text to a C++ behaviour + conversion report (dry run is read-only; .uasset probe only)");
                 sys("Settings migration (Fledge -> Fledge Advanced)", {}, {"migrate_settings"}, "fledge://docs/migrating_settings",
                     "copy the user's Fledge editor settings into Fledge Advanced (dry run is read-only; apply needs the destructive permission)");
                 sys("Testing & shipping", {}, {"play", "stop", "build_and_playtest", "explain_failure", "benchmark_scene", "take_screenshot", "build_package"},
                     "fledge://handbook", "playtest bot, diagnosis, benchmarks, packaging");
                 // Fledge Advanced (1.3.0): one entry per Advanced feature this executable provides, with its components and tools
                 // (ComponentTypeInfo::feature / Tool::feature). Fledge provides none, so nothing is added there.
                 for (const FeatureInfo* f : FeatureRegistry::get().providedFeatures()) {
                     if (f->edition != Edition::Advanced) continue;
                     std::vector<std::string> comps, tools;
                     for (const auto& t : ComponentRegistry::get().types())
                         if (t.feature == f->id) comps.push_back(t.name);
                     for (const auto& t : reg.all())
                         if (t.feature == f->id) tools.push_back(t.name);
                     sys(f->title.c_str(), comps, tools, "fledge://docs/advanced_gameplay", f->summary.c_str());
                 }
                 Json cats = Json::object();
                 for (auto c : {AiCategory::Safe, AiCategory::Destructive, AiCategory::ScriptBuild, AiCategory::Download, AiCategory::Packaging,
                                AiCategory::EditorControl})
                     cats[toString(c)] = s.ai.categoryAllowed(c);
                 Json tools = Json::array();
                 for (auto& t : reg.all()) tools.push_back(t.name);
                 Json features = Json::array(), unavailable = Json::array();
                 for (const FeatureInfo* f : FeatureRegistry::get().providedFeatures()) features.push_back(f->id);
                 for (const FeatureInfo* f : FeatureRegistry::get().unavailableFeatures())
                     unavailable.push_back({{"id", f->id}, {"title", f->title}, {"edition", editionId(f->edition)},
                                            {"plannedFor", f->plannedFor}});
                 return ToolResult::json({{"engine", "Fledge"},
                                          {"edition", editionId(currentEdition())},
                                          {"editionName", editionName(currentEdition())},
                                          {"version", FLEDGE_VERSION_STRING},
                                          {"features", features},
                                          {"unavailableFeatures", unavailable},
                                          {"mcpTier", currentEdition() == Edition::Advanced ? "core+advanced" : "core"},
                                          {"systems", systems},
                                          {"ai", {{"enabled", s.ai.enabled()}, {"level", toString(s.ai.level())}, {"categoriesAllowed", cats},
                                                  {"provider", s.ai.enabled() ? s.ai.providerInfo() : Json(nullptr)}}},
                                          {"toolCount", tools.size()},
                                          {"tools", tools},
                                          {"templates", {"Empty", "ThirdPerson", "CitySandbox"}},
                                          {"handbook", "fledge://handbook"}});
             }});
}

} // namespace fledge
