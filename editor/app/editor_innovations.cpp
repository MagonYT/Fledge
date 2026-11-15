// Signature features UI (docs/INNOVATIONS.md): Time Machine scrubber, Ask the Scene ("Why?"), Playtest Bots.
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
#include "editor_icons.h"
#include "fledge/editor/ask_scene.h"
#include "fledge/editor/playtest_bot.h"
#include "fledge/editor/capsule.h"
#include "fledge/editor/perf_advisor.h"
#include "fledge/editor/code_services.h"
#include "fledge/packager/packager.h"
#include "fledge/assets/image.h"

namespace fledge::editor {

// ---------------------------------------------------------------- Time Machine --
void EditorApp::drawTimeMachineBar(ImVec2 origin, ImVec2 size) {
    TimeMachine& tm = m_session.timeMachine;
    if (!tm.enabled) return;
    float s = effectiveScale();
    float w = std::min(size.x - 24 * s, 760 * s);
    ImVec2 pos{origin.x + (size.x - w) * 0.5f, origin.y + size.y - 74 * s};
    ImGui::SetCursorScreenPos(pos);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled({pos.x - 8 * s, pos.y - 6 * s}, {pos.x + w + 8 * s, pos.y + 56 * s}, IM_COL32(20, 22, 27, 215), 8 * s);
    ImGui::PushID("##timemachine");
    ImGui::BeginGroup();
    auto fmtT = [](int64_t step) { return std::format("{:.2f}s", static_cast<double>(step) * TimeMachine::kStep); };
    int64_t first = tm.firstStep(), last = std::max(tm.lastStep(), first + 1), cur = tm.currentStep();
    bool paused = m_session.paused();
    ImGui::TextColored(ImVec4(0.95f, 0.53f, 0.42f, 1), ICON_HISTORY " Time Machine");
    ImGui::SameLine();
    ImGui::TextDisabled("%s / %s   %zu snapshots, %.1f MB%s", fmtT(cur).c_str(), fmtT(tm.lastStep()).c_str(), tm.snapshotCount(),
                        static_cast<double>(tm.bytes()) / (1024.0 * 1024.0), tm.branches() ? std::format("   {} branch(es)", tm.branches()).c_str() : "");
    if (ImGui::Button(ICON_UNDO " 1s")) tm.seek(m_session, std::max(first, cur - 60));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rewind one second (pauses)");
    ImGui::SameLine();
    if (ImGui::Button(paused ? ICON_PLAY : ICON_PAUSE)) m_session.setPaused(!paused);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(paused && !tm.atEnd() ? "Continue from here (starts a new branch with the code loaded now)" : "Pause / resume");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(w - 330 * s);
    int64_t v = cur;
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.95f, 0.53f, 0.42f, 1));
    if (ImGui::SliderScalar("##scrub", ImGuiDataType_S64, &v, &first, &last, "")) tm.seek(m_session, std::clamp(v, first, tm.lastStep()));
    ImGui::PopStyleColor();
    trackRect("tm.scrub");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to scrub: the game is restored from snapshots and the recorded inputs are replayed");
    ImGui::SameLine();
    ImGui::BeginDisabled(!paused || tm.atEnd());
    if (ImGui::Button(ICON_BRANCH " Branch here")) {
        tm.branchHere();
        m_session.setPaused(false);
        toast(ToastLevel::Info, "Branched at " + fmtT(cur) + ": playing on with the code loaded now");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Edit a script (it hot reloads), then branch: the future is dropped and play continues from this moment with the new code");
    ImGui::EndGroup();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || ImGui::IsItemActive()) m_overlayHovered = true;
    ImGui::PopID();
}

// ---------------------------------------------------------------- Ask the Scene --
void EditorApp::drawWhyPopup() {
    if (!m_whyOpen) return;
    float s = effectiveScale();
    ImGui::SetNextWindowSize({560 * s, 460 * s}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (!ImGui::Begin(ICON_LIGHTBULB "  Ask the Scene: Why?###why", &m_whyOpen, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    if (!m_session.scene().isValid(m_whyEntity)) {
        ImGui::TextDisabled("The entity no longer exists.");
        ImGui::End();
        return;
    }
    WhyReport r = explainEntity(m_session, m_whyEntity);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(r.title.c_str());
    trackRect("why.title");
    for (auto& [h, lines] : r.sections) {
        ImGui::SeparatorText(h.c_str());
        for (auto& l : lines) ImGui::BulletText("%s", l.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::TextDisabled("Built from engine data only: components, materials, lights, scripts, undo history.");
    if (aiOn()) {  // the AI only rephrases the facts, and only when AI features are on
        if (ImGui::Button(ICON_COMMENT " Explain in plain words (AI)")) {
            sendAssistant("Explain to a beginner, in a few sentences, why this object looks and behaves the way it does. Use only these engine facts:\n\n" +
                          r.toText());
            if (PanelDef* p = panel("AI Assistant")) p->open = true;
        }
        trackRect("why.ai");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy")) ImGui::SetClipboardText(r.toText().c_str());
    ImGui::End();
}

// ---------------------------------------------------------------- Edit-in-Play with Keep --
void EditorApp::drawKeepDialog() {
    const auto& tw = m_session.lastPlayTweaks();
    if (tw.empty() || m_session.playing()) return;
    if (m_keepChecks.size() != tw.size()) m_keepChecks.assign(tw.size(), 1);
    float s = effectiveScale();
    ImGui::SetNextWindowSize({560 * s, 0}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    bool open = true;
    if (!ImGui::Begin(ICON_CHECK "  Keep play-mode changes?###keep", &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("You tweaked these while playing. The scene was restored; tick what to keep (one undo step).");
    trackRect("keep.window");
    for (size_t i = 0; i < tw.size(); ++i) {
        const auto& t = tw[i];
        ImGui::PushID(static_cast<int>(i));
        bool on = m_keepChecks[i] != 0;
        if (ImGui::Checkbox("##k", &on)) m_keepChecks[i] = on ? 1 : 0;
        ImGui::SameLine();
        ImGui::Text("%s  %s.%s", t.entityName.c_str(), t.component.c_str(), t.property.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s  ->  %s", t.before.dump().substr(0, 40).c_str(), t.after.dump().substr(0, 40).c_str());
        ImGui::PopID();
    }
    if (ImGui::Button(ICON_CHECK " Keep selected")) {
        std::vector<size_t> idx;
        for (size_t i = 0; i < tw.size(); ++i)
            if (m_keepChecks[i]) idx.push_back(i);
        if (auto st = m_session.keepPlayTweaks(idx); !st) toast(ToastLevel::Error, st.error);
        else toast(ToastLevel::Success, std::format("Kept {} change{} (Ctrl+Z undoes)", idx.size(), idx.size() == 1 ? "" : "s"));
        m_session.clearPlayTweaks();
        m_keepChecks.clear();
    }
    trackRect("keep.apply");
    ImGui::SameLine();
    if (ImGui::Button("Discard all") || !open) {
        m_session.clearPlayTweaks();
        m_keepChecks.clear();
    }
    ImGui::End();
}

// ---------------------------------------------------------------- Make it run well --
void EditorApp::drawPerfWindow() {
    if (!m_showPerf) return;
    float s = effectiveScale();
    ImGui::SetNextWindowSize({580 * s, 440 * s}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (!ImGui::Begin(ICON_GAUGE "  Make it run well###perf", &m_showPerf, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    if (m_perfRetestAt >= 0 && m_frame >= m_perfRetestAt) {  // re-test: frame time after the fixes settled
        m_perfAfter = m_frameMs;
        m_perfRetestAt = -1;
    }
    ImGui::BeginDisabled(!m_session.hasProject());
    if (ImGui::Button(ICON_SEARCH " Analyze", {140 * s, 0})) {
        m_perfReport = std::make_shared<PerfReport>(analyzePerformance(m_session, m_camPos));
        m_perfBefore = m_frameMs;
        m_perfAfter = 0;
    }
    trackRect("perf.analyze");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Editor frame time now: %.2f ms", m_frameMs);
    if (m_perfReport) {
        const PerfReport& r = *m_perfReport;
        ImGui::Text("%d meshes, %llu triangles, %d lights (%d with shadows)", r.meshes, static_cast<unsigned long long>(r.triangles), r.lights, r.shadowLights);
        if (r.suggestions.empty()) ImGui::TextColored({0.4f, 0.85f, 0.5f, 1}, ICON_CHECK " Nothing to fix: the scene looks lean.");
        bool anyFix = false;
        for (size_t i = 0; i < r.suggestions.size(); ++i) {
            const auto& sg = r.suggestions[i];
            anyFix |= sg.fixable;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextColored(sg.fixable ? ImVec4(0.4f, 0.85f, 0.5f, 1) : ImVec4(1, 0.78f, 0.3f, 1), "%s %s", sg.fixable ? ICON_WAND : ICON_LIGHTBULB, sg.kind.c_str());
            ImGui::SameLine();
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(sg.text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopID();
        }
        ImGui::BeginDisabled(!anyFix || m_session.playing());
        if (ImGui::Button(ICON_WAND " Apply fixes and re-test")) {
            int n = applyPerformanceFixes(m_session, r);
            toast(ToastLevel::Success, std::format("Changed {} object{} (one undo step)", n, n == 1 ? "" : "s"));
            m_perfRetestAt = m_frame + 90;
            m_perfReport = std::make_shared<PerfReport>(analyzePerformance(m_session, m_camPos));
        }
        ImGui::EndDisabled();
        if (m_perfAfter > 0) ImGui::Text("Re-test: %.2f ms -> %.2f ms per editor frame", m_perfBefore, m_perfAfter);
        else if (m_perfRetestAt >= 0) ImGui::TextDisabled("Re-testing...");
    }
    ImGui::End();
}

// ---------------------------------------------------------------- Capsule + Ship --
void EditorApp::startShip() {
    if (!m_session.hasProject() || m_shipRunning) return;
    if (m_session.playing()) {
        toast(ToastLevel::Warning, "Stop play mode before shipping");
        return;
    }
    if (m_session.dirty)
        if (auto st = m_session.saveScene(); !st) {
            toast(ToastLevel::Error, "Saving the scene failed: " + st.error);
            return;
        }
    const Project& proj = m_session.project();
    fs::path shipDir = proj.buildDir() / "Ship";
    std::error_code ec;
    fs::create_directories(shipDir, ec);
    // store-page screenshot from the editor view (main thread: needs the GPU)
    if (auto img = captureViewport(1280, 720)) writeFileBytes(shipDir / "store_screenshot_1280x720.png", encodePng(*img));
    m_shipRunning = true;
    m_shipResult.clear();
    PackageOptions opt;
    opt.projectFile = proj.file;
    opt.runtimeExe = m_session.runtimeExe();
    opt.moduleDir = m_session.toolsDir;
    fs::path root = proj.rootDir();
    std::string name = proj.gameName;
    runInBackground("Shipping " + name, [this, opt, shipDir, root, name](BackgroundTask& t) {
        auto setDetail = [&](const std::string& d) {
            std::lock_guard l(m_taskMutex);
            t.detail = d;
        };
        std::string log;
        t.progress = 0.05f;
        setDetail("packaging the game");
        auto r = packageProject(opt, [&](const std::string& msg, float fraction) {
            setDetail(msg);
            if (fraction >= 0) t.progress = std::min(0.6f, fraction * 0.6f);
        });
        if (!r) log += "Package failed: " + r.error() + "\n";
        else log += "Game: " + narrow(r->executable.wstring()) + "\n";
        t.progress = 0.6f;
        if (r) {
            setDetail("zipping");
            fs::path zip = shipDir / (name + ".zip");
            std::error_code ec2;
            fs::remove(zip, ec2);
            std::string cmd = "powershell.exe -NoProfile -NonInteractive -Command \"Compress-Archive -Path '" + narrow(r->outputDir.wstring()) +
                              "' -DestinationPath '" + narrow(zip.wstring()) + "'\"";
            auto z = code::runCapture(cmd, shipDir, 300000);
            log += z.exitCode == 0 && fs::exists(zip, ec2) ? "Zip: " + narrow(zip.wstring()) + "\n" : "Zip failed: " + z.output + "\n";
        }
        t.progress = 0.85f;
        setDetail("writing the capsule");
        auto c = exportCapsule(root, shipDir / (name + ".fledgecapsule"));
        log += c ? std::format("Capsule: {} ({} files, hash-verified)\n", narrow((shipDir / (name + ".fledgecapsule")).wstring()), c->files)
                 : "Capsule failed: " + c.error() + "\n";
        log += "Screenshot: " + narrow((shipDir / "store_screenshot_1280x720.png").wstring()) + "\n";
        log += "Installer: not built (stub - needs NSIS on this machine; see docs/INNOVATIONS.md)\n";
        m_mainQueue.post([this, log, ok = r.ok()] {
            m_shipRunning = false;
            m_shipResult = log;
            toast(ok ? ToastLevel::Success : ToastLevel::Error, ok ? "Shipped: Build/Ship has the game, zip, capsule and screenshot" : "Ship failed (see the Ship window)");
        });
        return log;
    });
}

void EditorApp::drawShipWindow() {
    if (!m_showShip) return;
    float s = effectiveScale();
    ImGui::SetNextWindowSize({560 * s, 420 * s}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (!ImGui::Begin(ICON_SHARE "  Ship & Capsule###ship", &m_showShip, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    bool has = m_session.hasProject();
    ImGui::SeparatorText("Ship (one button)");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Builds the standalone game, zips it, writes a capsule of the project and a 1280x720 store-page screenshot "
                        "into <project>/Build/Ship. (A per-game installer is not built yet.)");
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(!has || m_shipRunning || m_session.playing());
    if (ImGui::Button(m_shipRunning ? ICON_HOURGLASS " Shipping..." : ICON_SHARE " Ship", {160 * s, 0})) startShip();
    ImGui::EndDisabled();
    if (!m_shipResult.empty()) {
        ImGui::TextUnformatted(m_shipResult.c_str());
#ifdef _WIN32
        if (has && ImGui::SmallButton(ICON_FOLDER_OPEN " Open Build/Ship")) {
            fs::path dir = m_session.project().buildDir() / "Ship";
            ShellExecuteW(nullptr, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
#endif
    }
    ImGui::SeparatorText("Capsule (.fledgecapsule)");
    ImGui::BeginDisabled(!has);
    if (ImGui::Button(ICON_BOX " Export capsule")) {
        fs::path out = m_session.project().buildDir() / (m_session.project().name + ".fledgecapsule");
        auto r = exportCapsule(m_session.project().rootDir(), out);
        if (r) toast(ToastLevel::Success, std::format("Capsule written: {} ({} files, {:.1f} MB)", narrow(out.wstring()), r->files, static_cast<double>(r->bytes) / 1048576.0));
        else toast(ToastLevel::Error, r.error());
    }
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(-90 * s);
    ImGui::InputTextWithHint("##capin", "path to a .fledgecapsule to import", &m_capsuleImport);
    ImGui::SameLine();
    if (ImGui::Button("Import")) {
        std::string path = m_capsuleImport;
        std::erase(path, '"');
        auto r = importCapsule(pathFromUtf8(path), m_prefs.projectsDir);
        if (r) openProject(*r);
        else toast(ToastLevel::Error, "Import refused: " + r.error());
    }
    ImGui::TextDisabled("Import verifies every file's SHA-256 first, then opens a new folder in %s", narrow(m_prefs.projectsDir.wstring()).c_str());
    ImGui::End();
}

// ---------------------------------------------------------------- Playtest Bots --
void EditorApp::drawPlaytestBot() {
    if (!m_showBot) return;
    float s = effectiveScale();
    ImGui::SetNextWindowSize({560 * s, 470 * s}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (!ImGui::Begin(ICON_RUNNING "  Playtest Bot###bot", &m_showBot, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("A bot plays your game headlessly at full speed (WASD / jump / sprint / look, seeded), looking for softlocks, falls, "
                        "script errors and perf spikes. Every issue opens in the Time Machine at that moment.");
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(160 * s);
    ImGui::SliderFloat("Simulated seconds", &m_botSeconds, 5, 120, "%.0f s");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * s);
    ImGui::InputInt("Seed", &m_botSeed);
    if (aiOn()) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##goal", "Goal (optional): e.g. \"jump onto every platform\"", &m_botGoal);
        trackRect("bot.goal");
    }
    ImGui::BeginDisabled(!m_session.hasProject());
    if (ImGui::Button(ICON_PLAY " Run bot", {160 * s, 0})) {
        BotOptions o;
        o.seconds = m_botSeconds;
        o.seed = static_cast<uint32_t>(m_botSeed);
        if (aiOn()) o.goal = m_botGoal;
        m_botReport = std::make_shared<BotReport>(runPlaytestBot(m_session, o));
        if (!m_botReport->error.empty()) toast(ToastLevel::Error, "Bot: " + m_botReport->error);
    }
    trackRect("bot.run");
    ImGui::EndDisabled();
    if (m_botReport && m_botReport->ran) {
        const BotReport& r = *m_botReport;
        ImGui::SeparatorText("Report");
        ImGui::Text("Watched: %s   distance %.1f m   %d cells (2 m) visited   %.1f s simulated in %.2f s", r.subject.c_str(), r.distance, r.cellsVisited,
                    static_cast<double>(r.lastStep - r.firstStep) * TimeMachine::kStep, r.wallSeconds);
        for (auto& n : r.notes) ImGui::TextDisabled("%s", n.c_str());
        if (r.issues.empty()) ImGui::TextColored({0.4f, 0.85f, 0.5f, 1}, ICON_CHECK " No issues found");
        ImGui::BeginChild("##issues");
        for (size_t i = 0; i < r.issues.size(); ++i) {
            const BotIssue& is = r.issues[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextColored(is.kind == "script error" || is.kind == "fall" ? ImVec4(1, 0.45f, 0.45f, 1) : ImVec4(1, 0.78f, 0.3f, 1), "%s", is.kind.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("at %.2f s", static_cast<double>(is.step) * TimeMachine::kStep);
            ImGui::SameLine();
            ImGui::BeginDisabled(!m_session.playing());
            if (ImGui::SmallButton(ICON_HISTORY " Open in Time Machine")) {
                if (!m_session.timeMachine.seek(m_session, is.step)) toast(ToastLevel::Warning, "That moment is no longer in the Time Machine history");
            }
            ImGui::EndDisabled();
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(is.detail.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace fledge::editor
