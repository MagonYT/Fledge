// "Migrate all settings to Fledge Advanced": Settings > Migrate Settings, File menu / command palette (file.migrate_settings), the
// first-launch banner, the preview / result window and the live reload after a migration. The rules live in editor/core
// (settings_migration.h); this file is UI. docs/MIGRATING_SETTINGS.md.
#include <format>

#include <imgui.h>

#include "editor_app.h"
#include "editor_code.h"
#include "editor_icons.h"
#include "fledge/core/features.h"
#include "fledge/core/log.h"

namespace fledge::editor {

namespace {
ImVec4 actionColor(MigrationAction a) {
    switch (a) {
        case MigrationAction::Copy:
        case MigrationAction::Merge: return {0.35f, 0.8f, 0.45f, 1};
        case MigrationAction::Overwrite: return {1.0f, 0.75f, 0.3f, 1};
        case MigrationAction::Error: return {1.0f, 0.45f, 0.4f, 1};
        default: return {0.6f, 0.6f, 0.6f, 1};
    }
}

std::string joinNames(const std::vector<std::string>& v, size_t max = 8) {
    std::string s;
    for (size_t i = 0; i < v.size() && i < max; ++i) s += (i ? ", " : "") + v[i];
    if (v.size() > max) s += std::format(" (+{} more)", v.size() - max);
    return s;
}
} // namespace

void EditorApp::initMigrationOffer() {
    m_migEndpoints = migrationEndpointsForThisProcess(m_session.customSettingsDir);
    if (currentEdition() != Edition::Advanced || scriptedRun()) return;
    if (m_prefs.migrationOffer == "never" || m_prefs.migrationOffer == "done") return;
    std::error_code ec;
    bool sourceHasSettings = !m_migEndpoints.source.empty() && fs::is_regular_file(osPath(m_migEndpoints.source / "editor_settings.json"), ec);
    // First launch (no Fledge Advanced settings yet), or "Not now" last time.
    m_migrationBanner = sourceHasSettings && (m_firstRun ? migrationOfferApplies(m_migEndpoints) : m_prefs.migrationOffer == "later");
}

void EditorApp::openMigrationWindow(bool fromOffer) {
    m_migEndpoints = migrationEndpointsForThisProcess(m_session.customSettingsDir);
    m_migOptions = {};
    // From the first-launch offer Fledge Advanced has nothing of the user's yet (only what this session wrote, e.g. the window
    // layout), so Fledge's settings win; from Settings the safe merge is the default.
    if (fromOffer) m_migOptions.mode = MigrationMode::Overwrite;
    m_migResult.reset();
    m_showMigration = true;
    m_migrationBanner = false;
    refreshMigrationPreview();
}

void EditorApp::refreshMigrationPreview() {
    if (m_migEndpoints.source.empty()) {
        m_migPlan.reset();
        return;
    }
    m_migPlan = std::make_unique<MigrationResult>(planSettingsMigration(m_migEndpoints.source, m_migEndpoints.destination, m_migOptions));
}

void EditorApp::drawMigrationSettingsPage() {
    float s = effectiveScale();
    ImGui::PushTextWrapPos(0);
    if (currentEdition() != Edition::Advanced) {
        ImGui::SeparatorText("Fledge Advanced");
        ImGui::TextDisabled("%s Moving to Fledge Advanced? Open Fledge Advanced and use Settings > Migrate Settings there: it copies these "
                            "settings over and never changes this Fledge's settings.", ICON_INFO);
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::SeparatorText("From Fledge to Fledge Advanced");
    ImGui::TextUnformatted("Copies your Fledge editor settings into Fledge Advanced: AI provider settings (custom endpoints, presets), AI "
                           "control and MCP permissions, theme and UI preferences, default projects folder, recent projects, auto-save, "
                           "asset store import, script settings, window and workspace layouts, Code Editor settings.");
    ImGui::TextDisabled("Fledge's folder is only read. Logs, caches, downloads and edition-specific settings (Simple / Pro Mode) are not "
                        "copied. Every changed file is backed up first. You see a preview before anything is written.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy({0, 4 * s});
    if (ImGui::Button(ICON_FILES "  Migrate all settings to Fledge Advanced...")) openMigrationWindow(false);
    trackRect("migrate.open");
    ImGui::Dummy({0, 4 * s});
    MigrationEndpoints e = migrationEndpointsForThisProcess(m_session.customSettingsDir);
    ImGui::TextDisabled("From: %s", e.source.empty() ? "(not available)" : narrow(e.source.wstring()).c_str());
    ImGui::TextDisabled("To:   %s%s", narrow(e.destination.wstring()).c_str(), m_paths.portable ? "  (portable install)" : "");
    if (e.source.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s %s", ICON_INFO, e.note.c_str());
        ImGui::PopTextWrapPos();
    }
}

void EditorApp::drawMigrationBanner() {
    if (!m_migrationBanner || m_showMigration) return;
    float s = effectiveScale();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 40 * s}, ImGuiCond_Always, {0.5f, 0});
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking |
                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##MigrationOffer", nullptr, fl)) {
        ImGui::Text("%s  Welcome to Fledge Advanced. Bring your Fledge settings over (AI providers, theme, layouts, recent projects)?", ICON_FILES);
        if (ImGui::Button("Migrate...")) {
            openMigrationWindow(true);
        }
        trackRect("migrate.offer.migrate");
        ImGui::SameLine();
        if (ImGui::Button("Not now")) {
            m_migrationBanner = false;
            m_prefs.migrationOffer = "later";
            saveSettings();
        }
        trackRect("migrate.offer.later");
        ImGui::SameLine();
        if (ImGui::Button("Don't ask again")) {
            m_migrationBanner = false;
            m_prefs.migrationOffer = "never";
            saveSettings();
        }
        trackRect("migrate.offer.never");
        ImGui::SameLine();
        ImGui::TextDisabled("Later: Settings > Migrate Settings");
    }
    ImGui::End();
}

void EditorApp::drawMigrationWindow() {
    if (!m_showMigration) return;
    float s = effectiveScale();
    ImGui::SetNextWindowSize({720 * s, 560 * s}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (!ImGui::Begin(ICON_FILES "  Migrate Settings to Fledge Advanced###MigrateSettings", &m_showMigration,
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("From: %s", m_migEndpoints.source.empty() ? "(not available)" : narrow(m_migEndpoints.source.wstring()).c_str());
    ImGui::TextDisabled("To:   %s", narrow(m_migEndpoints.destination.wstring()).c_str());
    if (m_migEndpoints.source.empty()) {
        ImGui::TextColored({1, 0.75f, 0.3f, 1}, "%s %s", ICON_WARNING, m_migEndpoints.note.c_str());
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }
    if (m_migResult) {  // ---- result
        const MigrationResult& r = *m_migResult;
        ImGui::SeparatorText("Result");
        ImGui::TextColored(r.ok() ? ImVec4{0.35f, 0.85f, 0.45f, 1} : ImVec4{1, 0.45f, 0.4f, 1}, "%s %s", r.ok() ? ICON_CHECK : ICON_ERROR,
                           r.summary().c_str());
        trackRect("migrate.result");
        if (!r.backupDir.empty()) ImGui::TextDisabled("Backups of the replaced files: %s", narrow(r.backupDir.wstring()).c_str());
        for (auto& e : r.errors) ImGui::TextColored({1, 0.45f, 0.4f, 1}, "%s %s", ICON_ERROR, e.c_str());
        if (r.changesSomething())
            ImGui::TextUnformatted(ICON_INFO " The settings were applied in this editor. Restart the editor if anything looks off (fonts and the "
                                   "docked layout are fully rebuilt on the next start).");
        ImGui::PopTextWrapPos();
        if (ImGui::Button(ICON_COPY "  Copy report")) ImGui::SetClipboardText(r.text().c_str());
        ImGui::SameLine();
        if (ImGui::Button("Close")) m_showMigration = false;
        ImGui::BeginChild("items", {0, 0}, ImGuiChildFlags_Borders);
        for (auto& i : r.items) {
            ImGui::TextColored(actionColor(i.action), "%-9s", toString(i.action));
            ImGui::SameLine();
            ImGui::Text("%s  %s", i.path.c_str(), i.detail.c_str());
        }
        ImGui::EndChild();
        ImGui::End();
        return;
    }
    // ---- options + preview
    bool changed = false;
    ImGui::SeparatorText("How");
    int mode = m_migOptions.mode == MigrationMode::Overwrite ? 1 : 0;
    changed |= ImGui::RadioButton("Merge: only add what Fledge Advanced is missing (keeps its own settings)", &mode, 0);
    changed |= ImGui::RadioButton("Overwrite: Fledge's settings win (replaced files are backed up first)", &mode, 1);
    m_migOptions.mode = mode ? MigrationMode::Overwrite : MigrationMode::Merge;
    ImGui::SeparatorText("What");
    changed |= ImGui::Checkbox("AI keys: key-like values in the AI provider settings (same Windows user, same machine)", &m_migOptions.includeAiKeys);
    trackRect("migrate.keys");
    ImGui::TextDisabled("%s", kMigrationKeysNotice);
    changed |= ImGui::Checkbox("AI control and MCP permissions", &m_migOptions.includeAiPermissions);
    changed |= ImGui::Checkbox("Simple / Pro Mode choice (off: Fledge Advanced keeps starting in Pro Mode)", &m_migOptions.includeEditionMode);
    ImGui::PopTextWrapPos();
    if (changed) refreshMigrationPreview();
    ImGui::SeparatorText("Preview");
    if (!m_migPlan) refreshMigrationPreview();
    const MigrationResult& p = *m_migPlan;
    ImGui::TextUnformatted(p.summary().c_str());
    trackRect("migrate.preview");
    bool canRun = !p.refused && p.changesSomething();
    ImGui::BeginDisabled(!canRun);
    if (ImGui::Button(ICON_CHECK "  Migrate now")) {
        if (auto st = checkMigrationEndpoints(p, m_migEndpoints); !st) {
            toast(ToastLevel::Error, "Migration refused: " + st.error);
        } else {
            m_migResult = std::make_unique<MigrationResult>(runSettingsMigration(p, m_migOptions));
            if (m_migResult->changesSomething()) m_migrationReloadPending = true;
            if (m_migResult->ok()) m_prefs.migrationOffer = "done";
            toast(m_migResult->ok() ? ToastLevel::Success : ToastLevel::Error, m_migResult->summary());
        }
    }
    ImGui::EndDisabled();
    trackRect("migrate.run");
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) m_showMigration = false;
    if (!canRun && !p.refused) {
        ImGui::SameLine();
        ImGui::TextDisabled("Nothing to change.");
    }
    ImGui::BeginChild("items", {0, 0}, ImGuiChildFlags_Borders);
    for (auto& e : p.errors) ImGui::TextColored({1, 0.45f, 0.4f, 1}, "%s %s", ICON_ERROR, e.c_str());
    if (ImGui::BeginTable("plan", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 80 * s);
        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthFixed, 200 * s);
        ImGui::TableSetupColumn("Details");
        ImGui::TableHeadersRow();
        for (auto& i : p.items) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(actionColor(i.action), "%s", toString(i.action));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(i.path.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i.what.c_str());
            ImGui::TableNextColumn();
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(i.detail.c_str());
            if (!i.added.empty()) ImGui::TextDisabled("adds: %s", joinNames(i.added).c_str());
            if (!i.replaced.empty()) ImGui::TextDisabled("replaces: %s", joinNames(i.replaced).c_str());
            if (!i.kept.empty()) ImGui::TextDisabled("keeps Fledge Advanced's: %s", joinNames(i.kept).c_str());
            if (!i.withheld.empty()) ImGui::TextDisabled("not copied: %s", joinNames(i.withheld).c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndTable();
    }
    for (auto& n : p.notes) ImGui::TextDisabled("%s %s", ICON_INFO, n.c_str());
    ImGui::EndChild();
    ImGui::End();
}

// Called before ImGui::NewFrame (beginFrameWork) after a migration wrote the destination folder: the in-memory settings are
// reloaded from disk so this session neither ignores nor overwrites them on exit. Simple / Pro Mode and the current workspace stay
// as they are unless the migration changed them (then they switch the normal way).
void EditorApp::applyMigratedSettings() {
    bool wasSimple = m_prefs.simpleMode;
    std::string workspace = m_prefs.workspace, offer = m_prefs.migrationOffer;
    loadSettings();
    bool wantSimple = m_prefs.simpleMode;
    std::string wantWorkspace = m_prefs.workspace;
    m_prefs.simpleMode = wasSimple;
    m_prefs.workspace = workspace;
    m_prefs.migrationOffer = offer == "done" ? offer : "done";
    if (auto ini = readFileText(m_opt.settingsDir / "editor_layout.ini")) ImGui::LoadIniSettingsFromMemory(ini->c_str(), ini->size());
    if (wantSimple != wasSimple) setSimpleMode(wantSimple);
    else if (!wantSimple && wantWorkspace != workspace && wantWorkspace != "Simple") m_pendingWorkspace = wantWorkspace;
    if (m_code) m_code->loadSettings();
    applyTheme();
    m_fontsDirty = true;
    m_session.ai.applyTo(m_mcp->policy());
    rebuildProvider();
    applyLinkIpc();
    saveSettings();
    FLEDGE_LOG_INFO("editor", "migrated settings reloaded from {}", narrow(m_opt.settingsDir.wstring()));
}

} // namespace fledge::editor
