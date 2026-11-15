// Auto-save and crash recovery in the editor GUI (docs/UI.md "Auto-save and recovery"): the per-frame poll that feeds
// EditorSession::pollAutosave (the only caller that may write the real scene file), the "busy" rules that keep auto-save
// away from dialogs, drags and imports, the recovery dialog shown when a project opens with unsaved work from an
// earlier session, and the Settings page.
#include <ctime>
#include <format>

#include <GLFW/glfw3.h>
#include <imgui.h>

#include "editor_app.h"
#include "editor_icons.h"
#include "fledge/core/log.h"

namespace fledge::editor {

namespace {
std::string localTime(int64_t ms, const char* fmt) {
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof buf, fmt, &tm);
    return buf;
}
} // namespace

// Anything the user is in the middle of: a modal / popup, a held mouse button (gizmo, camera or slider drag, text selection),
// an active widget (typing into a field), mouse capture, Simulate, or background work (import / cook / package / script build).
bool EditorApp::autosaveBusy() {
    if (!ImGui::GetCurrentContext()) return false;
    if (m_session.recoveryOffer()) return true;
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return true;
    if (m_drag.active() || m_capture.mode() != CaptureMode::None || m_simulating) return true;
    if (m_cookRunning || m_packageRunning || m_scriptBuildRunning || m_aiBusy) return true;
    ImGuiIO& io = ImGui::GetIO();
    for (int b = 0; b < 5; ++b)
        if (io.MouseDown[b]) return true;
    if (ImGui::IsAnyItemActive() || io.WantTextInput) return true;
    {
        std::lock_guard l(m_taskMutex);
        for (const auto& t : m_tasks)
            if (!t->done) return true;
    }
    return false;
}

void EditorApp::updateAutosave() {
    if (!m_window || !ImGui::GetCurrentContext() || m_shuttingDown || scriptedRun()) return;
    GLFWwindow* gw = m_window->glfwHandle();
    EditorSession::AutosaveEnv env;
    env.busyUi = autosaveBusy();
    env.focused = !gw || m_opt.smokeFrames > 0 || glfwGetWindowAttrib(gw, GLFW_FOCUSED) == GLFW_TRUE;
    env.allowRealSave = true;  // the GUI session only: the headless MCP server never writes the real scene file
    auto r = m_session.pollAutosave(env);
    if (r.autoSaved) {
        m_autoSavedLabel = "Auto-saved " + localTime(wallClockMs(), "%H:%M");
        FLEDGE_LOG_INFO("editor", "{}", m_autoSavedLabel);
    }
    if (!r.error.empty() && r.error != m_lastAutosaveError) {
        m_lastAutosaveError = r.error;
        toast(ToastLevel::Warning, "Auto-save: " + r.error);
    }
    if (r.snapshot || r.autoSaved) m_lastAutosaveError.clear();
}

void EditorApp::drawRecoveryDialog() {
    const auto& offer = m_session.recoveryOffer();
    if (!offer || !m_session.hasProject()) return;
    float s = effectiveScale();
    const char* id = ICON_HISTORY "  Recover unsaved work?###Recovery";
    if (!ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({560 * s, 0}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) return;
    const auto& snap = offer->snapshot;
    ImGui::PushFont(m_fontLarge);
    ImGui::TextUnformatted(m_session.project().name.c_str());
    ImGui::PopFont();
    ImGui::TextWrapped("Fledge found a recovery snapshot of %s that is newer than the saved scene. The editor probably closed or "
                       "crashed before the changes were saved.", snap.scenePath.c_str());
    ImGui::Spacing();
    ImGui::BulletText("Recovered: %s, saved %s", offer->summary.c_str(), localTime(snap.createdMs, "%Y-%m-%d %H:%M:%S").c_str());
    if (offer->saved.exists) {
        size_t savedEntities = offer->saved.json.is_object() && offer->saved.json.contains("entities") && offer->saved.json["entities"].is_array()
                                   ? offer->saved.json["entities"].size() : 0;
        ImGui::BulletText("Saved scene: %zu entit%s, saved %s", savedEntities, savedEntities == 1 ? "y" : "ies",
                          localTime(offer->saved.mtimeMs, "%Y-%m-%d %H:%M:%S").c_str());
    } else {
        ImGui::BulletText("The scene file is missing.");
    }
    ImGui::BulletText("%zu snapshot%s kept", offer->snapshotCount, offer->snapshotCount == 1 ? "" : "s");
    ImGui::Spacing();
    ImGui::TextDisabled("Nothing is restored on its own. Restore opens the snapshot as unsaved changes: save to keep them or close "
                        "without saving to leave the file as it is.");
    ImGui::Separator();
    if (ImGui::Button(ICON_HISTORY "  Restore", {150 * s, 0})) {
        auto st = m_session.restoreRecovery();
        toast(st ? ToastLevel::Success : ToastLevel::Error, st ? "Recovered the unsaved scene. Save it to keep the changes." : "Restore failed: " + st.error);
        ImGui::CloseCurrentPopup();
    } else {
        trackRect("recovery.restore");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FOLDER_OPEN "  Open saved version", {190 * s, 0})) {
            m_session.keepSavedVersion();
            toast(ToastLevel::Info, "Opened the saved scene. The recovery snapshots stay until you save.");
            ImGui::CloseCurrentPopup();
        } else {
            trackRect("recovery.keep");
            ImGui::SameLine();
            if (ImGui::Button(ICON_TRASH "  Discard", {120 * s, 0})) {
                m_session.discardRecovery();
                toast(ToastLevel::Info, "Recovery snapshots deleted");
                ImGui::CloseCurrentPopup();
            }
            trackRect("recovery.discard");
        }
    }
    ImGui::EndPopup();
}

bool EditorApp::drawAutosaveSettings() {
    float s = effectiveScale();
    auto& a = m_session.autosave;
    bool changed = false;
    ImGui::SeparatorText("Recovery snapshots");
    changed |= ImGui::Checkbox("Keep recovery snapshots of unsaved changes", &a.recovery);
    ImGui::BeginDisabled(!a.recovery);
    ImGui::SetNextItemWidth(220 * s);
    changed |= ImGui::SliderInt("Snapshot every", &a.recoverySeconds, 5, 600, "%d s");
    ImGui::SetNextItemWidth(220 * s);
    changed |= ImGui::SliderInt("Snapshots kept", &a.recoveryKeep, 1, 20);
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("While the scene has unsaved changes, a copy is written to <project>/Intermediate/Recovery/ (never over your "
                        "scene files, never during Play). Saving or closing cleanly deletes them; after a crash the next open offers to "
                        "restore the work.");
    ImGui::PopTextWrapPos();
    ImGui::SeparatorText("Auto-save scenes");
    const char* modes[] = {"Off", "After a delay", "On focus loss"};
    int mode = static_cast<int>(a.mode);
    ImGui::SetNextItemWidth(220 * s);
    if (ImGui::Combo("Auto-save scenes", &mode, modes, 3)) {
        a.mode = static_cast<AutoSaveMode>(mode);
        changed = true;
    }
    ImGui::BeginDisabled(a.mode != AutoSaveMode::AfterDelay);
    ImGui::SetNextItemWidth(220 * s);
    changed |= ImGui::SliderInt("Delay after the last change", &a.delaySeconds, 5, 600, "%d s");
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Saves the scene file when it has unsaved changes. Not during Play, and not while a dialog, a drag, a gizmo "
                        "operation or an import is in progress. The file as you last saved it is kept as a backup: File > Revert Last "
                        "Auto-save. Off by default. Only this editor window auto-saves; an AI working through MCP never does it for you.");
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(!m_session.hasAutoSaveBackup() || m_session.playing());
    if (ImGui::Button(ICON_HISTORY "  Revert last auto-save")) m_commands.execute("file.revert_autosave");
    ImGui::EndDisabled();
    return changed;
}

} // namespace fledge::editor
