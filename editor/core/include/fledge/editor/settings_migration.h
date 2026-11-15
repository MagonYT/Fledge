#pragma once
// Settings migration: Fledge (Standard) -> Fledge Advanced (docs/MIGRATING_SETTINGS.md). UI-free; the editor's Settings page,
// the MCP tool migrate_settings, `Fledge --migrate-settings-from-standard` and the tests share it.
//
//   * planSettingsMigration(src, dst) is a dry run: what would be copied, merged, overwritten, skipped or excluded, per item.
//     runSettingsMigration(plan, options) re-reads both folders and applies the same rules.
//   * Copied: editor_settings.json (AI provider settings incl. custom endpoints and presets, AI control, MCP, UI preferences,
//     theme, default projects folder, recent projects, auto-save, asset store import, scripts, open panels), the window layout
//     (editor_layout.ini), saved workspace layouts (layouts/), Code Editor settings (code_editor.json).
//   * Excluded: Logs, crash dumps, Toolchains, StoreDownloads, Upscalers, Games (players' data), ipc, Intermediate / caches,
//     earlier Migration Backups, and anything this version does not know (reported, never copied).
//   * Edition-specific keys keep the destination's value: ui.simpleMode and ui.workspace (Simple / Pro Mode: Fledge starts in
//     Simple Mode, Fledge Advanced in Pro Mode) unless includeEditionMode, ui.migrationOffer, storeImport.linkIpc.
//   * Safety: the source is only read. A destination file that changes is first copied to <dst>/Migration Backups/<timestamp>/.
//     Writes are atomic (temp file + rename). Merge (default) only adds what is missing; Overwrite lets the source win.
//     Running twice changes nothing the second time. Symlinks / junctions are skipped, sizes are capped, src == dst or nested
//     folders are refused. Reports and logs carry setting names, never values.
//   * AI keys: provider keys live in Windows Credential Manager ("Fledge/llm/<id>"), which both editions already share for the
//     same Windows user, so nothing is copied for them. includeAiKeys covers key-like values typed into the plain settings
//     file (custom endpoint extra headers / extra body fields / URL query parameters whose names look secret).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/paths.h"
#include "fledge/core/utility.h"

namespace fledge::editor {

enum class MigrationMode { Merge, Overwrite };
const char* toString(MigrationMode m);  // "merge" | "overwrite"

enum class MigrationAction {
    Copy,       // new in the destination
    Merge,      // settings keys added to an existing destination file
    Overwrite,  // destination file replaced / keys replaced by the source's (backed up first)
    Unchanged,  // already the same (or nothing missing in merge mode)
    Skip,       // not copied: invalid, too large, a link, kept by merge mode, unknown item
    Exclude,    // never migrated (logs, caches, machine- or edition-specific)
    Error       // could not be read / written
};
const char* toString(MigrationAction a);

struct MigrationOptions {
    MigrationMode mode = MigrationMode::Merge;
    bool includeAiKeys = true;           // key-like values in the plain AI settings (see above); default ON, clearly labelled in the UI
    bool includeEditionMode = false;     // also copy the Simple / Pro Mode choice and current workspace
    bool includeAiPermissions = true;    // aiControl + mcp sections; forced off when an AI runs the migration (MCP)
    uint64_t maxFileBytes = 4ull << 20;  // per file
    uint64_t maxTotalBytes = 32ull << 20;
    size_t maxFiles = 500;
    // Backup folder name (tests inject a fixed one); default: local time "yyyy-mm-dd_hh-mm-ss".
    std::function<std::string()> timestamp;
};

struct MigrationItem {
    std::string path;      // relative to the user data folder, '/' separated ("editor_settings.json", "layouts/Level.ini", "Logs")
    std::string what;      // human readable ("Editor settings", "Workspace layout", "Log files")
    MigrationAction action = MigrationAction::Skip;
    std::string detail;    // why / what happened (names only, never setting values)
    uint64_t bytes = 0;
    std::vector<std::string> added, replaced, kept, withheld;  // settings key paths ("ui.theme", "ai.custom.extraHeaders[X-Api-Key]")
    Json toJson() const;
};

struct MigrationResult {
    fs::path source, destination;
    MigrationOptions options;
    bool applied = false;    // false: dry run / plan
    bool refused = false;    // nothing was attempted (see errors)
    std::vector<MigrationItem> items;
    std::vector<std::string> errors;
    std::vector<std::string> notes;
    fs::path backupDir;      // set when something was backed up
    int copied = 0, merged = 0, overwritten = 0, unchanged = 0, skipped = 0, excluded = 0, backedUp = 0, failed = 0;
    bool ok() const { return !refused && errors.empty(); }
    bool changesSomething() const { return copied + merged + overwritten > 0; }
    std::string summary() const;  // one line
    std::string text() const;     // multi-line report (CLI, UI copy button)
    Json toJson() const;
};
using MigrationPlan = MigrationResult;

// Dry run. Never writes anything.
MigrationPlan planSettingsMigration(const fs::path& source, const fs::path& destination, const MigrationOptions& options = {});
// Applies a plan (its source / destination; both folders are re-read, so a stale preview cannot write stale data).
MigrationResult runSettingsMigration(const MigrationPlan& plan, const MigrationOptions& options);
MigrationResult runSettingsMigration(const fs::path& source, const fs::path& destination, const MigrationOptions& options);

// Where this process migrates from and to. Never takes a path from the UI or an AI.
struct MigrationEndpoints {
    fs::path source;       // the Standard edition's user data folder (empty = not available here, see note)
    fs::path destination;  // this editor's user data folder (Fledge Advanced; <root>/UserData when portable; --settings-dir)
    std::string note;      // how the source was found, or why there is none
    bool sandboxed = false;  // build tree / FLEDGE_USER_DATA / --settings-dir: the real %APPDATA%\Fledge is never read
};
// Pure resolution. standardOverride: FLEDGE_STANDARD_USER_DATA (tests); userDataOverridden: FLEDGE_USER_DATA was set.
MigrationEndpoints resolveMigrationEndpoints(const EnginePaths& paths, const fs::path& customSettingsDir, const fs::path& appDataDir,
                                             const fs::path& standardOverride, bool userDataOverridden);
// From enginePaths(), platformAppDataDir() and the environment.
MigrationEndpoints migrationEndpointsForThisProcess(const fs::path& customSettingsDir = {});
// The plan must have exactly the endpoints' folders (refuses anything else).
Status checkMigrationEndpoints(const MigrationPlan& plan, const MigrationEndpoints& endpoints);
// Whether the first-launch offer applies: a source with editor settings and a destination without them yet.
bool migrationOfferApplies(const MigrationEndpoints& endpoints);

// Text for the confirmation dialog / docs about how API keys are handled (no values).
extern const char* const kMigrationKeysNotice;

} // namespace fledge::editor
