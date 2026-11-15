// Unreal Blueprint -> Fledge Script: editor glue and the MCP tool convert_blueprint (docs/BLUEPRINT_CONVERTER.md, docs/MCP.md).
//   convert_blueprint {text | path, className?, folder?, writeToProject?, dryRun?, unrealSpace?}
// A dry run (the default) only converts and returns the code + report: it is authorized like a read-only tool. Writing into
// the project is a normal file-writing (safe) tool call: Scripts/ sandbox, undoable, tagged AI, never overwrites.
#include <format>

#include "fledge/core/log.h"
#include "fledge/core/version.h"
#include "fledge/editor/blueprint_convert.h"
#include "fledge/editor/editor_session.h"
#include "fledge/mcp/mcp_server.h"

namespace fledge {

using mcp::ToolResult;

BlueprintJob makeBlueprintJob(const EditorSession& s, const std::string& className, const std::string& sourceName, bool unrealSpace) {
    BlueprintJob j;
    j.options.className = className;
    j.options.sourceName = sourceName;
    j.options.unrealSpace = unrealSpace;
    j.options.fledgeVersion = FLEDGE_VERSION_STRING;
    j.table = std::make_shared<blueprint::MappingTable>(blueprint::MappingTable::builtin());
    if (s.hasProject()) {
        fs::path extra = s.project().rootDir() / "blueprint_mappings.json";
        std::error_code ec;
        if (fs::exists(extra, ec)) {
            auto text = readFileText(extra);
            Json js = text ? Json::parse(*text, nullptr, false) : Json();
            auto t = js.is_discarded() ? Result<blueprint::MappingTable>::failure("not valid JSON") : blueprint::MappingTable::fromJson(js);
            if (t) {
                j.table->merge(*t);
                j.notes.push_back("project mappings merged from blueprint_mappings.json (" + std::to_string(t->functionCount()) + " functions)");
            } else {
                j.notes.push_back("blueprint_mappings.json ignored: " + t.error());
            }
        }
    }
    j.options.mappings = j.table.get();
    return j;
}

bool isBlueprintSourceFile(const fs::path& p) {
    std::string e = toLower(narrow(p.extension().wstring()));
    return e == ".t3d" || e == ".txt" || e == ".copy" || e == ".uasset" || e == ".umap";
}

Result<BlueprintWrite> writeBlueprintToProject(EditorSession& s, std::string_view text, const BlueprintJob& job, const std::string& folder, bool fromAi) {
    if (!s.hasProject()) return Result<BlueprintWrite>::failure(kNoProjectHelp);
    if (!blueprint::isSafeSubfolder(folder)) return Result<BlueprintWrite>::failure("folder must be a relative folder inside Scripts/ (no '..', drive letters or special names)");
    BlueprintWrite w;
    w.notes = job.notes;
    w.conversion = blueprint::convertText(text, job.options);
    if (!w.conversion.ok) return Result<BlueprintWrite>::failure(w.conversion.report.error);
    fs::path scripts = s.project().scriptsDir();
    std::string unique = blueprint::uniqueClassName(scripts, folder, w.conversion.className);
    if (unique != w.conversion.className) {
        w.notes.push_back("'" + w.conversion.className + "' already exists in Scripts/: the class was named '" + unique + "' (nothing is overwritten)");
        w.conversion = blueprint::renamed(w.conversion, text, job.options, unique);
        if (!w.conversion.ok) return Result<BlueprintWrite>::failure(w.conversion.report.error);
    }
    std::string prefix = folder.empty() ? "" : folder + "/";
    std::vector<std::pair<std::string, const std::string*>> files = {{prefix + w.conversion.cppFile, &w.conversion.cpp}};
    if (!w.conversion.headerFile.empty()) files.push_back({prefix + w.conversion.headerFile, &w.conversion.header});
    for (auto& [rel, content] : files) {
        auto full = resolveScriptPath(s.project(), rel);
        std::error_code ec;
        if (!full) return Result<BlueprintWrite>::failure(full.error());
        if (fs::exists(*full, ec)) return Result<BlueprintWrite>::failure("Scripts/" + rel + " already exists (existing files are never overwritten)");
    }
    for (auto& [rel, content] : files) {
        if (auto st = s.writeScriptFile(rel, *content, fromAi); !st) return Result<BlueprintWrite>::failure(st.error);
        w.files.push_back(rel);
    }
    // the report (Markdown + JSON) next to the build intermediates, never in Scripts/ (not source code)
    fs::path reports = s.project().rootDir() / "Intermediate" / "BlueprintConversions";
    std::error_code ec;
    fs::create_directories(osPath(reports), ec);
    blueprint::Report rep = w.conversion.report;
    rep.files.clear();
    for (auto& f : w.files) rep.files.push_back("Scripts/" + f);
    std::string base = w.conversion.className + ".blueprint-report";
    fs::path md = reports / pathFromUtf8(base + ".md"), js = reports / pathFromUtf8(base + ".json");
    for (int i = 2; (fs::exists(md, ec) || fs::exists(js, ec)) && i < 1000; ++i) {
        md = reports / pathFromUtf8(base + "_" + std::to_string(i) + ".md");
        js = reports / pathFromUtf8(base + "_" + std::to_string(i) + ".json");
    }
    if (writeFileText(md, rep.toMarkdown())) w.reportMarkdown = narrow(md.lexically_relative(s.project().rootDir()).generic_wstring());
    else w.notes.push_back("could not write the Markdown report");
    if (writeFileText(js, rep.toJson().dump(2))) w.reportJson = narrow(js.lexically_relative(s.project().rootDir()).generic_wstring());
    else w.notes.push_back("could not write the JSON report");
    w.conversion.report = rep;
    FLEDGE_LOG_INFO("blueprint", "converted Blueprint into Scripts/{} ({})", w.files.front(), rep.summary());
    return w;
}

// Dry runs of convert_blueprint are reads (engine_tools.cpp authorizes them like read-only tools).
bool isBlueprintDryRun(const Json& a) { return !(a.value("writeToProject", false) && !a.value("dryRun", false)); }

void registerBlueprintTools(mcp::ToolRegistry& reg, EditorSession& s) {
    Json schema = {{"type", "object"},
                   {"properties",
                    {{"text", {{"type", "string"}, {"description", "Blueprint nodes as T3D text (in the Unreal Blueprint editor: select nodes, Ctrl+C)"}}},
                     {"path", {{"type", "string"}, {"description", "absolute path of a .t3d / .txt / .copy text file, or a .uasset / .umap (probe only: not converted)"}}},
                     {"className", {{"type", "string"}, {"description", "C++ class name (default: the Blueprint or file name); made unique in Scripts/"}}},
                     {"folder", {{"type", "string"}, {"description", "Scripts/-relative folder for the generated files (default: Scripts/)"}}},
                     {"writeToProject", {{"type", "boolean"}, {"description", "write the .cpp (+ .h) into Scripts/ and the report into Intermediate/BlueprintConversions/ (default false)"}}},
                     {"dryRun", {{"type", "boolean"}, {"description", "only convert and return the code + report (default true unless writeToProject)"}}},
                     {"unrealSpace", {{"type", "boolean"}, {"description", "convert Unreal cm / X-forward Z-up to Fledge metres / -Z-forward Y-up where the Blueprint talks to the engine (default true)"}}},
                     {"includeCode", {{"type", "boolean"}, {"description", "return the generated code (default: true for dry runs)"}}}}},
                   {"additionalProperties", false}};
    reg.add({"convert_blueprint", "Convert an Unreal Blueprint",
             "Converts Unreal Engine Blueprint nodes (T3D clipboard text, or a .t3d file) into a Fledge Script C++ behaviour: events become "
             "lifecycle callbacks (BeginPlay -> OnStart, Tick -> OnUpdate, overlaps -> OnTriggerEnter, input events -> Input::Pressed / Axis), "
             "Branch / Sequence / loops / Switch become C++ control flow, Delay becomes a coroutine, variables become FLEDGE_PROPERTY, and nodes "
             "with no Fledge equivalent become compiling TODO(blueprint) stubs. Returns the code (dry run) and a conversion report (converted / "
             "approximated / stubbed per node, unrecognised constructs, variables, name map). writeToProject writes Scripts/<Class>.cpp without "
             "overwriting anything; then build_scripts and attach_script. .uasset files are only probed (not supported for conversion). "
             "Licence: Marketplace / Fab / Epic content may be licensed for Unreal Engine only; the user must have the right to convert it. "
             "Nothing is fetched from Epic and nothing in the input is executed.",
             schema, false, false, [&s](const Json& a) {
                 bool hasText = a.contains("text") && a["text"].is_string(), hasPath = a.contains("path") && a["path"].is_string();
                 if (hasText == hasPath) return ToolResult::error("pass exactly one of 'text' or 'path'");
                 bool write = !isBlueprintDryRun(a);
                 std::string cls = a.value("className", ""), folder = a.value("folder", "");
                 if (!cls.empty() && blueprint::sanitizeIdentifier(cls) != cls)
                     return ToolResult::error("className must be a C++ identifier (letters, digits, '_'), not a keyword or SDK name: try '" + blueprint::sanitizeIdentifier(cls) + "'");
                 if (!blueprint::isSafeSubfolder(folder)) return ToolResult::error("folder must be a relative folder inside Scripts/");
                 if (write && !s.hasProject()) return ToolResult::error(kNoProjectHelp);
                 std::string text, source = "pasted text";
                 BlueprintJob job = makeBlueprintJob(s, cls, "", a.value("unrealSpace", true));
                 if (hasPath) {
                     fs::path p = pathFromUtf8(a["path"].get<std::string>());
                     if (!p.is_absolute()) return ToolResult::error("path must be absolute");
                     if (!isBlueprintSourceFile(p)) return ToolResult::error("path must be a .t3d, .txt, .copy, .uasset or .umap file");
                     source = narrow(p.filename().wstring());
                     job.options.sourceName = source;
                     std::string e = toLower(narrow(p.extension().wstring()));
                     if (e == ".uasset" || e == ".umap") {
                         blueprint::Conversion c = blueprint::convertFile(p, job.options);
                         return ToolResult::json({{"ok", false}, {"written", false}, {"summary", c.report.summary()}, {"report", c.report.toJson()},
                                                  {"licence", blueprint::kLicenceNotice}});
                     }
                     auto t = blueprint::readT3DText(p, job.options.limits);
                     if (!t) return ToolResult::error(t.error());
                     text = std::move(*t);
                 } else {
                     text = a["text"].get<std::string>();
                     job.options.sourceName = source;
                 }
                 if (text.size() > job.options.limits.maxInputBytes)
                     return ToolResult::error("the Blueprint text is larger than the " + std::to_string(job.options.limits.maxInputBytes / (1024 * 1024)) + " MiB limit");
                 Json out;
                 blueprint::Conversion c;
                 if (write) {
                     auto r = writeBlueprintToProject(s, text, job, folder, true);
                     if (!r) {
                         blueprint::Conversion failed = blueprint::convertText(text, job.options);
                         if (!failed.ok) return ToolResult::error("not converted: " + failed.report.error);
                         return ToolResult::error(r.error());
                     }
                     c = r->conversion;
                     Json files = Json::array();
                     for (auto& f : r->files) files.push_back(f);
                     out["files"] = files;
                     out["reportMarkdown"] = r->reportMarkdown;
                     out["reportJson"] = r->reportJson;
                     out["notes"] = r->notes;
                     out["next"] = "build_scripts, then attach_script {entity, className: \"" + c.className + "\"}; review every TODO(blueprint)";
                 } else {
                     c = blueprint::convertText(text, job.options);
                     out["notes"] = job.notes;
                     if (c.ok) out["files"] = c.headerFile.empty() ? Json::array({c.cppFile}) : Json::array({c.cppFile, c.headerFile});
                 }
                 out["ok"] = c.ok;
                 out["written"] = write && c.ok;
                 out["className"] = c.className;
                 out["summary"] = c.report.summary();
                 out["counts"] = c.report.toJson()["counts"];
                 out["report"] = c.report.toJson();
                 out["licence"] = blueprint::kLicenceNotice;
                 if (c.ok && a.value("includeCode", !write)) {
                     const size_t cap = 200 * 1024;
                     out["code"] = {{"cpp", c.cpp.size() > cap ? c.cpp.substr(0, cap) + "\n// ... truncated ...\n" : c.cpp}, {"header", c.header},
                                    {"truncated", c.cpp.size() > cap}};
                 }
                 if (!c.ok) out["error"] = c.report.error;
                 return ToolResult::json(out);
             }});
}

} // namespace fledge
