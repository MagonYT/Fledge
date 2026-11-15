// Convert Blueprint panel: paste Unreal Blueprint nodes (T3D text) or load a .t3d file, preview the generated Fledge Script C++
// and the conversion report live, then save into Scripts/ (never overwriting) or copy. The logic lives in engine/blueprint and
// editor/core (blueprint_convert.h); this file is UI. docs/BLUEPRINT_CONVERTER.md.
#include <format>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#endif

#include "editor_app.h"
#include "editor_icons.h"
#include "fledge/core/log.h"

namespace fledge::editor {

namespace {
ImVec4 sevColor(blueprint::Severity s) {
    switch (s) {
        case blueprint::Severity::Converted: return {0.35f, 0.8f, 0.45f, 1};
        case blueprint::Severity::Approximated: return {1.0f, 0.75f, 0.3f, 1};
        case blueprint::Severity::Stubbed: return {1.0f, 0.45f, 0.4f, 1};
        default: return {0.6f, 0.6f, 0.6f, 1};
    }
}
} // namespace

void EditorApp::loadBlueprintFile() {
#ifdef _WIN32
    wchar_t buf[4096] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Unreal Blueprint text (*.t3d;*.txt;*.copy)\0*.t3d;*.txt;*.copy\0Unreal packages - probe only (*.uasset;*.umap)\0*.uasset;*.umap\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 4096;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    fs::path file(buf);
    std::string ext = toLower(narrow(file.extension().wstring()));
    m_bpSource = narrow(file.filename().wstring());
    if (m_bpClass.empty()) m_bpClass = blueprint::sanitizeIdentifier(narrow(file.stem().wstring()), "ConvertedBlueprint");
    if (ext == ".uasset" || ext == ".umap") {
        BlueprintJob job = makeBlueprintJob(m_session, m_bpClass, m_bpSource, m_bpUnrealSpace);
        m_bpResult = std::make_unique<blueprint::Conversion>(blueprint::convertFile(file, job.options));
        m_bpReportMd = m_bpResult->report.toMarkdown();
        m_bpStatus = m_bpResult->report.error;
        m_bpText.clear();
        m_bpLastConverted.clear();
        return;
    }
    auto t = blueprint::readT3DText(file);
    if (!t) {
        toast(ToastLevel::Error, "Convert Blueprint: " + t.error());
        return;
    }
    m_bpText = std::move(*t);
    refreshBlueprintPreview(true);
#endif
}

void EditorApp::refreshBlueprintPreview(bool force) {
    std::string key = m_bpText + "\x1f" + m_bpClass + "\x1f" + (m_bpUnrealSpace ? "1" : "0");
    if (!force && key == m_bpLastConverted) return;
    m_bpLastConverted = key;
    if (m_bpText.empty()) {
        m_bpResult.reset();
        m_bpReportMd.clear();
        m_bpStatus.clear();
        return;
    }
    std::string cls = m_bpClass.empty() ? std::string() : blueprint::sanitizeIdentifier(m_bpClass, "ConvertedBlueprint");
    BlueprintJob job = makeBlueprintJob(m_session, cls, m_bpSource, m_bpUnrealSpace);
    m_bpResult = std::make_unique<blueprint::Conversion>(blueprint::convertText(m_bpText, job.options));
    m_bpReportMd = m_bpResult->report.toMarkdown();
    m_bpStatus = m_bpResult->ok ? m_bpResult->report.summary() : m_bpResult->report.error;
    for (auto& n : job.notes) m_bpStatus += "  |  " + n;
}

void EditorApp::drawBlueprintContents(bool simple) {
    float s = effectiveScale();
    if (!m_prefs.blueprintNoticeAccepted) {  // one-time licence notice
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored({1.0f, 0.75f, 0.3f, 1}, ICON_WARNING "  Before you convert");
        ImGui::TextUnformatted(blueprint::kLicenceNotice);
        ImGui::TextDisabled("The converter works on hand-written test samples; real Unreal exports may contain constructs it does not know yet. "
                            "Everything it does not understand is listed in the report.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button(ICON_CHECK "  I understand", {simple ? -1.0f : 200 * s, 30 * s})) {
            m_prefs.blueprintNoticeAccepted = true;
            saveSettings();
        }
        return;
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled(simple ? "In Unreal: open the Blueprint, select the nodes (Ctrl+A), copy (Ctrl+C) and paste them here."
                               : "Paste Blueprint nodes copied in the Unreal Blueprint editor (Ctrl+C), or load a .t3d file. The C++ preview updates as you type.");
    ImGui::PopTextWrapPos();
    if (ImGui::Button(ICON_FOLDER_OPEN " Load from file...")) loadBlueprintFile();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FILE " Paste")) {
        if (const char* clip = ImGui::GetClipboardText()) {
            m_bpText = clip;
            m_bpSource = "pasted text";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_ROTATE " Clear")) {
        m_bpText.clear();
        m_bpSource = "pasted text";
    }
    ImGui::SetNextItemWidth(220 * s);
    ImGui::InputTextWithHint("Class name", "from the Blueprint / file name", &m_bpClass, ImGuiInputTextFlags_CharsNoBlank);
    if (!simple) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180 * s);
        ImGui::InputTextWithHint("Folder in Scripts/", "(Scripts/)", &m_bpFolder);
        ImGui::SameLine();
        ImGui::Checkbox("Unreal units and axes", &m_bpUnrealSpace);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Convert Unreal centimetres / X-forward Z-up to Fledge metres / -Z-forward Y-up where the Blueprint talks to the engine.");
    }
    float inputH = simple ? 140 * s : std::max(120 * s, ImGui::GetContentRegionAvail().y * 0.3f);
    ImGui::InputTextMultiline("##bptext", &m_bpText, {-1, inputH}, ImGuiInputTextFlags_AllowTabInput);
    if (m_bpText.size() > blueprint::Limits{}.maxInputBytes) ImGui::TextColored({1, 0.45f, 0.4f, 1}, "The text is over the 8 MiB limit.");
    // live preview for normal-sized input; very large pastes convert on demand
    bool small = m_bpText.size() < 512 * 1024;
    if (small) refreshBlueprintPreview(false);
    else if (ImGui::Button(ICON_BOLT " Convert")) refreshBlueprintPreview(true);

    const blueprint::Conversion* c = m_bpResult.get();
    bool canSave = c && c->ok && m_session.hasProject() && !m_session.playing();
    ImGui::BeginDisabled(!canSave);
    if (ImGui::Button(ICON_SAVE " Save into project", {simple ? -1.0f : 0.0f, 0})) {
        BlueprintJob job = makeBlueprintJob(m_session, m_bpClass.empty() ? std::string() : blueprint::sanitizeIdentifier(m_bpClass), m_bpSource, m_bpUnrealSpace);
        auto r = writeBlueprintToProject(m_session, m_bpText, job, m_bpFolder, false);
        if (r) {
            std::string files;
            for (auto& f : r->files) files += (files.empty() ? "Scripts/" : ", Scripts/") + f;
            toast(ToastLevel::Success, "Converted Blueprint saved: " + files + ". Build the scripts, then add the " + r->conversion.className +
                                           " behaviour to an entity. Report: " + r->reportMarkdown);
            for (auto& n : r->notes) toast(ToastLevel::Info, n);
            openScriptFile(r->files.front());  // shown in the Script Editor
        } else {
            toast(ToastLevel::Error, "Convert Blueprint: " + r.error());
        }
    }
    ImGui::EndDisabled();
    if (!m_session.hasProject() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Open a project first");
    if (!simple) {
        ImGui::SameLine();
        ImGui::BeginDisabled(!c || !c->ok);
        if (ImGui::Button(ICON_COPY " Copy C++")) ImGui::SetClipboardText(c->cpp.c_str());
        ImGui::SameLine();
        if (ImGui::Button(ICON_COPY " Copy report")) ImGui::SetClipboardText(m_bpReportMd.c_str());
        ImGui::EndDisabled();
    }
    if (!m_bpStatus.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(c && c->ok ? ImVec4{0.6f, 0.8f, 1.0f, 1} : ImVec4{1, 0.6f, 0.4f, 1}, "%s", m_bpStatus.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!c) return;
    if (c->ok) {
        const auto& r = c->report;
        ImGui::TextColored(sevColor(blueprint::Severity::Converted), "%d converted", r.count(blueprint::Severity::Converted));
        ImGui::SameLine();
        ImGui::TextColored(sevColor(blueprint::Severity::Approximated), "%d approximated", r.count(blueprint::Severity::Approximated));
        ImGui::SameLine();
        ImGui::TextColored(sevColor(blueprint::Severity::Stubbed), "%d stubbed (TODO)", r.count(blueprint::Severity::Stubbed));
        ImGui::SameLine();
        ImGui::TextDisabled("%d ignored, %d unrecognised", r.count(blueprint::Severity::Ignored), static_cast<int>(r.unrecognised.size()));
    }
    if (ImGui::BeginTabBar("bptabs")) {
        if (c->ok && ImGui::BeginTabItem(("C++: " + c->cppFile).c_str())) {
            std::string view = c->cpp;  // read-only, selectable
            ImGui::InputTextMultiline("##cpp", &view, {-1, -1}, ImGuiInputTextFlags_ReadOnly);
            ImGui::EndTabItem();
        }
        if (c->ok && !c->headerFile.empty() && ImGui::BeginTabItem(c->headerFile.c_str())) {
            std::string view = c->header;
            ImGui::InputTextMultiline("##h", &view, {-1, -1}, ImGuiInputTextFlags_ReadOnly);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Nodes")) {
            if (ImGui::BeginTable("bpnodes", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("Node");
                ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, 100 * s);
                ImGui::TableSetupColumn("Detail");
                ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, 50 * s);
                ImGui::TableHeadersRow();
                for (auto& n : c->report.nodes) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(n.title.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextColored(sevColor(n.severity), "%s", blueprint::toString(n.severity));
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", n.detail.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", n.line);
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Report")) {
            std::string view = m_bpReportMd;
            ImGui::InputTextMultiline("##rep", &view, {-1, -1}, ImGuiInputTextFlags_ReadOnly);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void EditorApp::drawBlueprintPanel() {
    PanelDef* p = panel("Convert Blueprint");
    if (!ImGui::Begin("Convert Blueprint", p ? &p->open : nullptr)) {
        ImGui::End();
        return;
    }
    drawBlueprintContents(false);
    ImGui::End();
}

void EditorApp::drawBlueprintWindow() {  // Simple Mode: a floating window with the same flow
    if (!m_showBlueprintSimple) return;
    float s = effectiveScale();
    ImGui::SetNextWindowSize({620 * s, 620 * s}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (!ImGui::Begin(ICON_CODE "  Convert an Unreal Blueprint###SimpleBlueprint", &m_showBlueprintSimple, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    drawBlueprintContents(true);
    ImGui::End();
}

} // namespace fledge::editor
