// MCP tool migrate_settings (docs/MIGRATING_SETTINGS.md, docs/MCP.md):
//   migrate_settings {apply?, mode?, includeAiKeys?, includeEditionMode?}
// Copies the Fledge (Standard) editor settings into this Fledge Advanced editor's user data folder. A dry run (the default) is
// authorized like a read-only tool; apply: true is authorized like a destructive tool (it rewrites user settings). The folders
// come from the install (migrationEndpointsForThisProcess), never from the arguments. An AI never migrates AI permissions:
// the aiControl and mcp sections are always left out here (the Settings page can copy them).
#include "fledge/core/features.h"
#include "fledge/editor/editor_session.h"
#include "fledge/editor/settings_migration.h"
#include "fledge/mcp/mcp_server.h"

namespace fledge {

using mcp::ToolResult;

bool isMigrationApply(const Json& a) { return a.is_object() && a.value("apply", false); }

void registerMigrationTools(mcp::ToolRegistry& reg, EditorSession& s) {
    Json schema = {{"type", "object"},
                   {"properties",
                    {{"apply", {{"type", "boolean"}, {"description", "write the changes (default false: dry run that only reports what would happen)"}}},
                     {"mode", {{"type", "string"}, {"enum", {"merge", "overwrite"}}, {"description", "merge (default): only add what Fledge Advanced is missing; overwrite: Fledge's values win (changed files are backed up first)"}}},
                     {"includeAiKeys", {{"type", "boolean"}, {"description", "copy key-like values typed into the AI settings file (custom endpoint headers / body / URL parameters). Default true. Keys in Windows Credential Manager are shared by both editions and are never copied"}}},
                     {"includeEditionMode", {{"type", "boolean"}, {"description", "also copy the Simple / Pro Mode choice and current workspace (default false: Fledge Advanced keeps Pro Mode)"}}}}},
                   {"additionalProperties", false}};
    reg.add({"migrate_settings", "Migrate settings from Fledge",
             "Fledge Advanced only. Copies the user's Fledge (Standard) editor settings into Fledge Advanced: AI provider settings (custom "
             "endpoints, presets), UI preferences and theme, default projects folder, recent projects, auto-save, asset store import, script "
             "settings, window and workspace layouts, Code Editor settings. Logs, caches, downloads, crash dumps and edition-specific keys "
             "are never copied; the AI control / MCP permission sections are never migrated through this tool. Dry run by default (a "
             "per-item preview); apply: true writes, backing up every changed file to 'Migration Backups/<timestamp>/' first. The Fledge "
             "folder is never modified. Results list setting names only, never values.",
             schema, false, false, [&s](const Json& a) {
                 if (currentEdition() != Edition::Advanced)
                     return ToolResult::error("migrate_settings copies Fledge settings into Fledge Advanced and runs in Fledge Advanced; this is " +
                                              std::string(editionName(currentEdition())) + ".");
                 std::string mode = a.value("mode", "merge");
                 if (mode != "merge" && mode != "overwrite") return ToolResult::error("mode must be 'merge' or 'overwrite'");
                 editor::MigrationEndpoints e = editor::migrationEndpointsForThisProcess(s.customSettingsDir);
                 if (e.source.empty()) return ToolResult::error("no Fledge folder to migrate from: " + e.note);
                 editor::MigrationOptions o;
                 o.mode = mode == "overwrite" ? editor::MigrationMode::Overwrite : editor::MigrationMode::Merge;
                 o.includeAiKeys = a.value("includeAiKeys", true);
                 o.includeEditionMode = a.value("includeEditionMode", false);
                 o.includeAiPermissions = false;  // an AI never changes its own permissions this way
                 editor::MigrationPlan plan = editor::planSettingsMigration(e.source, e.destination, o);
                 Json out;
                 if (!isMigrationApply(a) || plan.refused) {
                     out = plan.toJson();
                     if (!plan.refused) out["next"] = "call migrate_settings again with apply: true to write these changes (needs the destructive permission)";
                 } else {
                     if (auto st = editor::checkMigrationEndpoints(plan, e); !st) return ToolResult::error(st.error);
                     editor::MigrationResult r = editor::runSettingsMigration(plan, o);
                     if (r.changesSomething() && s.onUserSettingsMigrated) s.onUserSettingsMigrated();
                     out = r.toJson();
                     out["next"] = s.onUserSettingsMigrated ? "the editor reloaded its settings" : "restart Fledge Advanced to use the migrated settings";
                 }
                 out["keysNotice"] = editor::kMigrationKeysNotice;
                 out["sourceNote"] = e.note;
                 if (plan.refused) return ToolResult::error(plan.summary());
                 return ToolResult::json(out);
             }});
}

} // namespace fledge
