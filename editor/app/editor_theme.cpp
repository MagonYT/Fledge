// Themes (dark / light), fonts and DPI scaling.
#include <imgui.h>
#include <algorithm>

#include "fledge/core/log.h"
#include "editor_app.h"
#include "editor_code.h"

namespace fledge::editor {

namespace {
ImVec4 rgb(int r, int g, int b, float a = 1.0f) { return {r / 255.0f, g / 255.0f, b / 255.0f, a}; }

void baseStyle(ImGuiStyle& s) {
    s = ImGuiStyle();
    s.WindowRounding = 6;
    s.ChildRounding = 4;
    s.FrameRounding = 4;
    s.PopupRounding = 6;
    s.ScrollbarRounding = 8;
    s.GrabRounding = 4;
    s.TabRounding = 4;
    s.WindowPadding = {8, 8};
    s.FramePadding = {8, 4};
    s.ItemSpacing = {8, 6};
    s.ItemInnerSpacing = {6, 4};
    s.CellPadding = {6, 3};
    s.IndentSpacing = 16;
    s.ScrollbarSize = 12;
    s.GrabMinSize = 10;
    s.WindowBorderSize = 1;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.TabBorderSize = 0;
    s.TabBarBorderSize = 1;
    s.DockingSeparatorSize = 2;
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.SeparatorTextBorderSize = 1;
}

void darkColors(ImVec4* c) {
    ImGui::StyleColorsDark();
    // Fledge palette: the logo's ink-dark background and its coral -> rose gradient as the accent.
    const ImVec4 bg = rgb(21, 23, 28), panel = rgb(28, 30, 36), frame = rgb(39, 42, 49), frameHi = rgb(50, 53, 61);
    const ImVec4 accent = rgb(242, 135, 106), accentHi = rgb(250, 162, 132), accentLo = rgb(205, 104, 150);
    c[ImGuiCol_Text] = rgb(225, 227, 230);
    c[ImGuiCol_TextDisabled] = rgb(130, 134, 142);
    c[ImGuiCol_WindowBg] = panel;
    c[ImGuiCol_ChildBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = rgb(40, 41, 46, 0.98f);
    c[ImGuiCol_Border] = rgb(14, 15, 19);
    c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = frameHi;
    c[ImGuiCol_FrameBgActive] = rgb(70, 72, 80);
    c[ImGuiCol_TitleBg] = bg;
    c[ImGuiCol_TitleBgActive] = bg;
    c[ImGuiCol_TitleBgCollapsed] = bg;
    c[ImGuiCol_MenuBarBg] = bg;
    c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgb(70, 72, 78);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(90, 92, 100);
    c[ImGuiCol_ScrollbarGrabActive] = accentLo;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accentLo;
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = rgb(55, 57, 63);
    c[ImGuiCol_ButtonHovered] = rgb(72, 75, 83);
    c[ImGuiCol_ButtonActive] = accentLo;
    c[ImGuiCol_Header] = rgb(240, 130, 40, 0.30f);
    c[ImGuiCol_HeaderHovered] = rgb(240, 130, 40, 0.45f);
    c[ImGuiCol_HeaderActive] = rgb(240, 130, 40, 0.60f);
    c[ImGuiCol_Separator] = rgb(20, 21, 23);
    c[ImGuiCol_SeparatorHovered] = accentLo;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = rgb(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = accentLo;
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = bg;
    c[ImGuiCol_TabHovered] = rgb(60, 62, 68);
    c[ImGuiCol_TabSelected] = panel;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = bg;
    c[ImGuiCol_TabDimmedSelected] = panel;
    c[ImGuiCol_TabDimmedSelectedOverline] = rgb(120, 80, 40);
    c[ImGuiCol_DockingPreview] = rgb(240, 130, 40, 0.5f);
    c[ImGuiCol_DockingEmptyBg] = bg;
    c[ImGuiCol_TableHeaderBg] = rgb(44, 46, 51);
    c[ImGuiCol_TableBorderStrong] = rgb(20, 21, 23);
    c[ImGuiCol_TableBorderLight] = rgb(48, 50, 55);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.025f);
    c[ImGuiCol_TextSelectedBg] = rgb(240, 130, 40, 0.35f);
    c[ImGuiCol_NavCursor] = accentHi;
    c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.55f);
    c[ImGuiCol_PlotHistogram] = accent;
}

void lightColors(ImVec4* c) {
    ImGui::StyleColorsLight();
    const ImVec4 bg = rgb(232, 233, 236), panel = rgb(246, 246, 248), frame = rgb(255, 255, 255);
    const ImVec4 accent = rgb(222, 98, 80), accentLo = rgb(236, 150, 140);
    c[ImGuiCol_Text] = rgb(28, 30, 34);
    c[ImGuiCol_TextDisabled] = rgb(120, 124, 132);
    c[ImGuiCol_WindowBg] = panel;
    c[ImGuiCol_ChildBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = rgb(252, 252, 253, 0.99f);
    c[ImGuiCol_Border] = rgb(205, 207, 212);
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = rgb(240, 241, 244);
    c[ImGuiCol_FrameBgActive] = rgb(228, 230, 235);
    c[ImGuiCol_TitleBg] = bg;
    c[ImGuiCol_TitleBgActive] = bg;
    c[ImGuiCol_MenuBarBg] = bg;
    c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accentLo;
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = rgb(226, 228, 232);
    c[ImGuiCol_ButtonHovered] = rgb(212, 215, 221);
    c[ImGuiCol_ButtonActive] = accentLo;
    c[ImGuiCol_Header] = rgb(225, 110, 20, 0.20f);
    c[ImGuiCol_HeaderHovered] = rgb(225, 110, 20, 0.30f);
    c[ImGuiCol_HeaderActive] = rgb(225, 110, 20, 0.45f);
    c[ImGuiCol_Separator] = rgb(205, 207, 212);
    c[ImGuiCol_Tab] = bg;
    c[ImGuiCol_TabHovered] = rgb(250, 250, 252);
    c[ImGuiCol_TabSelected] = panel;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = bg;
    c[ImGuiCol_TabDimmedSelected] = panel;
    c[ImGuiCol_DockingPreview] = rgb(225, 110, 20, 0.4f);
    c[ImGuiCol_DockingEmptyBg] = bg;
    c[ImGuiCol_TextSelectedBg] = rgb(225, 110, 20, 0.25f);
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_ModalWindowDimBg] = rgb(30, 30, 30, 0.35f);
    c[ImGuiCol_PlotHistogram] = accent;
}
} // namespace

float EditorApp::effectiveScale() const {
    float s = m_prefs.uiScale > 0 ? m_prefs.uiScale : (m_window ? m_window->contentScale() : 1.0f);
    return std::clamp(s, 0.5f, 3.0f);
}

void EditorApp::applyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    baseStyle(s);
    if (m_prefs.theme == Theme::Light) lightColors(s.Colors);
    else darkColors(s.Colors);
    s.ScaleAllSizes(effectiveScale());
}

void EditorApp::rebuildFonts() {
    if (!m_fontsDirty) return;
    m_fontsDirty = false;
    float scale = effectiveScale();
    m_appliedScale = scale;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    float size = std::round(std::clamp(m_prefs.fontSize, 11.0f, 24.0f) * scale);
    fs::path fonts = m_paths.resources / "fonts";
    std::string ui = narrow((fonts / "Roboto-Medium.ttf").wstring());
    std::string icons = narrow((fonts / "fa-solid-900.ttf").wstring());
    std::string mono = narrow((fonts / "Cousine-Regular.ttf").wstring());
    std::error_code ec;
    static const ImWchar kIconRange[] = {0xe000, 0xf8ff, 0};
    auto addWithIcons = [&](float px) -> ImFont* {
        ImFont* f = nullptr;
        if (fs::exists(fonts / "Roboto-Medium.ttf", ec)) f = io.Fonts->AddFontFromFileTTF(ui.c_str(), px);
        if (!f) {
            ImFontConfig cfg;
            cfg.SizePixels = std::round(13.0f * scale);
            f = io.Fonts->AddFontDefault(&cfg);
        }
        if (fs::exists(fonts / "fa-solid-900.ttf", ec)) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.PixelSnapH = true;
            cfg.GlyphMinAdvanceX = px;  // monospaced icons line up in menus
            cfg.GlyphOffset = {0, 0.5f * scale};
            m_iconsLoaded = io.Fonts->AddFontFromFileTTF(icons.c_str(), px * 0.85f, &cfg, kIconRange) != nullptr;
        }
        return f;
    };
    m_fontUi = addWithIcons(size);
    m_fontLarge = addWithIcons(std::round(size * 1.9f));
    m_fontMono = fs::exists(fonts / "Cousine-Regular.ttf", ec) ? io.Fonts->AddFontFromFileTTF(mono.c_str(), std::round(size * 0.95f)) : nullptr;
    if (!m_fontMono) m_fontMono = m_fontUi;
    // Code workspace: JetBrains Mono at the code font size (Ctrl+wheel / Ctrl+= rebuild the atlas)
    float codePx = std::round((m_code ? m_code->settings.fontSize : 14.0f) * scale);
    m_fontCode = fs::exists(fonts / "JetBrainsMono-Regular.ttf", ec)
                     ? io.Fonts->AddFontFromFileTTF(narrow((fonts / "JetBrainsMono-Regular.ttf").wstring()).c_str(), codePx)
                     : nullptr;
    if (!m_fontCode) m_fontCode = m_fontMono;
    if (m_code) m_code->font = m_fontCode;
    io.FontDefault = m_fontUi;
    if (!m_iconsLoaded) FLEDGE_LOG_WARN("editor", "Icon font not found in {}; buttons show text only", narrow(fonts.wstring()));
    refreshFontTexture();  // before the first frame the backend creates the texture itself
    applyTheme();
}

} // namespace fledge::editor
