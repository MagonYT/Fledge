// Simple Mode: "Describe -> Play -> Share". One column next to the game view; Pro Mode is one toggle away.
// With AI features off the describe box is replaced by a template picker and quick actions (no AI UI at all).
#include <algorithm>
#include <cmath>
#include <format>

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include "editor_app.h"
#include "editor_code.h"
#include "editor_icons.h"
#include "fledge/world/partition.h"
#include "fledge/editor/playtest_bot.h"
#include "fledge/editor/perf_advisor.h"

namespace fledge::editor {

namespace {
struct TourStep {
    const char* target;  // trackRect id
    const char* title;
    const char* text;
};
const std::vector<TourStep>& tourSteps(bool ai) {
    static const std::vector<TourStep> withAi = {
        {"simple.describe", "1. Describe", "Type what you want - \"a red ball that bounces on a green floor\" - and press Make it. Every change can be undone."},
        {"simple.play", "2. Play", "Press Play (F5) to try your game right here. Press Stop to get back to editing."},
        {"simple.share", "3. Share", "Share builds a standalone game folder (an .exe) you can zip and send to friends."},
        {"simple.projects", "Your games", "All your games live here. Click one to open it, or + to start a new one."},
        {"simple.pro", "Pro Mode", "Want every tool - scene tree, inspector, code editor? Pro Mode is one click away. You can come back any time."},
    };
    static const std::vector<TourStep> noAi = {
        {"simple.quick", "1. Build", "Start from a template, then add things with the quick actions. Every change can be undone."},
        {"simple.play", "2. Play", "Press Play (F5) to try your game right here. Press Stop to get back to editing."},
        {"simple.share", "3. Share", "Share builds a standalone game folder (an .exe) you can zip and send to friends."},
        {"simple.projects", "Your games", "All your games live here. Click one to open it, or + to start a new one."},
        {"simple.pro", "Pro Mode", "Want every tool - scene tree, inspector, code editor? Pro Mode is one click away. You can come back any time."},
    };
    return ai ? withAi : noAi;
}
constexpr double kTourStepSeconds = 12.0;  // 5 steps = 60 s, auto-advancing

bool bigButton(const char* label, ImVec2 size, ImVec4 color, bool enabled = true) {
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(std::min(1.0f, color.x * 1.12f), std::min(1.0f, color.y * 1.12f), std::min(1.0f, color.z * 1.12f), 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x * 0.9f, color.y * 0.9f, color.z * 0.9f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    ImGui::BeginDisabled(!enabled);
    bool r = ImGui::Button(label, size);
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    return r;
}
} // namespace

void EditorApp::setSimpleMode(bool on) {
    if (on == m_prefs.simpleMode && (!on || m_prefs.workspace == "Simple")) return;
    m_prefs.simpleMode = on;
    if (on) {
        if (m_prefs.workspace != "Simple") m_prefs.proWorkspace = m_prefs.workspace;
        m_pendingWorkspace = "Simple";
        m_showWelcome = false;
        if (!m_prefs.tourDone && m_opt.smokeFrames <= 0) startTour();
    } else {
        std::string back = m_prefs.proWorkspace.empty() || m_prefs.proWorkspace == "Simple" ? "Level" : m_prefs.proWorkspace;
        if (back == "AI" && !aiOn()) back = "Level";
        m_pendingWorkspace = back;
        m_tourStep = -1;
    }
    saveSettings();
}

void EditorApp::startTour() {
    m_tourStep = 0;
    m_tourStepTime = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
}

void EditorApp::simpleNewGame(int templateIndex, const std::string& prompt) {
    if (m_templates.empty()) m_templates = listTemplates(m_paths.templates);
    if (templateIndex < 0 || templateIndex >= static_cast<int>(m_templates.size())) return;
    std::error_code ec;
    std::string base = "MyGame";
    std::string name = base;
    for (int i = 2; fs::exists(m_prefs.projectsDir / pathFromUtf8(name), ec) && i < 1000; ++i) name = base + std::to_string(i);
    m_wizardTemplate = templateIndex;
    m_wizardName = name;
    m_wizardLocation = narrow(m_prefs.projectsDir.wstring());
    createFromTemplate();
    if (!m_wizardError.empty()) {
        toast(ToastLevel::Error, m_wizardError);
        return;
    }
    if (!prompt.empty() && aiOn() && m_session.hasProject()) sendAssistant(prompt);
}

// ------------------------------------------------------------------ top bar --
void EditorApp::drawSimpleTopBar() {
    float s = effectiveScale();
    float h = ImGui::GetFrameHeight() + 14 * s;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10 * s, 7 * s});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
    if (ImGui::BeginViewportSideBar("##SimpleBar", ImGui::GetMainViewport(), ImGuiDir_Up, h, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        int lw = 0, lh = 0;
        if (ImTextureID logo = loadUiTexture(m_paths.resources / "icons" / "fledge_256.png", &lw, &lh)) {
            float sz = ImGui::GetFrameHeight();
            ImGui::Image(logo, {sz, sz});
            ImGui::SameLine();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Fledge");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_session.hasProject() ? ("  /  " + m_session.project().name).c_str() : "  make games, simply.");
        float rightW = ImGui::CalcTextSize(ICON_ARROW_RIGHT "  Pro Mode").x + ImGui::GetFrameHeight() * 2 + 40 * s;
        ImGui::SameLine(ImGui::GetWindowWidth() - rightW);
        if (ImGui::Button(ICON_INFO)) startTour();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the 60-second tour again");
        ImGui::SameLine(0, 4 * s);
        if (ImGui::Button(ICON_GEAR)) m_showSettings = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Settings");
        ImGui::SameLine(0, 4 * s);
        if (ImGui::Button(ICON_ARROW_RIGHT "  Pro Mode")) setSimpleMode(false);
        trackRect("simple.pro");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every tool: scene tree, inspector, code editor, graphics... (Ctrl+Shift+M)");
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ------------------------------------------------------------------ panel --
void EditorApp::drawSimplePanel() {
    PanelDef* pd = panel("Simple");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {18 * effectiveScale(), 16 * effectiveScale()});
    bool visible = ImGui::Begin("Simple", pd ? &pd->open : nullptr, ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleVar();
    if (!visible) {
        ImGui::End();
        return;
    }
    float s = effectiveScale();
    float w = ImGui::GetContentRegionAvail().x;
    m_simpleRight = ImGui::GetWindowPos().x + ImGui::GetWindowWidth();
    const ImVec4 coral(0.93f, 0.48f, 0.38f, 1), green(0.20f, 0.62f, 0.38f, 1), blue(0.27f, 0.47f, 0.80f, 1), grey(0.32f, 0.33f, 0.37f, 1);
    if (m_templates.empty()) m_templates = listTemplates(m_paths.templates);

    auto heading = [&](const char* text) {
        ImGui::PushFont(m_fontLarge);
        ImGui::TextUnformatted(text);
        ImGui::PopFont();
    };
    auto templatePicker = [&](const std::string& prompt) {
        ImGui::BeginGroup();
        float cw = (w - 8 * s) / 2;
        for (int i = 0; i < static_cast<int>(m_templates.size()); ++i) {
            const auto& t = m_templates[static_cast<size_t>(i)];
            ImGui::PushID(i);
            if (i % 2) ImGui::SameLine(0, 8 * s);
            ImGui::BeginGroup();
            if (ImGui::Button(("##tpl" + t.id).c_str(), {cw, 82 * s})) simpleNewGame(i, prompt);
            trackRect("simple.template." + t.id);
            ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(mn, mx, true);
            dl->AddText({mn.x + 10 * s, mn.y + 8 * s}, ImGui::GetColorU32(ImGuiCol_Text), t.name.c_str());
            std::string d = t.description.size() > 90 ? t.description.substr(0, 88) + "..." : t.description;
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {mn.x + 10 * s, mn.y + 30 * s}, ImGui::GetColorU32(ImGuiCol_TextDisabled), d.c_str(), nullptr, cw - 20 * s);
            dl->PopClipRect();
            ImGui::EndGroup();
            ImGui::PopID();
        }
        ImGui::EndGroup();
    };

    bool has = m_session.hasProject();
    bool playing = has && m_session.playing();
    if (!has) {
        heading("What do you want to make?");
        ImGui::Spacing();
        if (aiOn()) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextMultiline("##describe", &m_simplePrompt, {-1, 110 * s});
            trackRect("simple.describe");
            if (m_simplePrompt.empty()) {
                ImVec2 mn = ImGui::GetItemRectMin();
                ImGui::GetWindowDrawList()->AddText({mn.x + 8 * s, mn.y + 6 * s}, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                                    "Describe your game: \"a ball that rolls through a maze\"...");
            }
            if (bigButton(ICON_WAND "  Make it", {-1, 44 * s}, coral, !m_simplePrompt.empty() && !m_aiBusy)) {
                simpleNewGame(0, m_simplePrompt);  // the first (simplest) template, then the AI builds on it
                m_simplePrompt.clear();
            }
            ImGui::Spacing();
            ImGui::TextDisabled("...or start from a template:");
        } else {
            ImGui::TextDisabled("Pick a template to start:");
            trackRect("simple.quick");
        }
        templatePicker("");
    } else {
        heading(m_session.project().name.c_str());
        ImGui::Spacing();
        // ---- 1. Describe (AI) or quick actions (AI off)
        if (aiOn()) {
            ImGui::TextDisabled("DESCRIBE A CHANGE");
            ImGui::InputTextMultiline("##describe", &m_simplePrompt, {-1, 96 * s});
            trackRect("simple.describe");
            if (m_simplePrompt.empty()) {
                ImVec2 mn = ImGui::GetItemRectMin();
                ImGui::GetWindowDrawList()->AddText({mn.x + 8 * s, mn.y + 6 * s}, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                                    "\"three red cubes in a row and a warm light\"");
            }
            bool ready = m_provider != nullptr;
            if (bigButton(m_aiBusy ? ICON_HOURGLASS "  Working..." : ICON_WAND "  Make it", {-1, 40 * s}, coral, ready && !m_aiBusy && !m_simplePrompt.empty() && !playing)) {
                sendAssistant(m_simplePrompt);
                m_simplePrompt.clear();
            }
            if (!ready) {
                ImGui::TextDisabled("Connect an AI provider first:");
                ImGui::SameLine();
                if (ImGui::SmallButton("Settings > AI Providers")) {
                    m_settingsPage = "AI Providers";
                    m_showSettings = true;
                }
            }
            // the last few things the assistant said / did
            std::lock_guard l(m_chatMutex);
            int shown = 0;
            for (auto it = m_chat.rbegin(); it != m_chat.rend() && shown < 4; ++it) {
                if (it->kind == ChatLine::Kind::User) break;
                ++shown;
            }
            for (size_t i = m_chat.size() - static_cast<size_t>(shown); i < m_chat.size(); ++i) {
                const ChatLine& c = m_chat[i];
                ImGui::PushTextWrapPos(0);
                if (c.kind == ChatLine::Kind::Tool) ImGui::TextDisabled("%s %s%s", c.toolError ? ICON_ERROR : ICON_CHECK, c.toolName.c_str(), c.toolDone ? "" : "...");
                else if (c.kind == ChatLine::Kind::Error) ImGui::TextColored({1, 0.45f, 0.45f, 1}, "%s", c.text.c_str());
                else ImGui::TextUnformatted(c.text.substr(0, 400).c_str());
                ImGui::PopTextWrapPos();
            }
        }
        ImGui::Spacing();
        ImGui::TextDisabled("QUICK ACTIONS");
        trackRect("simple.quick");
        {
            struct Q {
                const char* id;
                const char* label;
            };
            const Q qs[] = {{"cube", ICON_CUBE " Cube"},       {"sphere", ICON_CIRCLE " Ball"},      {"light", ICON_LIGHTBULB " Light"},
                            {"ground", ICON_MAP " Ground"},    {"duplicate", ICON_COPY " Copy"},     {"delete", ICON_TRASH " Delete"},
                            {"undo", ICON_UNDO " Undo"},       {"redo", ICON_REDO " Redo"},          {"focus", ICON_CROSSHAIRS " Look at"}};
            float bw = (w - 16 * s) / 3;
            ImGui::BeginDisabled(playing);
            for (int i = 0; i < 9; ++i) {
                if (i % 3) ImGui::SameLine(0, 8 * s);
                std::string id = qs[i].id;
                bool needSel = id == "duplicate" || id == "delete" || id == "focus";
                ImGui::BeginDisabled((needSel && m_session.selection.empty()) || (id == "undo" && !m_session.history().canUndo()) ||
                                     (id == "redo" && !m_session.history().canRedo()));
                if (ImGui::Button(qs[i].label, {bw, 34 * s})) {
                    size_t n = m_session.scene().entities().size();
                    float x = static_cast<float>(static_cast<int>(n % 5) - 2) * 1.6f;
                    auto at = [&](float y) { return Json{{"position", {x, y, 1.5f}}}; };
                    if (id == "cube") m_session.select(m_session.createEntity("Cube", kNullEntity, {{"Transform", at(0.5f)}, {"MeshRenderer", {{"mesh", "builtin:cube"}}}}));
                    else if (id == "sphere") m_session.select(m_session.createEntity("Ball", kNullEntity, {{"Transform", at(0.5f)}, {"MeshRenderer", {{"mesh", "builtin:sphere"}}}}));
                    else if (id == "light") m_session.select(m_session.createEntity("Light", kNullEntity, {{"Transform", at(3.0f)}, {"Light", {{"type", "Point"}}}}));
                    else if (id == "ground")
                        m_session.select(m_session.createEntity("Ground", kNullEntity, {{"Transform", {{"position", {0, 0, 0}}, {"scale", {20, 1, 20}}}}, {"MeshRenderer", {{"mesh", "builtin:plane"}}}}));
                    else if (id == "duplicate") m_session.selection = m_session.duplicate(m_session.selection);
                    else if (id == "delete") (void)m_session.destroyEntities(m_session.selection);
                    else if (id == "undo") m_session.undo();
                    else if (id == "redo") m_session.redo();
                    else if (id == "focus") focusSelection();
                }
                trackRect(std::string("simple.quick.") + id);
                ImGui::EndDisabled();
            }
            ImGui::EndDisabled();
            if (!m_session.selection.empty())
                ImGui::TextDisabled("Selected: %s  (drag the arrows in the view to move it)", m_session.scene().displayName(m_session.selection.front()).c_str());
            else ImGui::TextDisabled("Click something in the view to select it.");
            int pending = m_session.imports().pendingCount();
            std::string storeLabel = pending ? std::format(ICON_DOWNLOAD "  Add from an asset store ({} waiting)", pending) : std::string(ICON_DOWNLOAD "  Add from an asset store");
            if (ImGui::Button(storeLabel.c_str(), {-1, 30 * s})) m_showSimpleStore = true;
            trackRect("simple.store");
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        // ---- 2. Play  3. Share
        float half = (w - 8 * s) / 2;
        if (bigButton(playing ? ICON_STOP "  Stop" : ICON_PLAY "  Play", {half, 52 * s}, playing ? grey : green)) {
            if (playing) {
                (void)m_session.stop();
            } else {
                m_userPlayPending = true;  // user-initiated: the mouse lock may engage
                if (auto st = m_session.play(); !st) toast(ToastLevel::Error, st.error);
            }
        }
        trackRect("simple.play");
        ImGui::SameLine(0, 8 * s);
        std::string shareLabel = ICON_SHARE "  Share";
        float shareProgress = -1;
        if (m_packageRunning) {
            std::lock_guard lock(m_taskMutex);
            for (auto& task : m_tasks)
                if (!task->done && task->name.rfind("Packaging", 0) == 0) {
                    shareProgress = task->progress;
                    shareLabel = task->detail.empty() ? std::string(ICON_HOURGLASS "  Building...") : task->detail;
                    break;
                }
            if (shareLabel.size() > 42) shareLabel.replace(39, std::string::npos, "...");
        }
        if (bigButton(shareLabel.c_str(), {half, 52 * s}, blue, !m_packageRunning && !playing)) startPackage();
        trackRect("simple.share");
        if (m_packageRunning && shareProgress >= 0) ImGui::ProgressBar(shareProgress, {half, 6 * s}, "");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Builds a standalone game folder (.exe) you can zip and send");
        ImGui::BeginDisabled(playing || !has);
        if (ImGui::Button(ICON_SERVER "  Host LAN", {half, 36 * s})) m_commands.execute("play.host");
        ImGui::SameLine(0, 8 * s);
        if (ImGui::Button(ICON_LINK "  Join...", {half, 36 * s})) m_commands.execute("play.join");
        ImGui::EndDisabled();
        if (!m_lastPackageDir.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("Your game is ready in %s - zip that folder and send it.", m_lastPackageDir.c_str());
            ImGui::PopTextWrapPos();
#ifdef _WIN32
            if (ImGui::SmallButton(ICON_FOLDER_OPEN " Open folder")) ShellExecuteW(nullptr, L"open", widen(m_lastPackageDir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
        }
        ImGui::Spacing();
        if (ImGui::Button(ICON_CODE "  Edit code", {half, 0})) {
            setSimpleMode(false);
            m_pendingWorkspace = "Code";
        }
        trackRect("simple.code");
        ImGui::SameLine(0, 8 * s);
        if (ImGui::Button(ICON_SAVE "  Save", {half, 0})) m_commands.execute("file.save");
    }

    // ---- projects strip (bottom)
    float stripH = 92 * s;
    float y = ImGui::GetWindowHeight() - stripH - 8 * s;
    if (ImGui::GetCursorPosY() < y) ImGui::SetCursorPosY(y);
    ImGui::Separator();
    ImGui::TextDisabled("YOUR GAMES");
    trackRect("simple.projects");
    ImGui::BeginChild("##strip", {0, stripH - ImGui::GetTextLineHeightWithSpacing()}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    if (ImGui::Button(ICON_PLUS "##new", {54 * s, 46 * s})) ImGui::OpenPopup("##newgame");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("New game from a template");
    if (ImGui::BeginPopup("##newgame")) {
        ImGui::TextDisabled("Start a new game from:");
        for (int i = 0; i < static_cast<int>(m_templates.size()); ++i)
            if (ImGui::Selectable(m_templates[static_cast<size_t>(i)].name.c_str())) simpleNewGame(i, "");
        ImGui::EndPopup();
    }
    int idx = 0;
    for (const auto& p : m_recent.items()) {
        if (idx++ >= 12) break;
        ImGui::SameLine(0, 8 * s);
        std::string name = narrow(p.stem().wstring());
        bool current = has && m_session.project().file == p;
        ImGui::PushStyleColor(ImGuiCol_Button, current ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
        if (ImGui::Button((name + "##p" + std::to_string(idx)).c_str(), {std::max(110 * s, ImGui::CalcTextSize(name.c_str()).x + 24 * s), 46 * s}) && !current)
            openProject(p);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", narrow(p.wstring()).c_str());
    }
    ImGui::EndChild();
    ImGui::End();
}

// ------------------------------------------------------------------ tour --
void EditorApp::drawTour() {
    if (m_tourStep < 0 || !m_prefs.simpleMode) return;
    const auto& steps = tourSteps(aiOn());
    if (m_tourStep >= static_cast<int>(steps.size())) {
        m_tourStep = -1;
        m_prefs.tourDone = true;
        saveSettings();
        return;
    }
    double t = ImGui::GetTime();
    if (t - m_tourStepTime > kTourStepSeconds) {
        ++m_tourStep;
        m_tourStepTime = t;
        return;
    }
    const TourStep& st = steps[static_cast<size_t>(m_tourStep)];
    float s = effectiveScale();
    auto it = m_uiRects.find(st.target);
    ImVec2 c = it != m_uiRects.end() ? it->second : ImGui::GetMainViewport()->GetCenter();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(t) * 4.0f);
    fg->AddCircle(c, (34 + 6 * pulse) * s, IM_COL32(242, 135, 106, 220), 48, 3 * s);
    ImVec2 pos{std::max(c.x + 50 * s, m_simpleRight + 16 * s), c.y - 20 * s};  // beside the Simple column, never over it
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float bw = 340 * s;
    if (pos.x + bw > vp->WorkPos.x + vp->WorkSize.x) pos.x = c.x - bw - 50 * s;
    pos.y = std::clamp(pos.y, vp->WorkPos.y + 10 * s, vp->WorkPos.y + vp->WorkSize.y - 170 * s);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize({bw, 0});
    ImGui::SetNextWindowFocus();
    ImGui::Begin("##tour", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking);
    ImGui::TextColored(ImVec4(0.95f, 0.53f, 0.42f, 1), "%s", st.title);
    ImGui::SameLine(bw - 60 * s);
    ImGui::TextDisabled("%d / %zu", m_tourStep + 1, steps.size());
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(st.text);
    ImGui::PopTextWrapPos();
    float frac = static_cast<float>((t - m_tourStepTime) / kTourStepSeconds);
    ImGui::ProgressBar(frac, {-1, 3 * s}, "");
    if (ImGui::Button(m_tourStep + 1 == static_cast<int>(steps.size()) ? "Done" : "Next")) {
        ++m_tourStep;
        m_tourStepTime = t;
    }
    trackRect("tour.next");
    ImGui::SameLine();
    if (ImGui::Button("Skip tour")) {
        m_tourStep = -1;
        m_prefs.tourDone = true;
        saveSettings();
    }
    trackRect("tour.skip");
    ImGui::End();
}

} // namespace fledge::editor
