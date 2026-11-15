// MCP tools for the Fledge Script loop: write scripts -> build -> attach -> play -> read the log.
// Keep docs/MCP.md and docs/FLEDGE_SCRIPT.md in sync.
//
// Security: scripts are native code. File tools are sandboxed to <project>/Scripts
// (resolveScriptPath), and compiling + loading (build_scripts) requires the explicit
// opt-in AiCategory::ScriptBuild (editor Settings > AI Control,
// fledge_mcp_server --allow-script-builds).
#include <format>

#include "fledge/core/log.h"
#include "fledge/editor/editor_session.h"
#include "fledge/editor/script_docs.h"
#include "fledge/mcp/mcp_server.h"

namespace fledge {

using mcp::ToolResult;

namespace {
Json obj(std::initializer_list<std::pair<const std::string, Json>> props, std::vector<std::string> required = {}) {
    Json p = Json::object();
    for (auto& [k, v] : props) p[k] = v;
    Json s = {{"type", "object"}, {"properties", p}, {"additionalProperties", false}};
    if (!required.empty()) s["required"] = required;
    return s;
}
const Json kEntity = {{"description", "entity id (integer) or exact entity name (string)"}};
const Json kPath = {{"type", "string"}, {"description", "path relative to the project's Scripts/ folder, e.g. \"Player.cpp\" or \"AI/Traffic.h\""}};

EntityId entityOf(EditorSession& s, const Json& ref) {
    if (ref.is_number_integer()) {
        auto id = ref.get<EntityId>();
        return s.scene().isValid(id) ? id : kNullEntity;
    }
    if (ref.is_string()) return s.scene().findByName(ref.get<std::string>());
    return kNullEntity;
}

std::string numbered(const std::string& text, int firstLine, int lastLine) {
    std::string out;
    int line = 1;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        if (line >= firstLine && (lastLine <= 0 || line <= lastLine)) out += std::format("{:4}| {}\n", line, text.substr(start, end - start));
        if (end == text.size()) break;
        start = end + 1;
        ++line;
    }
    return out;
}

Json classesJson(EditorSession& s) {
    Json classes = Json::array();
    for (auto& c : s.scriptModule().classes) {
        Json props = Json::array();
        for (auto& p : c.properties) {
            Json pj = {{"name", p.name}, {"type", p.isArray ? "array<" + p.type + ">" : p.type}, {"default", p.defaultValue}};
            if (p.hasRange) pj["range"] = {p.min, p.max};
            if (!p.tooltip.empty()) pj["tooltip"] = p.tooltip;
            if (!p.assetKind.empty()) pj["assetKind"] = p.assetKind;
            if (p.readOnly) pj["readOnly"] = true;
            props.push_back(pj);
        }
        Json users = Json::array();
        if (!c.isService)
            for (EntityId e : s.scene().entities())
                if (auto* bc = s.scene().get<BehavioursComponent>(e); bc && bc->list.is_array())
                    for (auto& item : bc->list)
                        if (item.is_object() && item.value("class", "") == c.name) users.push_back({{"id", e}, {"name", s.scene().displayName(e)}});
        classes.push_back({{"name", c.name}, {"kind", c.isService ? "service" : "behaviour"}, {"properties", props}, {"attachedTo", users}});
    }
    return classes;
}

ToolResult noProject() { return ToolResult::error(kNoProjectHelp); }
} // namespace

void registerScriptTools(mcp::ToolRegistry& reg, EditorSession& s) {
    reg.add({"list_scripts", "List scripts",
             "Lists the project's Fledge Script files (Scripts/), the classes in the loaded script module and the last build state. "
             "Read the resource fledge://script/api before writing scripts.",
             obj({}), true, false, [&s](const Json&) {
                 if (!s.hasProject()) return noProject();
                 Json files = Json::array();
                 std::error_code ec;
                 fs::path root = s.project().scriptsDir();
                 if (fs::is_directory(root, ec))
                     for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                         if (!it->is_regular_file()) continue;
                         auto ext = it->path().extension().string();
                         if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".inl") continue;
                         files.push_back({{"path", narrow(it->path().lexically_relative(root).generic_wstring())}, {"bytes", it->file_size(ec)}});
                     }
                 Json build = s.hasScriptBuild() ? s.lastScriptBuild().toJson(20) : Json(nullptr);
                 return ToolResult::json({{"scriptsDir", "Scripts/"},
                                          {"files", files},
                                          {"moduleLoaded", s.world().scripts().hasModule()},
                                          {"module", narrow(s.scriptModule().path.filename().wstring())},
                                          {"classes", classesJson(s)},
                                          {"lastBuild", build},
                                          {"aiBuildsAllowed", s.ai.categoryAllowed(AiCategory::ScriptBuild)},
                                          {"pendingAiEdits", s.aiTouchedScripts}});
             }});

    reg.add({"read_script", "Read script", "Returns a script file from Scripts/ (optionally a line range, with line numbers).",
             obj({{"path", kPath},
                  {"lineNumbers", {{"type", "boolean"}, {"description", "prefix lines with numbers (default true)"}}},
                  {"fromLine", {{"type", "integer"}}},
                  {"toLine", {{"type", "integer"}}}},
                 {"path"}),
             true, false, [&s](const Json& a) {
                 if (!s.hasProject()) return noProject();
                 auto p = resolveScriptPath(s.project(), a["path"].get<std::string>());
                 if (!p) return ToolResult::error(p.error());
                 auto text = readFileText(*p);
                 if (!text) return ToolResult::error("no such script: " + a["path"].get<std::string>());
                 if (!a.value("lineNumbers", true)) return ToolResult::text(*text);
                 return ToolResult::text(numbered(*text, a.value("fromLine", 1), a.value("toLine", 0)));
             }});

    reg.add({"create_script", "Create script",
             "Creates Scripts/<className>.cpp from a template (behaviour | service | character | empty) and returns its source. "
             "Undoable. Build with build_scripts, then attach_script.",
             obj({{"className", {{"type", "string"}, {"description", "C++ class name, e.g. PlayerController"}}},
                  {"template", {{"type", "string"}, {"enum", scriptTemplateKinds()}}}},
                 {"className"}),
             false, false, [&s](const Json& a) {
                 if (!s.hasProject()) return noProject();
                 std::string cls = a["className"], kind = a.value("template", "behaviour");
                 if (!isValidScriptClassName(cls)) return ToolResult::error("invalid class name '" + cls + "' (C++ identifier)");
                 auto p = resolveScriptPath(s.project(), cls + ".cpp");
                 if (!p) return ToolResult::error(p.error());
                 std::error_code ec;
                 if (fs::exists(*p, ec)) return ToolResult::error("Scripts/" + cls + ".cpp already exists (use write_script / edit_script)");
                 std::string src = scriptTemplateSource(kind, cls);
                 if (auto st = s.writeScriptFile(cls + ".cpp", src, true); !st) return ToolResult::error(st.error);
                 return ToolResult::json({{"path", cls + ".cpp"}, {"source", src}});
             }});

    reg.add({"write_script", "Write script",
             "Creates or replaces a file in Scripts/ (.cpp/.h/.hpp/.inl only; paths outside Scripts/ are rejected). Undoable. "
             "Changes take effect after build_scripts (hot reload, also while playing).",
             obj({{"path", kPath}, {"content", {{"type", "string"}}}}, {"path", "content"}), false, false, [&s](const Json& a) {
                 if (!s.hasProject()) return noProject();
                 std::string content = a["content"];
                 if (content.size() > 512 * 1024) return ToolResult::error("script too large (512 KiB max)");
                 if (auto st = s.writeScriptFile(a["path"], content, true); !st) return ToolResult::error(st.error);
                 return ToolResult::text(std::format("wrote Scripts/{} ({} bytes)", a["path"].get<std::string>(), content.size()));
             }});

    reg.add({"edit_script", "Edit script",
             "Replaces an exact text snippet in a script (must match exactly once unless replaceAll). Undoable.",
             obj({{"path", kPath},
                  {"find", {{"type", "string"}, {"description", "exact text to replace (include enough context to be unique)"}}},
                  {"replace", {{"type", "string"}}},
                  {"replaceAll", {{"type", "boolean"}}}},
                 {"path", "find", "replace"}),
             false, false, [&s](const Json& a) {
                 if (!s.hasProject()) return noProject();
                 auto p = resolveScriptPath(s.project(), a["path"].get<std::string>());
                 if (!p) return ToolResult::error(p.error());
                 auto text = readFileText(*p);
                 if (!text) return ToolResult::error("no such script: " + a["path"].get<std::string>());
                 std::string find = a["find"], repl = a["replace"];
                 if (find.empty()) return ToolResult::error("'find' is empty");
                 size_t count = 0;
                 for (size_t pos = text->find(find); pos != std::string::npos; pos = text->find(find, pos + find.size())) ++count;
                 if (count == 0) return ToolResult::error("'find' text not found (read_script to see the current content)");
                 bool all = a.value("replaceAll", false);
                 if (count > 1 && !all) return ToolResult::error(std::format("'find' matches {} times; add context or set replaceAll", count));
                 std::string out = *text;
                 for (size_t pos = out.find(find); pos != std::string::npos; pos = out.find(find, pos + repl.size())) {
                     out.replace(pos, find.size(), repl);
                     if (!all) break;
                 }
                 if (auto st = s.writeScriptFile(a["path"], out, true); !st) return ToolResult::error(st.error);
                 return ToolResult::text(std::format("replaced {} occurrence(s) in Scripts/{}", all ? count : 1, a["path"].get<std::string>()));
             }});

    reg.add({"build_scripts", "Build scripts",
             "Compiles Scripts/ with the configured C++ compiler and (hot) reloads the module, also while playing (properties are kept). "
             "Returns structured diagnostics {severity, file, line, column, message}. Requires 'Allow AI to build scripts' "
             "(editor Settings > Scripts, or fledge_mcp_server --allow-script-builds): compiled scripts are native code.",
             obj({{"force", {{"type", "boolean"}, {"description", "rebuild every file"}}}}), false, false, [&s](const Json& a) {
                 if (!s.hasProject()) return noProject();
                 auto r = s.buildScripts(a.value("force", false));
                 if (r.ok) s.aiTouchedScripts.clear();
                 Json j = r.toJson(100);
                 j["moduleLoaded"] = s.world().scripts().hasModule();
                 j["classes"] = Json::array();
                 for (auto& c : s.scriptModule().classes) j["classes"].push_back(c.name);
                 j["hotReloaded"] = s.playing() && r.ok && !r.upToDate;
                 ToolResult tr = ToolResult::json(j);
                 tr.isError = !r.ok;  // diagnostics still in structuredContent
                 return tr;
             }});

    reg.add({"get_build_log", "Get build log", "Returns the last script build's diagnostics and raw compiler/linker output.",
             obj({{"maxBytes", {{"type", "integer"}, {"description", "truncate the raw log (default 20000)"}}}}), true, false, [&s](const Json& a) {
                 if (!s.hasProject()) return noProject();
                 if (!s.hasScriptBuild()) {
                     auto log = readFileText(s.project().scriptBuildDir() / "build.log");
                     return ToolResult::json({{"lastBuild", nullptr}, {"log", log.value_or("")}});
                 }
                 const auto& r = s.lastScriptBuild();
                 size_t max = static_cast<size_t>(a.value("maxBytes", 20000));
                 Json j = r.toJson(200);
                 j["log"] = r.log.size() > max ? r.log.substr(0, max) + "\n...(truncated)" : r.log;
                 return ToolResult::json(j);
             }});

    reg.add({"list_behaviours", "List behaviours",
             "Lists the behaviour and service classes in the loaded script module with their properties (type, default, range) "
             "and the entities each behaviour is attached to.",
             obj({}), true, false, [&s](const Json&) {
                 if (!s.hasProject()) return noProject();
                 return ToolResult::json({{"moduleLoaded", s.world().scripts().hasModule()}, {"classes", classesJson(s)}});
             }});

    reg.add({"attach_script", "Attach script",
             "Attaches a behaviour class to an entity (Behaviours component), with optional property overrides. Undoable. "
             "While playing the object is created immediately.",
             obj({{"entity", kEntity}, {"class", {{"type", "string"}}}, {"properties", {{"type", "object"}}}}, {"entity", "class"}), false,
             false, [&s](const Json& a) {
                 EntityId e = entityOf(s, a["entity"]);
                 if (!e) return ToolResult::error("entity not found: " + a["entity"].dump());
                 if (auto st = s.attachBehaviour(e, a["class"], a.value("properties", Json::object())); !st) return ToolResult::error(st.error);
                 return ToolResult::json(s.scene().entityToJson(e));
             }});

    reg.add({"detach_script", "Detach script", "Removes a behaviour class from an entity. Undoable.",
             obj({{"entity", kEntity}, {"class", {{"type", "string"}}}}, {"entity", "class"}), false, false, [&s](const Json& a) {
                 EntityId e = entityOf(s, a["entity"]);
                 if (!e) return ToolResult::error("entity not found: " + a["entity"].dump());
                 if (auto st = s.detachBehaviour(e, a["class"]); !st) return ToolResult::error(st.error);
                 return ToolResult::json(s.scene().entityToJson(e));
             }});

    reg.add({"get_script_properties", "Get script properties",
             "Returns the effective property values of the behaviours on an entity (class defaults + scene overrides; live values while playing).",
             obj({{"entity", kEntity}, {"class", {{"type", "string"}, {"description", "optional: one behaviour class"}}}}, {"entity"}), true,
             false, [&s](const Json& a) {
                 EntityId e = entityOf(s, a["entity"]);
                 if (!e) return ToolResult::error("entity not found: " + a["entity"].dump());
                 Json out = Json::object();
                 auto* bc = s.scene().get<BehavioursComponent>(e);
                 if (!bc || !bc->list.is_array()) return ToolResult::json({{"entity", e}, {"behaviours", out}});
                 for (auto& item : bc->list) {
                     std::string cls = item.value("class", "");
                     if (a.contains("class") && a["class"] != cls) continue;
                     auto props = s.behaviourProperties(e, cls);
                     out[cls] = {{"enabled", item.value("enabled", true)}, {"properties", props ? *props : Json(props.error())}};
                 }
                 return ToolResult::json({{"entity", e}, {"playing", s.playing()}, {"behaviours", out}});
             }});

    reg.add({"set_script_property", "Set script property",
             "Sets one behaviour property on an entity (validated against the class's property list). Undoable in edit mode; "
             "while playing it changes the live object too (play-mode changes are discarded on stop).",
             obj({{"entity", kEntity}, {"class", {{"type", "string"}}}, {"property", {{"type", "string"}}}, {"value", Json::object()}},
                 {"entity", "class", "property", "value"}),
             false, false, [&s](const Json& a) {
                 EntityId e = entityOf(s, a["entity"]);
                 if (!e) return ToolResult::error("entity not found: " + a["entity"].dump());
                 if (auto st = s.setBehaviourProperty(e, a["class"], a["property"], a["value"]); !st) return ToolResult::error(st.error);
                 auto props = s.behaviourProperties(e, a["class"]);
                 return ToolResult::json({{"entity", e}, {"class", a["class"]}, {"properties", props ? *props : Json()}});
             }});

    reg.add({"get_runtime_log", "Get runtime log",
             "Returns recent log lines from scripts (category 'script') and the script host ('script'), e.g. after play + simulateFrames.",
             obj({{"maxLines", {{"type", "integer"}}}, {"includeEngine", {{"type", "boolean"}, {"description", "also other categories"}}}}),
             true, false, [](const Json& a) {
                 int max = a.value("maxLines", 200);
                 bool all = a.value("includeEngine", false);
                 Json lines = Json::array();
                 for (auto& e : Logger::get().recent(2000)) {
                     if (!all && e.category != "script" && e.category != "script") continue;
                     lines.push_back({{"t", e.timeSeconds}, {"level", toString(e.level)}, {"category", e.category}, {"message", e.message}});
                 }
                 if (static_cast<int>(lines.size()) > max) lines.erase(lines.begin(), lines.end() - max);
                 return ToolResult::json({{"lines", lines}});
             }});
}

void registerScriptResources(mcp::McpServer& server, EditorSession& s) {
    server.addResource({"fledge://script/api", "Fledge Script API reference", "Concise Fledge Script API reference for writing gameplay scripts",
                        "text/markdown", []() -> std::optional<std::string> { return std::string(scriptdocs::kApi); }});
    server.addResource({"fledge://script/examples", "Fledge Script examples", "Canonical Fledge Script examples (controller, camera, coroutines, events, services)",
                        "text/markdown", []() -> std::optional<std::string> { return std::string(scriptdocs::kExamples); }});
    server.addResource({"fledge://script/build", "Last script build", "Diagnostics of the last Fledge Script build", "application/json",
                        [&s]() -> std::optional<std::string> {
                            if (!s.hasScriptBuild()) return std::string("{\"lastBuild\": null}");
                            return s.lastScriptBuild().toJson(200).dump(2);
                        }});
    // Deprecated aliases (Fledge Script was called Perch before 1.0.1): clients that remember the old URIs still work.
    server.addResource({"fledge://perch/api", "Fledge Script API reference (deprecated URI)", "Use fledge://script/api", "text/markdown",
                        []() -> std::optional<std::string> { return std::string(scriptdocs::kApi); }});
    server.addResource({"fledge://perch/examples", "Fledge Script examples (deprecated URI)", "Use fledge://script/examples", "text/markdown",
                        []() -> std::optional<std::string> { return std::string(scriptdocs::kExamples); }});
}

} // namespace fledge
