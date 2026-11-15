// Full MCP Control tools (editor_control.h, docs/MCP.md "Full MCP Control"). All of them are in the AI category editorControl
// (ai_control.cpp), which the user turns on per session; the callWrapper (engine_tools.cpp) rate-limits them and writes the
// audit log. They return names, ids and UI state only - never a setting value that could be a secret.
#include <format>

#include "fledge/editor/editor_control.h"
#include "fledge/editor/editor_session.h"
#include "fledge/mcp/mcp_server.h"

namespace fledge {

using mcp::ToolResult;
using namespace editor;

namespace {
constexpr const char* kNoHost =
    "this tool drives the editor window, which this host does not have (fledge_mcp_server is headless); use the scene / asset / "
    "build tools instead";

Json schemaOf(std::initializer_list<std::pair<const std::string, Json>> props, std::vector<std::string> required = {}) {
    Json p = Json::object();
    for (auto& [k, v] : props) p[k] = v;
    Json s = {{"type", "object"}, {"properties", p}, {"additionalProperties", false}};
    if (!required.empty()) s["required"] = required;
    return s;
}

Json selectionJson(EditorSession& s) {
    Json sel = Json::array();
    for (EntityId e : s.selection)
        if (s.scene().isValid(e)) sel.push_back({{"id", e}, {"name", s.scene().displayName(e)}});
    return sel;
}
} // namespace

void registerEditorControlTools(mcp::ToolRegistry& reg, EditorSession& s) {
    const Json confirm = {{"type", "boolean"}, {"description", "required for destructive / external-program commands (see the policy in the listing)"}};

    reg.add({"list_editor_commands", "List editor commands",
             "Full MCP Control. Every editor command (menus, toolbar, command palette): id, title, category, shortcut, enabled, toggle "
             "state, and the MCP policy for running it (allowed / needs confirm: true / blocked, plus a category it also needs).",
             schemaOf({{"filter", {{"type", "string"}, {"description", "case-insensitive substring of id / title / category"}}}}), true, false,
             [&s](const Json& a) {
                 if (!s.controlHost) return ToolResult::error(kNoHost);
                 std::string f = toLower(a.value("filter", ""));
                 Json out = Json::array();
                 for (auto& c : s.controlHost->commands()) {
                     if (!f.empty() && toLower(c.id + " " + c.title + " " + c.category).find(f) == std::string::npos) continue;
                     if (out.size() >= kMaxListItems) break;
                     EditorCommandPolicy p = editorCommandPolicy(c.id);
                     Json j = {{"id", c.id}, {"title", c.title}, {"category", c.category}, {"enabled", c.enabled},
                               {"policy", p.gate == CommandGate::Blocked ? "blocked" : p.gate == CommandGate::NeedsConfirm ? "confirm" : "allowed"}};
                     if (!c.shortcut.empty()) j["shortcut"] = c.shortcut;
                     if (c.toggle) j["checked"] = c.checked;
                     if (!p.reason.empty()) j["why"] = p.reason;
                     if (p.alsoNeeds) j["alsoNeeds"] = toString(*p.alsoNeeds);
                     out.push_back(j);
                 }
                 return ToolResult::json({{"commands", out}, {"count", out.size()}});
             }});

    reg.add({"run_editor_command", "Run an editor command",
             "Full MCP Control. Runs an editor command by id exactly like its menu item / palette entry (no mouse or keyboard input). "
             "Commands that delete, exit, revert, package, build, or start an external program need confirm: true (and their "
             "category, e.g. packaging / scriptBuilds); AI-permission switches and OS file dialogs are refused. Commands take no "
             "arguments (args must be empty): use the dedicated tools for parameters.",
             schemaOf({{"id", {{"type", "string"}}}, {"args", {{"type", "object"}}}, {"confirm", confirm}}, {"id"}), false, false,
             [&s](const Json& a) {
                 if (!s.controlHost) return ToolResult::error(kNoHost);
                 std::string id = a.value("id", "");
                 if (a.contains("args") && a["args"].is_object() && !a["args"].empty())
                     return ToolResult::error("editor commands take no arguments; use the dedicated MCP tool (see list_capabilities)");
                 EditorCommandPolicy p = editorCommandPolicy(id);
                 if (p.gate == CommandGate::Blocked) return ToolResult::error("'" + id + "' is not available over MCP: " + p.reason);
                 if (p.alsoNeeds && !s.ai.categoryAllowed(*p.alsoNeeds))
                     return ToolResult::error(std::format("'{}' {}; it also needs the '{}' category (Settings > AI Control)", id, p.reason,
                                                          toString(*p.alsoNeeds)));
                 if (p.gate == CommandGate::NeedsConfirm && !a.value("confirm", false))
                     return ToolResult::error("'" + id + "' " + p.reason + ": call again with confirm: true if the user wants this");
                 bool known = false;
                 for (auto& c : s.controlHost->commands()) known = known || c.id == id;
                 if (!known) return ToolResult::error("unknown editor command '" + id + "' (list_editor_commands)");
                 if (auto st = s.controlHost->runCommand(id); !st) return ToolResult::error(st.error);
                 return ToolResult::json({{"ran", id}, {"state", s.controlHost->state()}});
             }});

    reg.add({"list_editor_panels", "List editor panels and windows",
             "Full MCP Control. Dockable panels (name, category, open) and the named windows / dialogs set_editor_window can open, plus "
             "the Settings pages.",
             schemaOf({}), true, false, [&s](const Json&) {
                 if (!s.controlHost) return ToolResult::error(kNoHost);
                 Json panels = Json::array();
                 for (auto& p : s.controlHost->panels())
                     panels.push_back({{"name", p.name}, {"category", p.category}, {"open", p.open}, {"placeholder", p.placeholder}});
                 return ToolResult::json({{"panels", panels}, {"windows", s.controlHost->windows()}, {"settingsPages", s.controlHost->settingsPages()}});
             }});

    reg.add({"set_editor_panel", "Open / close / focus a panel",
             "Full MCP Control. Opens, closes or focuses a dockable panel by name (list_editor_panels).",
             schemaOf({{"name", {{"type", "string"}}}, {"open", {{"type", "boolean"}}}, {"focus", {{"type", "boolean"}}}}, {"name"}), false, false,
             [&s](const Json& a) {
                 if (!s.controlHost) return ToolResult::error(kNoHost);
                 std::optional<bool> open;
                 if (a.contains("open") && a["open"].is_boolean()) open = a["open"].get<bool>();
                 if (auto st = s.controlHost->setPanel(a.value("name", ""), open, a.value("focus", false)); !st) return ToolResult::error(st.error);
                 return ToolResult::json({{"ok", true}, {"state", s.controlHost->state()}});
             }});

    reg.add({"set_editor_window", "Open / close an editor window",
             "Full MCP Control. Opens or closes a named window (settings, shortcuts, about, project_settings, palette, welcome, "
             "new_project, migrate_settings ...; see list_editor_panels). For settings, page selects a Settings page. Opening a window "
             "never changes a setting.",
             schemaOf({{"window", {{"type", "string"}}}, {"open", {{"type", "boolean"}}}, {"page", {{"type", "string"}}}}, {"window"}), false, false,
             [&s](const Json& a) {
                 if (!s.controlHost) return ToolResult::error(kNoHost);
                 if (auto st = s.controlHost->setWindow(a.value("window", ""), a.value("open", true), a.value("page", "")); !st)
                     return ToolResult::error(st.error);
                 return ToolResult::json({{"ok", true}, {"state", s.controlHost->state()}});
             }});

    reg.add({"get_editor_state", "Editor UI state",
             "Full MCP Control. What is open and selected: workspace, Simple / Pro Mode, open panels and windows, focused window, "
             "settings page, selection (ids + names), play state, project, edition, whether Full MCP Control is on. No setting values.",
             schemaOf({}), true, false, [&s](const Json&) {
                 Json st = s.controlHost ? s.controlHost->state() : Json::object();
                 st["selection"] = selectionJson(s);
                 st["playing"] = s.playing();
                 st["paused"] = s.paused();
                 st["project"] = s.hasProject() ? Json(s.project().name) : Json(nullptr);
                 st["editorWindow"] = s.controlHost != nullptr;
                 return ToolResult::json(st);
             }});

    reg.add({"get_editor_ui_tree", "Editor UI tree",
             "Full MCP Control. The editor's ImGui windows this frame: name, position, size, focused, collapsed, docked (at most 200). "
             "Use take_screenshot for the viewport image.",
             schemaOf({}), true, false, [&s](const Json&) {
                 if (!s.controlHost) return ToolResult::error(kNoHost);
                 return ToolResult::json(s.controlHost->uiTree(kMaxUiWindows));
             }});

    reg.add({"select_entities", "Select entities",
             "Full MCP Control. Replaces the editor selection (Hierarchy / Inspector / gizmo) with these entity ids ([] clears it).",
             schemaOf({{"entities", {{"type", "array"}, {"items", {{"type", "integer"}}}}}}, {"entities"}), false, false, [&s](const Json& a) {
                 if (!a["entities"].is_array() || a["entities"].size() > kMaxListItems) return ToolResult::error("entities must be an array of at most 1000 ids");
                 std::vector<EntityId> ids;
                 for (auto& e : a["entities"]) {
                     if (!e.is_number_integer()) return ToolResult::error("entity ids must be integers");
                     EntityId id = e.get<EntityId>();
                     if (!s.scene().isValid(id)) return ToolResult::error(std::format("no entity {}", id));
                     ids.push_back(id);
                 }
                 s.selection.clear();
                 for (EntityId id : ids) s.select(id, true);
                 return ToolResult::json({{"selection", selectionJson(s)}});
             }});

    reg.add({"get_editor_preferences", "Editor preferences",
             "Full MCP Control. The editor preferences an AI may change (theme, UI scale, font size, camera speed, mouse, viewport "
             "overlays, startup). AI provider, AI control, MCP and path settings are not exposed.",
             schemaOf({}), true, false, [&s](const Json&) {
                 if (!s.controlHost) return ToolResult::error(kNoHost);
                 return ToolResult::json({{"preferences", s.controlHost->preferences()}, {"keys", editorPreferenceKeys()}});
             }});

    reg.add({"set_editor_preference", "Set an editor preference",
             "Full MCP Control. Changes one editor preference (get_editor_preferences lists the keys and current values) and saves it.",
             schemaOf({{"key", {{"type", "string"}}}, {"value", Json::object()}}, {"key", "value"}), false, false, [&s](const Json& a) {
                 if (!s.controlHost) return ToolResult::error(kNoHost);
                 std::string key = a.value("key", "");
                 if (auto st = validateEditorPreference(key, a["value"]); !st) return ToolResult::error(st.error);
                 if (auto st = s.controlHost->setPreference(key, a["value"]); !st) return ToolResult::error(st.error);
                 return ToolResult::json({{"ok", true}, {"preferences", s.controlHost->preferences()}});
             }});

    reg.add({"open_project", "Open a project",
             "Full MCP Control. Opens a .fledge project in the editor. Only projects inside the projects folder or a recent project's "
             "folder; with unsaved changes in the current scene it needs confirm: true (they are kept as a recovery snapshot, not saved).",
             schemaOf({{"path", {{"type", "string"}, {"description", "absolute path of the .fledge file"}}}, {"confirm", confirm}}, {"path"}), false, false,
             [&s](const Json& a) {
                 if (!s.controlHost) return ToolResult::error(kNoHost);
                 fs::path p = pathFromUtf8(a.value("path", ""));
                 if (auto st = checkProjectPath(p, s.controlHost->projectRoots()); !st) return ToolResult::error(st.error);
                 if (s.dirty && !a.value("confirm", false))
                     return ToolResult::error("the current scene has unsaved changes: save_scene first, or call again with confirm: true");
                 if (auto st = s.controlHost->openProject(p); !st) return ToolResult::error(st.error);
                 return ToolResult::json({{"opened", s.hasProject() ? s.project().name : std::string()}});
             }});
}

} // namespace fledge
