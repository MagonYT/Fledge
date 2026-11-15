// Editor shell: menus, toolbar, status bar, toasts, workspaces and layouts,
// the panel registry (with placeholder panels for upcoming milestones),
// background tasks, Output log, Settings window, Welcome screen + New Project wizard.
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <format>
#include <thread>

#include <GLFW/glfw3.h>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#endif

#include "fledge/core/legacy.h"
#include "fledge/scene/components.h"
#include "fledge/assets/image.h"
#include "fledge/core/log.h"
#include "fledge/core/version.h"
#include "editor_app.h"
#include "editor_code.h"
#include "editor_icons.h"
#include "fledge/editor/toolchain_installer.h"

namespace fledge::editor {

namespace {
double steadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

ImVec4 levelColor(LogLevel l) {
    switch (l) {
        case LogLevel::Error: return {1.0f, 0.42f, 0.42f, 1};
        case LogLevel::Warn: return {1.0f, 0.78f, 0.30f, 1};
        case LogLevel::Info: return ImGui::GetStyleColorVec4(ImGuiCol_Text);
        default: return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    }
}

#ifdef _WIN32
void setTaskbarProgress(HWND hwnd, float fraction, bool active, bool failed) {
    static ITaskbarList3* bar = nullptr;
    if (!hwnd) return;
    if (!bar) {
        HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        (void)init;
        if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList3, reinterpret_cast<void**>(&bar))) || !bar) return;
        bar->HrInit();
    }
    if (!active) {
        bar->SetProgressState(hwnd, TBPF_NOPROGRESS);
        return;
    }
    if (failed) {
        bar->SetProgressState(hwnd, TBPF_ERROR);
        return;
    }
    if (fraction < 0) {
        bar->SetProgressState(hwnd, TBPF_INDETERMINATE);
        return;
    }
    bar->SetProgressState(hwnd, TBPF_NORMAL);
    bar->SetProgressValue(hwnd, static_cast<ULONGLONG>(std::clamp(fraction, 0.0f, 1.0f) * 1000.0f), 1000);
}

void notifyFinished(HWND hwnd, bool ok, const std::string& text) {
    if (!hwnd) return;
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = hwnd;
    icon.uID = 0xF1ED;
    icon.uFlags = NIF_INFO | NIF_ICON | NIF_TIP;
    icon.hIcon = LoadIconW(nullptr, ok ? IDI_INFORMATION : IDI_ERROR);
    icon.dwInfoFlags = ok ? NIIF_INFO : NIIF_ERROR;
    wcscpy_s(icon.szInfoTitle, ok ? L"Fledge finished packaging" : L"Fledge packaging failed");
    std::wstring body = widen(text);
    if (body.size() > 240) body.resize(240);
    wcscpy_s(icon.szInfo, body.c_str());
    wcscpy_s(icon.szTip, L"Fledge");
    Shell_NotifyIconW(NIM_ADD, &icon);
    Shell_NotifyIconW(NIM_MODIFY, &icon);
    FLASHWINFO flash{};
    flash.cbSize = sizeof(flash);
    flash.hwnd = hwnd;
    flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
    flash.uCount = 8;
    FlashWindowEx(&flash);
    MessageBeep(ok ? MB_OK : MB_ICONHAND);
}

void scheduleShutdown() {
    std::wstring args = L"shutdown.exe /s /t 60 /c \"Fledge finished packaging. Shutting down in 60 seconds. Run shutdown /a to cancel.\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, args.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}
#endif

const char* levelIcon(ToastLevel l) {
    switch (l) {
        case ToastLevel::Success: return ICON_CHECK;
        case ToastLevel::Warning: return ICON_WARNING;
        case ToastLevel::Error: return ICON_ERROR;
        default: return ICON_INFO;
    }
}

ImVec4 toastColor(ToastLevel l) {
    switch (l) {
        case ToastLevel::Success: return {0.35f, 0.85f, 0.45f, 1};
        case ToastLevel::Warning: return {1.0f, 0.75f, 0.25f, 1};
        case ToastLevel::Error: return {1.0f, 0.40f, 0.40f, 1};
        default: return {0.45f, 0.70f, 1.0f, 1};
    }
}

// Small accent-coloured toggle button.
bool toggleButton(const char* label, bool on, const char* tooltip = nullptr) {
    if (on) {
        ImVec4 a = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(a.x, a.y, a.z, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(a.x, a.y, a.z, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.08f, 0.09f, 1));
    }
    bool pressed = ImGui::Button(label);
    if (on) ImGui::PopStyleColor(3);
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
    return pressed;
}
} // namespace

StatusSink& StatusSink::operator=(const std::string& text) {
    if (!app || text.empty()) return *this;
    std::string l = toLower(text);
    bool bad = l.find("fail") != std::string::npos || l.find("error") != std::string::npos || l.find("cannot") != std::string::npos ||
               l.find("not ") != std::string::npos || l.find("invalid") != std::string::npos;
    app->toast(bad ? ToastLevel::Warning : ToastLevel::Info, text);
    return *this;
}

void EditorApp::toast(ToastLevel level, std::string text) {
    if (level == ToastLevel::Error) FLEDGE_LOG_ERROR("editor", "{}", text);
    else if (level == ToastLevel::Warning) FLEDGE_LOG_WARN("editor", "{}", text);
    else FLEDGE_LOG_INFO("editor", "{}", text);
    m_toasts.push_back({level, std::move(text), ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0});
    while (m_toasts.size() > 6) m_toasts.pop_front();
}

void EditorApp::trackRect(const std::string& id) {
    if (!(m_tourStep >= 0 && startsWith(id, "simple."))) return;  // the tour points at Simple Mode items
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    m_uiRects[id] = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
}

// ------------------------------------------------------------------ panels --
PanelDef* EditorApp::panel(const std::string& name) {
    for (auto& p : m_panels)
        if (p.name == name) return &p;
    return nullptr;
}

void EditorApp::registerPanels() {
    m_panels = {
        {"Viewport", ICON_VIDEO, "Scene", [] {}},  // drawn by drawViewport (needs dt)
        {"Scene Hierarchy", ICON_LIST, "Scene", [this] { drawHierarchy(); }},
        {"Inspector", ICON_SLIDERS, "Scene", [this] { drawInspector(); }},
        {"Content Browser", ICON_FOLDER, "Assets", [this] { drawContentBrowser(); }},
        {"Material Editor", ICON_PALETTE, "Assets", [this] { drawMaterialEditor(); }},
        {"UI Editor", ICON_PALETTE, "Assets", [this] { drawUiEditor(); }},
        {"Render Settings", ICON_SUN, "Rendering", [this] { drawRenderSettings(); }},
        {"Graphics & Platform", ICON_GAUGE, "Rendering", [this] { drawGraphicsPanel(); }, false},
        {"Output", ICON_TERMINAL, "Tools", [this] { drawOutput(); }},
        {"History", ICON_HISTORY, "Tools", [this] { drawHistory(); }},
        {"AI Assistant", ICON_ROBOT, "AI", [this] { drawAssistant(); }, true, false, true},
        {"Script Editor", ICON_CODE, "Scripting", [this] { drawScriptEditor(); }, false},
        {"Build Output", ICON_BOLT, "Scripting", [this] { drawBuildOutput(); }, false},
        {"Code", ICON_CODE, "Code", [this] { drawCodePanel(); }, false},
        {"Simple", ICON_FEATHER, "Simple", [this] { drawSimplePanel(); }, false},
        {"Animation", ICON_FILM, "Animation", [this] { drawAnimationPanel(); }, false},
        {"World Streaming", ICON_MAP, "World", [this] { drawStreamingPanel(); }, false},
        {"Audio Mixer", ICON_SLIDERS, "Audio", [this] { drawAudioPanel(); }, false},
        {"Missions", ICON_MAP, "Gameplay", [this] { drawMissionsPanel(); }, false},
        {"Gameplay", ICON_CUBES, "Gameplay", [this] { drawGameplayPanel(); }, false},
        {"Asset Store Import", ICON_DOWNLOAD, "Assets", [this] { drawStoreImportPanel(); }, false},
        {"Convert Blueprint", ICON_CODE, "Scripting", [this] { drawBlueprintPanel(); }, false},
    };
    for (auto& p : m_panels) {
        std::string name = p.name;
        bool ai = p.aiFeature;
        m_commands.add({"panel." + toLower(name), "Show Panel: " + name, "Window", 0, p.icon,
                        [this, name] {
                            if (PanelDef* d = panel(name)) {
                                d->open = true;
                                ImGui::SetWindowFocus(name.c_str());
                            }
                        },
                        nullptr, [this, name] { PanelDef* d = panel(name); return d && d->open; }});
        if (ai) {  // registered last above
            Command c = *m_commands.find("panel." + toLower(name));
            c.aiFeature = true;
            m_commands.add(c);
        }
    }
    // AI-only commands disappear (menus, palette, shortcuts) while the master switch is off.
    for (const char* id : {"ai.assistant", "ai.new_chat", "ai.settings", "mcp.toggle", "view.workspace.ai"})
        if (const Command* c = m_commands.find(id)) {
            Command copy = *c;
            copy.aiFeature = true;
            m_commands.add(copy);
        }
    m_commands.hidden = [this](const Command& c) { return c.aiFeature && !aiOn(); };
}

void EditorApp::drawPanels(float dt) {
    std::vector<std::string> focus;
    focus.swap(m_focusQueue);
    for (auto& p : m_panels) {
        if (!p.open || (p.aiFeature && !aiOn())) continue;
        if (std::find(focus.begin(), focus.end(), p.name) != focus.end()) ImGui::SetNextWindowFocus();  // selects its dock tab
        if (p.name == "Viewport") {
            drawViewport(dt);
            continue;
        }
        p.draw();
    }
}

bool EditorApp::drawPlaceholderPanel(const char* title, const char* milestone, const std::vector<std::string>& planned) {
    PanelDef* p = panel(title);
    if (!ImGui::Begin(title, p ? &p->open : nullptr)) {
        ImGui::End();
        return false;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
    ImGui::TextUnformatted(ICON_HOURGLASS "  Placeholder");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("- arrives with the %s milestone", milestone);
    ImGui::Separator();
    ImGui::TextDisabled("Planned:");
    for (auto& s : planned) ImGui::BulletText("%s", s.c_str());
    return true;  // caller adds content and calls ImGui::End()
}



// -------------------------------------------------------------- workspaces --
namespace {
const std::vector<std::string>& workspaceNames() {
    static const std::vector<std::string> n = {"Level", "Scripting", "Code", "Animation", "AI"};
    return n;
}
std::vector<std::string> workspacePanels(const std::string& ws) {
    if (ws == "Scripting") return {"Viewport", "Scene Hierarchy", "Inspector", "Script Editor", "Build Output", "Output", "AI Assistant"};
    if (ws == "Code") return {"Code", "AI Assistant"};
    if (ws == "Simple") return {"Simple", "Viewport"};  // the AI panel is hidden entirely while AI is off
    if (ws == "Animation") return {"Viewport", "Scene Hierarchy", "Inspector", "Animation", "Content Browser"};
    if (ws == "AI") return {"Viewport", "Scene Hierarchy", "AI Assistant", "History", "Output", "Inspector"};
    return {"Viewport", "Scene Hierarchy", "Inspector", "Content Browser", "Output", "History", "AI Assistant", "Material Editor",
            "Render Settings", "Graphics & Platform"};
}
constexpr const char* kDockName = "FledgeDockSpaceV7";  // bump when default layouts change
} // namespace

void EditorApp::buildWorkspaceLayout(const std::string& ws, unsigned dockId) {
    for (auto& p : m_panels) p.open = false;
    for (auto& name : workspacePanels(ws))
        if (PanelDef* p = panel(name)) p->open = true;
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dockId;
    auto split = [&](ImGuiDir dir, float ratio) { return ImGui::DockBuilderSplitNode(center, dir, ratio, nullptr, &center); };
    if (ws == "Scripting") {
        ImGuiID left = split(ImGuiDir_Left, 0.16f);
        ImGuiID right = split(ImGuiDir_Right, 0.30f);
        ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.55f, nullptr, &right);
        ImGuiID bottom = split(ImGuiDir_Down, 0.28f);
        ImGui::DockBuilderDockWindow("Scene Hierarchy", left);
        ImGui::DockBuilderDockWindow("Viewport", right);
        ImGui::DockBuilderDockWindow("Inspector", rightBottom);
        ImGui::DockBuilderDockWindow("AI Assistant", rightBottom);
        ImGui::DockBuilderDockWindow("Build Output", bottom);
        ImGui::DockBuilderDockWindow("Output", bottom);
        ImGui::DockBuilderDockWindow("Script Editor", center);
    } else if (ws == "Simple") {
        ImGuiID left = split(ImGuiDir_Left, 0.30f);
        ImGui::DockBuilderDockWindow("Simple", left);
        ImGui::DockBuilderDockWindow("Viewport", center);
        if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(left)) n->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
        if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(center)) n->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
    } else if (ws == "Code") {
        ImGuiID right = split(ImGuiDir_Right, 0.26f);
        ImGui::DockBuilderDockWindow("AI Assistant", right);
        ImGui::DockBuilderDockWindow("Code", center);
    } else if (ws == "Animation") {
        ImGuiID bottom = split(ImGuiDir_Down, 0.34f);
        ImGuiID bottomRight = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.30f, nullptr, &bottom);
        ImGuiID left = split(ImGuiDir_Left, 0.17f);
        ImGuiID right = split(ImGuiDir_Right, 0.25f);
        ImGui::DockBuilderDockWindow("Animation", bottom);
        ImGui::DockBuilderDockWindow("Content Browser", bottomRight);
        ImGui::DockBuilderDockWindow("Scene Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Viewport", center);
    } else if (ws == "AI") {
        ImGuiID right = split(ImGuiDir_Right, 0.36f);
        ImGuiID left = split(ImGuiDir_Left, 0.17f);
        ImGuiID bottom = split(ImGuiDir_Down, 0.30f);
        ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.45f, nullptr, &left);
        ImGui::DockBuilderDockWindow("AI Assistant", right);
        ImGui::DockBuilderDockWindow("Scene Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", leftBottom);
        ImGui::DockBuilderDockWindow("History", bottom);
        ImGui::DockBuilderDockWindow("Output", bottom);
        ImGui::DockBuilderDockWindow("Viewport", center);
    } else {  // Level
        ImGuiID left = split(ImGuiDir_Left, 0.16f);
        ImGuiID right = split(ImGuiDir_Right, 0.27f);
        ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.48f, nullptr, &right);
        ImGuiID bottom = split(ImGuiDir_Down, 0.28f);
        ImGui::DockBuilderDockWindow("Scene Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Render Settings", right);
        ImGui::DockBuilderDockWindow("Graphics & Platform", right);
        ImGui::DockBuilderDockWindow("AI Assistant", rightBottom);
        ImGui::DockBuilderDockWindow("Material Editor", rightBottom);
        ImGui::DockBuilderDockWindow("Content Browser", bottom);
        ImGui::DockBuilderDockWindow("Output", bottom);
        ImGui::DockBuilderDockWindow("History", bottom);
        ImGui::DockBuilderDockWindow("Viewport", center);
    }
    // Front tabs of each dock node (the last docked window would win otherwise); Viewport last keeps keyboard focus there.
    if (ws == "Scripting") m_focusQueue = {"AI Assistant", "Inspector", "Build Output", "Viewport", "Script Editor"};
    else if (ws == "Animation") m_focusQueue = {"Animation", "Viewport"};
    else if (ws == "Code") m_focusQueue = {"AI Assistant", "Code"};
    else if (ws == "Simple") m_focusQueue = {"Viewport", "Simple"};
    else if (ws == "AI") m_focusQueue = {"History", "AI Assistant", "Viewport"};
    else m_focusQueue = {"Inspector", "Content Browser", "AI Assistant", "Viewport"};
    if (m_frontGraphics && ws == "Level") m_focusQueue.insert(m_focusQueue.end() - 1, "Graphics & Platform");
    // Panels that are not part of this workspace still get a home if opened later.
    for (auto& p : m_panels)
        if (!p.open && !ImGui::DockBuilderGetNode(dockId)) ImGui::DockBuilderDockWindow(p.name.c_str(), center);
    ImGui::DockBuilderFinish(dockId);
}

void EditorApp::switchWorkspace(const std::string& name) {
    if (name == "Simple") {
        setSimpleMode(true);
        return;
    }
    if (std::find(workspaceNames().begin(), workspaceNames().end(), name) == workspaceNames().end()) return;
    if (m_prefs.simpleMode) {  // picking a Pro workspace leaves Simple Mode
        m_prefs.simpleMode = false;
        m_tourStep = -1;
    }
    if (name == "AI" && !aiOn()) return;
    m_pendingWorkspace = name;
}

void EditorApp::resetLayout() { m_pendingReset = true; }

void EditorApp::saveLayout() {
    fs::path dir = m_opt.settingsDir / "layouts";
    std::error_code ec;
    fs::create_directories(dir, ec);
    size_t n = 0;
    const char* ini = ImGui::SaveIniSettingsToMemory(&n);
    Json open = Json::array();
    for (auto& p : m_panels)
        if (p.open) open.push_back(p.name);
    bool ok = writeFileText(dir / pathFromUtf8(m_prefs.workspace + ".ini"), std::string_view(ini, n)) &&
              writeFileText(dir / pathFromUtf8(m_prefs.workspace + ".panels.json"), open.dump());
    toast(ok ? ToastLevel::Success : ToastLevel::Error, ok ? "Layout saved for workspace " + m_prefs.workspace : "Cannot save the layout");
}

// Called before ImGui::NewFrame: font rebuilds and workspace switches (loading
// .ini settings mid-frame would confuse the docking system).
void EditorApp::beginFrameWork() {
    if (m_migrationReloadPending && !m_aiBusy) {  // a settings migration was applied (Settings page or MCP); never under a running assistant
        m_migrationReloadPending = false;
        applyMigratedSettings();
    }
    if (std::fabs(effectiveScale() - m_appliedScale) > 0.01f) m_fontsDirty = true;  // moved to another monitor / DPI change
    rebuildFonts();
    if (m_pendingWorkspace.empty()) return;
    std::string target = m_pendingWorkspace;
    m_pendingWorkspace.clear();
    if (target == m_prefs.workspace && !m_firstLayout) return;
    fs::path dir = m_opt.settingsDir / "layouts";
    auto ini = readFileText(dir / pathFromUtf8(target + ".ini"));
    auto open = readFileText(dir / pathFromUtf8(target + ".panels.json"));
    m_prefs.workspace = target;
    if (ini && open) {
        ImGui::LoadIniSettingsFromMemory(ini->c_str(), ini->size());
        Json j = Json::parse(*open, nullptr, false);
        for (auto& p : m_panels) p.open = false;
        if (j.is_array())
            for (auto& n : j)
                if (n.is_string())
                    if (PanelDef* p = panel(n.get<std::string>())) p->open = true;
    } else {
        m_pendingReset = true;
    }
}

// ----------------------------------------------------------------- menu bar --
void EditorApp::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    auto item = [this](const char* id, const char* labelOverride = nullptr) {
        const Command* c = m_commands.find(id);
        if (!c) return;
        std::string label = std::string(c->icon) + "  " + (labelOverride ? labelOverride : c->title.c_str());
        std::string key = CommandRegistry::chordName(c->shortcut);
        bool checked = c->checked && c->checked();
        if (ImGui::MenuItem(label.c_str(), key.empty() ? nullptr : key.c_str(), checked, m_commands.isEnabled(*c))) m_commands.execute(id);
    };
    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s", ICON_CUBE);
    if (ImGui::BeginMenu("File")) {
        item("file.new_project");
        item("file.open_project");
        if (ImGui::BeginMenu(ICON_CLOCK "  Open Recent", !m_recent.items().empty())) {
            for (const auto& p : m_recent.items()) {
                std::error_code ec;
                bool exists = fs::exists(p, ec);
                if (ImGui::MenuItem(narrow(p.wstring()).c_str(), nullptr, false, exists)) {
                    openProject(p);
                    break;
                }
            }
            ImGui::EndMenu();
        }
        item("file.welcome");
        ImGui::Separator();
        item("file.save");
        item("file.revert_autosave");
        item("file.cook");
        item("file.cook_force");
        item("file.package");
        item("project.settings");
        item("file.reveal_project");
        item("file.migrate_settings");  // Fledge Advanced only (the command is not registered in Fledge)
        ImGui::Separator();
        item("file.exit");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        auto& h = m_session.history();
        std::string undo = h.canUndo() ? "Undo " + h.commands()[h.cursor() - 1].label : "Undo";
        std::string redo = h.canRedo() ? "Redo " + h.commands()[h.cursor()].label : "Redo";
        item("edit.undo", undo.c_str());
        item("edit.redo", redo.c_str());
        ImGui::Separator();
        item("edit.duplicate");
        item("edit.delete");
        item("edit.select_none");
        item("edit.focus");
        ImGui::Separator();
        if (ImGui::BeginMenu(ICON_PLUS "  Create")) {
            item("edit.create_empty");
            item("edit.create_cube");
            item("edit.create_light");
            item("physics.create_car");
            item("physics.create_truck");
            item("physics.create_bike");
            ImGui::EndMenu();
        }
        item("physics.add");
        ImGui::Separator();
        item("tool.translate");
        item("tool.rotate");
        item("tool.scale");
        item("tool.space");
        item("tool.snap");
        ImGui::Separator();
        item("view.palette");
        item("edit.settings");
        item("ai.toggle");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::BeginMenu(ICON_TABLE "  Workspace")) {
            for (auto& ws : workspaceNames())
                if (ws != "AI" || aiOn()) item(("view.workspace." + toLower(ws)).c_str());
            ImGui::Separator();
            item("view.layout.save");
            item("view.layout.reset");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_PALETTE "  Theme")) {
            item("view.theme.dark");
            item("view.theme.light");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        item("view.grid");
        item("view.stats");
        item("view.orientation");
        if (ImGui::BeginMenu(ICON_EYE "  View Mode")) {
            for (int i = 0; i <= static_cast<int>(DebugView::Wireframe); ++i) item(std::format("view.mode.{}", i).c_str());
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_VIDEO "  Camera")) {
            for (const char* c : {"top", "bottom", "front", "back", "right", "left"}) item((std::string("view.camera.") + c).c_str());
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Graphics")) {
        item("graphics.panel");
        if (ImGui::BeginMenu(ICON_WAND "  Quality Preset")) {
            for (const char* p : {"low", "medium", "high", "ultra", "everything"}) item((std::string("graphics.preset.") + p).c_str());
            ImGui::EndMenu();
        }
        item("graphics.auto_tune");
        ImGui::Separator();
        item("graphics.preview");
        item("graphics.path_trace");
        ImGui::Separator();
        item("project.settings");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        std::string cat;
        for (auto& p : m_panels) {
            if (p.aiFeature && !aiOn()) continue;
            if (p.category != cat) {
                if (!cat.empty()) ImGui::Separator();
                cat = p.category;
            }
            std::string label = std::string(p.icon) + "  " + p.name + (p.placeholder ? "  (placeholder)" : "");
            if (ImGui::MenuItem(label.c_str(), nullptr, p.open)) p.open = !p.open;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Play")) {
        item("play.play");
        item("play.pause");
        item("play.step");
        item("play.stop");
        ImGui::Separator();
        item("play.host");
        item("play.join");
        ImGui::Separator();
        item("physics.simulate");
        item("physics.simulate_keep");
        item("physics.debug");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Scripts")) {
        item("scripts.build");
        item("scripts.rebuild");
        item("scripts.reload");
        ImGui::Separator();
        item("scripts.new");
        item("scripts.convert_blueprint");
        item("scripts.open_ide");
        item("help.script");
        ImGui::EndMenu();
    }
    if (aiOn() && ImGui::BeginMenu("AI")) {
        item("ai.toggle");
        if (ImGui::BeginMenu(ICON_SHIELD "  Control Level")) {
            for (const char* l : {"off", "ask", "auto", "full"}) item((std::string("ai.level.") + l).c_str());
            ImGui::EndMenu();
        }
        ImGui::Separator();
        item("ai.assistant");
        item("ai.new_chat");
        item("ai.settings");
        ImGui::Separator();
        item("mcp.toggle");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        item("help.shortcuts");
        item("help.docs");
        item("help.licenses");
        item("help.userdata");
        ImGui::Separator();
        item("help.about");
        ImGui::EndMenu();
    }
    // Right side: project + palette hint
    std::string proj = m_session.hasProject() ? m_session.project().name + (m_session.dirty ? "  *" : "") : "no project";
    std::string hint = std::string(ICON_SEARCH) + "  Ctrl+P";
    float w = ImGui::CalcTextSize(proj.c_str()).x + ImGui::CalcTextSize(hint.c_str()).x + 60;
    ImGui::SameLine(ImGui::GetWindowWidth() - w);
    ImGui::TextDisabled("%s", proj.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(hint.c_str())) openPalette(false);
    ImGui::EndMainMenuBar();
}

// ------------------------------------------------------------------ toolbar --
void EditorApp::drawToolbar() {
    float s = effectiveScale();
    float h = ImGui::GetFrameHeight() + 10 * s;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8 * s, 5 * s});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar * 0;
    if (ImGui::BeginViewportSideBar("##Toolbar", ImGui::GetMainViewport(), ImGuiDir_Up, h, flags)) {
        // workspaces (left)
        for (auto& ws : workspaceNames()) {
            if (ws == "AI" && !aiOn()) continue;
            const Command* c = m_commands.find("view.workspace." + toLower(ws));
            std::string label = std::string(c ? c->icon : "") + " " + ws;
            if (toggleButton(label.c_str(), m_prefs.workspace == ws, (ws + " workspace (" + CommandRegistry::chordName(c->shortcut) + ")").c_str()))
                switchWorkspace(ws);
            trackRect("ws." + ws);
            ImGui::SameLine(0, 2 * s);
        }
        ImGui::SameLine(0, 16 * s);
        // transform tools
        auto tool = [&](const char* id, const char* icon, bool on, const char* tip) {
            if (toggleButton(icon, on, tip)) m_commands.execute(id);
            ImGui::SameLine(0, 2 * s);
        };
        tool("tool.translate", ICON_MOVE, m_gizmo.mode == GizmoMode::Translate, "Move (W)");
        tool("tool.rotate", ICON_ROTATE, m_gizmo.mode == GizmoMode::Rotate, "Rotate (E)");
        tool("tool.scale", ICON_SCALE, m_gizmo.mode == GizmoMode::Scale, "Scale (R)");
        ImGui::SameLine(0, 6 * s);
        tool("tool.space", m_gizmo.space == GizmoSpace::Local ? ICON_CUBE " Local" : ICON_GLOBE " World", false, "Gizmo space (L)");
        tool("tool.snap", ICON_MAGNET, m_gizmo.snap, "Snapping (Ctrl+G)");
        if (ImGui::ArrowButton("##snapopts", ImGuiDir_Down)) ImGui::OpenPopup("snapopts");
        if (ImGui::BeginPopup("snapopts")) {
            ImGui::SetNextItemWidth(110 * s);
            ImGui::DragFloat("Move step", &m_gizmo.translateSnap, 0.05f, 0.01f, 100.0f, "%.2f m");
            ImGui::SetNextItemWidth(110 * s);
            ImGui::DragFloat("Rotate step", &m_gizmo.rotateSnapDeg, 1.0f, 1.0f, 90.0f, "%.0f deg");
            ImGui::SetNextItemWidth(110 * s);
            ImGui::DragFloat("Scale step", &m_gizmo.scaleSnap, 0.01f, 0.01f, 10.0f, "%.2f");
            ImGui::EndPopup();
        }
        // play controls (centered)
        float playW = ImGui::GetFrameHeight() * 6 + 10 * s;
        float centerX = (ImGui::GetWindowWidth() - playW) * 0.5f;
        if (ImGui::GetCursorPosX() < centerX) ImGui::SameLine(centerX);
        else ImGui::SameLine(0, 16 * s);
        bool playing = m_session.playing(), paused = m_session.paused();
        ImGui::BeginDisabled(!m_session.hasProject());
        if (!playing || paused) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.85f, 0.45f, 1));
            if (ImGui::Button(ICON_PLAY)) m_commands.execute("play.play");
            ImGui::PopStyleColor();
        } else if (toggleButton(ICON_PLAY, true, "Playing")) {
        }
        trackRect("tb.play");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Play (F5)");
        ImGui::SameLine(0, 2 * s);
        ImGui::BeginDisabled(!playing);
        if (toggleButton(ICON_PAUSE, paused, "Pause / resume (F6)")) m_commands.execute("play.pause");
        trackRect("tb.pause");
        ImGui::SameLine(0, 2 * s);
        ImGui::BeginDisabled(!paused);
        if (ImGui::Button(ICON_STEP)) m_commands.execute("play.step");
        trackRect("tb.step");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Step one frame (F10)");
        ImGui::EndDisabled();
        ImGui::SameLine(0, 2 * s);
        ImGui::PushStyleColor(ImGuiCol_Text, playing ? ImVec4(1, 0.45f, 0.45f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Button(ICON_STOP)) m_commands.execute("play.stop");
        ImGui::PopStyleColor();
        trackRect("tb.stop");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Stop (Shift+F5)");
        ImGui::EndDisabled();
        ImGui::SameLine(0, 8 * s);
        ImGui::BeginDisabled(!m_session.hasProject() || playing);
        if (ImGui::Button(ICON_SERVER)) m_commands.execute("play.host");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Host a LAN game on UDP %d (WASD, mouse look, Space jump)", m_hostPort);
        ImGui::SameLine(0, 2 * s);
        if (ImGui::Button(ICON_LINK)) m_commands.execute("play.join");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Join a host (IP:port)");
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        // right side: save / cook / package / settings
        float rightW = ImGui::GetFrameHeight() * 5 + ImGui::CalcTextSize("Package").x + ImGui::CalcTextSize("Simple").x + 56 * s;
        ImGui::SameLine(ImGui::GetWindowWidth() - rightW);
        if (ImGui::Button(ICON_FEATHER " Simple")) setSimpleMode(true);
        trackRect("toolbar.simple");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Simple Mode: Describe, Play, Share (Ctrl+Shift+M)");
        ImGui::SameLine(0, 8 * s);
        ImGui::BeginDisabled(!m_session.hasProject() || playing);
        if (ImGui::Button(ICON_SAVE)) m_commands.execute("file.save");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Save scene (Ctrl+S)");
        ImGui::SameLine(0, 2 * s);
        ImGui::BeginDisabled(m_cookRunning);
        if (ImGui::Button(ICON_FIRE)) m_commands.execute("file.cook");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Cook content in the background (incremental)");
        ImGui::EndDisabled();
        ImGui::SameLine(0, 2 * s);
        ImGui::BeginDisabled(m_packageRunning);
        if (ImGui::Button(ICON_BOX " Package")) m_commands.execute("file.package");
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine(0, 2 * s);
        if (ImGui::Button(ICON_GEAR)) m_showSettings = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Settings (Ctrl+,)");
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// --------------------------------------------------------------- status bar --
void EditorApp::drawStatusBar() {
    float s = effectiveScale();
    float h = ImGui::GetFrameHeight();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8 * s, 2 * s});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
    if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, h, ImGuiWindowFlags_NoSavedSettings)) {
#ifdef _WIN32
        {
            HWND hwnd = m_window ? static_cast<HWND>(m_window->nativeHandle()) : nullptr;
            float shown = -1;
            bool active = false;
            std::lock_guard lock(m_taskMutex);
            for (auto& task : m_tasks)
                if (!task->done) {
                    active = true;
                    shown = task->progress;
                }
            setTaskbarProgress(hwnd, shown, active, false);
        }
#endif
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s %s  %s", editionName(currentEdition()), FLEDGE_VERSION_STRING,
                            m_device ? rhi::toString(m_device->backend()) : "");  // edition first (docs/EDITIONS.md)
        if (!m_session.netStatus().empty()) {
            ImGui::SameLine(0, 14 * s);
            ImGui::Text("%s %s", ICON_SERVER, m_session.netStatus().c_str());
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", currentEdition() == Edition::Advanced ? "Fledge Advanced: the full edition (Pro Mode by default)"
                                                                         : "Fledge: the lean edition (Simple Mode by default)");
        ImGui::SameLine(0, 14 * s);
        if (m_session.ai.categoryAllowed(AiCategory::EditorControl)) {  // Full MCP Control is on: always visible, one click revokes it
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.15f, 0.15f, 1));
            if (ImGui::SmallButton(ICON_WARNING " FULL MCP CONTROL  " ICON_XMARK)) setFullMcpControl(false);
            ImGui::PopStyleColor();
            trackRect("status.fullcontrol");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("An AI may drive the whole editor (menus, panels, windows, preferences). Click to turn it off.");
            ImGui::SameLine(0, 14 * s);
        }
        // play state
        if (m_session.playing()) {
            ImGui::TextColored(m_session.paused() ? ImVec4(1, 0.78f, 0.3f, 1) : ImVec4(0.35f, 0.85f, 0.45f, 1), "%s %s",
                               m_session.paused() ? ICON_PAUSE : ICON_PLAY, m_session.paused() ? "PAUSED" : "PLAYING");
        } else {
            ImGui::TextDisabled("%s Edit", ICON_CUBES);
        }
        ImGui::SameLine(0, 14 * s);
        size_t ents = m_session.scene().entities().size();
        ImGui::TextDisabled("%s %zu entities   %zu selected", ICON_LIST, ents, m_session.selection.size());
        if (!m_autoSavedLabel.empty()) {
            ImGui::SameLine(0, 18 * s);
            ImGui::TextDisabled("%s %s", ICON_SAVE, m_autoSavedLabel.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The scene was saved automatically (Settings > Auto-save).\nFile > Revert Last Auto-save loads the file as it was before.");
            trackRect("status.autosave");
        }
        // background tasks
        {
            std::lock_guard l(m_taskMutex);
            for (auto& t : m_tasks) {
                if (t->done) continue;
                ImGui::SameLine(0, 18 * s);
                float p = t->progress;
                std::string label = t->detail.empty() ? t->name : t->detail;
                if (p >= 0) label = std::format("{:.0f}%  {}", p * 100.0f, label);
                if (label.size() > 88) label.replace(85, std::string::npos, "...");
                ImGui::SetNextItemWidth(420 * s);
                if (p < 0) ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), {420 * s, h * 0.7f}, label.c_str());
                else ImGui::ProgressBar(std::clamp(p, 0.0f, 1.0f), {420 * s, h * 0.7f}, label.c_str());
                if (ImGui::IsItemHovered() && !t->detail.empty()) ImGui::SetTooltip("%s", t->detail.c_str());
            }
        }
        // script build state
        if (m_session.hasProject() && (m_session.project().hasScripts() || m_session.hasScriptBuild())) {
            ImGui::SameLine(0, 18 * s);
            const auto& b = m_session.lastScriptBuild();
            if (m_scriptBuildRunning) ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s Scripts: building", ICON_BOLT);
            else if (!m_session.hasScriptBuild()) ImGui::TextDisabled("%s Scripts: not built", ICON_BOLT);
            else if (b.ok) ImGui::TextColored({0.35f, 0.85f, 0.45f, 1}, "%s Scripts OK (%.1fs)", ICON_BOLT, b.seconds);
            else ImGui::TextColored({1, 0.45f, 0.45f, 1}, "%s Scripts: %d error%s", ICON_BOLT, b.errors(), b.errors() == 1 ? "" : "s");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fledge Script files - click for Build Output");
            if (ImGui::IsItemClicked()) m_commands.execute("panel.build output");
            trackRect("status.scripts");
        }
        // right side: AI, MCP, perf
        std::string ai, key, mcp;
        ImVec4 aiCol, mcpCol;
        const char* keyState = m_keySource == ai::KeySource::Store ? "key: stored" : m_keySource == ai::KeySource::Environment ? "key: env" : "no key";
        bool needsKey = m_aiConfig.needsKey();
        if (m_aiBusy) {
            ai = std::string(ICON_ROBOT) + " AI working...";
            aiCol = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        } else if (m_provider) {
            ai = std::string(ICON_ROBOT) + " " + m_provider->name() + (needsKey ? std::string("  ") + keyState : "");
            aiCol = {0.35f, 0.85f, 0.45f, 1};
        } else {
            ai = std::string(ICON_ROBOT) + " AI: not configured";
            aiCol = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        }
        double since = steadySeconds() - m_mcpLastRequest.load();
        if (!m_mcpHttp.running()) {
            mcp = std::string(ICON_SERVER) + " MCP: off";
            mcpCol = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        } else if (since < 5.0) {
            mcp = std::format("{} MCP: client active :{} ({} req)", ICON_SERVER, m_mcpHttp.port(), m_mcpRequests.load());
            mcpCol = {0.35f, 0.85f, 0.45f, 1};
        } else {
            mcp = std::format("{} MCP: listening :{}", ICON_SERVER, m_mcpHttp.port());
            mcpCol = {0.45f, 0.70f, 1.0f, 1};
        }
        if (!aiOn()) {
            ai = std::string(ICON_ROBOT) + " AI off";
            aiCol = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            mcp.clear();
        } else {
            ai += std::string("  [") + toString(m_session.ai.level()) + "]";
        }
        std::string perf = std::format("{:.0f} FPS  {:.1f} ms", ImGui::GetIO().Framerate, m_frameMs);
        float w = ImGui::CalcTextSize(ai.c_str()).x + ImGui::CalcTextSize(mcp.c_str()).x + ImGui::CalcTextSize(perf.c_str()).x + 60 * s;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20, ImGui::GetWindowWidth() - w));
        ImGui::TextColored(aiCol, "%s", ai.c_str());
        trackRect("status.ai");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", aiOn() ? "AI provider, key source (never the key) and control level - click for AI settings"
                                           : "AI features are off - click to turn them on");
        if (ImGui::IsItemClicked()) {
            if (!aiOn()) setAiEnabled(true);
            else {
                m_showSettings = true;
                m_settingsPage = "AI Control";
            }
        }
        if (!mcp.empty()) {
            ImGui::SameLine(0, 16 * s);
            ImGui::TextColored(mcpCol, "%s", mcp.c_str());
            if (ImGui::IsItemClicked()) {
                m_showSettings = true;
                m_settingsPage = "MCP Server";
            }
        }
        ImGui::SameLine(0, 16 * s);
        ImGui::TextDisabled("%s", perf.c_str());
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void EditorApp::drawToasts() {
    double now = ImGui::GetTime();
    float s = effectiveScale();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float y = vp->WorkPos.y + vp->WorkSize.y - 12 * s;
    int i = 0;
    for (auto it = m_toasts.rbegin(); it != m_toasts.rend(); ++it, ++i) {
        double life = it->level == ToastLevel::Error ? 7.0 : 4.0;
        double age = now - it->time;
        if (age > life) continue;
        float alpha = static_cast<float>(std::clamp((life - age) / 0.5, 0.0, 1.0) * std::clamp(age / 0.15, 0.0, 1.0));
        ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x - 14 * s, y}, ImGuiCond_Always, {1, 1});
        ImGui::SetNextWindowBgAlpha(0.95f * alpha);
        ImGui::SetNextWindowSizeConstraints({200 * s, 0}, {440 * s, 400 * s});
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * s);
        std::string id = std::format("##toast{}", i);
        ImGui::Begin(id.c_str(), nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoInputs);
        ImGui::TextColored(toastColor(it->level), "%s", levelIcon(it->level));
        ImGui::SameLine();
        ImGui::PushTextWrapPos(400 * s);
        ImGui::TextUnformatted(it->text.c_str());
        ImGui::PopTextWrapPos();
        y -= ImGui::GetWindowHeight() + 6 * s;
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
    while (!m_toasts.empty() && now - m_toasts.front().time > 8.0) m_toasts.pop_front();
}

// ---------------------------------------------------------- background work --
BackgroundTask& EditorApp::startTask(const std::string& name) {
    std::lock_guard l(m_taskMutex);
    auto t = std::make_unique<BackgroundTask>();
    t->id = m_nextTaskId++;
    t->name = name;
    m_tasks.push_back(std::move(t));
    return *m_tasks.back();
}

void EditorApp::runInBackground(const std::string& name, std::function<std::string(BackgroundTask&)> work) {
    BackgroundTask& task = startTask(name);
    BackgroundTask* tp = &task;
    m_taskThreads.emplace_back([this, tp, work = std::move(work)] {
        std::string result = work(*tp);
        tp->done = true;
        m_mainQueue.post([this, tp, result] {
            (void)result;
            std::lock_guard l(m_taskMutex);
            std::erase_if(m_tasks, [&](const std::unique_ptr<BackgroundTask>& t) { return t.get() == tp; });
        });
    });
}

void EditorApp::startCook(bool force) {
    if (!m_session.hasProject() || m_cookRunning) return;
    m_cookRunning = true;
    Project project = m_session.project();
    size_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(project.contentDir(), ec); it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file() && it->path().extension() != ".meta") ++total;
    runInBackground(force ? "Cooking (force)" : "Cooking", [this, project, total, force](BackgroundTask& t) {
        size_t n = 0;
        CookOptions opt;
        opt.force = force;
        auto r = cookProject(project, opt, [&](const std::string& msg) {
            ++n;
            t.progress = total ? std::min(1.0f, static_cast<float>(n) / static_cast<float>(total)) : -1.0f;
            std::lock_guard l(m_taskMutex);
            t.detail = msg;
        });
        std::string msg = r ? std::format("Cook finished: {} cooked, {} up to date, {} removed", r->cooked, r->skipped, r->removed)
                            : "Cook failed: " + r.error();
        bool ok = r.ok();
        m_mainQueue.post([this, msg, ok] {
            m_cookRunning = false;
            toast(ok ? ToastLevel::Success : ToastLevel::Error, msg);
        });
        return msg;
    });
}

void EditorApp::startPackage() {
    if (!m_session.hasProject() || m_packageRunning) return;
    m_prefs.notifyWhenPackageDone = true;
    m_prefs.openFolderWhenDone = true;
    m_prefs.packageRunInBackground = true;
    saveSettings();
    beginPackage(m_prefs.singleExePackage);
}

void EditorApp::drawPackageDialog() {
    if (!m_showPackageDialog) return;
    float s = effectiveScale();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({460 * s, 0}, ImGuiCond_Appearing);
    if (!ImGui::Begin(ICON_BOX "  Package###PackageDialog", &m_showPackageDialog, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Choose how the game folder is laid out. DLLs and the browser runtime, when the project uses one, always sit next to the executable.");
    ImGui::Spacing();
    bool single = m_prefs.singleExePackage;
    if (ImGui::RadioButton("One executable  (content packed inside the exe)", single)) m_prefs.singleExePackage = true;
    if (ImGui::RadioButton("Folder  (exe plus a content file beside it)", !m_prefs.singleExePackage)) m_prefs.singleExePackage = false;
    ImGui::TextDisabled(m_prefs.singleExePackage ? "Share the exe. It is one file, and it is slower to write."
                                                 : "Share the whole folder. The exe stays the runtime; the game data is a separate file.");
    ImGui::Spacing();
    ImGui::SeparatorText("While it runs");
    if (ImGui::Checkbox("Run in background", &m_prefs.packageRunInBackground)) saveSettings();
    ImGui::SameLine();
    ImGui::TextDisabled("Minimize Fledge. The taskbar keeps the progress.");
    if (ImGui::Checkbox("Notify me when it finishes", &m_prefs.notifyWhenPackageDone)) saveSettings();
    if (ImGui::Checkbox("Open the folder when it finishes", &m_prefs.openFolderWhenDone)) saveSettings();
    if (ImGui::Checkbox("Shut down the computer when packaging finishes", &m_prefs.shutdownAfterPackage)) saveSettings();
    if (m_prefs.shutdownAfterPackage)
        ImGui::TextDisabled("Only after a successful package. Windows waits 60 seconds. Cancel with: shutdown /a");
    ImGui::Spacing();
    if (ImGui::Button("Package", {120 * s, 0})) {
        m_showPackageDialog = false;
        saveSettings();
        beginPackage(m_prefs.singleExePackage);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {120 * s, 0})) m_showPackageDialog = false;
    ImGui::End();
}

void EditorApp::drawEditorSplash() {
    if (!m_editorSplash) return;
    if (m_splashStart == 0) m_splashStart = ImGui::GetTime();
    bool skip = m_frame > 4 && (ImGui::GetTime() - m_splashStart > 1.8 || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Space) ||
                                 ImGui::IsMouseClicked(ImGuiMouseButton_Left));
    if (skip) {
        m_editorSplash = false;
        return;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.07f, 0.09f, 1));
    ImGui::Begin("##FledgeSplash", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    float s = effectiveScale();
    ImGui::SetCursorPos({vp->WorkSize.x * 0.5f - 80 * s, vp->WorkSize.y * 0.5f - 70 * s});
    ImGui::PushFont(m_fontLarge ? m_fontLarge : ImGui::GetFont());
    ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.96f, 1), "FLEDGE");
    ImGui::PopFont();
    ImGui::SetCursorPosX(vp->WorkSize.x * 0.5f - 90 * s);
    ImGui::TextColored(ImVec4(0.93f, 0.45f, 0.22f, 1), "%s", editionName(currentEdition()));
    ImGui::SetCursorPosX(vp->WorkSize.x * 0.5f - 70 * s);
    ImGui::TextDisabled("%s", FLEDGE_VERSION_STRING);
    ImGui::SetCursorPos({vp->WorkSize.x * 0.5f - 110 * s, vp->WorkSize.y * 0.5f + 28 * s});
    ImGui::TextDisabled("Click or press Space to continue");
    ImGui::End();
    ImGui::PopStyleColor();
}

void EditorApp::beginPackage(bool singleExe) {
    if (!m_session.hasProject() || m_packageRunning) return;
    if (m_session.playing()) {
        toast(ToastLevel::Warning, "Stop play mode before packaging");
        return;
    }
    if (m_session.dirty)
        if (auto st = m_session.saveScene(); !st) {
            toast(ToastLevel::Error, "Saving the scene failed: " + st.error);
            return;
        }
    m_packageRunning = true;
    bool minimizeWhilePackaging = m_prefs.packageRunInBackground;
    bool notify = m_prefs.notifyWhenPackageDone;
    bool openFolder = m_prefs.openFolderWhenDone;
    bool shutdown = m_prefs.shutdownAfterPackage;
    if (minimizeWhilePackaging && m_window && m_window->glfwHandle()) glfwIconifyWindow(m_window->glfwHandle());
    PackageOptions opt;
    opt.projectFile = m_session.project().file;
    opt.runtimeExe = m_session.runtimeExe();
    opt.moduleDir = m_session.toolsDir;
    opt.embedPak = singleExe;
    runInBackground("Packaging " + m_session.project().gameName, [this, opt, notify, openFolder, shutdown](BackgroundTask& t) {
        t.progress = 0;
        auto r = packageProject(opt, [&](const std::string& msg, float fraction) {
            if (fraction >= 0) t.progress = fraction;
            std::lock_guard l(m_taskMutex);
            t.detail = msg;
        });
        std::string msg = r ? "Packaged: " + narrow(r->executable.wstring()) : "Package failed: " + r.error();
        bool ok = r.ok();
        std::string dir = r ? narrow(r->executable.parent_path().wstring()) : std::string();
        m_mainQueue.post([this, msg, ok, dir, notify, openFolder, shutdown] {
            m_packageRunning = false;
            if (ok) m_lastPackageDir = dir;
            toast(ok ? ToastLevel::Success : ToastLevel::Error, msg);
#ifdef _WIN32
            HWND hwnd = m_window ? static_cast<HWND>(m_window->nativeHandle()) : nullptr;
            setTaskbarProgress(hwnd, 1, false, !ok);
            if (notify) notifyFinished(hwnd, ok, msg);
            if (ok && openFolder && !dir.empty()) ShellExecuteW(nullptr, L"open", widen(dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (ok && shutdown) {
                scheduleShutdown();
                toast(ToastLevel::Warning, "Windows will shut down in 60 seconds. Cancel with: shutdown /a");
            }
#else
            (void)notify;
            (void)openFolder;
            (void)shutdown;
#endif
        });
        return msg;
    });
}

// ---------------------------------------------------------------- output log --
void EditorApp::drawOutput() {
    PanelDef* p = panel("Output");
    if (!ImGui::Begin("Output", p ? &p->open : nullptr)) {
        ImGui::End();
        return;
    }
    auto entries = Logger::get().recent(2000);
    int counts[4] = {0, 0, 0, 0};
    std::vector<std::string> categories;
    auto slot = [](LogLevel l) { return l == LogLevel::Error ? 3 : l == LogLevel::Warn ? 2 : l == LogLevel::Info ? 1 : 0; };
    for (auto& e : entries) {
        ++counts[slot(e.level)];
        if (std::find(categories.begin(), categories.end(), e.category) == categories.end()) categories.push_back(e.category);
    }
    std::sort(categories.begin(), categories.end());
    if (ImGui::Button(ICON_ERASER " Clear")) Logger::get().clearRecent();
    ImGui::SameLine();
    const char* names[4] = {"Debug", "Info", "Warnings", "Errors"};
    const char* icons[4] = {ICON_CODE, ICON_INFO, ICON_WARNING, ICON_ERROR};
    for (int i = 0; i < 4; ++i) {
        std::string label = std::format("{} {}  {}###lvl{}", icons[i], names[i], counts[i], i);
        if (toggleButton(label.c_str(), m_logLevels[i])) m_logLevels[i] = !m_logLevels[i];
        ImGui::SameLine(0, 2);
    }
    ImGui::SameLine(0, 10);
    ImGui::SetNextItemWidth(140 * effectiveScale());
    if (ImGui::BeginCombo("##cat", m_logCategory.empty() ? "All categories" : m_logCategory.c_str())) {
        if (ImGui::Selectable("All categories", m_logCategory.empty())) m_logCategory.clear();
        for (auto& c : categories)
            if (ImGui::Selectable(c.c_str(), c == m_logCategory)) m_logCategory = c;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - 120 * effectiveScale()));
    ImGui::InputTextWithHint("##filter", ICON_FILTER "  filter text", &m_consoleFilter);
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &m_consoleAutoScroll);
    ImGui::Separator();
    ImGui::PushFont(m_fontMono);
    ImGui::BeginChild("log", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    std::string f = toLower(m_consoleFilter);
    ImGuiListClipper clipper;
    std::vector<const LogEntry*> shown;
    for (auto& e : entries) {
        if (!m_logLevels[slot(e.level)]) continue;
        if (!m_logCategory.empty() && e.category != m_logCategory) continue;
        if (!f.empty() && toLower(e.message).find(f) == std::string::npos && toLower(e.category).find(f) == std::string::npos) continue;
        shown.push_back(&e);
    }
    clipper.Begin(static_cast<int>(shown.size()));
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const LogEntry& e = *shown[i];
            ImGui::TextDisabled("%8.2f", e.timeSeconds);
            ImGui::SameLine();
            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%-9s", e.category.c_str());
            ImGui::SameLine();
            ImGui::TextColored(levelColor(e.level), "%s", e.message.c_str());
        }
    if (m_consoleAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::PopFont();
    ImGui::End();
}

// ----------------------------------------------------------------- settings --
void EditorApp::drawToolchainPrompt() {
    if (!m_toolchainPrompt) return;
    const editor::ToolchainEntry& tc = editor::pinnedToolchain();
    const char* id = "Install C++ toolchain##tcprompt";
    if (!ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);
    float s = effectiveScale();
    ImGui::SetNextWindowSize({620 * s, 0}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(tc.title.c_str());
        ImGui::TextDisabled("%s", tc.url.c_str());
        ImGui::TextDisabled("%.1f MB, SHA-256 %s", static_cast<double>(tc.size) / 1e6, tc.sha256.c_str());
        ImGui::Separator();
        ImGui::PushTextWrapPos(600 * s);
        ImGui::TextUnformatted(tc.licenses.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        if (ImGui::Button(ICON_DOWNLOAD " Accept and install", {200 * s, 0}) && !m_toolchainBusy) {
            m_toolchainBusy = true;
            m_toolchainPrompt = false;
            ImGui::CloseCurrentPopup();
            fs::path root = m_paths.userData / "Toolchains";
            runInBackground("Installing C++ toolchain", [this, root](BackgroundTask& t) {
                editor::ToolchainInstaller inst(std::shared_ptr<ai::HttpTransport>(ai::createDefaultHttpTransport()), root);
                auto r = inst.install(editor::pinnedToolchain(), /*licenseAccepted (the user's click)*/ true, [&](const std::string& stage, double f) {
                    t.progress = static_cast<float>(f);
                    std::lock_guard l(m_taskMutex);
                    t.detail = stage;
                });
                m_mainQueue.post([this, r] {
                    m_toolchainBusy = false;
                    if (r) {
                        m_session.scriptSettings.compiler = *r;
                        saveSettings();
                        toast(ToastLevel::Success, "C++ toolchain installed: Fledge Script files can be built now");
                    } else {
                        toast(ToastLevel::Error, "Toolchain install failed: " + r.error());
                    }
                });
                return r ? std::string("ok") : r.error();
            });
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120 * s, 0})) {
            m_toolchainPrompt = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorApp::drawSettingsWindow() {
    if (!m_showSettings) return;
    float s = effectiveScale();
    ImGui::SetNextWindowSize({760 * s, 520 * s}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (!ImGui::Begin(ICON_GEAR "  Settings###Settings", &m_showSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    const char* pages[] = {"Appearance", "Input", "Viewport", "Paths", "Migrate Settings", "Scripts", "Code Editor", "Auto-save", "Asset Store Import",
                           "AI Control", "AI Providers", "MCP Server"};
    const char* pageIcons[] = {ICON_PALETTE, ICON_KEYBOARD, ICON_VIDEO, ICON_FOLDER, ICON_FILES, ICON_BOLT, ICON_CODE, ICON_SAVE, ICON_DOWNLOAD,
                               ICON_SHIELD, ICON_ROBOT, ICON_SERVER};
    ImGui::BeginChild("pages", {170 * s, 0}, ImGuiChildFlags_Borders);
    for (int i = 0; i < 12; ++i) {
        if (!aiOn() && (std::string_view(pages[i]) == "AI Providers" || std::string_view(pages[i]) == "MCP Server")) continue;  // only while AI is on
        std::string label = std::string(pageIcons[i]) + "  " + pages[i];
        if (ImGui::Selectable(label.c_str(), m_settingsPage == pages[i])) m_settingsPage = pages[i];
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("page", {0, 0});
    const std::string& pg = m_settingsPage;
    bool changed = false;
    if (pg == "Appearance") {
        ImGui::SeparatorText("Theme");
        int theme = m_prefs.theme == Theme::Light ? 1 : 0;
        bool pick = ImGui::RadioButton(ICON_MOON " Dark", &theme, 0);
        ImGui::SameLine(0, 24 * s);
        pick |= ImGui::RadioButton(ICON_SUN " Light", &theme, 1);
        if (pick) {
            m_prefs.theme = theme ? Theme::Light : Theme::Dark;
            applyTheme();
            changed = true;
        }
        ImGui::SeparatorText("Scale and typography");
        bool autoScale = m_prefs.uiScale <= 0;
        if (ImGui::Checkbox("Follow monitor DPI", &autoScale)) {
            m_prefs.uiScale = autoScale ? 0.0f : effectiveScale();
            m_fontsDirty = changed = true;
        }
        ImGui::BeginDisabled(autoScale);
        float scalePct = effectiveScale() * 100;
        ImGui::SetNextItemWidth(220 * s);
        if (ImGui::SliderFloat("UI scale", &scalePct, 75, 250, "%.0f%%", ImGuiSliderFlags_AlwaysClamp) && !autoScale) m_prefs.uiScale = scalePct / 100;
        if (ImGui::IsItemDeactivatedAfterEdit()) m_fontsDirty = changed = true;
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(220 * s);
        ImGui::SliderFloat("Font size", &m_prefs.fontSize, 12, 20, "%.0f px");
        if (ImGui::IsItemDeactivatedAfterEdit()) m_fontsDirty = changed = true;
        ImGui::TextDisabled("Monitor DPI scale: %.0f%%   Fonts: Roboto (UI), Cousine (log), Font Awesome Free (icons)%s",
                            (m_window ? m_window->contentScale() : 1.0f) * 100, m_iconsLoaded ? "" : "  - icon font missing");
        ImGui::SeparatorText("Startup");
        changed |= ImGui::Checkbox("Show the Welcome screen when no project is open", &m_prefs.showWelcomeOnStartup);
        changed |= ImGui::Checkbox("Show the Fledge splash on startup", &m_prefs.showSplash);
        changed |= ImGui::Checkbox("Reopen the last project on startup", &m_prefs.restoreLastProject);
    } else if (pg == "Input") {
        ImGui::SeparatorText("Viewport camera");
        ImGui::SetNextItemWidth(220 * s);
        changed |= ImGui::SliderFloat("Fly speed", &m_prefs.cameraSpeed, 0.5f, 50.0f, "%.1f m/s", ImGuiSliderFlags_Logarithmic);
        ImGui::SetNextItemWidth(220 * s);
        changed |= ImGui::SliderFloat("Mouse sensitivity", &m_prefs.mouseSensitivity, 0.2f, 3.0f, "%.2fx");
        changed |= ImGui::Checkbox("Invert mouse Y", &m_prefs.invertY);
        ImGui::SeparatorText("Play");
        changed |= ImGui::Checkbox("Lock mouse during Play", &m_prefs.lockMouseInPlay);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Hide the cursor and lock it to the Game view while playing. Esc releases it, a click in the view takes it back.");
        ImGui::SeparatorText("Keyboard");
        if (ImGui::Button(ICON_KEYBOARD "  Show keyboard shortcuts")) m_showShortcuts = true;
    } else if (pg == "Viewport") {
        changed |= ImGui::Checkbox(ICON_GRID "  Grid", &m_prefs.showGrid);
        changed |= ImGui::Checkbox(ICON_GAUGE "  Stats overlay", &m_prefs.showStats);
        changed |= ImGui::Checkbox(ICON_CUBE "  Orientation cube", &m_prefs.showOrientation);
    } else if (pg == "Paths") {
        auto row = [](const char* k, const fs::path& v) {
            ImGui::TextDisabled("%s", k);
            ImGui::SameLine(130);
            ImGui::TextUnformatted(narrow(v.wstring()).c_str());
        };
        ImGui::SeparatorText("Install (resolved relative to the executable)");
        row("Install root", m_paths.root);
        row("Templates", m_paths.templates);
        row("Samples", m_paths.samples);
        row("Docs", m_paths.docs);
        row("Resources", m_paths.resources);
        ImGui::SeparatorText("User data");
        row("Settings", m_opt.settingsDir);
        ImGui::TextDisabled(m_paths.portable ? "Portable mode: ON (portable.txt or --portable) - settings live next to the install."
                                             : "Portable mode: off. Put an empty portable.txt next to bin/ (or pass --portable) to keep settings in the install folder.");
        ImGui::SeparatorText("Projects");
        std::string loc = narrow(m_prefs.projectsDir.wstring());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##projects", &loc)) {
            m_prefs.projectsDir = pathFromUtf8(loc);
            changed = true;
        }
        ImGui::TextDisabled("Default location for new projects and for copies of the samples.");
    } else if (pg == "Migrate Settings") {
        drawMigrationSettingsPage();
    } else if (pg == "Scripts") {
        auto& ss = m_session.scriptSettings;
        ImGui::SeparatorText("Compiler (Fledge Script files are C++20, compiled to a DLL)");
        std::string comp = narrow(ss.compiler.wstring());
        ImGui::SetNextItemWidth(-90 * s);
        if (ImGui::InputTextWithHint("##cxx", "auto: FLEDGE_CXX, PATH, C:\\Strawberry\\c\\bin, MSYS2...", &comp)) {
            ss.compiler = pathFromUtf8(comp);
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Detect")) {
            auto tc = findToolchain(ss.compiler);
            if (tc) toast(ToastLevel::Success, std::format("g++ {} at {}", tc->version, narrow(tc->cxx.wstring())));
            else toast(ToastLevel::Error, tc.error());
        }
        {  // one-click portable compiler into the Fledge data folder (pinned + SHA-256, licences shown first)
            const editor::ToolchainEntry& tc = editor::pinnedToolchain();
            editor::ToolchainInstaller inst(nullptr, m_paths.userData / "Toolchains");
            if (inst.isInstalled(tc)) {
                ImGui::TextDisabled("%s Installed: %s", ICON_CHECK, tc.title.c_str());
            } else if (ImGui::Button(ICON_DOWNLOAD " Install C++ toolchain...")) {
                m_toolchainPrompt = true;
            }
            trackRect("scripts.toolchain");
            ImGui::SameLine();
            ImGui::TextDisabled("%s, %.0f MB, into %s", tc.title.c_str(), static_cast<double>(tc.size) / 1e6,
                                narrow((m_paths.userData / "Toolchains").wstring()).c_str());
        }
        ImGui::TextDisabled("SDK headers: %s", narrow(m_paths.sdk.wstring()).c_str());
        changed |= ImGui::Checkbox("Build automatically when a script changes (hot reload, also while playing)", &ss.autoBuild);
        ImGui::SeparatorText("External editor");
        ImGui::SetNextItemWidth(-1);
        changed |= ImGui::InputTextWithHint("##ide", "empty = system default app; e.g. code -g \"{file}:{line}\" \"{scripts}\"", &m_externalEditor);
        ImGui::TextDisabled("Placeholders: {file} {line} {scripts} {project}. compile_commands.json is written to Scripts/ for clangd / VS Code.");
        ImGui::SeparatorText("AI and native code");
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s Whether the AI may build and load scripts is set in Settings > AI Control (Script builds). Scripts are "
                            "native code that runs with your user's rights; AI tools can only write inside Scripts/ (undoable).", ICON_INFO);
        ImGui::PopTextWrapPos();
    } else if (pg == "Code Editor") {
        m_code->drawSettings();
    } else if (pg == "Auto-save") {
        changed |= false;
        drawAutosaveSettings();
        changed = true;  // cheap: the settings file is rewritten when the page is left open and edited (see drawAutosaveSettings)
    } else if (pg == "Asset Store Import") {
        drawStoreImportSettings();
    } else if (pg == "AI Control") {
        drawAiControlSettings();
    } else if (pg == "AI Providers" && aiOn()) {
        drawAiProviderSettings();
    } else if (pg == "MCP Server" && aiOn()) {
        bool on = m_mcpHttp.running();
        if (ImGui::Checkbox("Run the MCP HTTP server now", &on)) m_commands.execute("mcp.toggle");
        changed |= ImGui::Checkbox("Start it automatically with the editor", &m_mcpAutoStart);
        ImGui::SetNextItemWidth(140 * s);
        changed |= ImGui::InputInt("Port", &m_mcpPortSetting);
        m_mcpPortSetting = std::clamp(m_mcpPortSetting, 1024, 65535);
        ImGui::TextDisabled("Binds to 127.0.0.1 only. Endpoint: http://127.0.0.1:%d/mcp. Also available over stdio: fledge_mcp_server.", m_mcpPortSetting);
        drawFullMcpControlSettings();
        const char* tok = std::getenv("FLEDGE_MCP_TOKEN");
        bool hasTok = tok && *tok;
        ImGui::TextColored(hasTok ? ImVec4(0.35f, 0.85f, 0.45f, 1) : ImVec4(1, 0.75f, 0.3f, 1), "%s %s", hasTok ? ICON_LOCK : ICON_WARNING,
                           hasTok ? "Bearer token required (FLEDGE_MCP_TOKEN is set)"
                                  : "No bearer token: any local process can call the server while it runs. Set FLEDGE_MCP_TOKEN to require one.");
        ImGui::SeparatorText("Permissions (shared by MCP clients and the in-editor assistant)");
        bool pol = false;
        pol |= ImGui::Checkbox("Read-only mode (only tools that do not modify anything)", &m_mcpReadOnly);
        pol |= ImGui::Checkbox("Allow destructive tools", &m_mcpAllowDestructive);
        if (m_mcpAllowDestructive)
            ImGui::TextColored({1, 0.45f, 0.45f, 1}, "%s Destructive tools can delete entities and replace scenes. Every AI edit is still undoable.",
                               ICON_WARNING);
        ImGui::TextDisabled("Per-tool opt-in for destructive tools:");
        auto& policy = m_mcp->policy();
        for (const auto& t : m_tools.all()) {
            if (!t.destructive) continue;
            bool allowed = policy.allowedDestructive.contains(t.name);
            if (ImGui::Checkbox(t.name.c_str(), &allowed)) {
                if (allowed) policy.allowedDestructive.insert(t.name);
                else policy.allowedDestructive.erase(t.name);
                pol = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", t.title.c_str());
        }
        if (pol) {
            rebuildProvider();
            changed = true;
        }
    }
    ImGui::EndChild();
    if (changed) saveSettings();
    ImGui::End();
}

// ------------------------------------------------------------------ welcome --
ImTextureID EditorApp::loadUiTexture(const fs::path& png, int* w, int* h) {
    std::string key = narrow(png.wstring());
    if (auto it = m_uiTextures.find(key); it != m_uiTextures.end()) return it->second.first;
    ImTextureID id = 0;
    rhi::TextureHandle th;
    if (auto bytes = readFileBytes(png))
        if (auto img = decodeImage(*bytes)) {
            // UI images are drawn as-is by ImGui (no sRGB framebuffer): upload unconverted (non-sRGB) with mips
            rhi::TextureDesc d;
            d.width = img->width;
            d.height = img->height;
            d.srgb = false;
            int mips = 1;
            while ((std::max(img->width, img->height) >> mips) > 0) ++mips;
            d.mips = mips;
            d.sampler = {rhi::Filter::Linear, true, rhi::Wrap::Clamp, false};
            rhi::TextureSubresource sub{0, 0, img->width, img->height, img->rgba.data(), img->rgba.size()};
            th = m_device->createTexture(d, std::span(&sub, 1));
            m_device->generateMips(th);
            id = static_cast<ImTextureID>(m_device->nativeTexture(th));
            if (w) *w = img->width;
            if (h) *h = img->height;
        }
    m_uiTextures[key] = {id, th};
    return id;
}

#ifdef _WIN32
// A real window, painted and message-pumped on the UI thread, while the project open runs beside it.
// Without this the UI thread never returns to Windows and the editor is marked Not Responding.
struct LoadOverlay {
    HWND hwnd = nullptr;
    HWND editor = nullptr;
    std::wstring title = L"Opening project";
    std::wstring detail = L"Starting";
    float fraction = -1;

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<LoadOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            self = static_cast<LoadOverlay*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (msg == WM_PAINT && self) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(RGB(16, 18, 22));
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(240, 240, 242));
            RECT titleRc{24, 22, rc.right - 24, 52};
            DrawTextW(dc, self->title.c_str(), -1, &titleRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            SetTextColor(dc, RGB(170, 176, 184));
            RECT detailRc{24, 56, rc.right - 24, 96};
            DrawTextW(dc, self->detail.c_str(), -1, &detailRc, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
            RECT bar{24, 108, rc.right - 24, 124};
            HBRUSH track = CreateSolidBrush(RGB(40, 44, 52));
            FillRect(dc, &bar, track);
            DeleteObject(track);
            if (self->fraction >= 0) {
                RECT fill = bar;
                fill.right = fill.left + static_cast<LONG>((fill.right - fill.left) * std::clamp(self->fraction, 0.0f, 1.0f));
                HBRUSH orange = CreateSolidBrush(RGB(232, 122, 46));
                FillRect(dc, &fill, orange);
                DeleteObject(orange);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        if (msg == WM_ERASEBKGND) return 1;
        return DefWindowProcW(hwnd, msg, w, l);
    }

    explicit LoadOverlay(HWND owner, const std::wstring& project) : editor(owner) {
        title = L"Opening " + project;
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"FledgeLoadOverlay";
            wc.hCursor = LoadCursorW(nullptr, IDC_WAIT);
            RegisterClassW(&wc);
            registered = true;
        }
        RECT wr{};
        if (owner) GetWindowRect(owner, &wr);
        const int w = 560, h = 160;
        int x = wr.left + std::max(0L, (wr.right - wr.left - w) / 2);
        int y = wr.top + std::max(0L, (wr.bottom - wr.top - h) / 2);
        hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"FledgeLoadOverlay", L"Fledge", WS_POPUP | WS_VISIBLE, x, y, w, h, owner,
                                nullptr, GetModuleHandleW(nullptr), this);
        pump(L"Starting", -1);
    }
    ~LoadOverlay() {
        if (hwnd) DestroyWindow(hwnd);
    }
    void pump(const std::wstring& stage, float frac) {
        detail = stage;
        fraction = frac;
        if (editor) {
            std::wstring caption = title + L" - " + stage;
            SetWindowTextW(editor, caption.c_str());
            setTaskbarProgress(editor, frac, true, false);
        }
        if (hwnd) {
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);
        }
        MSG msg;
        std::vector<MSG> later;
        for (int n = 0; n < 64 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++n) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            if (msg.hwnd == hwnd) DispatchMessageW(&msg);
            else if (msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST) continue;
            else if (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) continue;
            else later.push_back(msg);
        }
        for (auto& m : later) PostMessageW(m.hwnd, m.message, m.wParam, m.lParam);
    }
};
#endif

bool EditorApp::openProject(const fs::path& fileIn) {
    // Editions: a project that needs features this edition lacks is declined before anything touches it (docs/EDITIONS.md).
    if (EditionCheck check = checkProjectEdition(fileIn); !check.ok) {
        showDecline(check);
        return false;
    }
    const std::string previousTitle = m_session.hasProject()
                                          ? std::format("{} Editor - {}", editionName(currentEdition()), m_session.project().name)
                                          : std::format("{} Editor", editionName(currentEdition()));
    m_window->setTitle(std::format("{} Editor - Loading {}...", editionName(currentEdition()), fileIn.stem().string()));
    if (m_session.playing()) (void)m_session.stop();
    fs::path file = fileIn;
#ifdef _WIN32
    HWND hwnd = m_window ? static_cast<HWND>(m_window->nativeHandle()) : nullptr;
    LoadOverlay overlay(hwnd, widen(fileIn.stem().string()));
    struct Shared {
        std::mutex mu;
        std::string stage = "Starting";
        float fraction = 0.02f;
    } shared;
    m_session.setLoadProgress([&](const std::string& stage, float fraction) {
        std::lock_guard lock(shared.mu);
        shared.stage = stage;
        shared.fraction = fraction;
    });
    Status st;
    std::string migrated;
    std::atomic<bool> done{false};
    std::thread worker([&] {
        try {
        if (legacy::isLegacyProjectFile(file)) {
            auto m = legacy::migrateProject(file);
            if (!m) {
                st = Status::failure("Could not convert the old project: " + m.error());
                done = true;
                return;
            }
            file = m->projectFile;
            migrated = std::format("Converted an older project to Fledge: a converted copy was created in {} ({} files renamed). "
                                   "The original folder is unchanged.",
                                   narrow(file.parent_path().wstring()), m->filesRenamed);
        }
        st = m_session.openProject(file);
        } catch (const std::exception& e) {
            st = Status::failure(std::string("Opening the project failed: ") + e.what());
        } catch (...) {
            st = Status::failure("Opening the project failed");
        }
        done = true;
    });
    while (!done.load()) {
        std::string stage;
        float fraction = -1;
        {
            std::lock_guard lock(shared.mu);
            stage = shared.stage;
            fraction = shared.fraction;
        }
        overlay.pump(widen(stage), fraction);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    worker.join();
    m_session.setLoadProgress({});
    if (!migrated.empty() && st) toast(ToastLevel::Success, migrated);
#else
    if (legacy::isLegacyProjectFile(file)) {  // pre-0.8 project: convert a copy, never the original
        auto m = legacy::migrateProject(file);
        if (!m) {
            m_window->setTitle(previousTitle);
            toast(ToastLevel::Error, "Could not convert the old project: " + m.error());
            return false;
        }
        file = m->projectFile;
        toast(ToastLevel::Success, std::format("Converted an older project to Fledge: a converted copy was created in {} ({} files renamed). "
                                               "The original folder is unchanged.",
                                               narrow(file.parent_path().wstring()), m->filesRenamed));
    }
    auto st = m_session.openProject(file);
#endif
    if (!st) {
        m_window->setTitle(previousTitle);
#ifdef _WIN32
        setTaskbarProgress(hwnd, 1, false, true);
#endif
        if (const EditionCheck* d = m_session.lastDecline()) showDecline(*d);
        else toast(ToastLevel::Error, st.error);
        return false;
    }
#ifdef _WIN32
    m_session.world().preloadSceneAssets([&](int done, int total, const std::string& label) {
        float f = total > 0 ? 0.86f + 0.13f * static_cast<float>(done) / static_cast<float>(total) : 0.95f;
        overlay.pump(widen(std::format("Loading {}  ({}/{})", label, done, std::max(total, 1))), f);
    });
    setTaskbarProgress(hwnd, 1, false, false);
#endif
    m_session.startWatching();
    m_recent.add(m_session.project().file);
    releaseThumbnails();
    m_session.selection.clear();
    m_camPos = {0, 3, 8};
    m_camYaw = 0;
    m_camPitch = radians(-15);
    m_camDist = 8;
    if (auto v = m_session.world().primaryView(16.0f / 9.0f)) {  // start where the game camera looks
        Mat4 inv = v->view.inverse();
        m_camPos = inv.c[3].xyz();
        Vec3 f = normalize(-inv.c[2].xyz());
        m_camYaw = std::atan2(-f.x, -f.z);
        m_camPitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
        m_camPos += f * 0.75f;  // step past the camera entity so its pick volume is behind us, not under every click
    }
    m_window->setTitle(std::format("{} Editor - {}", editionName(currentEdition()), m_session.project().name));
    saveSettings();
    toast(ToastLevel::Success, "Opened " + m_session.project().name);
    return true;
}

void EditorApp::createFromTemplate() {
    m_wizardError.clear();
    if (m_wizardTemplate < 0 || m_wizardTemplate >= static_cast<int>(m_templates.size())) {
        m_wizardError = "Pick a template";
        return;
    }
    std::string loc = m_wizardLocation;
    std::erase(loc, '"');
    std::error_code ec;
    fs::create_directories(pathFromUtf8(loc), ec);
    auto r = createProjectFromTemplate(m_templates[m_wizardTemplate], pathFromUtf8(loc), m_wizardName);
    if (!r) {
        m_wizardError = r.error();
        return;
    }
    if (openProject(*r)) {
        m_showWelcome = false;
        toast(ToastLevel::Success, "Created " + m_wizardName + " from " + m_templates[m_wizardTemplate].name);
    }
}

void EditorApp::drawWelcome() {
    if (m_showWelcome && !ImGui::IsPopupOpen("Welcome to Fledge")) {
        ImGui::OpenPopup("Welcome to Fledge");
        m_templates = listTemplates(m_paths.templates);
        m_samples = listSamples(m_paths.samples);
        if (m_wizardLocation.empty()) m_wizardLocation = narrow(m_prefs.projectsDir.wstring());
    }
    float s = effectiveScale();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 size{std::min(980 * s, vp->WorkSize.x - 40), std::min(600 * s, vp->WorkSize.y - 40)};
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
    bool open = true;
    m_welcomeVisible = ImGui::BeginPopupModal("Welcome to Fledge", &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
    if (!m_welcomeVisible) {
        if (m_showWelcome && !open) m_showWelcome = false;
        return;
    }
    // header
    int lw = 0, lh = 0;
    ImTextureID logo = loadUiTexture(m_paths.resources / "icons" / "fledge_256.png", &lw, &lh);
    if (logo) ImGui::Image(logo, {64 * s, 64 * s});
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushFont(m_fontLarge);
    ImGui::TextUnformatted("Fledge");
    ImGui::PopFont();
    ImGui::TextDisabled("Make games, simply.   v%s%s", FLEDGE_VERSION_STRING, m_paths.portable ? "  (portable)" : "");
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetWindowWidth() - 40 * s);
    if (ImGui::Button(ICON_XMARK)) {
        m_showWelcome = false;
        ImGui::CloseCurrentPopup();
    }
    trackRect("welcome.close");
    ImGui::Separator();

    if (ImGui::BeginTabBar("welcomeTabs")) {
        ImGuiTabItemFlags f0 = 0, f1 = 0;
        static int lastTab = -1;
        if (m_welcomeTab != lastTab) {
            (m_welcomeTab == 1 ? f1 : f0) = ImGuiTabItemFlags_SetSelected;
            lastTab = m_welcomeTab;
        }
        if (ImGui::BeginTabItem(ICON_HOUSE "  Start", nullptr, f0)) {
            if (ImGui::IsItemActivated()) m_welcomeTab = lastTab = 0;
            ImGui::BeginChild("left", {size.x * 0.48f, 0});
            ImGui::SeparatorText(ICON_CLOCK "  Recent projects");
            if (m_recent.items().empty()) ImGui::TextDisabled("No recent projects yet.");
            int i = 0;
            for (const auto& p : std::vector<fs::path>(m_recent.items())) {
                std::error_code ec;
                bool exists = fs::exists(p, ec);
                ImGui::PushID(i++);
                ImGui::BeginDisabled(!exists);
                std::string label = std::string(ICON_FOLDER) + "  " + narrow(p.stem().wstring());
                if (ImGui::Selectable(label.c_str(), false, 0, {0, ImGui::GetTextLineHeight() * 2.1f})) {
                    if (openProject(p)) {
                        m_showWelcome = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndDisabled();
                ImGui::SameLine(30 * s);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetTextLineHeight());
                ImGui::TextDisabled("%s%s", narrow(p.parent_path().wstring()).c_str(), exists ? "" : "  (missing)");
                ImGui::PopID();
            }
            ImGui::SeparatorText(ICON_FOLDER_OPEN "  Open a project file");
            ImGui::SetNextItemWidth(-90 * s);
            ImGui::InputTextWithHint("##open", "path to .fledge", &m_openPathInput);
            ImGui::SameLine();
            if (ImGui::Button("Open")) {
                std::string p = m_openPathInput;
                std::erase(p, '"');
                if (openProject(pathFromUtf8(p))) {
                    m_showWelcome = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("right", {0, 0});
            ImGui::SeparatorText(ICON_PLUS "  Create");
            if (ImGui::Button(ICON_WAND "  New project from a template...", {-1, 0})) m_welcomeTab = 1;
            trackRect("welcome.new");
            ImGui::SeparatorText(ICON_BOOK "  Samples");
            if (m_samples.empty()) ImGui::TextDisabled("No samples found in %s", narrow(m_paths.samples.wstring()).c_str());
            for (auto& smp : m_samples) {
                ImGui::PushID(smp.name.c_str());
                std::string label = std::string(ICON_CUBES) + "  " + smp.name;
                if (ImGui::Selectable(label.c_str())) {
                    auto r = copySampleProject(smp, m_prefs.projectsDir / "Samples");
                    if (!r) toast(ToastLevel::Error, r.error());
                    else if (openProject(*r)) {
                        m_showWelcome = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens a copy in %s", narrow((m_prefs.projectsDir / "Samples").wstring()).c_str());
                ImGui::PopID();
            }
            ImGui::TextDisabled("Samples are copied to your projects folder first,\nso the install stays untouched.");
            ImGui::SeparatorText(ICON_LIGHTBULB "  Tips");
            ImGui::TextDisabled("Ctrl+P  go to anything     Ctrl+Shift+P  commands\nF5  play     F1  keyboard shortcuts");
            ImGui::Checkbox("Show this screen when no project is open", &m_prefs.showWelcomeOnStartup);
            bool aiEnabled = aiOn();
            if (ImGui::Checkbox(ICON_ROBOT " Enable AI features (assistant, MCP server)", &aiEnabled)) setAiEnabled(aiEnabled);
            trackRect("welcome.ai");
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_WAND "  New Project", nullptr, f1)) {
            if (ImGui::IsItemActivated()) m_welcomeTab = lastTab = 1;
            ImGui::TextDisabled("1. Pick a template");
            float cardW = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
            float cardH = cardW * 9.0f / 16.0f;
            for (int i = 0; i < static_cast<int>(m_templates.size()); ++i) {
                const auto& t = m_templates[i];
                ImGui::PushID(i);
                if (i > 0) ImGui::SameLine();
                ImGui::BeginGroup();
                bool sel = m_wizardTemplate == i;
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImTextureID tex = t.preview.empty() ? 0 : loadUiTexture(t.preview);
                if (ImGui::InvisibleButton("card", {cardW, cardH + ImGui::GetTextLineHeightWithSpacing() * 3.6f})) m_wizardTemplate = i;
                trackRect("wizard.template." + t.id);
                bool hov = ImGui::IsItemHovered();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 p1{p0.x + cardW, p0.y + cardH + ImGui::GetTextLineHeightWithSpacing() * 3.6f};
                dl->AddRectFilled(p0, p1, ImGui::GetColorU32(hov ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 6);
                if (tex) dl->AddImageRounded(tex, {p0.x + 4, p0.y + 4}, {p1.x - 4, p0.y + cardH}, {0, 0}, {1, 1}, IM_COL32_WHITE, 4);
                else dl->AddText(m_fontLarge, m_fontLarge->FontSize, {p0.x + cardW * 0.42f, p0.y + cardH * 0.35f},
                                 ImGui::GetColorU32(ImGuiCol_TextDisabled), ICON_CUBES);
                if (sel) dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_CheckMark), 6, 0, 2.5f * s);
                ImGui::SetCursorScreenPos({p0.x + 8, p0.y + cardH + 6});
                ImGui::PushTextWrapPos(p0.x + cardW - 8);
                ImGui::TextUnformatted(t.name.c_str());
                ImGui::TextDisabled("%s", t.status.c_str());
                ImGui::PopTextWrapPos();
                ImGui::SetCursorScreenPos(p1);
                ImGui::Dummy({0, 0});
                ImGui::EndGroup();
                ImGui::PopID();
            }
            if (m_templates.empty()) ImGui::TextColored({1, 0.5f, 0.4f, 1}, "No templates found in %s", narrow(m_paths.templates.wstring()).c_str());
            else if (m_wizardTemplate < static_cast<int>(m_templates.size())) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("%s", m_templates[m_wizardTemplate].description.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::Spacing();
            ImGui::TextDisabled("2. Name and location");
            ImGui::SetNextItemWidth(300 * s);
            ImGui::InputText("Name", &m_wizardName);
            trackRect("wizard.name");
            ImGui::SetNextItemWidth(-80 * s);
            ImGui::InputText("Location", &m_wizardLocation);
            bool valid = isValidProjectName(m_wizardName);
            fs::path dest = pathFromUtf8(m_wizardLocation) / pathFromUtf8(m_wizardName);
            ImGui::TextDisabled("%s %s", ICON_ARROW_RIGHT, valid ? narrow(dest.wstring()).c_str() : "letters, digits, spaces, '-' and '_' only");
            if (!m_wizardError.empty()) ImGui::TextColored({1, 0.45f, 0.45f, 1}, "%s %s", ICON_ERROR, m_wizardError.c_str());
            ImGui::BeginDisabled(!valid || m_templates.empty());
            if (ImGui::Button(ICON_CHECK "  Create project", {200 * s, 0})) {
                createFromTemplate();
                if (!m_showWelcome) ImGui::CloseCurrentPopup();
            }
            trackRect("wizard.create");
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!m_showWelcome) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void EditorApp::drawJoinDialog() {
    if (!m_showJoin) return;
    ImGui::OpenPopup("Join Multiplayer");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("Join Multiplayer", &m_showJoin, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Same LAN, same Fledge build. The host clicks Host; you join their IP.");
        ImGui::SetNextItemWidth(240);
        ImGui::InputText("Address", &m_joinAddress);
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("Host port", &m_hostPort);
        if (m_hostPort < 1) m_hostPort = 1;
        if (m_hostPort > 65535) m_hostPort = 65535;
        ImGui::TextDisabled("Hosting uses UDP %d on all interfaces. Windows may ask about the firewall.", m_hostPort);
        if (ImGui::Button("Join", {120, 0})) {
            m_userPlayPending = true;
            stopSimulation(false);
            if (auto st = m_session.playJoin(m_joinAddress); !st) toast(ToastLevel::Error, st.error);
            else m_showJoin = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Host instead", {120, 0})) {
            m_showJoin = false;
            m_commands.execute("play.host");
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) m_showJoin = false;
        ImGui::EndPopup();
    }
}

void EditorApp::drawAbout() {
    if (!m_showAbout) return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::Begin(ICON_INFO "  About Fledge###About", &m_showAbout,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::PushFont(m_fontLarge);
        ImGui::TextUnformatted(editionName(currentEdition()));
        ImGui::PopFont();
        if (currentEdition() == Edition::Advanced) {
            ImGui::Text("Fledge Advanced - the full edition.  v%s", FLEDGE_VERSION_STRING);
            size_t mods = FeatureRegistry::get().modules().size();
            ImGui::TextDisabled("Everything in Fledge, plus %zu Advanced modules (scaffolds in this release; see the roadmap).", mods);
        } else {
            ImGui::Text("Fledge Engine - make games, simply.  v%s", FLEDGE_VERSION_STRING);
        }
        ImGui::TextDisabled("Editor, runtime, packager (fledgepack) and MCP server.");
        ImGui::TextDisabled("Edition: %s (%s). Projects made with Fledge open in both editions.", editionName(currentEdition()),
                            editionId(currentEdition()));
        ImGui::Separator();
        ImGui::TextDisabled("Built with GLFW, Dear ImGui, nlohmann/json, stb, cgltf.");
        ImGui::TextDisabled("Fonts: Roboto, Cousine (Apache-2.0); icons: Font Awesome Free (CC BY 4.0 / OFL-1.1).");
        ImGui::TextDisabled("Full texts: %s", narrow(m_paths.licenses.wstring()).c_str());
        drawAdvancedHint();
    }
    ImGui::End();
}

// -------------------------------------------------------------------- frame --
void EditorApp::frame(float dt) {
    ImGuiIO& io = ImGui::GetIO();
    bool flying = (m_vpHovered && ImGui::IsMouseDown(ImGuiMouseButton_Right)) || m_drag.active() ||
                  (m_session.playing() && m_vpFocused);  // the focused game viewport owns plain keys while playing
    m_commands.dispatchShortcuts(io.WantTextInput, flying, m_session.playing() && m_vpFocused && !io.WantTextInput);
    if (m_prefs.simpleMode) {
        drawSimpleTopBar();  // no menus or advanced toolbars in Simple Mode
    } else {
        drawMenuBar();
        drawToolbar();
    }
    drawStatusBar();
    ImGuiID dockId = ImGui::GetID(kDockName);
    if (m_firstLayout) {
        m_firstLayout = false;
        if (!ImGui::DockBuilderGetNode(dockId)) m_pendingReset = true;
    }
    if (m_pendingReset) {
        m_pendingReset = false;
        buildWorkspaceLayout(m_prefs.workspace, dockId);
    }
    ImGui::DockSpaceOverViewport(dockId, ImGui::GetMainViewport());
    updateStoreImport();  // dropped files, downloads watcher (may open the Asset Store Import panel)
    drawPanels(dt);
    drawCommandPalette();
    drawShortcutsDialog();
    drawNewScriptDialog();
    drawAiApproval();
    drawJoinDialog();
    drawSettingsWindow();
    drawMigrationWindow();
    drawMigrationBanner();
    drawToolchainPrompt();
    drawProjectSettings();
    drawLicenseDialog();
    drawStoreConsentDialog();
    drawSimpleStoreImport();
    drawBlueprintWindow();
    drawAbout();
    drawDecline();
    drawRecoveryDialog();
    drawPackageDialog();
    drawEditorSplash();
    drawWhyPopup();
    drawPlaytestBot();
    drawShipWindow();
    drawKeepDialog();
    drawPerfWindow();
    if (!m_prefs.simpleMode) drawWelcome();
    drawTour();
    drawToasts();
}

} // namespace fledge::editor
