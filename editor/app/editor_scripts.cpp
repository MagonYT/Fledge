// Fledge Script in the editor: script build service (watcher -> background build ->
// hot reload), Script Editor (syntax highlighting, error squiggles), Build Output,
// New Script wizard, Behaviours in the Inspector, external IDE, play-mode input.
#include <imgui.h>
#include <GLFW/glfw3.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_set>

#include "fledge/core/log.h"
#include "editor_app.h"
#include "editor_code.h"
#include "editor_icons.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace fledge::editor {

namespace {
double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

bool isSourceFile(const std::string& p) {
    return endsWith(p, ".cpp") || endsWith(p, ".h") || endsWith(p, ".hpp") || endsWith(p, ".inl") || endsWith(p, ".cc");
}

// ------------------------------------------------------- syntax highlighting --
struct Span {
    size_t begin, end;
    int kind;  // 0 text, 1 keyword, 2 type, 3 string, 4 number, 5 comment, 6 preprocessor, 7 function, 8 macro
};

const std::unordered_set<std::string_view>& keywords() {
    static const std::unordered_set<std::string_view> k = {
        "alignas", "alignof", "auto", "bool", "break", "case", "catch", "char", "class", "co_await", "co_return", "co_yield", "const",
        "constexpr", "consteval", "continue", "decltype", "default", "delete", "do", "double", "else", "enum", "explicit", "extern",
        "false", "float", "for", "friend", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "nullptr",
        "operator", "override", "private", "protected", "public", "return", "short", "signed", "sizeof", "static", "static_assert",
        "static_cast", "struct", "switch", "template", "this", "throw", "true", "try", "typedef", "typename", "unsigned", "using",
        "virtual", "void", "volatile", "while", "final", "uint32_t", "uint64_t", "int64_t", "size_t", "dynamic_cast", "reinterpret_cast"};
    return k;
}
const std::unordered_set<std::string_view>& scriptTypes() {
    static const std::unordered_set<std::string_view> t = {
        "Behaviour", "Service", "Object", "Entity", "Vec3", "Quat", "Color", "Task", "Scene", "Input", "Time", "Log", "Events", "RayHit",
        "Collision", "AssetRef", "Range", "Tooltip", "AssetKind", "Hidden", "ReadOnly", "std", "string", "vector", "json", "Value",
        "TimerHandle", "CoroutineHandle", "SubscriptionId", "fledge", "script"};
    return t;
}

void tokenize(std::string_view line, bool& inBlock, std::vector<Span>& out) {
    out.clear();
    size_t i = 0, n = line.size();
    auto push = [&](size_t b, size_t e, int k) {
        if (e > b) out.push_back({b, e, k});
    };
    size_t firstNonSpace = line.find_first_not_of(" \t");
    if (!inBlock && firstNonSpace != std::string_view::npos && line[firstNonSpace] == '#') {
        size_t c = line.find("//", firstNonSpace);
        push(0, c == std::string_view::npos ? n : c, 6);
        if (c != std::string_view::npos) push(c, n, 5);
        return;
    }
    while (i < n) {
        if (inBlock) {
            size_t e = line.find("*/", i);
            if (e == std::string_view::npos) {
                push(i, n, 5);
                return;
            }
            push(i, e + 2, 5);
            i = e + 2;
            inBlock = false;
            continue;
        }
        char c = line[i];
        if (c == '/' && i + 1 < n && line[i + 1] == '/') {
            push(i, n, 5);
            return;
        }
        if (c == '/' && i + 1 < n && line[i + 1] == '*') {
            inBlock = true;
            continue;
        }
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < n && line[j] != c) j += line[j] == '\\' ? 2 : 1;
            push(i, std::min(n, j + 1), 3);
            i = j + 1;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '.' || line[j] == '\'')) ++j;
            push(i, j, 4);
            i = j;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_')) ++j;
            std::string_view w = line.substr(i, j - i);
            int kind = 0;
            if (keywords().contains(w)) kind = 1;
            else if (w == "FLEDGE_PROPERTY" || w == "FLEDGE_BEHAVIOUR" || w == "FLEDGE_SERVICE" || w.starts_with("PERCH_")) kind = 8;  // PERCH_*: pre-1.0.1 scripts
            else if (scriptTypes().contains(w)) kind = 2;
            else {
                size_t k = j;
                while (k < n && line[k] == ' ') ++k;
                if (k < n && line[k] == '(') kind = 7;
                else if (std::isupper(static_cast<unsigned char>(w[0])) && w.size() > 1) kind = 2;
            }
            push(i, j, kind);
            i = j;
            continue;
        }
        size_t j = i + 1;
        while (j < n && !std::isalnum(static_cast<unsigned char>(line[j])) && line[j] != '_' && line[j] != '"' && line[j] != '\'' && line[j] != '/')
            ++j;
        push(i, j, 0);
        i = j;
    }
}

ImU32 tokenColor(int kind, bool light) {
    static const ImU32 dark[] = {IM_COL32(220, 222, 226, 255), IM_COL32(198, 120, 221, 255), IM_COL32(86, 182, 194, 255),
                                 IM_COL32(229, 168, 108, 255), IM_COL32(181, 206, 168, 255), IM_COL32(110, 140, 110, 255),
                                 IM_COL32(200, 120, 160, 255), IM_COL32(229, 205, 120, 255), IM_COL32(245, 150, 70, 255)};
    static const ImU32 lite[] = {IM_COL32(30, 32, 36, 255),  IM_COL32(150, 40, 170, 255), IM_COL32(20, 120, 140, 255),
                                 IM_COL32(170, 80, 20, 255), IM_COL32(40, 120, 40, 255),  IM_COL32(110, 130, 110, 255),
                                 IM_COL32(160, 50, 110, 255), IM_COL32(130, 100, 10, 255), IM_COL32(200, 90, 10, 255)};
    int k = kind < 0 ? 0 : (kind > 8 ? 8 : kind);
    return light ? lite[k] : dark[k];
}

std::vector<std::string_view> splitLines(const std::string& text) {
    std::vector<std::string_view> lines;
    size_t s = 0;
    for (;;) {
        size_t e = text.find('\n', s);
        if (e == std::string::npos) {
            lines.emplace_back(text.data() + s, text.size() - s);
            break;
        }
        lines.emplace_back(text.data() + s, e - s);
        s = e + 1;
    }
    return lines;
}

int imguiKeyToCode(ImGuiKey k) {
    if (k >= ImGuiKey_A && k <= ImGuiKey_Z) return 'A' + (k - ImGuiKey_A);
    if (k >= ImGuiKey_0 && k <= ImGuiKey_9) return '0' + (k - ImGuiKey_0);
    if (k >= ImGuiKey_F1 && k <= ImGuiKey_F12) return 290 + (k - ImGuiKey_F1);
    switch (k) {
        case ImGuiKey_Space: return 32;
        case ImGuiKey_Escape: return 256;
        case ImGuiKey_Enter: return 257;
        case ImGuiKey_Tab: return 258;
        case ImGuiKey_Backspace: return 259;
        case ImGuiKey_Insert: return 260;
        case ImGuiKey_Delete: return 261;
        case ImGuiKey_RightArrow: return 262;
        case ImGuiKey_LeftArrow: return 263;
        case ImGuiKey_DownArrow: return 264;
        case ImGuiKey_UpArrow: return 265;
        case ImGuiKey_LeftShift: return 340;
        case ImGuiKey_LeftCtrl: return 341;
        case ImGuiKey_LeftAlt: return 342;
        case ImGuiKey_RightShift: return 344;
        case ImGuiKey_RightCtrl: return 345;
        case ImGuiKey_RightAlt: return 346;
        default: return -1;
    }
}
} // namespace

// ------------------------------------------------------------- build service --
void EditorApp::startScriptBuild(bool force, bool userInitiated) {
    if (!m_session.hasProject() || m_shuttingDown) return;
    if (m_scriptBuildRunning) {
        m_scriptRebuildQueued = true;
        return;
    }
    if (userInitiated) m_session.aiTouchedScripts.clear();
    m_scriptBuildRunning = true;
    m_scriptBuildStart = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    Project project = m_session.project();
    ScriptBuildOptions opt = m_session.scriptBuildOptions();
    opt.force = force;
    runInBackground("Building scripts", [this, project, opt](BackgroundTask& t) {
        t.progress = 0;
        int total = 0, done = 0;
        auto r = buildScripts(project, opt, [&](const std::string& msg) {
            if (msg.starts_with("Compiling ")) total = std::max(1, std::atoi(msg.c_str() + 10)) + 1;  // + link
            if (msg.starts_with("[") || msg.starts_with("Linking")) ++done;
            if (total) t.progress = std::min(1.0f, static_cast<float>(done) / static_cast<float>(total));
            std::lock_guard l(m_taskMutex);
            t.detail = msg;
        });
        m_mainQueue.post([this, r] {
            m_scriptBuildRunning = false;
            uint32_t gen = m_session.world().scripts().moduleGeneration();
            m_session.applyScriptBuild(r);
            const auto& b = m_session.lastScriptBuild();
            bool reloaded = m_session.world().scripts().moduleGeneration() != gen;
            if (!b.ok) {
                toast(ToastLevel::Error, std::format("Script build failed: {} error{} (Build Output)", b.errors(), b.errors() == 1 ? "" : "s"));
                if (PanelDef* p = panel("Build Output")) p->open = true;
            } else if (reloaded) {
                toast(ToastLevel::Success, std::format("Scripts built in {:.1f}s{}", b.seconds,
                                                       m_session.playing() ? " - hot reloaded (properties kept)" : ""));
            }
            if (m_scriptRebuildQueued) {
                m_scriptRebuildQueued = false;
                startScriptBuild(false, false);
            }
        });
        return std::string(r.ok ? "ok" : "failed");
    });
}

void EditorApp::updateScripts() {
    if (!m_session.hasProject()) {
        m_scriptWatcher.reset();
        m_scriptProject.clear();
        return;
    }
    const Project& p = m_session.project();
    if (m_scriptProject != p.file) {  // project (re)opened
        m_scriptProject = p.file;
        std::error_code ec;
        fs::create_directories(p.scriptsDir(), ec);
        m_scriptWatcher = FileWatcher::create(p.scriptsDir());
        m_scriptFile.clear();
        m_scriptText.clear();
        m_scriptSaved.clear();
        if (m_session.scriptSettings.autoBuild && p.hasScripts()) startScriptBuild(false, false);  // incremental: ~0.1 s when up to date
        if (!m_opt.openScript.empty()) openScriptFile(std::exchange(m_opt.openScript, std::string()), 1);
    }
    if (m_scriptWatcher) {
        for (auto& f : m_scriptWatcher->poll()) {
            if (!isSourceFile(f)) continue;  // build outputs (compile_commands.json, ...) never trigger builds
            m_scriptChangeTime = nowSec();
            if (f == m_scriptFile && m_scriptText == m_scriptSaved) {  // changed outside (AI, IDE): reload if not dirty
                if (auto t = readFileText(p.scriptsDir() / pathFromUtf8(f))) m_scriptText = m_scriptSaved = *t;
            }
        }
    }
    if (m_scriptChangeTime > 0 && nowSec() - m_scriptChangeTime > 0.35) {
        m_scriptChangeTime = -1;
        bool blocked = !m_session.aiTouchedScripts.empty() && !m_session.ai.categoryAllowed(AiCategory::ScriptBuild);
        if (blocked) toast(ToastLevel::Warning, "AI edited scripts: review them, then press Build (AI builds are disabled in Settings > Scripts)");
        else if (m_session.scriptSettings.autoBuild) startScriptBuild(false, false);
    }
}

// ------------------------------------------------------------------ files --
void EditorApp::openScriptFile(const std::string& rel, int line) {
    if (!m_session.hasProject() || rel.empty()) return;
    auto path = resolveScriptPath(m_session.project(), rel);
    if (!path) {
        toast(ToastLevel::Error, path.error());
        return;
    }
    if (rel != m_scriptFile) {
        if (m_scriptText != m_scriptSaved && !m_scriptFile.empty()) saveScriptFile();  // keep edits when switching files
        auto text = readFileText(*path);
        m_scriptFile = rel;
        m_scriptText = m_scriptSaved = text.value_or("");
    }
    m_scriptJumpLine = line;
    m_scriptJumpTime = ImGui::GetTime();
    if (m_prefs.workspace == "Code" && m_code) {  // the Code workspace is the script editor there
        m_code->openFile(*path, std::max(1, line));
        return;
    }
    if (PanelDef* p = panel("Script Editor")) p->open = true;
    m_focusQueue.push_back("Script Editor");
}

bool EditorApp::saveScriptFile() {
    if (m_scriptFile.empty()) return false;
    auto st = m_session.writeScriptFile(m_scriptFile, m_scriptText, false);
    if (!st) {
        toast(ToastLevel::Error, st.error);
        return false;
    }
    m_scriptSaved = m_scriptText;
    return true;
}

void EditorApp::openInExternalIde(const std::string& rel, int line) {
    if (!m_session.hasProject()) return;
    fs::path scripts = m_session.project().scriptsDir();
    fs::path file = rel.empty() ? scripts : scripts / pathFromUtf8(rel);
#ifdef _WIN32
    if (m_externalEditor.empty()) {
        auto r = reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", file.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (r <= 32) toast(ToastLevel::Error, "No program is associated with this file type; set Settings > Scripts > External editor");
        return;
    }
    std::string cmd = m_externalEditor;
    auto replace = [&](const std::string& key, const std::string& value) {
        for (size_t pos = cmd.find(key); pos != std::string::npos; pos = cmd.find(key, pos + value.size())) cmd.replace(pos, key.size(), value);
    };
    replace("{file}", narrow(file.wstring()));
    replace("{line}", std::to_string(std::max(1, line)));
    replace("{scripts}", narrow(scripts.wstring()));
    replace("{project}", narrow(m_session.project().rootDir().wstring()));
    std::wstring full = L"cmd.exe /d /c " + widen(cmd);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, full.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        toast(ToastLevel::Info, "Opened in external editor");
    } else {
        toast(ToastLevel::Error, "Cannot start the external editor: " + cmd);
    }
#else
    (void)file;
    (void)line;
#endif
}

// ---------------------------------------------------------- Script Editor --
void EditorApp::drawScriptEditor() {
    PanelDef* pd = panel("Script Editor");
    if (!ImGui::Begin("Script Editor", pd ? &pd->open : nullptr)) {
        m_scriptEditorFocused = false;
        ImGui::End();
        return;
    }
    m_scriptEditorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    float s = effectiveScale();
    if (!m_session.hasProject()) {
        ImGui::TextDisabled("Open a project to edit its Fledge Script files.");
        ImGui::End();
        return;
    }
    const Project& proj = m_session.project();
    const auto& build = m_session.lastScriptBuild();
    // ---- file list
    ImGui::BeginChild("files", {200 * s, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    if (ImGui::Button(ICON_PLUS " New")) m_showNewScript = true;
    trackRect("scripts.new");
    ImGui::SameLine();
    ImGui::BeginDisabled(m_scriptBuildRunning);
    if (ImGui::Button(ICON_BOLT " Build")) m_commands.execute("scripts.build");
    trackRect("scripts.build");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ICON_EXTERNAL)) openInExternalIde(m_scriptFile, 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open in the external editor (Settings > Scripts)");
    ImGui::Separator();
    std::error_code ec;
    std::vector<std::string> files;
    if (fs::is_directory(proj.scriptsDir(), ec))
        for (auto it = fs::recursive_directory_iterator(proj.scriptsDir(), ec); it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file() && isSourceFile(narrow(it->path().filename().wstring())))
                files.push_back(narrow(it->path().lexically_relative(proj.scriptsDir()).generic_wstring()));
    std::sort(files.begin(), files.end());
    if (files.empty()) ImGui::TextDisabled("No scripts yet.\nPress New.");
    for (auto& f : files) {
        int errs = 0, warns = 0;
        for (auto& d : build.diagnostics)
            if (d.file == f) (d.severity == Diagnostic::Severity::Error ? errs : warns) += d.severity == Diagnostic::Severity::Note ? 0 : 1;
        std::string label = std::string(endsWith(f, ".cpp") ? ICON_CODE : ICON_FILE) + "  " + f;
        if (f == m_scriptFile && m_scriptText != m_scriptSaved) label += " *";
        if (ImGui::Selectable(label.c_str(), f == m_scriptFile)) openScriptFile(f);
        trackRect("scripts.file." + f);
        if (errs || warns) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 24 * s + ImGui::GetCursorPosX());
            ImGui::TextColored(errs ? ImVec4(1, 0.45f, 0.45f, 1) : ImVec4(1, 0.78f, 0.3f, 1), "%d", errs ? errs : warns);
        }
    }
    ImGui::Separator();
    auto& scripts = m_session.world().scripts();
    ImGui::TextDisabled("%s %s", ICON_CUBES, scripts.hasModule() ? narrow(scripts.module().path.filename().wstring()).c_str() : "no module");
    for (auto& c : scripts.module().classes) ImGui::TextDisabled("  %s %s", c.isService ? ICON_GLOBE : ICON_CODE, c.name.c_str());
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- editor
    ImGui::BeginChild("editor", {0, 0});
    if (m_scriptFile.empty()) {
        ImGui::TextDisabled("Select a script on the left, or create one with New.");
        ImGui::TextDisabled("API: docs/FLEDGE_SCRIPT.md - the AI reads fledge://script/api.");
        ImGui::EndChild();
        ImGui::End();
        return;
    }
    bool dirty = m_scriptText != m_scriptSaved;
    ImGui::Text("%s %s%s", ICON_CODE, m_scriptFile.c_str(), dirty ? " *" : "");
    ImGui::SameLine();
    ImGui::BeginDisabled(!dirty);
    if (ImGui::SmallButton(ICON_SAVE " Save")) saveScriptFile();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_EXTERNAL " Open in IDE")) openInExternalIde(m_scriptFile, m_scriptCursorLine);
    ImGui::SameLine();
    ImGui::TextDisabled("Ln %d, Col %d   Ctrl+S save%s", m_scriptCursorLine, m_scriptCursorCol,
                        m_session.scriptSettings.autoBuild ? " (auto build + hot reload)" : "");
    drawCodeEditor();
    ImGui::EndChild();
    ImGui::End();
}

void EditorApp::drawCodeEditor() {
    bool light = m_prefs.theme == Theme::Light;
    ImGui::PushFont(m_fontMono);
    const float lineH = ImGui::GetTextLineHeight();
    auto lines = splitLines(m_scriptText);
    float gutterW = ImGui::CalcTextSize("00000").x + 14;
    float maxLineW = 0;
    for (auto& l : lines) maxLineW = std::max(maxLineW, ImGui::CalcTextSize(l.data(), l.data() + l.size()).x);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, light ? IM_COL32(252, 252, 253, 255) : IM_COL32(24, 25, 28, 255));
    ImGui::BeginChild("code", {0, 0}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImVec2 visMin = ImGui::GetWindowPos(), visSize = ImGui::GetWindowSize();
    float scrollY = ImGui::GetScrollY();
    ImGui::SetCursorPos({gutterW, 0});
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 size{std::max(avail.x, maxLineW + 60), (static_cast<float>(lines.size()) + 6) * lineH};
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));  // we draw the text ourselves (highlighted)
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    ImGui::InputTextMultiline("##src", &m_scriptText, size, ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_NoHorizontalScroll);
    ImGuiID inputId = ImGui::GetItemID();
    trackRect("scripts.code");
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImVec2 textOrigin{ImGui::GetItemRectMin().x + 4, ImGui::GetItemRectMin().y};
    lines = splitLines(m_scriptText);  // the edit may have reallocated the buffer
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // diagnostics for this file
    std::unordered_map<int, std::vector<const Diagnostic*>> diagByLine;
    for (auto& d : m_session.lastScriptBuild().diagnostics)
        if (d.file == m_scriptFile && d.line > 0) diagByLine[d.line].push_back(&d);

    // visible range
    int first = std::max(0, static_cast<int>(scrollY / lineH) - 1);
    int last = std::min(static_cast<int>(lines.size()) - 1, static_cast<int>((scrollY + visSize.y) / lineH) + 1);
    bool inBlock = false;
    std::vector<Span> spans;
    ImFont* font = ImGui::GetFont();
    float fs = ImGui::GetFontSize();
    double t = ImGui::GetTime();
    for (int i = 0; i <= last; ++i) {
        std::string_view line = lines[static_cast<size_t>(i)];
        tokenize(line, inBlock, spans);  // also run for lines above the view (block comment state)
        if (i < first) continue;
        float y = textOrigin.y + static_cast<float>(i) * lineH;
        // jump highlight
        if (m_scriptJumpLine == i + 1 && t - m_scriptJumpTime < 2.5)
            dl->AddRectFilled({visMin.x, y}, {visMin.x + visSize.x + ImGui::GetScrollX(), y + lineH}, IM_COL32(240, 130, 40, 50));
        for (auto& sp : spans) {
            float x = textOrigin.x + ImGui::CalcTextSize(line.data(), line.data() + sp.begin).x;
            dl->AddText(font, fs, {x, y}, tokenColor(sp.kind, light), line.data() + sp.begin, line.data() + sp.end);
        }
        // gutter
        float gx = ImGui::GetWindowPos().x + 4 + ImGui::GetScrollX() * 0;
        std::string num = std::to_string(i + 1);
        ImU32 numCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        if (auto it = diagByLine.find(i + 1); it != diagByLine.end()) {
            bool err = std::any_of(it->second.begin(), it->second.end(), [](const Diagnostic* d) { return d->severity == Diagnostic::Severity::Error; });
            ImU32 col = err ? IM_COL32(240, 80, 80, 255) : IM_COL32(240, 190, 60, 255);
            numCol = col;
            // squiggle under the line's text
            size_t b = line.find_first_not_of(" \t");
            float x0 = textOrigin.x + (b == std::string_view::npos ? 0 : ImGui::CalcTextSize(line.data(), line.data() + b).x);
            float x1 = std::max(x0 + 20, textOrigin.x + ImGui::CalcTextSize(line.data(), line.data() + line.size()).x);
            float yy = y + lineH - 2;
            for (float x = x0; x < x1; x += 4) dl->AddLine({x, yy + ((static_cast<int>((x - x0) / 4) & 1) ? 1.5f : -0.5f)},
                                                          {x + 4, yy + ((static_cast<int>((x - x0) / 4) & 1) ? -0.5f : 1.5f)}, col, 1.2f);
            ImVec2 mp = ImGui::GetIO().MousePos;
            if (ImGui::IsWindowHovered() && mp.y >= y && mp.y < y + lineH) {
                std::string tip;
                for (auto* d : it->second) tip += std::format("{}: {}\n", toString(d->severity), d->message);
                ImGui::SetTooltip("%s", tip.c_str());
            }
        }
        dl->AddText(font, fs, {ImGui::GetWindowPos().x + gutterW - 8 - ImGui::CalcTextSize(num.c_str()).x - ImGui::GetScrollX() * 0, y}, numCol,
                    num.c_str());
        (void)gx;
    }
    // caret (the input's own caret uses the transparent text color)
    if (ImGuiInputTextState* st = ImGui::GetInputTextState(inputId)) {
        int cursor = std::clamp(st->GetCursorPos(), 0, static_cast<int>(m_scriptText.size()));
        int lineNo = static_cast<int>(std::count(m_scriptText.begin(), m_scriptText.begin() + cursor, '\n'));
        size_t lineStart = m_scriptText.rfind('\n', cursor > 0 ? static_cast<size_t>(cursor) - 1 : 0);
        lineStart = (lineStart == std::string::npos || cursor == 0) ? 0 : lineStart + 1;
        if (cursor > 0 && m_scriptText[static_cast<size_t>(cursor) - 1] == '\n') lineStart = static_cast<size_t>(cursor);
        float x = textOrigin.x + ImGui::CalcTextSize(m_scriptText.data() + lineStart, m_scriptText.data() + cursor).x;
        float y = textOrigin.y + static_cast<float>(lineNo) * lineH;
        if (std::fmod(t, 1.0) < 0.6) dl->AddLine({x, y}, {x, y + lineH}, light ? IM_COL32(20, 20, 20, 255) : IM_COL32(255, 255, 255, 230), 1.5f);
        m_scriptCursorLine = lineNo + 1;
        m_scriptCursorCol = cursor - static_cast<int>(lineStart) + 1;
        // keep the caret visible
        float top = static_cast<float>(lineNo) * lineH;
        if (top < scrollY) ImGui::SetScrollY(top);
        else if (top + lineH > scrollY + visSize.y - lineH * 1.5f) ImGui::SetScrollY(top + lineH * 2.5f - visSize.y);
    }
    if (m_scriptJumpLine > 0 && t - m_scriptJumpTime < 0.05 + ImGui::GetIO().DeltaTime) {
        ImGui::SetScrollY(std::max(0.0f, (static_cast<float>(m_scriptJumpLine) - 1) * lineH - visSize.y * 0.35f));
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// ------------------------------------------------------------- Build Output --
void EditorApp::drawBuildOutput() {
    PanelDef* pd = panel("Build Output");
    if (!ImGui::Begin("Build Output", pd ? &pd->open : nullptr)) {
        ImGui::End();
        return;
    }
    const auto& b = m_session.lastScriptBuild();
    ImGui::BeginDisabled(m_scriptBuildRunning || !m_session.hasProject());
    if (ImGui::Button(ICON_BOLT " Build")) m_commands.execute("scripts.build");
    ImGui::SameLine();
    if (ImGui::Button(ICON_REDO " Rebuild all")) m_commands.execute("scripts.rebuild");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ICON_COPY " Copy log")) ImGui::SetClipboardText(b.log.c_str());
    ImGui::SameLine();
    ImGui::Checkbox("Raw log", &m_buildShowLog);
    ImGui::SameLine();
    if (m_scriptBuildRunning) ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s Building...", ICON_SPINNER);
    else if (!m_session.hasScriptBuild()) ImGui::TextDisabled("No build yet");
    else if (b.ok)
        ImGui::TextColored({0.35f, 0.85f, 0.45f, 1}, "%s Build succeeded in %.2fs (%d/%d compiled, %d warnings) -> %s", ICON_CHECK, b.seconds,
                           b.compiled, b.sources + 1, b.warnings(), narrow(b.module.filename().wstring()).c_str());
    else ImGui::TextColored({1, 0.45f, 0.45f, 1}, "%s Build failed: %d errors, %d warnings (%.2fs)", ICON_ERROR, b.errors(), b.warnings(), b.seconds);
    ImGui::Separator();
    ImGui::PushFont(m_fontMono);
    if (m_buildShowLog) {
        ImGui::BeginChild("log", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(b.log.c_str());
        ImGui::EndChild();
    } else if (ImGui::BeginTable("diag", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 22 * effectiveScale());
        ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, 200 * effectiveScale());
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
        int i = 0;
        for (auto& d : b.diagnostics) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImVec4 col = d.severity == Diagnostic::Severity::Error ? ImVec4(1, 0.45f, 0.45f, 1)
                         : d.severity == Diagnostic::Severity::Warning ? ImVec4(1, 0.78f, 0.3f, 1)
                                                                       : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            ImGui::TextColored(col, "%s", d.severity == Diagnostic::Severity::Error ? ICON_ERROR : d.severity == Diagnostic::Severity::Warning ? ICON_WARNING : ICON_INFO);
            ImGui::TableNextColumn();
            std::string loc = d.file.empty() ? "" : std::format("{}:{}:{}", d.file, d.line, d.column);
            ImGui::PushID(i++);
            if (ImGui::Selectable(loc.empty() ? "-" : loc.c_str(), false, ImGuiSelectableFlags_SpanAllColumns) && !d.file.empty())
                openScriptFile(d.file, d.line);
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", d.message.c_str());
        }
        if (b.diagnostics.empty() && m_session.hasScriptBuild()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("no diagnostics");
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();
    ImGui::End();
}

// ------------------------------------------------------------- New Script --
void EditorApp::drawNewScriptDialog() {
    if (m_showNewScript) {
        ImGui::OpenPopup("New Script");
        m_showNewScript = false;
        m_newScriptError.clear();
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (!ImGui::BeginPopupModal("New Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    static const char* kinds[] = {"behaviour", "service", "character", "empty"};
    static const char* desc[] = {"Per-entity script (Behaviour) with a property and OnStart/OnUpdate",
                                 "Game-wide singleton (Service): missions, wanted level, economy...",
                                 "Third-person character controller (input -> movement -> facing)", "Just the include and namespace"};
    ImGui::SetNextItemWidth(260 * effectiveScale());
    ImGui::InputText("Class name", &m_newScriptName);
    trackRect("newscript.name");
    ImGui::SetNextItemWidth(260 * effectiveScale());
    ImGui::Combo("Template", &m_newScriptKind, kinds, 4);
    ImGui::TextDisabled("%s", desc[std::clamp(m_newScriptKind, 0, 3)]);
    ImGui::TextDisabled("Creates Scripts/%s.cpp", m_newScriptName.c_str());
    if (!m_newScriptError.empty()) ImGui::TextColored({1, 0.45f, 0.45f, 1}, "%s", m_newScriptError.c_str());
    if (ImGui::Button(ICON_CHECK " Create", {120 * effectiveScale(), 0})) {
        auto r = createScript(m_session.project(), m_newScriptName, kinds[std::clamp(m_newScriptKind, 0, 3)]);
        if (!r) m_newScriptError = r.error();
        else {
            openScriptFile(m_newScriptName + ".cpp");
            toast(ToastLevel::Success, "Created Scripts/" + m_newScriptName + ".cpp");
            ImGui::CloseCurrentPopup();
        }
    }
    trackRect("newscript.create");
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// --------------------------------------------------- Behaviours (Inspector) --
namespace {
bool editScalar(const script::PropertyDesc& p, Json& v, const char* label, EditorApp* app, EditorSession& session,
                const std::function<bool(AssetType, std::string&)>& picker) {
    bool changed = false;
    (void)app;
    if (p.type == "float") {
        float f = v.is_number() ? v.get<float>() : 0.0f;
        changed = p.hasRange ? ImGui::SliderFloat(label, &f, p.min, p.max) : ImGui::DragFloat(label, &f, 0.05f);
        if (changed) v = f;
    } else if (p.type == "int") {
        int i = v.is_number() ? v.get<int>() : 0;
        changed = p.hasRange ? ImGui::SliderInt(label, &i, static_cast<int>(p.min), static_cast<int>(p.max)) : ImGui::DragInt(label, &i);
        if (changed) v = i;
    } else if (p.type == "bool") {
        bool b = v.is_boolean() && v.get<bool>();
        changed = ImGui::Checkbox(label, &b);
        if (changed) v = b;
    } else if (p.type == "string") {
        std::string sv = v.is_string() ? v.get<std::string>() : "";
        changed = ImGui::InputText(label, &sv);
        if (changed) v = sv;
    } else if (p.type == "vec3" || p.type == "color") {
        float f[3] = {0, 0, 0};
        if (v.is_array() && v.size() >= 3)
            for (int k = 0; k < 3; ++k) f[k] = v[static_cast<size_t>(k)].is_number() ? v[static_cast<size_t>(k)].get<float>() : 0;
        changed = p.type == "color" ? ImGui::ColorEdit3(label, f) : ImGui::DragFloat3(label, f, 0.05f);
        if (changed) v = Json::array({f[0], f[1], f[2]});
    } else if (p.type == "quat") {
        Vec3 deg{0, 0, 0};
        if (v.is_array() && v.size() == 4) {
            Vec3 e = Quat{v[0].get<float>(), v[1].get<float>(), v[2].get<float>(), v[3].get<float>()}.toEuler();
            deg = {degrees(e.x), degrees(e.y), degrees(e.z)};
        } else if (v.is_array() && v.size() == 3) {
            deg = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
        }
        changed = ImGui::DragFloat3(label, &deg.x, 0.5f);
        if (changed) v = Json::array({deg.x, deg.y, deg.z});  // SDK accepts euler degrees
    } else if (p.type == "entity") {
        EntityId cur = v.is_number_integer() ? v.get<EntityId>() : 0;
        std::string preview = cur && session.scene().isValid(cur) ? std::format("{} (#{})", session.scene().displayName(cur), cur)
                              : cur                              ? std::format("missing #{}", cur)
                                                                 : "(none)";
        if (ImGui::BeginCombo(label, preview.c_str())) {
            if (ImGui::Selectable("(none)", cur == 0)) {
                v = 0;
                changed = true;
            }
            for (EntityId e : session.scene().depthFirstOrder()) {
                std::string n = std::format("{} (#{})", session.scene().displayName(e), e);
                if (ImGui::Selectable(n.c_str(), e == cur)) {
                    v = e;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
    } else if (p.type == "asset") {
        std::string sv = v.is_string() ? v.get<std::string>() : "";
        changed = ImGui::InputText(label, &sv);
        if (changed) v = sv;
        ImGui::SameLine();
        if (ImGui::SmallButton("...")) ImGui::OpenPopup("pickasset");
        if (ImGui::BeginPopup("pickasset")) {
            std::string picked;
            AssetType kind = p.assetKind.empty() ? AssetType::Unknown : assetTypeFromString(p.assetKind);
            if (picker(kind, picked)) {
                v = picked;
                changed = true;
            }
            ImGui::EndPopup();
        }
    } else {
        ImGui::TextDisabled("%s: %s (%s)", label, v.dump().c_str(), p.type.c_str());
    }
    return changed;
}
} // namespace

void EditorApp::drawBehavioursInspector(EntityId e) {
    auto* bc = m_session.scene().get<BehavioursComponent>(e);
    if (!bc || !bc->list.is_array()) return;
    auto& scripts = m_session.world().scripts();
    std::string toDetach;
    Json list = bc->list;  // copy: edits go through the session (undo)
    for (size_t i = 0; i < list.size(); ++i) {
        const Json& item = list[i];
        std::string cls = item.value("class", "?");
        bool enabled = item.value("enabled", true);
        ImGui::PushID(static_cast<int>(i));
        const script::ClassDesc* cd = scripts.module().find(cls);
        ImGui::Indent(4);
        bool open = ImGui::TreeNodeEx("beh", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap,
                                      "%s %s%s", ICON_CODE, cls.c_str(), cd ? "" : "  (not in the loaded module)");
        trackRect("beh." + cls);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60 * effectiveScale() + ImGui::GetCursorPosX());
        if (ImGui::Checkbox("##en", &enabled)) (void)m_session.setBehaviourEnabled(e, cls, enabled);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enabled");
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_XMARK)) toDetach = cls;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Detach %s", cls.c_str());
        if (open) {
            if (!cd) {
                ImGui::TextDisabled("Build the scripts to edit its properties. Stored overrides:");
                ImGui::TextWrapped("%s", item.value("properties", Json::object()).dump().c_str());
            } else {
                auto props = m_session.behaviourProperties(e, cls);
                Json values = props ? *props : Json::object();
                const Json overrides = item.value("properties", Json::object());
                for (auto& p : cd->properties) {
                    if (p.hidden) continue;
                    ImGui::PushID(p.name.c_str());
                    Json v = values.contains(p.name) ? values[p.name] : p.defaultValue;
                    bool changed = false;
                    ImGui::BeginDisabled(p.readOnly);
                    auto picker = [this](AssetType kind, std::string& picked) {
                        bool chosen = false;
                        if (ImGui::MenuItem("(none)")) {
                            picked.clear();
                            chosen = true;
                        }
                        for (auto& r : m_session.registry().all())
                            if ((kind == AssetType::Unknown || r.type == kind) && ImGui::MenuItem(r.path.c_str())) {
                                picked = r.path;  // content path: readable and what Scene::Spawn expects
                                chosen = true;
                            }
                        return chosen;
                    };
                    if (p.isArray) {
                        if (!v.is_array()) v = Json::array();
                        if (ImGui::TreeNodeEx("arr", ImGuiTreeNodeFlags_SpanAvailWidth, "%s [%zu]", p.name.c_str(), v.size())) {
                            int remove = -1;
                            for (size_t k = 0; k < v.size(); ++k) {
                                ImGui::PushID(static_cast<int>(k));
                                std::string lbl = std::format("[{}]", k);
                                changed |= editScalar(p, v[k], lbl.c_str(), this, m_session, picker);
                                ImGui::SameLine();
                                if (ImGui::SmallButton("-")) remove = static_cast<int>(k);
                                ImGui::PopID();
                            }
                            if (remove >= 0) {
                                v.erase(static_cast<size_t>(remove));
                                changed = true;
                            }
                            if (ImGui::SmallButton(ICON_PLUS " Add")) {
                                Json def = p.type == "float" || p.type == "int" || p.type == "entity" ? Json(0)
                                           : p.type == "bool"                                         ? Json(false)
                                           : p.type == "vec3" || p.type == "color"                    ? Json::array({0, 0, 0})
                                           : p.type == "quat"                                         ? Json::array({0, 0, 0, 1})
                                                                                                      : Json("");
                                v.push_back(def);
                                changed = true;
                            }
                            ImGui::TreePop();
                        }
                    } else {
                        changed = editScalar(p, v, p.name.c_str(), this, m_session, picker);
                    }
                    ImGui::EndDisabled();
                    bool overridden = overrides.contains(p.name);
                    if (ImGui::IsItemHovered() && (!p.tooltip.empty() || overridden))
                        ImGui::SetTooltip("%s%s%s", p.tooltip.c_str(), p.tooltip.empty() ? "" : "\n",
                                          overridden ? "Overridden in this scene" : "Class default");
                    if (changed) {
                        auto st = m_session.setBehaviourProperty(e, cls, p.name, v, std::format("beh:{}:{}:{}", e, cls, p.name));
                        if (!st) toast(ToastLevel::Warning, st.error);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) m_session.history().closeMerge();
                    ImGui::PopID();
                }
                if (cd->properties.empty()) ImGui::TextDisabled("(no properties)");
            }
            ImGui::TreePop();
        }
        ImGui::Unindent(4);
        ImGui::PopID();
    }
    if (!toDetach.empty())
        if (auto st = m_session.detachBehaviour(e, toDetach); !st) toast(ToastLevel::Warning, st.error);
}

// Add Component > Script submenu entries.
void EditorApp::drawAddScriptMenu(EntityId e) {
    auto& mod = m_session.world().scripts().module();
    if (!m_session.world().scripts().hasModule()) {
        ImGui::TextDisabled("No script module loaded (build the scripts)");
        return;
    }
    int n = 0;
    for (auto& c : mod.classes) {
        if (c.isService) continue;
        ++n;
        if (ImGui::MenuItem(c.name.c_str()))
            if (auto st = m_session.attachBehaviour(e, c.name); !st) toast(ToastLevel::Warning, st.error);
    }
    if (!n) ImGui::TextDisabled("No behaviours in the module");
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_PLUS " New Script...")) m_showNewScript = true;
}

// ---------------------------------------------------------- play-mode input --
void EditorApp::feedPlayInput() {
    if (!m_session.playing()) return;
    if (m_session.scriptedInputOnly) return;  // MCP play {exclusiveInput}: only scripted input drives this run (1.3.0)
    script::InputFrame f;
    ImGuiIO& io = ImGui::GetIO();
    bool locked = m_capture.mode() == CaptureMode::PlayLock;
    if (!io.MouseDown[0]) m_suppressGameLeft = false;
    // retained game UI (UiScreen buttons): the pointer in game-view pixels while the cursor is free
    auto& ptr = m_session.world().uiPointer;
    bool free = m_vpHovered && !locked;
    ptr.pos = free ? Vec2{io.MousePos.x - m_vpOrigin.x, io.MousePos.y - m_vpOrigin.y} : Vec2{-1, -1};
    ptr.down = free && io.MouseDown[0];
    GLFWwindow* window = m_window ? m_window->glfwHandle() : nullptr;
    const bool keyboardFocus = window && glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE &&
                               (m_vpFocused || locked) && !io.WantTextInput;
    if (keyboardFocus) {
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
            int code = imguiKeyToCode(static_cast<ImGuiKey>(k));
            if (code >= 0 && code < 512 && ImGui::IsKeyDown(static_cast<ImGuiKey>(k))) f.down[static_cast<size_t>(code)] = true;
        }
        for (int b = 0; b < 3; ++b) f.down[static_cast<size_t>(500 + b)] = io.MouseDown[b] && (m_vpHovered || locked);
        if (m_suppressGameLeft) f.down[500] = false;  // the click that took the mouse back is not a game click
        if (m_vpHovered || locked) {
            ImVec2 d = viewportMouseDelta();
            f.mouseDx = d.x;
            f.mouseDy = d.y;
        }
        const InputState& wi = m_window->input();  // gamepad (GLFW), when the viewport has focus
        if (wi.padConnected) {
            f.pad = wi.padAxes;
            for (size_t k = 400; k < 418; ++k) f.down[k] = wi.raw[k];
        }
    }
    m_session.liveInput = f;  // consumed by each fixed step (and recorded by the Time Machine)
}

} // namespace fledge::editor
