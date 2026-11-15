#pragma once
// AI master switch + AI control level (per-user settings; a project file can never change them).
//
//   enabled = false : every AI feature is off - no assistant, no MCP server (HTTP closed, stdio exits),
//                     no provider/network calls, every AI tool call refused.
//   level           : Off       - AI may only read (inspect the project/scene, read logs/docs)
//                     AskFirst  - every change needs the user's approval (Allow once / Allow for session)
//                     AutoSafe  - undoable scene/asset/script edits run automatically; risky categories ask
//                     Full      - everything permitted runs without asking
//   categories      : risky action kinds the AI may use at all (unchecked = refused at every level):
//                     destructive tools, script builds (native code), downloads, packaging.
//   editorControl   : Full MCP Control (1.2.4, docs/MCP.md "Full MCP Control"): the editor-UI tools (commands, panels, windows,
//                     selection, preferences, open project). OFF at every start; only the user turns it on, per session, in
//                     Settings > MCP Server. Never persisted, never set by a flag, env var, settings file or tool.
//
// Secrets (API keys, tokens) are never reachable by any tool at any level. Every AI action is
// recorded in the undo history tagged [AI].
//
// Thread-safety: authorize()/needsApproval()/grant*() may be called from any thread.
#include <mutex>
#include <set>
#include <string>

#include "fledge/core/json.h"
#include "fledge/mcp/mcp_server.h"

namespace fledge {

enum class AiLevel { Off, AskFirst, AutoSafe, Full };
enum class AiCategory { Read, Safe, Destructive, ScriptBuild, Download, Packaging, EditorControl };
const char* toString(AiLevel l);
const char* toString(AiCategory c);
AiLevel aiLevelFromString(std::string_view s);  // "off" | "ask" | "auto" | "full" (also the enum names)

// Tools of the editorControl tier (list_editor_commands, run_editor_command, ...).
bool isEditorControlTool(const std::string& name);
// Category of a tool (by annotations and name).
AiCategory aiCategoryOf(const mcp::Tool& tool);

struct AiDecision {
    bool allowed = false;
    bool needsApproval = false;  // allowed only after the user approves (interactive hosts)
    std::string reason;          // human readable when refused / when asking
};

class AiController {
public:
    // ---- settings (per user)
    bool enabled() const;
    void setEnabled(bool on);
    AiLevel level() const;
    void setLevel(AiLevel l);
    bool categoryAllowed(AiCategory c) const;  // Read/Safe always true
    void setCategoryAllowed(AiCategory c, bool allowed);
    Json toJson() const;
    void fromJson(const Json& j);  // missing keys keep defaults

    // ---- provider summary (set by the host; reported by list_capabilities). Never contains keys; not persisted.
    void setProviderInfo(Json info) {
        std::lock_guard l(m_mutex);
        m_providerInfo = std::move(info);
    }
    Json providerInfo() const {
        std::lock_guard l(m_mutex);
        return m_providerInfo;
    }

    // ---- decisions
    // What would happen if the AI called this tool now (does not consume approvals).
    AiDecision evaluate(const mcp::Tool& tool) const;
    // Final check right before a tool runs: consumes a one-time approval if one was granted.
    AiDecision authorize(const mcp::Tool& tool);
    // Interactive hosts: after the user approved evaluate().needsApproval.
    void grantOnce(const std::string& toolName);
    void grantForSession(const std::string& toolName);
    void clearSessionGrants();

    // Mirror into the MCP tool policy (which tools are listed / callable at all).
    void applyTo(mcp::ToolPolicy& policy) const;

private:
    mutable std::mutex m_mutex;
    Json m_providerInfo;  // null = no provider ready
    bool m_enabled = true;
    AiLevel m_level = AiLevel::AutoSafe;
    bool m_destructive = false, m_scriptBuilds = false, m_downloads = false, m_packaging = true;
    bool m_editorControl = false;  // session only (see editorControl above)
    std::multiset<std::string> m_once;
    std::set<std::string> m_session;
};

} // namespace fledge
