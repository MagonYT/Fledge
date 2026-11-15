// Editions in the editor UI (docs/EDITIONS.md): the "this project needs Fledge Advanced" dialog shown when a project is
// declined, and the one "available in Fledge Advanced" hint. Advanced-only panels and menu items are simply not
// registered in Fledge, so there is no greyed-out clutter to draw here.
#include <cstdio>
#include <format>

#include <imgui.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "editor_app.h"
#include "editor_icons.h"
#include "fledge/core/log.h"
#include "fledge/core/version.h"

namespace fledge::editor {

namespace {
void shellOpenPath(const fs::path& p) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)p;
#endif
}
} // namespace

void EditorApp::showDecline(const EditionCheck& check) {
    m_decline = check;
    m_showDecline = true;
    FLEDGE_LOG_WARN("editor", "{} - {}", narrow(check.projectFile.wstring()), check.summary());
    // Scripted runs (tests, --smoke-test) read this line; nothing else is printed for a decline.
    std::printf("PROJECT-DECLINED edition=%s required=%s missing=%zu: %s\n", editionId(currentEdition()), editionId(check.required),
                check.missing.size(), check.summary().c_str());
    std::fflush(stdout);
}

void EditorApp::drawDecline() {
    if (!m_showDecline || !m_decline) return;
    float s = effectiveScale();
    const char* id = ICON_INFO "  Project not opened###EditionDecline";
    if (!ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({560 * s, 0}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(id, &m_showDecline, ImGuiWindowFlags_NoSavedSettings)) {
        const EditionCheck& c = *m_decline;
        ImGui::PushFont(m_fontLarge);
        ImGui::TextUnformatted(c.title().c_str());
        ImGui::PopFont();
        ImGui::TextDisabled("%s", narrow(c.projectFile.wstring()).c_str());
        ImGui::Separator();
        ImGui::TextWrapped("%s %s cannot edit this project. It uses:", editionName(currentEdition()), FLEDGE_VERSION_STRING);
        for (const auto& m : c.missing) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s  (%s)", m.title.c_str(), m.reason.c_str());
        }
        ImGui::Spacing();
        ImGui::TextWrapped("To keep it safe, the project was not opened and nothing in its folder was changed: no migration, no lock or "
                           "temporary files, no save on exit.");
        if (currentEdition() == Edition::Standard)
            ImGui::TextWrapped("Fledge Advanced opens it. Projects made with Fledge always open in Fledge Advanced; a project that "
                               "uses Advanced features opens only there.");
        ImGui::Spacing();
        if (ImGui::Button(ICON_FOLDER_OPEN "  Open the project folder")) shellOpenPath(c.projectFile.parent_path());
        ImGui::SameLine();
        if (ImGui::Button(ICON_INFO "  Learn about Fledge Advanced")) shellOpenPath(m_paths.docs / "EDITIONS.md");
        ImGui::SameLine();
        if (ImGui::Button("Close")) m_showDecline = false;
        if (!m_showDecline) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void EditorApp::drawAdvancedHint() {
    // One quiet line in the About box and Project Settings; never a popup, never repeated nags.
    if (currentEdition() != Edition::Standard) return;
    ImGui::TextDisabled("Weapons, ballistics and fluid movement are in Fledge Advanced; vehicle damage, replays and more are planned (docs/EDITIONS.md).");
}

} // namespace fledge::editor
