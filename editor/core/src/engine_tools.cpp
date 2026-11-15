// The engine tool catalog exposed over MCP and to the in-editor assistant.
// Keep docs/MCP.md in sync when adding tools.
//
// Every scene mutation goes through EditorSession ops, and the registry's
// callWrapper runs each mutating tool call inside ONE history transaction
// tagged EditSource::AI ("AI: <tool>"), so AI edits are undoable as a unit and
// a failing tool call is rolled back completely.
#include <chrono>
#include <format>
#include <map>
#include <set>

#include "fledge/core/log.h"
#include "fledge/editor/editor_control.h"
#include "fledge/editor/editor_session.h"
#include "fledge/editor/engine_docs.h"
#include "fledge/mcp/mcp_server.h"
#include "fledge/render/renderer.h"

namespace fledge {

using mcp::Tool;
using mcp::ToolResult;

namespace {

Json obj(std::initializer_list<std::pair<const std::string, Json>> props, std::vector<std::string> required = {}) {
    Json p = Json::object();
    for (auto& [k, v] : props) p[k] = v;
    Json s = {{"type", "object"}, {"properties", p}, {"additionalProperties", false}};
    if (!required.empty()) s["required"] = required;
    return s;
}
const Json kEntityRef = {{"description", "entity id (integer) or exact entity name (string)"}};

EntityId resolveEntity(EditorSession& s, const Json& ref) {
    if (ref.is_number_integer()) {
        auto id = ref.get<EntityId>();
        return s.scene().isValid(id) ? id : kNullEntity;
    }
    if (ref.is_string()) return s.scene().findByName(ref.get<std::string>());
    return kNullEntity;
}

ToolResult noProject() { return ToolResult::error(kNoProjectHelp); }

// 1.3.2: an Animator.character set by a tool is loaded right away (a failed earlier load of the same path is retried), so a
// brand-new .fchar works at once and a wrong path is an error instead of an Animator that silently never draws.
std::string checkCharacterRef(EditorSession& s, const Json& value) {
    if (!value.is_string()) return {};
    std::string ref = value.get<std::string>();
    if (ref.empty()) return {};
    if (s.world().animation().reloadCharacter(ref)) return {};
    return "Animator.character '" + ref + "' could not be loaded: " + s.world().animation().characterError(ref) +
           " (a content path like Characters/Hero.fchar, or builtin:mannequin)";
}
std::string checkCharacterRefs(EditorSession& s, const Json& components) {
    if (!components.is_object() || !components.contains("Animator") || !components["Animator"].is_object()) return {};
    const Json& an = components["Animator"];
    return an.contains("character") ? checkCharacterRef(s, an["character"]) : std::string();
}
ToolResult notFound(const Json& ref) { return ToolResult::error("entity not found: " + ref.dump()); }

Json entitySummary(EditorSession& s, EntityId e) {
    Json j = {{"id", e}, {"name", s.scene().displayName(e)}, {"components", s.scene().componentNames(e)}};
    if (EntityId p = s.scene().parentOf(e)) j["parent"] = p;
    j["worldPosition"] = s.scene().worldTransform(e).position;
    return j;
}

Json hierarchyNode(EditorSession& s, EntityId e) {
    Json children = Json::array();
    for (EntityId c : s.scene().childrenOf(e)) children.push_back(hierarchyNode(s, c));
    return {{"id", e}, {"name", s.scene().displayName(e)}, {"children", children}};
}

// Tools that must not be wrapped in an AI scene transaction.
const std::set<std::string> kNoTransaction = {"undo", "redo", "play", "stop", "load_scene", "save_scene", "build_package", "cook_assets",
                                              "build_scripts", "build_and_playtest", "benchmark_scene", "import_asset_url",
                                              "migrate_settings", "list_editor_commands", "run_editor_command", "list_editor_panels",
                                              "set_editor_panel", "set_editor_window", "get_editor_state", "get_editor_ui_tree",
                                              "select_entities", "get_editor_preferences", "set_editor_preference", "open_project"};

// 1.3.2: tools that edit the open scene. While the editor plays they edit the play world (thrown away on Stop): they take an
// optional `target` ("play" | "edit") and warn without one (docs/MCP.md "Edits while playing").
const std::set<std::string> kSceneEditTools = {"create_entity", "set_component_property", "set_components", "set_parent", "duplicate_entity",
                                               "delete_entity", "remove_component", "instantiate_asset", "assign_material", "create_character",
                                               "set_animator", "set_anim_graph", "add_physics", "create_vehicle_from_preset", "attach_script",
                                               "detach_script", "set_script_property", "add_audio_source", "make_player"};

// play {input}: [{"frames": N, "keys": ["W"], "actions": ["Jump"], "axes": {"MoveY": 1, "MoveX": -0.5}, "mouseDx": 0, "mouseDy": 0}, ...]
// Segments run in order; frames after the last segment get no input. A bare object = one segment for every frame.
Json playInputSchema() {
    Json seg = {{"type", "object"},
                {"properties",
                 {{"frames", {{"type", "integer"}, {"minimum", 1}, {"description", "frames this segment lasts (default: the rest)"}}},
                  {"keys", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "key names held, e.g. W, Space, Left, PadA"}}},
                  {"actions", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "input map actions held"}}},
                  {"axes", {{"type", "object"}, {"description", "input map axes, e.g. {\"MoveY\": 1, \"MoveX\": -0.5} (fractions use the gamepad)"}}},
                  {"mouseDx", {{"type", "number"}}},
                  {"mouseDy", {{"type", "number"}}}}}};
    return {{"description", "scripted input for the simulated frames (segments in order)"}, {"oneOf", Json::array({seg, {{"type", "array"}, {"items", seg}}})}};
}

Result<EditorSession::InputFeed> scriptedInput(EditorSession& s, const Json& spec) {
    if (spec.is_null()) return EditorSession::InputFeed{};
    Json segs = spec.is_array() ? spec : Json::array({spec});
    struct Seg {
        int frames;
        script::InputFrame in;
    };
    std::vector<Seg> list;
    const script::InputMap& map = s.world().scripts().input();
    for (auto& g : segs) {
        if (!g.is_object()) return Result<EditorSession::InputFeed>::failure("input: every segment must be an object");
        Seg seg{g.value("frames", 1 << 30), {}};
        for (auto& k : g.value("keys", Json::array())) {
            int code = k.is_string() ? script::InputMap::keyCode(k.get<std::string>()) : -1;
            if (code < 0 || code >= 512) return Result<EditorSession::InputFeed>::failure("input: unknown key " + k.dump());
            seg.in.down[static_cast<size_t>(code)] = true;
        }
        for (auto& a : g.value("actions", Json::array()))
            if (!a.is_string() || !map.drive(a.get<std::string>(), 1.0f, seg.in))
                return Result<EditorSession::InputFeed>::failure("input: unknown action " + a.dump() + " (see the project's input map)");
        Json axes = g.value("axes", Json::object());  // a named value: items() must not iterate a destroyed temporary
        for (auto& [name, v] : axes.items())
            if (!v.is_number() || !map.drive(name, v.get<float>(), seg.in))
                return Result<EditorSession::InputFeed>::failure("input: unknown axis '" + name + "' or bad value");
        seg.in.mouseDx += g.value("mouseDx", 0.0f);
        seg.in.mouseDy += g.value("mouseDy", 0.0f);
        list.push_back(seg);
    }
    return EditorSession::InputFeed([list](int frame, script::InputFrame& in) {
        int at = 0;
        for (auto& seg : list) {
            if (frame < at + seg.frames) {
                in = seg.in;
                return;
            }
            at += seg.frames;
        }
        in = {};
    });
}
} // namespace

void registerScriptTools(mcp::ToolRegistry& reg, EditorSession& s);
void registerGraphicsTools(mcp::ToolRegistry& reg, EditorSession& s);
void registerWorldTools(mcp::ToolRegistry& reg, EditorSession& s);
void registerAnimTools(mcp::ToolRegistry& reg, EditorSession& s);
void registerPhysicsTools(mcp::ToolRegistry& reg, EditorSession& s);
void registerAudioTools(mcp::ToolRegistry& reg, EditorSession& s);
void registerGameplayTools(mcp::ToolRegistry& reg, EditorSession& s);
void registerAuthoringTools(mcp::ToolRegistry& reg, EditorSession& s);
void registerStoreTools(mcp::ToolRegistry& reg, EditorSession& s);
void registerBlueprintTools(mcp::ToolRegistry& reg, EditorSession& s);
bool isBlueprintDryRun(const Json& args);
void registerMigrationTools(mcp::ToolRegistry& reg, EditorSession& s);
void registerEditorControlTools(mcp::ToolRegistry& reg, EditorSession& s);
bool isMigrationApply(const Json& args);
void registerScriptResources(mcp::McpServer& server, EditorSession& s);

mcp::Tool aiAuthorizationView(const mcp::Tool& tool, const Json& args) {
    Tool view = tool;
    if (tool.name == "convert_blueprint" && isBlueprintDryRun(args)) view.readOnly = true;  // only converts
    if (tool.name == "migrate_settings") {  // dry run: a read; apply: rewrites the user's settings
        view.readOnly = !isMigrationApply(args);
        view.destructive = !view.readOnly;
    }
    return view;
}

void registerEngineTools(mcp::ToolRegistry& reg, EditorSession& s) {
    reg.callWrapper = [&s](const Tool& tool, const Json& args, const std::function<ToolResult()>& run) {
        // AI master switch + control level: every tool call here comes from an AI (MCP client or assistant).
        // Tools whose permission depends on their arguments are authorized as aiAuthorizationView() says (dry runs as reads,
        // migrate_settings apply as destructive). Every call, allowed or refused, goes to the audit log (editor_control.h).
        Tool view = aiAuthorizationView(tool, args);
        AiCategory cat = aiCategoryOf(view);
        auto decide = [&]() -> ToolResult {
            if (view.readOnly && !tool.readOnly) {
                if (AiDecision d = s.ai.authorize(view); !d.allowed) return ToolResult::error(d.reason);
                return run();
            }
            if (AiDecision d = s.ai.authorize(view); !d.allowed) {
                FLEDGE_LOG_WARN("mcp", "AI call refused: {}", d.reason);
                return ToolResult::error(d.reason);
            }
            if (cat == AiCategory::EditorControl) {  // Full MCP Control: rate limit + argument size cap
                if (!s.editorControlLimiter) s.editorControlLimiter = std::make_shared<editor::RateLimiter>(editor::kRateLimitCalls, editor::kRateLimitSeconds);
                double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                if (args.dump().size() > editor::kMaxArgsBytes) return ToolResult::error("arguments too large for an editor-control tool");
                if (!s.editorControlLimiter->allow(now))
                    return ToolResult::error(std::format("rate limit: at most {} editor-control calls per {:.0f} s", editor::kRateLimitCalls,
                                                         editor::kRateLimitSeconds));
            }
            // 1.3.2 scene edits while the editor plays (docs/MCP.md "Edits while playing"): they change the play world, which
            // Stop throws away. No target: done, with a warning saying so. target "play": done, no warning (inspect-and-tweak).
            // target "edit": done in the play world AND replayed on the edit scene right after Stop (kept, undoable).
            const bool sceneEdit = kSceneEditTools.contains(tool.name);
            std::string target;
            if (sceneEdit && args.contains("target")) {
                target = args["target"].is_string() ? args["target"].get<std::string>() : "?";
                if (target != "play" && target != "edit") return ToolResult::error(tool.name + ": target must be \"play\" or \"edit\"");
                if (target == "play" && !s.playing())
                    return ToolResult::error(tool.name + ": target \"play\" needs Play mode; the editor is not playing (omit target to edit the scene)");
            }
            const bool duringPlay = sceneEdit && s.playing();
            ToolResult r;
            if (tool.readOnly || kNoTransaction.contains(tool.name)) {
                r = run();
            } else {
                s.beginEdit("AI: " + tool.name, EditSource::AI);
                r = run();
                if (r.isError) s.cancelEdit();  // atomic: a failed AI call leaves no partial edits
                else s.endEdit();
            }
            if (duringPlay && !r.isError) {
                std::string note;
                if (target == "edit") {
                    Json replay = args;
                    replay.erase("target");
                    s.deferredEdits.push_back({tool.name, replay});
                    note = std::format("applied to the play world now and queued for the edit scene: replayed right after Stop ({} queued)",
                                       s.deferredEdits.size());
                } else if (target.empty()) {
                    note = "the editor is in Play: this changed the PLAY world only, and Stop restores the pre-play scene (the edit is lost). "
                           "Pass target: \"edit\" to also apply it to the saved scene after Stop, or target: \"play\" to silence this warning.";
                }
                if (!note.empty()) {
                    if (r.structured.is_object()) r.structured[target == "edit" ? "deferred" : "warning"] = note;
                    r.content.push_back({{"type", "text"}, {"text", (target == "edit" ? "note: " : "warning: ") + note}});
                }
            }
            return r;
        };
        ToolResult r = decide();
        if (s.auditLog) {
            std::string err;
            if (r.isError && r.content.is_array() && !r.content.empty() && r.content[0].contains("text") && r.content[0]["text"].is_string())
                err = r.content[0]["text"].get<std::string>();
            s.auditLog->record(s.aiCaller, tool.name, toString(cat), args, !r.isError, err);
        }
        return r;
    };

    // ------------------------------------------------------- read-only ----
    reg.add({"get_project_info", "Project info", "Returns the open project, current scene, play state, undo state and the auto-save / crash-recovery state (read-only: snapshot count, pending recovery, whether true auto-save is active in this session).", obj({}), true,
             false, [&s](const Json&) {
                 if (!s.hasProject()) return ToolResult::json({{"project", nullptr}, {"entityCount", s.scene().entities().size()}});
                 const auto& p = s.project();
                 return ToolResult::json({{"name", p.name},
                                          {"gameName", p.gameName},
                                          {"startupScene", p.startupScene},
                                          {"modules", p.modules},
                                          {"edition", editionId(currentEdition())},
                                          {"requiredFeatures", [&] { Json r = Json::array(); for (auto& f : p.requiredFeatures) r.push_back(f.toJson()); return r; }()},
                                          {"minEdition", editionId(p.minEdition())},
                                          {"currentScene", s.world().currentScenePath()},
                                          {"entityCount", s.scene().entities().size()},
                                          {"playing", s.playing()},
                                          {"canUndo", s.history().canUndo()},
                                          {"canRedo", s.history().canRedo()},
                                          {"unsavedChanges", s.dirty},
                                          {"editsQueuedForStop", s.deferredEdits.size()},
                                          {"autosave", s.autosaveState()}});
             }});

    reg.add({"list_entities", "List entities",
             "Lists all entities (depth-first: parents before children) with id, name, parent, components and world position.",
             obj({{"nameContains", {{"type", "string"}, {"description", "optional case-insensitive filter"}}}}), true, false,
             [&s](const Json& a) {
                 std::string f = toLower(a.value("nameContains", ""));
                 Json list = Json::array();
                 for (EntityId e : s.scene().depthFirstOrder())
                     if (f.empty() || toLower(s.scene().displayName(e)).find(f) != std::string::npos) list.push_back(entitySummary(s, e));
                 return ToolResult::json({{"scene", s.scene().name}, {"entities", list}});
             }});

    reg.add({"get_hierarchy", "Get hierarchy", "Returns the scene as a parent/child tree of {id, name, children}.", obj({}), true, false,
             [&s](const Json&) {
                 Json roots = Json::array();
                 for (EntityId r : s.scene().roots()) roots.push_back(hierarchyNode(s, r));
                 return ToolResult::json({{"scene", s.scene().name}, {"roots", roots}});
             }});

    reg.add({"get_entity", "Get entity",
             "Returns every component/property of one entity (Transform is LOCAL to the parent), plus parent, children and world transform.",
             obj({{"entity", kEntityRef}}, {"entity"}), true, false, [&s](const Json& a) {
                 EntityId e = resolveEntity(s, a["entity"]);
                 if (!e) return notFound(a["entity"]);
                 Json j = s.scene().entityToJson(e);
                 j["parent"] = s.scene().parentOf(e);
                 j["children"] = s.scene().childrenOf(e);
                 Transform w = s.scene().worldTransform(e);
                 j["world"] = {{"position", w.position}, {"rotation", w.rotation}, {"scale", w.scale}};
                 if (auto* t = s.scene().get<TransformComponent>(e)) {
                     // bone socket (saved only when set; shown here either way so clients discover the field)
                     if (j["components"].contains("Transform") && !j["components"]["Transform"].contains("attachBone"))
                         j["components"]["Transform"]["attachBone"] = "";
                     if (!t->attachBone.empty()) {
                         EntityId p = s.scene().parentOf(e);
                         bool resolved = p && s.scene().boneResolver && s.scene().boneResolver(p, t->attachBone).has_value();
                         j["socket"] = {{"bone", t->attachBone}, {"parent", p}, {"resolved", resolved}};
                     }
                 }
                 return ToolResult::json(j);
             }});

    reg.add({"describe_components", "Describe component types",
             "Lists every component type with its properties and value schema (from the reflection system).", obj({}), true, false,
             [](const Json&) {
                 Json list = Json::array();
                 for (auto& t : ComponentRegistry::get().types()) {
                     Json d = t.type.describe();
                     d["removable"] = t.removable;
                     if (!t.feature.empty()) d["feature"] = t.feature;  // Advanced module component
                     list.push_back(d);
                 }
                 return ToolResult::json({{"components", list}, {"edition", editionId(currentEdition())}, {"editionName", editionName(currentEdition())}});
             }});

    reg.add({"list_assets", "List assets",
             "Lists registered assets (GUID, content path, type: Mesh/Texture/Material/Model/Scene), optionally filtered by type.",
             obj({{"type", {{"type", "string"}, {"enum", {"Mesh", "Texture", "Material", "Model", "Scene"}}}}}), true, false,
             [&s](const Json& a) {
                 std::string filter = a.value("type", "");
                 Json list = Json::array();
                 for (auto& r : s.registry().all())
                     if (filter.empty() || filter == toString(r.type))
                         list.push_back({{"guid", r.guid.toString()}, {"path", "content:/" + r.path}, {"type", toString(r.type)}});
                 return ToolResult::json({{"assets", list}, {"builtinMeshes", {"builtin:cube", "builtin:plane", "builtin:sphere"}}});
             }});

    reg.add({"get_material", "Get material", "Returns a material's properties (and their schema via describe_components-like 'schema').",
             obj({{"material", {{"type", "string"}, {"description", "material GUID or content path"}}}}, {"material"}), true, false,
             [&s](const Json& a) {
                 auto m = s.loadMaterial(a["material"]);
                 if (!m) return ToolResult::error("material not found: " + a["material"].dump());
                 return ToolResult::json({{"material", m->toJson()}, {"schema", Material::typeInfo().describe()}});
             }});

    reg.add({"read_log", "Read engine log", "Returns recent engine log lines (secrets are redacted).",
             obj({{"maxLines", {{"type", "integer"}, {"description", "default 100, max 1000"}}},
                  {"minLevel", {{"type", "string"}, {"enum", {"trace", "debug", "info", "warn", "error"}}}}}),
             true, false, [](const Json& a) {
                 int n = std::clamp(a.value("maxLines", 100), 1, 1000);
                 std::string lvl = a.value("minLevel", "trace");
                 int minL = lvl == "debug" ? 1 : lvl == "info" ? 2 : lvl == "warn" ? 3 : lvl == "error" ? 4 : 0;
                 std::string out;
                 for (auto& e : Logger::get().recent(static_cast<size_t>(n)))
                     if (static_cast<int>(e.level) >= minL)
                         out += std::format("[{:8.3f}][{}][{}] {}\n", e.timeSeconds, toString(e.level), e.category, e.message);
                 return ToolResult::text(out.empty() ? "(log empty)" : out);
             }});

    reg.add({"get_history", "Undo history", "Lists undo history entries (label, source user/AI) and the current position.",
             obj({{"maxEntries", {{"type", "integer"}}}}), true, false, [&s](const Json& a) {
                 const auto& cmds = s.history().commands();
                 size_t n = static_cast<size_t>(std::clamp(a.value("maxEntries", 50), 1, 1000));
                 Json list = Json::array();
                 for (size_t i = cmds.size() > n ? cmds.size() - n : 0; i < cmds.size(); ++i)
                     list.push_back({{"index", i}, {"label", cmds[i].label}, {"source", toString(cmds[i].source)},
                                     {"applied", i < s.history().cursor()}, {"entities", cmds[i].after.size()}});
                 return ToolResult::json({{"entries", list}, {"cursor", s.history().cursor()}, {"playing", s.playing()}});
             }});

    reg.add({"take_screenshot", "Viewport screenshot", "Renders the editor viewport and returns a PNG image.",
             obj({{"width", {{"type", "integer"}}}, {"height", {{"type", "integer"}}}}), true, false, [&s](const Json& a) {
                 if (!s.screenshotProvider)
                     return ToolResult::error("screenshots need a GPU-backed host: run the Fledge editor with --mcp-http "
                                              "(the headless fledge_mcp_server has no renderer)");
                 int w = std::clamp(a.value("width", 960), 64, 4096), h = std::clamp(a.value("height", 540), 64, 4096);
                 auto img = s.screenshotProvider(w, h);
                 if (!img) return ToolResult::error(img.error());
                 Bytes png = encodePng(*img);
                 return ToolResult::image(base64Encode(png), std::format("{}x{} viewport capture", w, h));
             }});

    // -------------------------------------------------------- mutating ----
    reg.add({"create_entity", "Create entity",
             "Creates an entity (one undo step). Optional parent, local position and components, e.g. "
             "{\"name\":\"Crate\",\"position\":[0,1,0],\"components\":{\"MeshRenderer\":{\"mesh\":\"builtin:cube\",\"color\":[1,0,0]}}}",
             obj({{"name", {{"type", "string"}}},
                  {"parent", kEntityRef},
                  {"position", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                  {"components", {{"type", "object"}, {"description", "{ComponentName: {property: value}}"}}}},
                 {"name"}),
             false, false, [&s](const Json& a) {
                 EntityId parent = kNullEntity;
                 if (a.contains("parent") && !a["parent"].is_null()) {
                     parent = resolveEntity(s, a["parent"]);
                     if (!parent) return notFound(a["parent"]);
                 }
                 Json comps = a.value("components", Json::object());
                 if (!comps.is_object()) return ToolResult::error("'components' must be an object");
                 if (auto err = checkCharacterRefs(s, comps); !err.empty()) return ToolResult::error("create_entity: " + err);
                 if (a.contains("position")) comps["Transform"]["position"] = a["position"];
                 EntityId e = s.createEntity(a["name"].get<std::string>(), parent, comps);
                 if (!e) return ToolResult::error("create_entity failed (see read_log): check component names and values");
                 s.select(e);
                 return ToolResult::json(entitySummary(s, e));
             }});

    reg.add({"set_component_property", "Set component property",
             "Sets one reflected property, e.g. entity \"Sun\", component \"Light\", property \"intensity\", value 2.0. "
             "Adds the component if missing. Transform values are LOCAL to the parent.",
             obj({{"entity", kEntityRef}, {"component", {{"type", "string"}}}, {"property", {{"type", "string"}}},
                  {"value", {{"description", "JSON value matching the property schema (see describe_components)"}}}},
                 {"entity", "component", "property", "value"}),
             false, false, [&s](const Json& a) {
                 EntityId e = resolveEntity(s, a["entity"]);
                 if (!e) return notFound(a["entity"]);
                 if (a["component"] == "Animator" && a["property"] == "character")
                     if (auto err = checkCharacterRef(s, a["value"]); !err.empty()) return ToolResult::error(err);
                 if (auto st = s.setProperty(e, a["component"], a["property"], a["value"]); !st) return ToolResult::error(st.error);
                 return ToolResult::json(s.scene().entityToJson(e));
             }});

    reg.add({"set_components", "Set several components",
             "Adds/updates several components on an entity at once: {\"entity\":5,\"components\":{\"Transform\":{\"scale\":[2,2,2]},"
             "\"Rotator\":{\"degreesPerSecond\":90}}}",
             obj({{"entity", kEntityRef}, {"components", {{"type", "object"}}}}, {"entity", "components"}), false, false,
             [&s](const Json& a) {
                 EntityId e = resolveEntity(s, a["entity"]);
                 if (!e) return notFound(a["entity"]);
                 if (auto err = checkCharacterRefs(s, a["components"]); !err.empty()) return ToolResult::error("set_components: " + err);
                 if (auto st = s.applyComponents(e, a["components"]); !st) return ToolResult::error(st.error);
                 return ToolResult::json(s.scene().entityToJson(e));
             }});

    reg.add({"set_parent", "Set parent",
             "Re-parents an entity (parent null = make it a root). keepWorldTransform (default true) keeps it in place visually.",
             obj({{"entity", kEntityRef},
                  {"parent", {{"description", "entity id/name, or null for root"}}},
                  {"index", {{"type", "integer"}, {"description", "optional position among siblings"}}},
                  {"keepWorldTransform", {{"type", "boolean"}}}},
                 {"entity", "parent"}),
             false, false, [&s](const Json& a) {
                 EntityId e = resolveEntity(s, a["entity"]);
                 if (!e) return notFound(a["entity"]);
                 EntityId p = kNullEntity;
                 if (!a["parent"].is_null()) {
                     p = resolveEntity(s, a["parent"]);
                     if (!p) return notFound(a["parent"]);
                 }
                 if (auto st = s.setParent(e, p, a.value("index", -1), a.value("keepWorldTransform", true)); !st)
                     return ToolResult::error(st.error);
                 return ToolResult::json(entitySummary(s, e));
             }});

    reg.add({"duplicate_entity", "Duplicate entity", "Duplicates an entity with its children (next to the original).",
             obj({{"entity", kEntityRef}}, {"entity"}), false, false, [&s](const Json& a) {
                 EntityId e = resolveEntity(s, a["entity"]);
                 if (!e) return notFound(a["entity"]);
                 auto out = s.duplicate({e});
                 if (out.empty()) return ToolResult::error("duplicate failed");
                 return ToolResult::json(entitySummary(s, out[0]));
             }});

    reg.add({"undo", "Undo", "Undoes the last edit (user or AI). Not available while playing.", obj({}), false, false,
             [&s](const Json&) {
                 if (s.playing()) return ToolResult::error("stop play mode first");
                 size_t c = s.history().cursor();
                 if (!s.undo()) return ToolResult::error("nothing to undo");
                 const auto& cmd = s.history().commands()[c - 1];
                 return ToolResult::text(std::format("undid '{}' ({})", cmd.label, toString(cmd.source)));
             }});

    reg.add({"redo", "Redo", "Redoes the last undone edit.", obj({}), false, false, [&s](const Json&) {
        if (s.playing()) return ToolResult::error("stop play mode first");
        size_t c = s.history().cursor();
        if (!s.redo()) return ToolResult::error("nothing to redo");
        const auto& cmd = s.history().commands()[c];
        return ToolResult::text(std::format("redid '{}' ({})", cmd.label, toString(cmd.source)));
    }});

    reg.add({"load_scene", "Load scene", "Loads a .fscene from the project content (clears undo history).",
             obj({{"path", {{"type", "string"}}}}, {"path"}), false, false, [&s](const Json& a) {
                 if (!s.hasProject()) return noProject();
                 std::string p = a["path"];
                 if (!startsWith(p, "content:/")) p = "content:/" + p;
                 if (auto st = s.loadScene(p); !st) return ToolResult::error(st.error);
                 return ToolResult::text(std::format("loaded {} ({} entities)", p, s.scene().entities().size()));
             }});

    reg.add({"save_scene", "Save scene", "Saves the open scene (optionally to a new content:/...fscene path).",
             obj({{"path", {{"type", "string"}}}}), false, false, [&s](const Json& a) {
                 if (!s.hasProject()) return noProject();
                 if (auto st = s.saveScene(a.value("path", "")); !st) return ToolResult::error(st.error);
                 return ToolResult::text("saved " + s.world().currentScenePath());
             }});

    reg.add({"import_asset", "Import asset",
             "Imports a local file into the project: glTF 2.0 (.gltf/.glb -> meshes, textures, materials, .fmodel), .obj meshes or "
             ".png/.jpg/.tga/.bmp textures. Writes files (not undoable); with instantiate=true the created entities are undoable.",
             obj({{"sourcePath", {{"type", "string"}, {"description", "absolute path on this machine"}}},
                  {"background", {{"type", "boolean"}, {"description", "queue a new asset in the editor and poll get_asset_import_job for the result"}}},
                  {"instantiate", {{"type", "boolean"}, {"description", "also add the mesh/model to the scene"}}},
                  {"fixZeroWeights", {{"type", "boolean"}, {"description", "1.3.2 skinned models: vertices with all skin weights zero take the "
                                                                          "weights of their nearest weighted vertex (default false: reported only)"}}},
                  {"fixZeroNormals", {{"type", "boolean"}, {"description", "1.3.3 glTF / FBX: vertices with zero-length / NaN normals get normals "
                                                                          "regenerated from the geometry (default false: reported only, in warnings)"}}}},
                 {"sourcePath"}),
             false, false, [&s](const Json& a) {
                 if (!s.hasProject()) return noProject();
                 ModelImportOptions options;
                 options.fixZeroWeights = a.value("fixZeroWeights", false);
                 options.fixZeroNormals = a.value("fixZeroNormals", false);
                 if (a.value("background", false)) {
                     if (!s.startBackgroundAssetImport) return ToolResult::error("background import requires the graphical editor");
                     auto job = s.startBackgroundAssetImport(pathFromUtf8(a["sourcePath"].get<std::string>()), options,
                                                            a.value("instantiate", false));
                     if (!job) return ToolResult::error(job.error());
                     return ToolResult::json({{"id", *job}, {"state", "importing"}});
                 }
                 auto r = s.importAsset(pathFromUtf8(a["sourcePath"].get<std::string>()), options);
                 if (!r) return ToolResult::error(r.error());
                 Json created = Json::array();
                 for (auto& c : r->created)
                     if (c.type != AssetType::Other) created.push_back({{"guid", c.guid.toString()}, {"path", "content:/" + c.path}, {"type", toString(c.type)}});
                 Json out = {{"primary", {{"guid", r->primary.guid.toString()}, {"path", "content:/" + r->primary.path}, {"type", toString(r->primary.type)}}},
                             {"created", created}, {"warnings", r->warnings}};
                 if (a.value("instantiate", false)) {
                     auto e = s.instantiateAsset(r->primary.guid.toString());
                     if (!e) return ToolResult::error("imported, but instantiate failed: " + e.error());
                     out["entity"] = *e;
                 }
                 return ToolResult::json(out);
             }});

    reg.add({"get_asset_import_job", "Get background asset import",
             "Returns the state and result of an import_asset call made with background enabled.",
             obj({{"id", {{"type", "integer"}}}}, {"id"}),
             true, false, [&s](const Json& a) {
                 Json job = s.backgroundAssetImportStatus ? s.backgroundAssetImportStatus(a["id"].get<int>()) : Json();
                 if (job.is_null()) return ToolResult::error("no background asset import with that id");
                 return ToolResult::json(job);
             }});

    reg.add({"reimport_asset", "Re-import asset",
             "Re-runs the importer for an asset (glTF models re-import from their stored source copy) and live-reloads it. A .fchar "
             "character (1.3.0) is reloaded from disk: every Animator using it picks up the new clips and skeleton.",
             obj({{"asset", {{"type", "string"}, {"description", "GUID or content path"}}},
                  {"fixZeroWeights", {{"type", "boolean"}, {"description", "1.3.2: re-import a glTF / FBX model with (true) or without (false) the "
                                                                          "zero-weight fix; omitted = what the first import used"}}},
                  {"fixZeroNormals", {{"type", "boolean"}, {"description", "1.3.3: re-import with (true) or without (false) the zero-length "
                                                                          "normal fix; omitted = what the first import used"}}}},
                 {"asset"}),
             false, false, [&s](const Json& a) {
                 std::string ref = a["asset"].is_string() ? a["asset"].get<std::string>() : "";
                 std::string resolved = s.registry().resolve(ref);
                 std::string path = resolved.empty() ? ref : resolved;
                 if (path.size() > 6 && toLower(path.substr(path.size() - 6)) == ".fchar") {
                     std::string rel = startsWith(path, "content:/") ? path.substr(9) : path;
                     if (!s.vfs().exists("content:/" + rel)) return ToolResult::error("character not found: " + ref);
                     // every cache key naming this file (with or without content:/) is dropped; Animators reload on their next update
                     int dropped = s.world().animation().forgetCharacter(rel);
                     const anim::Character* ch = s.world().animation().character(rel);
                     if (!ch) return ToolResult::error("character '" + rel + "' could not be loaded: " + s.world().animation().characterError(rel));
                     Json clips = Json::array();
                     for (auto& c : ch->clips) clips.push_back(c.name);
                     return ToolResult::json({{"reloaded", true}, {"character", rel}, {"cacheEntriesDropped", dropped},
                                              {"bones", ch->skeleton.bones.size()}, {"clips", clips}});
                 }
                 std::optional<bool> fix;
                 if (a.contains("fixZeroWeights") && a["fixZeroWeights"].is_boolean()) fix = a["fixZeroWeights"].get<bool>();
                 std::optional<bool> fixN;
                 if (a.contains("fixZeroNormals") && a["fixZeroNormals"].is_boolean()) fixN = a["fixZeroNormals"].get<bool>();
                 auto r = s.reimportAsset(a["asset"], fix, fixN);
                 if (!r) return ToolResult::error(r.error());
                 return ToolResult::json({{"reimported", r->created.size()}, {"warnings", r->warnings}});
             }});

    reg.add({"instantiate_asset", "Instantiate asset", "Adds a Mesh or Model asset to the scene (Model = entity tree). Undoable.",
             obj({{"asset", {{"type", "string"}}}, {"parent", kEntityRef}, {"position", {{"type", "array"}, {"items", {{"type", "number"}}}}}},
                 {"asset"}),
             false, false, [&s](const Json& a) {
                 EntityId parent = kNullEntity;
                 if (a.contains("parent") && !a["parent"].is_null()) {
                     parent = resolveEntity(s, a["parent"]);
                     if (!parent) return notFound(a["parent"]);
                 }
                 Vec3 pos = a.contains("position") ? a["position"].get<Vec3>() : Vec3{0, 0, 0};
                 auto e = s.instantiateAsset(a["asset"], parent, pos);
                 if (!e) return ToolResult::error(e.error());
                 s.select(*e);
                 Json tree = hierarchyNode(s, *e);
                 return ToolResult::json(tree);
             }});

    reg.add({"cook_assets", "Cook assets",
             "Incrementally cooks source assets (OBJ -> .cmesh, images -> .ctex with mips/BC compression) into <project>/Intermediate/Cooked.",
             obj({{"force", {{"type", "boolean"}, {"description", "re-cook everything"}}}}), false, false, [&s](const Json& a) {
                 auto r = s.cook(a.value("force", false));
                 if (!r) return ToolResult::error(r.error());
                 return ToolResult::json({{"cooked", r->cooked}, {"upToDate", r->skipped}, {"removed", r->removed}, {"passthrough", r->passthrough},
                                          {"cookedBytes", r->cookedBytes}, {"files", r->cookedFiles}});
             }});

    reg.add({"create_material", "Create material",
             "Creates Materials/<name>.fmat with optional properties (see get_material schema), e.g. "
             "{\"name\":\"Red\",\"properties\":{\"baseColor\":[1,0,0],\"roughness\":0.3}}. Undoable.",
             obj({{"name", {{"type", "string"}}}, {"properties", {{"type", "object"}}}, {"assignTo", kEntityRef}}, {"name"}), false, false,
             [&s](const Json& a) {
                 auto r = s.createMaterial(a["name"], a.value("properties", Json::object()));
                 if (!r) return ToolResult::error(r.error());
                 Json out = {{"guid", r->guid.toString()}, {"path", "content:/" + r->path}};
                 if (a.contains("assignTo")) {
                     EntityId e = resolveEntity(s, a["assignTo"]);
                     if (!e) return notFound(a["assignTo"]);
                     if (auto st = s.assignMaterial(e, r->guid.toString()); !st) return ToolResult::error(st.error);
                     out["assignedTo"] = e;
                 }
                 return ToolResult::json(out);
             }});

    reg.add({"set_material_property", "Set material property",
             "Sets one material property, e.g. material \"Materials/Red.fmat\", property \"roughness\", value 0.2. Undoable; live-updates the viewport.",
             obj({{"material", {{"type", "string"}}}, {"property", {{"type", "string"}}}, {"value", {{"description", "JSON value"}}}},
                 {"material", "property", "value"}),
             false, false, [&s](const Json& a) {
                 if (auto st = s.setMaterialProperty(a["material"], a["property"], a["value"]); !st) return ToolResult::error(st.error);
                 return ToolResult::json(s.loadMaterial(a["material"])->toJson());
             }});

    reg.add({"assign_material", "Assign material",
             "Sets an entity's MeshRenderer.material (adds a MeshRenderer if missing); on an animated character (Animator, no "
             "MeshRenderer) it sets Animator.material instead. Undoable.",
             obj({{"entity", kEntityRef}, {"material", {{"type", "string"}}}}, {"entity", "material"}), false, false, [&s](const Json& a) {
                 EntityId e = resolveEntity(s, a["entity"]);
                 if (!e) return notFound(a["entity"]);
                 if (auto st = s.assignMaterial(e, a["material"]); !st) return ToolResult::error(st.error);
                 return ToolResult::json(s.scene().entityToJson(e));
             }});

    // Contract (docs/MCP.md "play"): the call returns only once Play is running - the pre-play scene snapshot is taken,
    // scripts / services are created - and reports the real state. simulateFrames N advances exactly N fixed 1/60 s
    // frames; 0 enters Play (or stays in it) without advancing. Everything done during Play is discarded by stop.
    reg.add({"create_project", "Create project",
             "Creates a new project folder <directory>/<name> and opens it. Work done before any project was open moves in: the current "
             "scene becomes the startup scene and materials created meanwhile are written to Content/ with the same GUIDs.",
             obj({{"directory", {{"type", "string"}, {"description", "absolute parent folder"}}}, {"name", {{"type", "string"}}}},
                 {"directory", "name"}),
             false, false, [&s](const Json& a) {
                 if (auto st = s.createProject(pathFromUtf8(a["directory"].get<std::string>()), a["name"].get<std::string>()); !st)
                     return ToolResult::error(st.error);
                 return ToolResult::json({{"project", narrow(s.project().file.wstring())},
                                          {"scene", s.world().currentScenePath()},
                                          {"entities", s.scene().entities().size()}});
             }});
    reg.add({"play", "Play",
             "Enters play mode and returns once it is running (pre-play scene snapshot taken; restored on stop, so edits made during Play "
             "never reach the saved scene). simulateFrames: N fixed 1/60 s frames are advanced before returning (deterministic, same "
             "update order as real time, also while paused); 0 = enter Play, advance nothing (scripts' OnStart runs on the first frame). "
             "input: scripted input for those frames, e.g. [{\"frames\": 60, \"axes\": {\"MoveY\": 1, \"MoveX\": -0.5}}, {\"frames\": 30, "
             "\"actions\": [\"Jump\"]}] (keys / actions / axes / mouseDx / mouseDy). Returns playing, frame (frames since Play started), snapshotTaken, scriptsStarted. Read get_runtime_log after. "
             "Lock-step (1.3.0): pauseAfter: true pauses Play once the frames are done, so screenshots / get_entity see exactly that frame "
             "(play again with simulateFrames to continue; real time stays stopped). exclusiveInput: true ignores the editor's live keyboard / "
             "mouse in the game view until stop (or exclusiveInput: false), so a person's mouse cannot change a scripted run.",
             obj({{"simulateFrames", {{"type", "integer"}, {"minimum", 0}, {"maximum", 36000}}}, {"input", playInputSchema()},
                  {"pauseAfter", {{"type", "boolean"}}}, {"exclusiveInput", {{"type", "boolean"}}}}),
             false, false, [&s](const Json& a) {
                 if (!s.playing())
                     if (auto st = s.play(); !st) return ToolResult::error(st.error);
                 if (a.contains("exclusiveInput") && a["exclusiveInput"].is_boolean()) s.scriptedInputOnly = a["exclusiveInput"].get<bool>();
                 int frames = a.value("simulateFrames", 0);
                 if (frames > 0) {
                     auto feed = scriptedInput(s, a.value("input", Json()));
                     if (!feed) return ToolResult::error(feed.error());
                     s.simulateFrames(frames, *feed);
                 }
                 if (a.value("pauseAfter", false)) s.setPaused(true);
                 return ToolResult::json({{"playing", s.playing()},
                                          {"frame", s.playFrames()},
                                          {"simulatedFrames", frames},
                                          {"snapshotTaken", s.playSnapshotTaken()},
                                          {"paused", s.paused()},
                                          {"exclusiveInput", s.scriptedInputOnly},
                                          {"scriptsStarted", s.playFrames() > 0},
                                          {"scriptObjects", s.world().scripts().objectCount()},
                                          {"behaviours", s.world().scripts().behaviourCount()},
                                          {"scriptModule", narrow(s.scriptModule().path.filename().wstring())}});
             }});

    reg.add({"set_input_map", "Set input map",
             "Replaces the project's input map (the \"input\" block of the .fledge, read by Fledge Script Input:: and scripted play input) and "
             "applies it at once, also while playing (1.3.0; before, edits on disk were only read when the project was opened). Shape: "
             "{\"actions\": {\"Fire\": [\"MouseLeft\", \"F\"]}, \"axes\": {\"MoveX\": {\"positive\": [\"D\"], \"negative\": [\"A\"]}, \"LookX\": "
             "{\"mouse\": \"x\", \"scale\": 0.1, \"gamepad\": \"rightx\", \"padScale\": 12}}}; entries replace the defaults of the same name. "
             "save: also write the .fledge (default true). reload: true re-reads the block from the .fledge on disk instead of taking `input`.",
             obj({{"input", {{"type", "object"}}}, {"save", {{"type", "boolean"}}}, {"reload", {{"type", "boolean"}}}}), false, false,
             [&s](const Json& a) {
                 if (!s.hasProject()) return ToolResult::error("no project is open");
                 Json input;
                 if (a.value("reload", false)) {
                     auto text = readFileText(s.project().file);
                     Json pj = text ? Json::parse(*text, nullptr, false) : Json();
                     if (!pj.is_object()) return ToolResult::error("cannot read the project file");
                     input = pj.value("input", Json::object());
                 } else {
                     if (!a.contains("input")) return ToolResult::error("input: required (or reload: true)");
                     input = a["input"];
                 }
                 if (!input.is_object()) return ToolResult::error("input: must be an object");
                 script::InputMap probe;  // validate on a scratch map first: a bad map never replaces the working one
                 if (auto st = probe.load(input); !st) return ToolResult::error(st.error);
                 script::InputMap& live = s.world().scripts().input();
                 live.setDefaults();
                 (void)live.load(input);
                 bool save = a.value("save", true) && !a.value("reload", false);
                 if (auto st = s.setProjectInput(input, save); !st) return ToolResult::error(st.error);
                 return ToolResult::json({{"applied", true}, {"saved", save}, {"actions", live.actionNames()}, {"axes", live.axisNames()}});
             }});
    reg.add({"stop", "Stop", "Leaves play mode: restores the pre-play scene exactly and clears all runtime input (held keys / axes).",
             obj({}), false, false, [&s](const Json&) {
                 int64_t frames = s.playFrames();
                 bool was = s.playing();
                 if (auto st = s.stop(); !st) return ToolResult::error(st.error);
                 Json out = {{"stopped", was}, {"playing", false}, {"framesPlayed", frames}, {"sceneRestored", was}};
                 // 1.3.2: edits made during Play with target "edit", replayed on the edit scene: {applied, failed: [{tool, args, error}]}
                 if (!s.lastDeferredReport.is_null()) out["editsReplayed"] = s.lastDeferredReport;
                 return ToolResult::json(out);
             }});
    reg.add({"build_package", "Build & package game",
             "Packages the project into <project>/Build/<GameName>/<GameName>.exe (content embedded as .fpak).",
             obj({{"sidecarPak", {{"type", "boolean"}, {"description", "write the pak next to the exe instead of embedding"}}}}), false,
             false, [&s](const Json& a) {
                 auto r = s.package(a.value("sidecarPak", false));
                 if (!r) return ToolResult::error(r.error());
                 return ToolResult::json({{"executable", narrow(r->executable.wstring())},
                                          {"files", r->fileCount},
                                          {"pakBytes", r->pakBytes},
                                          {"modules", r->copiedModules},
                                          {"iconApplied", r->iconApplied}});
             }});

    registerScriptTools(reg, s);
    registerGraphicsTools(reg, s);
    registerWorldTools(reg, s);
    registerAnimTools(reg, s);
    registerPhysicsTools(reg, s);
    registerAudioTools(reg, s);
    registerGameplayTools(reg, s);
    registerAuthoringTools(reg, s);
    registerStoreTools(reg, s);
    registerBlueprintTools(reg, s);
    registerMigrationTools(reg, s);
    registerEditorControlTools(reg, s);

    // ----------------------------------------------------- editions ----
    reg.add({"describe_edition", "Describe edition",
             "The edition of this Fledge (Fledge or Fledge Advanced), the features it provides, the features it does not (planned or "
             "Advanced-only, with why), the linked Advanced modules, and the open project's requiredFeatures / minEdition. Tools of "
             "unavailable features answer with a structured 'requires_fledge_advanced' error (docs/EDITIONS.md).",
             obj({}), true, false, [&s](const Json&) {
                 Json j = FeatureRegistry::get().describe();
                 j["version"] = FLEDGE_VERSION_STRING;
                 j["mcpTier"] = currentEdition() == Edition::Advanced ? "core+advanced" : "core";
                 {  // 1.3.1: GPU skinning bone limit of this edition / backend (docs/GRAPHICS.md "GPU skinning")
                     ForwardRenderer* r = s.world().renderer();
                     j["gpuSkinning"] = {{"maxBones", maxGpuSkinBones()},
                                         {"standardMaxBones", kMaxGpuSkinBones},
                                         {"advancedMaxBones", kMaxGpuSkinBonesAdvanced},
                                         {"backend", r ? rhi::toString(r->device().backend()) : "none"},
                                         {"backendMaxBones", r ? r->gpuSkinBoneLimit() : 0},
                                         {"note", r ? r->gpuSkinNote() : std::string("no renderer: nothing is drawn")},
                                         {"gpuBackends", Json::array({"OpenGL", "D3D11", "D3D10", "Vulkan"})},
                                         {"cpuBackends", Json::array({"D3D12", "D3D9"})},
                                         {"maxInfluences", r ? r->gpuSkinInfluences() : 0}};  // 1.3.2: 8 where the variants exist
                     // 1.3.2: engine features of Fledge Advanced (docs/EDITIONS.md "Engine features that are Fledge Advanced only")
                     const bool adv = advancedRenderFeatures();
                     j["renderFeatures"] = {{"viewmodelPass", true},
                                            {"leaderPose", adv},
                                            {"multiMaterialCharacters", adv},
                                            {"projectedDecals", adv && r && r->decalsSupported()},
                                            {"note", adv ? std::string() : advancedRenderNote("Leader pose, multi-material characters and projected decals")}};
                 }
                 if (s.hasProject()) {
                     Json req = Json::array();
                     for (const auto& r : s.project().requiredFeatures) req.push_back(r.toJson());
                     j["project"] = {{"name", s.project().name}, {"requiredFeatures", req}, {"minEdition", editionId(s.project().minEdition())},
                                     {"opensIn", s.project().minEdition() == Edition::Advanced ? Json::array({"advanced"})
                                                                                               : Json::array({"standard", "advanced"})}};
                 }
                 j["docs"] = "docs/EDITIONS.md";
                 return ToolResult::json(j);
             }});
    for (auto& hook : FeatureRegistry::get().editorToolHooks) hook(reg, s);

    // ----------------------------------------------------- destructive ----
    reg.add({"delete_entity", "Delete entity", "Deletes an entity, its children and all their components (undoable).",
             obj({{"entity", kEntityRef}}, {"entity"}), false, true, [&s](const Json& a) {
                 EntityId e = resolveEntity(s, a["entity"]);
                 if (!e) return notFound(a["entity"]);
                 std::string n = s.scene().displayName(e);
                 size_t count = 1 + s.scene().descendants(e).size();
                 if (auto st = s.destroyEntities({e}); !st) return ToolResult::error(st.error);
                 return ToolResult::text(std::format("deleted entity {} '{}' ({} entities incl. children)", e, n, count));
             }});

    reg.add({"remove_component", "Remove component", "Removes a component (Name and Transform cannot be removed).",
             obj({{"entity", kEntityRef}, {"component", {{"type", "string"}}}}, {"entity", "component"}), false, true,
             [&s](const Json& a) {
                 EntityId e = resolveEntity(s, a["entity"]);
                 if (!e) return notFound(a["entity"]);
                 if (auto st = s.removeComponent(e, a["component"]); !st) return ToolResult::error(st.error);
                 return ToolResult::json(s.scene().entityToJson(e));
             }});

    reg.add({"new_scene", "New scene", "Replaces the open scene with an empty one (camera + sun). Undoable until the scene is reloaded.",
             obj({{"name", {{"type", "string"}}}}), false, true, [&s](const Json& a) {
                 if (auto st = s.newScene(a.value("name", "Untitled")); !st) return ToolResult::error(st.error);
                 return ToolResult::text("new scene created (not saved yet)");
             }});

    // 1.3.2 edits while playing: every scene-edit tool accepts `target`, and stop() replays the target "edit" calls on the
    // restored edit scene (each as its own AI undo step; refused calls are reported, never half-applied).
    for (const std::string& name : kSceneEditTools)
        if (Tool* t = reg.findMutable(name); t && t->inputSchema.is_object()) {
            if (!t->inputSchema.contains("properties") || !t->inputSchema["properties"].is_object()) t->inputSchema["properties"] = Json::object();
            t->inputSchema["properties"]["target"] = {
                {"type", "string"},
                {"enum", {"play", "edit"}},
                {"description", "while the editor plays: \"play\" = change only the running play world (lost on Stop, no warning); \"edit\" = "
                                "also apply it to the edit scene right after Stop. Omitted during Play: play world + a warning."}};
        }
    // the handlers are copied (they capture the session only), so a replay never touches a registry that is gone
    std::map<std::string, std::function<ToolResult(const Json&)>> handlers;
    for (const std::string& name : kSceneEditTools)
        if (const Tool* t = reg.find(name)) handlers[name] = t->handler;
    s.replayDeferredEdits = [handlers, &s](const std::vector<EditorSession::DeferredEdit>& queue) {
        Json failed = Json::array();
        int applied = 0;
        for (const auto& d : queue) {
            auto h = handlers.find(d.tool);
            if (h == handlers.end()) continue;
            s.beginEdit("AI: " + d.tool + " (edit made during Play)", EditSource::AI);
            ToolResult r;
            try {
                r = h->second(d.args);
            } catch (const std::exception& e) {
                r = ToolResult::error(e.what());
            }
            std::string err;
            if (r.isError && r.content.is_array() && !r.content.empty() && r.content[0].contains("text")) err = r.content[0]["text"].get<std::string>();
            if (r.isError) {
                s.cancelEdit();
                failed.push_back({{"tool", d.tool}, {"args", d.args}, {"error", err}});
            } else {
                s.endEdit();
                ++applied;
            }
        }
        if (!failed.empty()) FLEDGE_LOG_WARN("mcp", "{} edit(s) made during Play could not be applied to the edit scene after Stop", failed.size());
        return Json{{"applied", applied}, {"failed", failed}};
    };
}

void registerEngineResources(mcp::McpServer& server, EditorSession& s) {
    registerScriptResources(server, s);
    for (auto& [id, text] : enginedocs::kDocs)  // the AI handbook also has a short name
        if (std::string_view(id) == "ai_handbook") {
            std::string body = text;
            server.addResource({"fledge://handbook", "Fledge AI handbook",
                                "How to build games with Fledge through MCP: workflow, tools per task, recipes, limits. Read this first.", "text/markdown",
                                [body]() -> std::optional<std::string> { return body; }});
        }
    for (auto& [id, text] : enginedocs::kDocs) {  // docs/<SYSTEM>.md, embedded at build time
        std::string body = text;
        server.addResource({std::string("fledge://docs/") + id, std::string("Fledge docs: ") + id,
                            "Engine system guide (components, editor tools, MCP tools, Fledge Script API, limits)", "text/markdown",
                            [body]() -> std::optional<std::string> { return body; }});
    }
    server.addResource({"fledge://project", "Project manifest", "The open .fledge (never contains credentials)",
                        "application/json", [&s]() -> std::optional<std::string> {
                            if (!s.hasProject()) return std::nullopt;
                            return s.project().toJson().dump(2);
                        }});
    server.addResource({"fledge://scene", "Current scene", "The open scene as .fscene JSON", "application/json",
                        [&s]() -> std::optional<std::string> { return s.scene().toJson().dump(2); }});
    server.addResource({"fledge://assets", "Asset registry", "Registered assets", "application/json",
                        [&s]() -> std::optional<std::string> { return s.registry().toJson().dump(2); }});
    server.addResource({"fledge://history", "Undo history", "Undo/redo history with sources (user/AI)", "application/json",
                        [&s]() -> std::optional<std::string> {
                            Json list = Json::array();
                            for (auto& c : s.history().commands()) list.push_back({{"label", c.label}, {"source", toString(c.source)}});
                            return Json{{"entries", list}, {"cursor", s.history().cursor()}}.dump(2);
                        }});
    server.addResource({"fledge://log", "Engine log", "Recent log lines", "text/plain", []() -> std::optional<std::string> {
                            std::string out;
                            for (auto& e : Logger::get().recent(300))
                                out += std::format("[{}][{}] {}\n", toString(e.level), e.category, e.message);
                            return out;
                        }});
}

} // namespace fledge
