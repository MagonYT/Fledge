#include "fledge/editor/ai_control.h"

#include <format>

#include "fledge/core/utility.h"

namespace fledge {

const char* toString(AiLevel l) {
    switch (l) {
        case AiLevel::Off: return "off";
        case AiLevel::AskFirst: return "ask";
        case AiLevel::AutoSafe: return "auto";
        case AiLevel::Full: return "full";
    }
    return "auto";
}

const char* toString(AiCategory c) {
    switch (c) {
        case AiCategory::Read: return "read";
        case AiCategory::Safe: return "safe";
        case AiCategory::Destructive: return "destructive";
        case AiCategory::ScriptBuild: return "scriptBuilds";
        case AiCategory::Download: return "downloads";
        case AiCategory::Packaging: return "packaging";
        case AiCategory::EditorControl: return "editorControl";
    }
    return "safe";
}

AiLevel aiLevelFromString(std::string_view s) {
    std::string l = toLower(s);
    if (l == "off" || l == "readonly" || l == "read-only") return AiLevel::Off;
    if (l == "ask" || l == "askfirst" || l == "ask-first") return AiLevel::AskFirst;
    if (l == "full" || l == "fullcontrol") return AiLevel::Full;
    return AiLevel::AutoSafe;
}

bool isEditorControlTool(const std::string& name) {
    static const std::set<std::string> k = {"list_editor_commands", "run_editor_command", "list_editor_panels", "set_editor_panel",
                                            "set_editor_window", "get_editor_state", "get_editor_ui_tree", "select_entities",
                                            "get_editor_preferences", "set_editor_preference", "open_project"};
    return k.contains(name);
}

AiCategory aiCategoryOf(const mcp::Tool& t) {
    if (isEditorControlTool(t.name)) return AiCategory::EditorControl;  // Full MCP Control: even the read-only ones
    if (t.name == "build_scripts") return AiCategory::ScriptBuild;
    if (t.name == "install_upscaler_version" || t.name == "import_asset_url") return AiCategory::Download;  // requests a download
    if (t.name == "build_package" || t.name == "cook_assets") return AiCategory::Packaging;
    if (t.destructive) return AiCategory::Destructive;
    if (t.readOnly) return AiCategory::Read;
    return AiCategory::Safe;
}

bool AiController::enabled() const {
    std::lock_guard l(m_mutex);
    return m_enabled;
}
void AiController::setEnabled(bool on) {
    std::lock_guard l(m_mutex);
    m_enabled = on;
    if (!on) {
        m_once.clear();
        m_session.clear();
    }
}
AiLevel AiController::level() const {
    std::lock_guard l(m_mutex);
    return m_level;
}
void AiController::setLevel(AiLevel lv) {
    std::lock_guard l(m_mutex);
    m_level = lv;
}
bool AiController::categoryAllowed(AiCategory c) const {
    std::lock_guard l(m_mutex);
    switch (c) {
        case AiCategory::Destructive: return m_destructive;
        case AiCategory::ScriptBuild: return m_scriptBuilds;
        case AiCategory::Download: return m_downloads;
        case AiCategory::Packaging: return m_packaging;
        case AiCategory::EditorControl: return m_editorControl;
        default: return true;
    }
}
void AiController::setCategoryAllowed(AiCategory c, bool a) {
    std::lock_guard l(m_mutex);
    switch (c) {
        case AiCategory::Destructive: m_destructive = a; break;
        case AiCategory::ScriptBuild: m_scriptBuilds = a; break;
        case AiCategory::Download: m_downloads = a; break;
        case AiCategory::Packaging: m_packaging = a; break;
        case AiCategory::EditorControl: m_editorControl = a; break;  // session only: never in toJson / fromJson
        default: break;
    }
}

Json AiController::toJson() const {
    std::lock_guard l(m_mutex);
    return {{"enabled", m_enabled},
            {"level", toString(m_level)},
            {"allow", {{"destructive", m_destructive}, {"scriptBuilds", m_scriptBuilds}, {"downloads", m_downloads}, {"packaging", m_packaging}}}};
}

void AiController::fromJson(const Json& j) {
    if (!j.is_object()) return;
    std::lock_guard l(m_mutex);
    m_enabled = j.value("enabled", m_enabled);
    if (j.contains("level") && j["level"].is_string()) m_level = aiLevelFromString(j["level"].get<std::string>());
    if (j.contains("allow") && j["allow"].is_object()) {
        const Json& a = j["allow"];
        m_destructive = a.value("destructive", m_destructive);
        m_scriptBuilds = a.value("scriptBuilds", m_scriptBuilds);
        m_downloads = a.value("downloads", m_downloads);
        m_packaging = a.value("packaging", m_packaging);
    }
}

AiDecision AiController::evaluate(const mcp::Tool& tool) const {
    AiCategory cat = aiCategoryOf(tool);
    bool catAllowed = categoryAllowed(cat);
    std::lock_guard l(m_mutex);
    AiDecision d;
    if (!m_enabled) {
        d.reason = "AI features are turned off in this editor (Settings > AI > Enable AI features). No AI action can run.";
        return d;
    }
    if (cat == AiCategory::Read) {
        d.allowed = true;
        return d;
    }
    if (m_level == AiLevel::Off && cat == AiCategory::EditorControl) {
        d.reason = std::format("'{}' is a Full MCP Control tool; AI control level 'Off' refuses it (it also needs Settings > MCP Server > "
                               "Full MCP Control).", tool.name);
        return d;
    }
    if (m_level == AiLevel::Off) {
        d.reason = std::format("AI control level is 'Off' (read-only): '{}' changes the project. The user can raise the level in "
                               "Settings > AI control.", tool.name);
        return d;
    }
    if (!catAllowed) {
        d.reason = std::format("'{}' is a {} action, which the user has not allowed for AI (Settings > AI control > {}).", tool.name,
                               toString(cat), toString(cat));
        if (cat == AiCategory::EditorControl)
            d.reason = std::format("'{}' is a Full MCP Control tool (drives the editor UI). It is off by default and only the user can turn it on "
                                   "for this editor session in Settings > MCP Server > Full MCP Control (no flag or file enables it).", tool.name);
        if (cat == AiCategory::ScriptBuild)
            d.reason += " Scripts compile to native code that runs inside the editor, so a person must opt in (editor Settings > AI "
                        "control > Script builds, or fledge_mcp_server --allow-script-builds). Until then, ask the user to press Build.";
        return d;
    }
    bool risky = cat != AiCategory::Safe;
    bool ask = m_level == AiLevel::AskFirst || (m_level == AiLevel::AutoSafe && risky);
    if (ask && !m_session.contains(tool.name)) {
        d.needsApproval = true;
        d.reason = std::format("'{}' needs the user's approval (AI control level '{}'). Approve it in the editor's prompt; headless "
                               "servers refuse such calls (start fledge_mcp_server with a higher --ai-control level if intended).",
                               tool.name, toString(m_level));
        return d;
    }
    d.allowed = true;
    return d;
}

AiDecision AiController::authorize(const mcp::Tool& tool) {
    AiDecision d = evaluate(tool);
    if (d.allowed || !d.needsApproval) return d;
    std::lock_guard l(m_mutex);
    if (auto it = m_once.find(tool.name); it != m_once.end()) {
        m_once.erase(it);
        return {true, false, {}};
    }
    d.allowed = false;
    return d;
}

void AiController::grantOnce(const std::string& name) {
    std::lock_guard l(m_mutex);
    m_once.insert(name);
}
void AiController::grantForSession(const std::string& name) {
    std::lock_guard l(m_mutex);
    m_session.insert(name);
}
void AiController::clearSessionGrants() {
    std::lock_guard l(m_mutex);
    m_session.clear();
    m_once.clear();
}

void AiController::applyTo(mcp::ToolPolicy& policy) const {
    policy.allowDestructive = categoryAllowed(AiCategory::Destructive);
    policy.readOnlyMode = level() == AiLevel::Off || !enabled();
}

} // namespace fledge
