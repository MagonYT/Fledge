// Full MCP Control in the editor window (fledge/editor/editor_control.h, docs/MCP.md "Full MCP Control"): the EditorControlHost
// behind list_editor_commands / run_editor_command / panels / windows / UI state / preferences / open_project, the per-session
// switch in Settings > MCP Server (warning + confirmation, never persisted).
#include <format>

#include <imgui.h>
#include <imgui_internal.h>

#include "editor_app.h"
#include "editor_icons.h"
#include "fledge/core/features.h"
#include "fledge/core/log.h"

namespace fledge::editor {

namespace {
const std::vector<std::string> kSettingsPages = {"Appearance", "Input", "Viewport", "Paths", "Migrate Settings", "Scripts", "Code Editor",
                                                 "Auto-save", "Asset Store Import", "AI Control", "AI Providers", "MCP Server"};
} // namespace

class EditorControlHostImpl final : public EditorControlHost {
public:
    explicit EditorControlHostImpl(EditorApp& app) : a(app) {}

    std::vector<EditorCommandInfo> commands() const override {
        std::vector<EditorCommandInfo> out;
        for (const Command& c : a.m_commands.all()) {
            if (a.m_commands.isHidden(c) || !c.run) continue;
            EditorCommandInfo i{c.id, c.title, c.category, CommandRegistry::chordName(c.shortcut), a.m_commands.isEnabled(c)};
            i.toggle = static_cast<bool>(c.checked);
            i.checked = c.checked && c.checked();
            out.push_back(std::move(i));
        }
        return out;
    }
    Status runCommand(const std::string& id) override {
        const Command* c = a.m_commands.find(id);
        if (!c || !c->run || a.m_commands.isHidden(*c)) return Status::failure("unknown editor command '" + id + "'");
        if (!a.m_commands.isEnabled(*c)) return Status::failure("'" + id + "' is disabled right now (e.g. it needs an open project)");
        a.m_commands.execute(id);
        return {};
    }
    std::vector<EditorPanelInfo> panels() const override {
        std::vector<EditorPanelInfo> out;
        for (auto& p : a.m_panels)
            if (!(p.aiFeature && !a.aiOn())) out.push_back({p.name, p.category, p.open, p.placeholder});
        return out;
    }
    Status setPanel(const std::string& name, std::optional<bool> open, bool focus) override {
        PanelDef* p = a.panel(name);
        if (!p || (p->aiFeature && !a.aiOn())) return Status::failure("unknown panel '" + name + "' (list_editor_panels)");
        if (open) p->open = *open;
        if (focus) {
            p->open = true;
            a.m_focusQueue.push_back(name);
        }
        return {};
    }
    std::vector<std::string> windows() const override {
        std::vector<std::string> w = {"settings", "shortcuts", "about", "project_settings", "palette", "welcome", "new_project", "ship", "perf"};
        if (currentEdition() == Edition::Advanced) w.push_back("migrate_settings");
        return w;
    }
    std::vector<std::string> settingsPages() const override { return kSettingsPages; }
    Status setWindow(const std::string& name, bool open, const std::string& page) override {
        if (name == "settings") {
            if (!page.empty()) {
                if (std::find(kSettingsPages.begin(), kSettingsPages.end(), page) == kSettingsPages.end())
                    return Status::failure("unknown Settings page '" + page + "'");
                a.m_settingsPage = page;
            }
            a.m_showSettings = open;
        } else if (name == "shortcuts") a.m_showShortcuts = open;
        else if (name == "about") a.m_showAbout = open;
        else if (name == "project_settings") {
            if (open && !a.m_session.hasProject()) return Status::failure("no project is open");
            a.m_showProjectSettings = open;
        } else if (name == "palette") {
            if (open) a.openPalette(false);
            else a.m_showPalette = false;
        } else if (name == "welcome" || name == "new_project") {
            a.m_showWelcome = open;
            if (open) a.m_welcomeTab = name == "new_project" ? 1 : 0;
        } else if (name == "ship") a.m_showShip = open;
        else if (name == "perf") {
            if (open && !a.m_session.hasProject()) return Status::failure("no project is open");
            a.m_showPerf = open;
        } else if (name == "migrate_settings" && currentEdition() == Edition::Advanced) {
            if (open) a.openMigrationWindow(false);
            else a.m_showMigration = false;
        } else {
            return Status::failure("unknown window '" + name + "' (list_editor_panels)");
        }
        return {};
    }
    Json state() const override {
        Json panels = Json::array(), wins = Json::array();
        for (auto& p : a.m_panels)
            if (p.open && !(p.aiFeature && !a.aiOn())) panels.push_back(p.name);
        auto win = [&](bool on, const char* n) {
            if (on) wins.push_back(n);
        };
        win(a.m_showSettings, "settings");
        win(a.m_showShortcuts, "shortcuts");
        win(a.m_showAbout, "about");
        win(a.m_showProjectSettings, "project_settings");
        win(a.m_showPalette, "palette");
        win(a.m_showWelcome, "welcome");
        win(a.m_showShip, "ship");
        win(a.m_showPerf, "perf");
        win(a.m_showMigration, "migrate_settings");
        ImGuiContext* g = ImGui::GetCurrentContext();
        std::string focused = g && g->NavWindow ? g->NavWindow->Name : "";
        return {{"workspace", a.m_prefs.workspace},
                {"simpleMode", a.m_prefs.simpleMode},
                {"edition", editionId(currentEdition())},
                {"openPanels", panels},
                {"openWindows", wins},
                {"focusedWindow", focused.substr(0, focused.find("###"))},
                {"settingsPage", a.m_settingsPage},
                {"mcpHttpRunning", a.m_mcpHttp.running()},
                {"fullMcpControl", a.m_session.ai.categoryAllowed(AiCategory::EditorControl)}};
    }
    Json uiTree(size_t maxWindows) const override {
        Json out = Json::array();
        ImGuiContext* g = ImGui::GetCurrentContext();
        if (g)
            for (ImGuiWindow* w : g->Windows) {
                if (!w->WasActive || (w->Flags & ImGuiWindowFlags_ChildWindow)) continue;
                if (out.size() >= maxWindows) break;
                std::string name = w->Name;
                out.push_back({{"name", name.substr(0, name.find("###"))}, {"pos", {w->Pos.x, w->Pos.y}}, {"size", {w->Size.x, w->Size.y}},
                               {"focused", g->NavWindow == w}, {"collapsed", static_cast<bool>(w->Collapsed)}, {"docked", static_cast<bool>(w->DockIsActive)}});
            }
        return {{"windows", out}, {"count", out.size()}};
    }
    Json preferences() const override {
        const EditorPrefs& p = a.m_prefs;
        return {{"theme", p.theme == Theme::Light ? "light" : "dark"}, {"uiScale", p.uiScale}, {"fontSize", p.fontSize},
                {"cameraSpeed", p.cameraSpeed}, {"mouseSensitivity", p.mouseSensitivity}, {"invertY", p.invertY},
                {"lockMouseInPlay", p.lockMouseInPlay}, {"showGrid", p.showGrid}, {"showStats", p.showStats},
                {"showOrientation", p.showOrientation}, {"showWelcome", p.showWelcomeOnStartup}, {"restoreLastProject", p.restoreLastProject}};
    }
    Status setPreference(const std::string& k, const Json& v) override {
        EditorPrefs& p = a.m_prefs;
        if (k == "theme") {
            p.theme = v == "light" ? Theme::Light : Theme::Dark;
            a.applyTheme();
        } else if (k == "uiScale") {
            p.uiScale = v.get<float>();
            a.m_fontsDirty = true;
        } else if (k == "fontSize") {
            p.fontSize = v.get<float>();
            a.m_fontsDirty = true;
        } else if (k == "cameraSpeed") p.cameraSpeed = v.get<float>();
        else if (k == "mouseSensitivity") p.mouseSensitivity = v.get<float>();
        else if (k == "invertY") p.invertY = v.get<bool>();
        else if (k == "lockMouseInPlay") p.lockMouseInPlay = v.get<bool>();
        else if (k == "showGrid") p.showGrid = v.get<bool>();
        else if (k == "showStats") p.showStats = v.get<bool>();
        else if (k == "showOrientation") p.showOrientation = v.get<bool>();
        else if (k == "showWelcome") p.showWelcomeOnStartup = v.get<bool>();
        else if (k == "restoreLastProject") p.restoreLastProject = v.get<bool>();
        else return Status::failure("unknown preference");
        a.saveSettings();
        return {};
    }
    std::vector<fs::path> projectRoots() const override {
        std::vector<fs::path> r = {a.m_prefs.projectsDir};
        for (auto& p : a.m_recent.items()) r.push_back(p.parent_path());
        return r;
    }
    Status openProject(const fs::path& file) override {
        if (a.m_session.playing()) return Status::failure("stop Play mode first");
        if (!a.openProject(file)) return Status::failure("the project could not be opened (see read_log)");
        return {};
    }

private:
    EditorApp& a;
};

void EditorApp::initEditorControl() {
    m_controlHost = std::make_unique<EditorControlHostImpl>(*this);
    m_session.controlHost = m_controlHost.get();
    m_session.auditLog = std::make_shared<McpAuditLog>(m_opt.settingsDir / "Logs");
}

void EditorApp::setFullMcpControl(bool on) {
    m_session.ai.setCategoryAllowed(AiCategory::EditorControl, on);
    m_fullControlConfirm = false;
    FLEDGE_LOG_WARN("mcp", "Full MCP Control turned {} by the user (audit log: {})", on ? "ON" : "off",
                    m_session.auditLog ? narrow(m_session.auditLog->currentFile().wstring()) : std::string("none"));
    if (ImGui::GetCurrentContext())
        toast(on ? ToastLevel::Warning : ToastLevel::Info, on ? "Full MCP Control is ON for this session" : "Full MCP Control is off");
}

void EditorApp::drawFullMcpControlSettings() {
    ImGui::SeparatorText("Full MCP Control");
    ImGui::PushTextWrapPos(0);
    bool on = m_session.ai.categoryAllowed(AiCategory::EditorControl);
    if (on) {
        ImGui::TextColored({1, 0.45f, 0.45f, 1}, "%s ON for this session: an AI may run any editor command, open / close panels and windows, "
                           "change the selection and editor preferences and open projects.", ICON_WARNING);
        if (ImGui::Button(ICON_XMARK "  Turn Full MCP Control off")) setFullMcpControl(false);
        trackRect("mcp.fullcontrol.off");
    } else if (!m_fullControlConfirm) {
        ImGui::TextDisabled("Off. Lets an AI drive the whole editor through MCP: every menu / palette command, panels, windows, selection, "
                            "UI state, editor preferences, opening projects. Lasts until you turn it off or close the editor.");
        if (ImGui::Button("Turn on Full MCP Control...")) m_fullControlConfirm = true;
        trackRect("mcp.fullcontrol.on");
    } else {
        ImGui::TextColored({1, 0.75f, 0.3f, 1}, "%s An AI connected to this editor could then do anything you can do from the menus. "
                           "Still never allowed: reading API or licence keys, changing AI permissions (control level, categories, this "
                           "switch), OS file dialogs. Deleting, exiting, reverting, building, packaging and starting external programs "
                           "need the AI to pass confirm: true. Every AI call is written to the audit log.", ICON_WARNING);
        if (ImGui::Button(ICON_CHECK "  I understand, turn it on for this session")) setFullMcpControl(true);
        trackRect("mcp.fullcontrol.confirm");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) m_fullControlConfirm = false;
    }
    if (m_session.auditLog) ImGui::TextDisabled("Audit log: %s", narrow(m_session.auditLog->currentFile().wstring()).c_str());
    ImGui::PopTextWrapPos();
}

} // namespace fledge::editor
