#pragma once
// Full MCP Control (1.2.4, docs/MCP.md "Full MCP Control"): MCP tools that drive the editor UI itself - the command registry
// (every menu item / palette command, input-free), panels and windows, selection, UI state, the ImGui window tree, editor
// preferences and opening a project. UI-free: the editor GUI implements EditorControlHost; the headless fledge_mcp_server has
// no host, so only select_entities works there. Tests use a fake host.
//
// Permission: the tools belong to the AI category "editorControl" (ai_control.h), OFF at every editor start, turned on only by
// the user in Settings > MCP Server (per session, never persisted, no flag / env var / file). Level Off refuses them, ask / auto
// ask first, full runs them. The existing levels and categories are unchanged.
//
// Guard rails that stay under Full MCP Control:
//   * no secret is ever read or returned (AI keys, licence keys): the tools expose names, ids and UI state only;
//   * AI-permission switches (ai.level.*, ai.toggle, mcp.toggle) and commands that open blocking OS dialogs are refused;
//   * destructive / external-program commands (Delete, Exit, Revert auto-save, Package, Build / Reload scripts, open IDE /
//     Explorer / terminal / game preview, restore link handlers) need `confirm: true` AND, where an existing category covers
//     them (packaging, script builds), that category too; open_project with unsaved changes needs `confirm: true`;
//   * paths: open_project only opens a .fledge inside the projects folder or a recent project's folder; no other tool takes a path;
//   * rate limit (kRateLimitCalls per kRateLimitSeconds for the editorControl tools together) and size caps on arguments and
//     results;
//   * every AI tool call (all tiers) is written to the audit log <user data>/Logs/mcp-audit-<date>.jsonl: time, caller (transport
//     + MCP session prefix), tool, arguments with secrets redacted, result status.
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/utility.h"
#include "fledge/editor/ai_control.h"

namespace fledge::editor {

struct EditorCommandInfo {
    std::string id, title, category, shortcut;
    bool enabled = true;
    bool toggle = false, checked = false;
};

struct EditorPanelInfo {
    std::string name, category;
    bool open = false, placeholder = false;
};

// Implemented by the editor GUI (EditorApp). All calls happen on the main thread between frames.
class EditorControlHost {
public:
    virtual ~EditorControlHost() = default;
    virtual std::vector<EditorCommandInfo> commands() const = 0;
    virtual Status runCommand(const std::string& id) = 0;  // the same code path as the menu / palette
    virtual std::vector<EditorPanelInfo> panels() const = 0;
    virtual Status setPanel(const std::string& name, std::optional<bool> open, bool focus) = 0;
    virtual std::vector<std::string> windows() const = 0;  // named dialogs: settings, shortcuts, about, ...
    virtual Status setWindow(const std::string& name, bool open, const std::string& page) = 0;
    virtual std::vector<std::string> settingsPages() const = 0;
    virtual Json state() const = 0;                         // workspace, mode, open windows, focus (no secrets)
    virtual Json uiTree(size_t maxWindows) const = 0;       // ImGui windows: name, position, size, focus, collapsed
    virtual Json preferences() const = 0;                   // the whitelisted keys only (editorPreferenceKeys)
    virtual Status setPreference(const std::string& key, const Json& value) = 0;  // value already validated
    virtual std::vector<fs::path> projectRoots() const = 0; // where open_project may open from
    virtual Status openProject(const fs::path& projectFile) = 0;
};

// ---- policy (pure, unit-tested) -------------------------------------------------------------------------------------
enum class CommandGate { Allowed, NeedsConfirm, Blocked };
struct EditorCommandPolicy {
    CommandGate gate = CommandGate::Allowed;
    std::string reason;
    std::optional<AiCategory> alsoNeeds;  // an existing category the command also requires (packaging, script builds)
};
EditorCommandPolicy editorCommandPolicy(const std::string& commandId);

// Editor preferences an AI may change (never AI provider / AI control / MCP settings, keys or paths).
const std::vector<std::string>& editorPreferenceKeys();
Status validateEditorPreference(const std::string& key, const Json& value);

// open_project: a .fledge file inside one of the roots (lexically, after normalisation; no '..' escapes).
Status checkProjectPath(const fs::path& file, const std::vector<fs::path>& roots);

// ---- limits ---------------------------------------------------------------------------------------------------------
constexpr size_t kRateLimitCalls = 30;
constexpr double kRateLimitSeconds = 10.0;
constexpr size_t kMaxArgsBytes = 16 * 1024;
constexpr size_t kMaxListItems = 1000;
constexpr size_t kMaxUiWindows = 200;

class RateLimiter {
public:
    RateLimiter(size_t calls, double seconds) : m_calls(calls), m_seconds(seconds) {}
    bool allow(double now);  // now: seconds (monotonic)
private:
    size_t m_calls;
    double m_seconds;
    std::deque<double> m_times;
    std::mutex m_mutex;
};

// ---- audit log ------------------------------------------------------------------------------------------------------
class McpAuditLog {
public:
    explicit McpAuditLog(fs::path dir) : m_dir(std::move(dir)) {}
    // One JSON line: {"time","caller","tool","category","args","ok","error"?}. Secrets in args / error are redacted.
    void record(const std::string& caller, const std::string& tool, const std::string& category, const Json& args, bool ok,
                const std::string& error = {});
    fs::path currentFile() const;
    static Json redactArgs(const Json& args);  // secret-looking keys -> "[REDACTED]", registered secrets masked, capped
    static constexpr uint64_t kMaxFileBytes = 16ull << 20;

private:
    fs::path m_dir;
    mutable std::mutex m_mutex;
};

} // namespace fledge::editor
