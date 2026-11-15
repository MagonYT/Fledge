// Code workspace UI (see editor_code.h, docs/CODE_EDITOR.md).
#include "editor_code.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>
#include <cstring>
#include <regex>

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
#include "fledge/editor/fuzzy.h"
#include "fledge/packager/script_build.h"

namespace fledge::editor {

using namespace fledge::code;

namespace {
double now() { return ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0; }

bool isTextFile(const fs::path& p) {
    static const std::set<std::string> ext = {".cpp", ".h", ".hpp", ".cc", ".cxx", ".inl", ".c", ".json", ".md", ".txt", ".cmake", ".glsl",
                                              ".hlsl", ".hlsli", ".vert", ".frag", ".fx", ".fledge", ".fscene", ".fmat", ".fmodel", ".meta",
                                              ".ini", ".yaml", ".yml", ".clangd", ".gitignore", ".py", ".lua", ".bat", ".ps1", ".csv", ".xml"};
    std::string e = toLower(narrow(p.extension().wstring()));
    std::string n = toLower(narrow(p.filename().wstring()));
    return ext.count(e) || n == "cmakelists.txt" || n == ".clangd" || n == ".gitignore";
}

bool skipDir(const std::string& name) {
    return name == "Intermediate" || name == "Build" || name == ".git" || name == ".vs" || name == ".vscode" || name == "node_modules" ||
           name == ".cache";
}

std::string relOf(const fs::path& root, const fs::path& p) {
    if (root.empty()) return narrow(p.generic_wstring());
    std::error_code ec;
    fs::path r = p.lexically_relative(root);
    std::string s = narrow(r.generic_wstring());
    if (s.empty() || startsWith(s, "..")) return narrow(p.generic_wstring());
    return s;
}

const char* fileIcon(Lang l) {
    switch (l) {
        case Lang::Cpp: return ICON_CODE;
        case Lang::Json: return ICON_LIST;
        case Lang::Markdown: return ICON_BOOK;
        case Lang::Shader: return ICON_PALETTE;
        case Lang::CMake: return ICON_GEAR;
        default: return ICON_FILE;
    }
}

const char* lspLanguageId(Lang l) {
    switch (l) {
        case Lang::Cpp: return "cpp";
        case Lang::Shader: return "hlsl";
        case Lang::Json: return "json";
        case Lang::Markdown: return "markdown";
        case Lang::CMake: return "cmake";
        default: return "plaintext";
    }
}

std::string utf8(unsigned int c) {
    std::string s;
    if (c < 0x80) s += static_cast<char>(c);
    else if (c < 0x800) {
        s += static_cast<char>(0xC0 | (c >> 6));
        s += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        s += static_cast<char>(0xE0 | (c >> 12));
        s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (c >> 18));
        s += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (c & 0x3F));
    }
    return s;
}

ImU32 rgb(uint32_t c) { return IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, 255); }

// gdb reports C:/a\b.cpp, c:\a\b.cpp ...: compare normalised, case-insensitively (Windows paths)
std::string pathKey(const fs::path& p) { return toLower(narrow(p.lexically_normal().generic_wstring())); }
bool samePath(const fs::path& a, const fs::path& b) { return pathKey(a) == pathKey(b); }
} // namespace

// ============================================================================== settings ===
const std::vector<CodeKeyDef>& codeKeyDefs() {
    static const std::vector<CodeKeyDef> d = {
        {"find", "Find", ImGuiMod_Ctrl | ImGuiKey_F},
        {"replace", "Replace", ImGuiMod_Ctrl | ImGuiKey_H},
        {"search", "Search in Files", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F},
        {"gotoLine", "Go to Line", ImGuiMod_Ctrl | ImGuiKey_G},
        {"gotoSymbol", "Go to Symbol in File", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O},
        {"quickOpen", "Quick Open File", ImGuiMod_Ctrl | ImGuiKey_E},
        {"comment", "Toggle Line Comment", ImGuiMod_Ctrl | ImGuiKey_Slash},
        {"selectNext", "Add Selection to Next Match", ImGuiMod_Ctrl | ImGuiKey_D},
        {"cursorAbove", "Add Cursor Above", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_UpArrow},
        {"cursorBelow", "Add Cursor Below", ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_DownArrow},
        {"lineUp", "Move Line Up", ImGuiMod_Alt | ImGuiKey_UpArrow},
        {"lineDown", "Move Line Down", ImGuiMod_Alt | ImGuiKey_DownArrow},
        {"deleteLine", "Delete Line", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_K},
        {"undo", "Undo", ImGuiMod_Ctrl | ImGuiKey_Z},
        {"redo", "Redo", ImGuiMod_Ctrl | ImGuiKey_Y},
        {"format", "Format Document", ImGuiMod_Shift | ImGuiMod_Alt | ImGuiKey_F},
        {"definition", "Go to Definition", ImGuiKey_F12},
        {"references", "Find All References", ImGuiMod_Shift | ImGuiKey_F12},
        {"rename", "Rename Symbol", ImGuiKey_F2},
        {"complete", "Trigger Suggestions", ImGuiMod_Ctrl | ImGuiKey_Space},
        {"wordWrap", "Toggle Word Wrap", ImGuiMod_Alt | ImGuiKey_Z},
        {"zoomIn", "Zoom In", ImGuiMod_Ctrl | ImGuiKey_Equal},
        {"zoomOut", "Zoom Out", ImGuiMod_Ctrl | ImGuiKey_Minus},
        {"split", "Split Editor", ImGuiMod_Ctrl | ImGuiKey_Backslash},
        {"closeTab", "Close Editor", ImGuiMod_Ctrl | ImGuiKey_W},
        {"terminal", "Toggle Panel / Terminal", ImGuiMod_Ctrl | ImGuiKey_GraveAccent},
        {"run", "Run (Play)", ImGuiKey_F5},
        {"breakpoint", "Toggle Breakpoint", ImGuiKey_F9},
        {"debug", "Start Debugging (gdb, separate process)", ImGuiMod_Ctrl | ImGuiKey_F5},
        {"debugContinue", "Debug: Continue (while stopped)", ImGuiKey_F5},
        {"stepOver", "Debug: Step Over", ImGuiKey_F10},
        {"stepInto", "Debug: Step Into", ImGuiKey_F11},
        {"stepOut", "Debug: Step Out", ImGuiMod_Shift | ImGuiKey_F11},
        {"debugStop", "Debug: Stop", ImGuiMod_Shift | ImGuiKey_F5},
        {"fold", "Fold", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_LeftBracket},
        {"unfold", "Unfold", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_RightBracket},
    };
    return d;
}

Json CodeSettings::toJson() const {
    Json k = Json::object();
    for (auto& [id, chord] : keys) k[id] = static_cast<int>(chord);
    return {{"fontSize", fontSize}, {"tabSize", tabSize},     {"formatOnSave", formatOnSave}, {"autoSave", autoSave},
            {"theme", theme},       {"wordWrap", wordWrap},   {"minimap", minimap},           {"lineNumbers", lineNumbers},
            {"indentGuides", indentGuides}, {"autoClose", autoClose}, {"whitespace", whitespace}, {"useClangd", useClangd},
            {"clangdPath", clangdPath}, {"shell", shell}, {"keys", k}};
}

void CodeSettings::fromJson(const Json& j) {
    if (!j.is_object()) return;
    fontSize = std::clamp(j.value("fontSize", fontSize), 8.0f, 32.0f);
    tabSize = std::clamp(j.value("tabSize", tabSize), 1, 8);
    formatOnSave = j.value("formatOnSave", formatOnSave);
    autoSave = std::clamp(j.value("autoSave", autoSave), 0, 2);
    theme = std::clamp(j.value("theme", theme), 0, 2);
    wordWrap = j.value("wordWrap", wordWrap);
    minimap = j.value("minimap", minimap);
    lineNumbers = j.value("lineNumbers", lineNumbers);
    indentGuides = j.value("indentGuides", indentGuides);
    autoClose = j.value("autoClose", autoClose);
    whitespace = std::clamp(j.value("whitespace", whitespace), 0, 2);
    useClangd = j.value("useClangd", useClangd);
    clangdPath = j.value("clangdPath", clangdPath);
    shell = j.value("shell", shell);
    if (j.contains("keys") && j["keys"].is_object())
        for (auto& [id, v] : j["keys"].items())
            if (v.is_number_integer()) keys[id] = static_cast<ImGuiKeyChord>(v.get<int>());
}

// ============================================================================== lifecycle ===
CodeWorkspace::CodeWorkspace(EditorApp& app) : m_app(app) {
    for (auto& k : codeKeyDefs()) settings.keys[k.id] = k.def;
    loadSettings();
}

CodeWorkspace::~CodeWorkspace() {
    stopDebugging();
    saveSession();
    for (auto& t : m_terms) t->stop();
    m_lsp.shutdown();
}

void CodeWorkspace::loadSettings() {
    auto t = readFileText(m_app.m_opt.settingsDir / "code_editor.json");
    if (t) settings.fromJson(Json::parse(*t, nullptr, false));
}

void CodeWorkspace::saveSettings() {
    std::error_code ec;
    fs::create_directories(m_app.m_opt.settingsDir, ec);
    writeFileText(m_app.m_opt.settingsDir / "code_editor.json", settings.toJson().dump(2));
}

void CodeWorkspace::onProjectChanged() {
    fs::path newRoot = m_app.m_session.hasProject() ? m_app.m_session.project().rootDir() : fs::path();
    if (newRoot == m_root) return;
    saveSession();
    for (auto& g : m_groups) g = {};
    m_docs.clear();
    m_lsp.shutdown();
    m_lspTried = false;
    m_index.clear();
    m_indexTime = -100;
    m_fileCacheTime = -100;
    m_gitTime = -100;
    m_hits.clear();
    m_refs.clear();
    for (auto& t : m_terms) t->stop();
    m_terms.clear();
    m_root = newRoot;
    if (!m_root.empty()) loadSession();
}

void CodeWorkspace::saveSession() {
    if (m_root.empty()) return;
    fs::path file = m_root / "Intermediate" / "Editor" / "code_session.json";
    std::error_code fec;
    if (m_docs.empty() && !fs::exists(file, fec)) return;  // never used here: leave the project untouched
    Json j;
    j["version"] = 1;
    j["split"] = m_split;
    j["activeGroup"] = m_activeGroup;
    j["activity"] = activity;
    j["bottomTab"] = bottomTab;
    j["showBottom"] = showBottom;
    j["expanded"] = Json(std::vector<std::string>(m_expanded.begin(), m_expanded.end()));
    for (int g = 0; g < 2; ++g) {
        Json tabs = Json::array();
        for (Doc* d : m_groups[g].tabs) {
            if (!d->virtualName.empty()) continue;
            Json t = {{"rel", d->rel},
                      {"line", d->buf.cursors[0].pos.line},
                      {"col", d->buf.cursors[0].pos.col},
                      {"scrollY", d->scrollY},
                      {"pinned", d->pinned},
                      {"active", m_groups[g].active == d},
                      {"folded", Json(std::vector<int>(d->folded.begin(), d->folded.end()))},
                      {"breakpoints", Json(std::vector<int>(d->breakpoints.begin(), d->breakpoints.end()))}};
            if (d->buf.dirty()) t["unsaved"] = d->buf.text();  // hot exit: unsaved edits survive a restart
            tabs.push_back(t);
        }
        j["groups"].push_back(tabs);
    }
    fs::path dir = m_root / "Intermediate" / "Editor";
    std::error_code ec;
    fs::create_directories(dir, ec);
    writeFileText(dir / "code_session.json", j.dump(1));
}

void CodeWorkspace::loadSession() {
    auto t = readFileText(m_root / "Intermediate" / "Editor" / "code_session.json");
    if (!t) return;
    Json j = Json::parse(*t, nullptr, false);
    if (!j.is_object()) return;
    m_split = j.value("split", false);
    activity = j.value("activity", activity);
    bottomTab = j.value("bottomTab", bottomTab);
    showBottom = j.value("showBottom", true);
    for (auto& e : j.value("expanded", Json::array()))
        if (e.is_string()) m_expanded.insert(e.get<std::string>());
    auto groups = j.value("groups", Json::array());
    for (int g = 0; g < 2 && g < static_cast<int>(groups.size()); ++g) {
        for (auto& tab : groups[static_cast<size_t>(g)]) {
            fs::path p = m_root / pathFromUtf8(tab.value("rel", ""));
            std::error_code ec;
            if (!fs::is_regular_file(p, ec)) continue;
            m_activeGroup = g;
            Doc* d = openDoc(p, true);
            if (!d) continue;
            d->pinned = tab.value("pinned", false);
            if (tab.contains("unsaved") && tab["unsaved"].is_string()) {
                std::string saved = d->buf.text();
                d->buf.setText(saved);
                d->buf.replaceRange({0, 0}, {d->buf.lineCount() - 1, static_cast<int>(d->buf.line(d->buf.lineCount() - 1).size())}, tab["unsaved"].get<std::string>());
            }
            d->buf.setCursor({tab.value("line", 0), tab.value("col", 0)});
            d->scrollY = tab.value("scrollY", 0.0f);
            for (auto& f : tab.value("folded", Json::array())) d->folded.insert(f.get<int>());
            for (auto& b : tab.value("breakpoints", Json::array())) d->breakpoints.insert(b.get<int>());
            if (tab.value("active", false)) m_groups[g].active = d;
        }
    }
    m_activeGroup = std::clamp(j.value("activeGroup", 0), 0, m_split ? 1 : 0);
}

// ============================================================================== documents ===
CodeWorkspace::Doc* CodeWorkspace::findDoc(const fs::path& p) {
    fs::path n = p.lexically_normal();
    for (auto& d : m_docs)
        if (d->virtualName.empty() && d->path == n) return d.get();
    return nullptr;
}

CodeWorkspace::Doc* CodeWorkspace::openDoc(const fs::path& p, bool pin) {
    Group& g = m_groups[m_activeGroup];
    Doc* d = findDoc(p);
    if (!d) {
        auto text = readFileText(p);
        if (!text) {
            m_app.toast(ToastLevel::Error, "Cannot open " + narrow(p.wstring()));
            return nullptr;
        }
        auto doc = std::make_unique<Doc>();
        doc->path = p.lexically_normal();
        doc->rel = relOf(m_root, doc->path);
        doc->crlf = text->find("\r\n") != std::string::npos;
        doc->buf.setText(*text);
        doc->buf.tabSize = settings.tabSize;
        doc->buf.autoClose = settings.autoClose;
        doc->lang = langForPath(narrow(p.filename().wstring()));
        std::error_code ec;
        doc->mtime = fs::last_write_time(p, ec);
        d = doc.get();
        m_docs.push_back(std::move(doc));
    }
    if (std::find(g.tabs.begin(), g.tabs.end(), d) == g.tabs.end()) {
        // a preview (unpinned) tab is replaced by the next preview
        if (!pin)
            for (auto it = g.tabs.begin(); it != g.tabs.end(); ++it)
                if ((*it)->preview && !(*it)->buf.dirty()) {
                    *it = d;
                    d->preview = true;
                    g.active = d;
                    return d;
                }
        g.tabs.push_back(d);
        d->preview = !pin;
    } else if (pin) {
        d->preview = false;
    }
    g.active = d;
    return d;
}

void CodeWorkspace::openFile(const fs::path& file, int line, int col, bool pin) {
    if (m_root.empty()) onProjectChanged();
    Doc* d = openDoc(file, pin);
    if (!d) return;
    if (line > 0) {
        d->buf.setCursor({line - 1, std::max(0, col - 1)});
        d->reveal = true;
    }
    m_editorFocused = true;
}

void CodeWorkspace::activate(Doc* d, int group) {
    m_activeGroup = group;
    m_groups[group].active = d;
}

void CodeWorkspace::closeDoc(Doc* d, int group) {
    Group& g = m_groups[group];
    auto it = std::find(g.tabs.begin(), g.tabs.end(), d);
    if (it == g.tabs.end()) return;
    size_t idx = static_cast<size_t>(it - g.tabs.begin());
    g.tabs.erase(it);
    if (g.active == d) g.active = g.tabs.empty() ? nullptr : g.tabs[std::min(idx, g.tabs.size() - 1)];
    bool stillShown = false;
    for (auto& gg : m_groups) stillShown = stillShown || std::find(gg.tabs.begin(), gg.tabs.end(), d) != gg.tabs.end();
    if (!stillShown) {
        if (d->lspOpen) m_lsp.didClose(d->path);
        m_docs.erase(std::remove_if(m_docs.begin(), m_docs.end(), [d](auto& u) { return u.get() == d; }), m_docs.end());
    }
    if (m_split && m_groups[1].tabs.empty()) {
        m_split = false;
        m_activeGroup = 0;
    }
}

void CodeWorkspace::requestClose(Doc* d, int group) {
    if (!d) return;
    if (d->buf.dirty() && d->virtualName.empty()) {  // ask first (VS Code style)
        m_pendingClose = d;
        m_pendingCloseGroup = group;
        return;
    }
    closeDoc(d, group);
}

void CodeWorkspace::drawUnsavedPrompt() {
    if (!m_pendingClose) return;
    bool exists = false;
    for (auto& u : m_docs) exists = exists || u.get() == m_pendingClose;
    if (!exists) {
        m_pendingClose = nullptr;
        return;
    }
    const char* id = "Unsaved changes##codeclose";
    if (!ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);
    float s = m_app.effectiveScale();
    if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Do you want to save the changes you made to %s?", narrow(m_pendingClose->path.filename().wstring()).c_str());
        ImGui::TextDisabled("Your changes will be lost if you don't save them.");
        ImGui::Spacing();
        Doc* d = m_pendingClose;
        int g = m_pendingCloseGroup;
        if (ImGui::Button("Save", {110 * s, 0}) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            m_pendingClose = nullptr;
            if (saveDoc(d)) closeDoc(d, g);
            ImGui::CloseCurrentPopup();
        }
        m_app.trackRect("code.unsaved.save");
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", {110 * s, 0})) {
            m_pendingClose = nullptr;
            closeDoc(d, g);  // the edits are discarded (the file on disk is unchanged)
            ImGui::CloseCurrentPopup();
        }
        m_app.trackRect("code.unsaved.discard");
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {110 * s, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_pendingClose = nullptr;
            ImGui::CloseCurrentPopup();
        }
        m_app.trackRect("code.unsaved.cancel");
        ImGui::EndPopup();
    }
}

bool CodeWorkspace::saveDoc(Doc* d) {
    if (!d || d->readOnly) return false;
    std::string text = d->buf.text();
    if (d->crlf) {
        std::string out;
        out.reserve(text.size() + static_cast<size_t>(d->buf.lineCount()));
        for (char c : text) {
            if (c == '\n') out += '\r';
            out += c;
        }
        text.swap(out);
    }
    // files inside Scripts/ go through the session (sandbox + hot reload bookkeeping)
    bool ok = false;
    if (m_app.m_session.hasProject()) {
        fs::path scripts = m_app.m_session.project().scriptsDir();
        std::string rel = relOf(scripts, d->path);
        if (rel != narrow(d->path.generic_wstring()) && resolveScriptPath(m_app.m_session.project(), rel)) {
            auto st = m_app.m_session.writeScriptFile(rel, text, false);
            ok = st.ok;
            if (!ok) m_app.toast(ToastLevel::Error, st.error);
        }
    }
    if (!ok && !m_app.m_session.hasProject()) ok = writeFileText(d->path, text);
    if (!ok && m_app.m_session.hasProject() && relOf(m_app.m_session.project().scriptsDir(), d->path) == narrow(d->path.generic_wstring()))
        ok = writeFileText(d->path, text);
    if (!ok) return false;
    d->buf.markSaved();
    d->preview = false;
    std::error_code ec;
    d->mtime = fs::last_write_time(d->path, ec);
    if (d->lspOpen) m_lsp.didSave(d->path);
    m_index.addFile(narrow(d->path.wstring()), d->buf.text(), d->lang);
    m_gitTime = -100;
    return true;
}

bool CodeWorkspace::saveActive() {
    Doc* d = active();
    if (!d) return false;
    auto finish = [this, d] {
        if (saveDoc(d)) m_app.toast(ToastLevel::Success, "Saved " + d->rel);
    };
    if (settings.formatOnSave && d->lang == Lang::Cpp && m_lsp.state() == LspClient::State::Ready) formatDoc(d, finish);
    else finish();
    return true;
}

void CodeWorkspace::saveAll() {
    for (auto& d : m_docs)
        if (d->buf.dirty() && d->virtualName.empty()) saveDoc(d.get());
}

bool CodeWorkspace::anyDirty() const {
    for (auto& d : m_docs)
        if (d->buf.dirty() && d->virtualName.empty()) return true;
    return false;
}

void CodeWorkspace::reloadIfChanged() {
    for (auto& d : m_docs) {
        if (!d->virtualName.empty()) continue;
        std::error_code ec;
        auto t = fs::last_write_time(d->path, ec);
        if (ec || t == d->mtime) continue;
        d->mtime = t;
        if (d->buf.dirty()) continue;  // keep the user's edits; saving overwrites
        if (auto text = readFileText(d->path)) {
            Pos keep = d->buf.cursors[0].pos;
            d->buf.setText(*text);
            d->buf.setCursor(d->buf.clamp(keep));
        }
    }
}

void CodeWorkspace::jumpTo(const fs::path& file, Pos p) {
    Doc* d = openDoc(file, true);
    if (!d) return;
    d->buf.setCursor(d->buf.clamp(p));
    d->reveal = true;
    // unfold the target
    for (auto it = d->folded.begin(); it != d->folded.end();) {
        bool hides = false;
        for (auto& f : d->folds)
            if (f.startLine == *it && p.line > f.startLine && p.line <= f.endLine) hides = true;
        it = hides ? d->folded.erase(it) : std::next(it);
    }
    m_editorFocused = true;
}

std::string CodeWorkspace::activeText() const { return active() ? active()->buf.text() : std::string(); }
std::string CodeWorkspace::activeRel() const { return active() ? active()->rel : std::string(); }
TextBuffer* CodeWorkspace::activeBuffer() { return active() ? &active()->buf : nullptr; }
std::string CodeWorkspace::terminalText(int i) const { return i >= 0 && i < static_cast<int>(m_terms.size()) ? m_terms[static_cast<size_t>(i)]->screen.text() : ""; }

std::vector<std::pair<int, int>> CodeWorkspace::findMatchesActive() {
    std::vector<std::pair<int, int>> out;
    if (!active() || m_findQuery.empty()) return out;
    if (auto m = findAll(active()->buf, m_findQuery, m_findOpt))
        for (auto& x : *m) out.push_back({x.start.line, x.start.col});
    return out;
}

std::vector<std::string> CodeWorkspace::projectFiles() {
    if (m_root.empty()) return {};
    if (now() - m_fileCacheTime < 3.0) return m_fileCache;
    m_fileCacheTime = now();
    m_fileCache.clear();
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(m_root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        std::string name = narrow(it->path().filename().wstring());
        if (it->is_directory(ec)) {
            if (skipDir(name)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && isTextFile(it->path())) m_fileCache.push_back(relOf(m_root, it->path()));
        if (m_fileCache.size() > 20000) break;
    }
    std::sort(m_fileCache.begin(), m_fileCache.end());
    return m_fileCache;
}

// ============================================================================== services ===
std::string CodeWorkspace::lspStatus() const {
    if (m_lsp.state() == LspClient::State::Ready) {
        std::string n = m_lsp.serverName();  // "clangd clangd version 23.1.0 (https://...)" -> "clangd 23.1.0"
        size_t v = n.find("version ");
        if (v != std::string::npos) n = "clangd " + n.substr(v + 8, n.find(' ', v + 8) - (v + 8));
        return n.empty() ? "clangd" : n;
    }
    if (m_lsp.state() == LspClient::State::Starting) return "clangd: starting...";
    return "Basic symbol index (clangd unavailable)";
}

void CodeWorkspace::startLsp() {
    m_lspTried = true;
    if (!settings.useClangd || m_root.empty()) {
        m_output += "[lsp] clangd disabled - using the basic symbol index\n";
        return;
    }
    fs::path exe = settings.clangdPath.empty() ? fs::path() : pathFromUtf8(settings.clangdPath);
    std::error_code ec;
    if (exe.empty() || !fs::exists(exe, ec)) exe = m_app.m_paths.resources / "clangd" / "bin" / "clangd.exe";
    if (!fs::exists(exe, ec)) exe = findOnPath("clangd.exe");
    if (exe.empty() || !fs::exists(exe, ec)) {
        m_output += "[lsp] clangd not found (bundled resources/clangd or PATH) - using the basic symbol index\n";
        return;
    }
    std::string cmd = quoteArg(narrow(exe.wstring())) + " --log=error --background-index --header-insertion=never --pch-storage=memory";
    if (m_app.m_session.hasProject()) {
        fs::path scripts = m_app.m_session.project().scriptsDir();
        // compile_commands.json for the Fledge Script files (same flags as the real build)
        (void)writeScriptIdeFiles(m_app.m_session.project(), m_app.m_session.scriptBuildOptions());
        cmd += " " + quoteArg("--compile-commands-dir=" + narrow(scripts.wstring()));
        fs::path cxx = m_app.m_session.scriptBuildOptions().toolchain.cxx;
        if (cxx.empty())
            if (auto tc = findToolchain()) cxx = tc->cxx;
        if (!cxx.empty()) cmd += " " + quoteArg("--query-driver=" + narrow(cxx.generic_wstring()));
    }
    if (m_lsp.startProcess(cmd, m_root)) m_output += "[lsp] started " + cmd + "\n";
    else m_output += "[lsp] " + m_lsp.status() + "\n";
}

void CodeWorkspace::lspSync(Doc* d) {
    if (!d || !d->virtualName.empty() || m_lsp.state() == LspClient::State::Off || m_lsp.state() == LspClient::State::Failed) return;
    if (d->lang != Lang::Cpp) return;
    if (!d->lspOpen) {
        m_lsp.didOpen(d->path, d->buf.text(), lspLanguageId(d->lang));
        d->lspOpen = true;
        d->lspSent = d->buf.version();
    } else if (d->lspSent != d->buf.version()) {
        m_lsp.didChange(d->path, d->buf.text());
        d->lspSent = d->buf.version();
    }
}

void CodeWorkspace::indexProject() {
    m_indexTime = now();
    m_index.clear();
    int n = 0;
    for (auto& rel : projectFiles()) {
        fs::path p = m_root / pathFromUtf8(rel);
        Lang l = langForPath(rel);
        if (l != Lang::Cpp && l != Lang::Shader) continue;
        if (auto t = readFileText(p)) {
            m_index.addFile(narrow(p.wstring()), *t, l);
            if (++n > 400) break;
        }
    }
    // Fledge Script SDK headers: completion for the engine API even without clangd
    std::error_code ec;
    fs::path sdk = m_app.m_paths.sdk;
    if (!sdk.empty() && fs::is_directory(sdk, ec))
        for (auto it = fs::recursive_directory_iterator(sdk, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec) && it->path().extension() == ".h")
                if (auto t = readFileText(it->path())) m_index.addFile(narrow(it->path().wstring()), *t, Lang::Cpp);
}

std::vector<code::Diagnostic> CodeWorkspace::diagnosticsFor(const fs::path& file) {
    std::vector<code::Diagnostic> out;
    fs::path n = file.lexically_normal();
    auto key = [](const fs::path& p) { return toLower(narrow(p.lexically_normal().generic_wstring())); };
    std::string k = key(n);
    for (auto& [f, ds] : m_lsp.diagnostics())
        if (key(f) == k) out.insert(out.end(), ds.begin(), ds.end());
    if (out.empty() && m_app.m_session.hasProject()) {  // last script build (g++) when clangd has nothing
        fs::path scripts = m_app.m_session.project().scriptsDir();
        for (auto& d : m_app.m_session.lastScriptBuild().diagnostics) {
            if (d.line <= 0 || key(scripts / pathFromUtf8(d.file)) != k) continue;
            code::Diagnostic cd;
            cd.start = {d.line - 1, std::max(0, d.column - 1)};
            cd.end = {d.line - 1, std::max(0, d.column - 1) + 1};
            cd.severity = d.severity == fledge::Diagnostic::Severity::Error ? 1 : d.severity == fledge::Diagnostic::Severity::Warning ? 2 : 3;
            cd.message = d.message;
            cd.source = "build";
            out.push_back(cd);
        }
    }
    return out;
}

void CodeWorkspace::update() {
    if (m_app.m_session.hasProject() ? m_app.m_session.project().rootDir() != m_root : !m_root.empty()) onProjectChanged();
    m_lsp.poll();
    for (auto& t : m_terms) t->poll();
    updateDebugger();
    if (m_root.empty()) return;
    if (!m_lspTried && !m_docs.empty()) startLsp();
    for (auto& d : m_docs) {
        d->buf.tabSize = settings.tabSize;
        d->buf.autoClose = settings.autoClose;
        if (d->lang == Lang::Cpp && now() - d->lastEdit > 0.3) lspSync(d.get());
    }
    if (now() - m_indexTime > 30.0 && !m_docs.empty()) indexProject();
    static double lastCheck = 0;
    if (now() - lastCheck > 1.0) {
        lastCheck = now();
        reloadIfChanged();
    }
    // auto save
    if (settings.autoSave == 1)
        for (auto& d : m_docs)
            if (d->buf.dirty() && d->virtualName.empty() && now() - d->lastEdit > 1.0) saveDoc(d.get());
    if (settings.autoSave == 2 && m_wasFocused && !focused()) saveAll();
    m_wasFocused = focused();
    static double lastSession = 0;
    if (now() - lastSession > 10.0) {
        lastSession = now();
        saveSession();
    }
}

// ============================================================================== metrics ===
bool CodeWorkspace::light() const { return settings.theme == 2 || (settings.theme == 0 && m_app.m_prefs.theme == Theme::Light); }

float CodeWorkspace::colX(const std::string& line, int col) const {
    int vx = 0;
    int n = std::min(col, static_cast<int>(line.size()));
    for (int i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(line[static_cast<size_t>(i)]);
        if (c == '\t') vx = (vx / settings.tabSize + 1) * settings.tabSize;
        else if ((c & 0xC0) != 0x80) ++vx;
    }
    return static_cast<float>(vx) * m_charW;
}

int CodeWorkspace::xToCol(const std::string& line, float x) const {
    int vx = 0;
    for (int i = 0; i < static_cast<int>(line.size()); ++i) {
        unsigned char c = static_cast<unsigned char>(line[static_cast<size_t>(i)]);
        if ((c & 0xC0) == 0x80) continue;
        int next = c == '\t' ? (vx / settings.tabSize + 1) * settings.tabSize : vx + 1;
        if (static_cast<float>(vx + next) * 0.5f * m_charW >= x) return i;
        vx = next;
    }
    return static_cast<int>(line.size());
}

ImU32 CodeWorkspace::tokColor(Tok t) const {
    bool l = light();
    switch (t) {
        case Tok::Keyword: return l ? IM_COL32(175, 0, 219, 255) : IM_COL32(197, 134, 192, 255);
        case Tok::Type: return l ? IM_COL32(38, 127, 153, 255) : IM_COL32(78, 201, 176, 255);
        case Tok::Number: return l ? IM_COL32(9, 134, 88, 255) : IM_COL32(181, 206, 168, 255);
        case Tok::String: return l ? IM_COL32(163, 21, 21, 255) : IM_COL32(206, 145, 120, 255);
        case Tok::Comment: return l ? IM_COL32(0, 128, 0, 255) : IM_COL32(106, 153, 85, 255);
        case Tok::Preprocessor: return l ? IM_COL32(0, 0, 255, 255) : IM_COL32(86, 156, 214, 255);
        case Tok::Function: return l ? IM_COL32(121, 94, 38, 255) : IM_COL32(220, 220, 170, 255);
        case Tok::Heading: return l ? IM_COL32(0, 0, 128, 255) : IM_COL32(86, 156, 214, 255);
        case Tok::Emphasis: return l ? IM_COL32(128, 0, 128, 255) : IM_COL32(206, 145, 120, 255);
        case Tok::Code: return l ? IM_COL32(128, 60, 0, 255) : IM_COL32(206, 145, 120, 255);
        case Tok::Key: return l ? IM_COL32(4, 81, 165, 255) : IM_COL32(156, 220, 254, 255);
        case Tok::Punct: return l ? IM_COL32(60, 60, 60, 255) : IM_COL32(212, 212, 212, 255);
        default: return l ? IM_COL32(31, 31, 31, 255) : IM_COL32(212, 212, 212, 255);
    }
}

std::string CodeWorkspace::commentPrefix(Lang l) const {
    switch (l) {
        case Lang::CMake: return "#";
        case Lang::Markdown:
        case Lang::Json:
        case Lang::Plain: return "";
        default: return "//";
    }
}

bool CodeWorkspace::keyPressed(const char* id) const {
    auto it = settings.keys.find(id);
    if (it == settings.keys.end() || !it->second) return false;
    return ImGui::IsKeyChordPressed(it->second);
}

std::vector<CodeWorkspace::Row> CodeWorkspace::layoutRows(Doc* d, int wrapCols) {
    if (d->foldsVersion != d->buf.version()) {
        d->folds = foldingRanges(d->buf, d->lang);
        d->foldsVersion = d->buf.version();
        // drop folds that no longer exist
        for (auto it = d->folded.begin(); it != d->folded.end();) {
            bool ok = std::any_of(d->folds.begin(), d->folds.end(), [&](auto& f) { return f.startLine == *it; });
            it = ok ? std::next(it) : d->folded.erase(it);
        }
    }
    std::vector<Row> rows;
    rows.reserve(static_cast<size_t>(d->buf.lineCount()));
    for (int l = 0; l < d->buf.lineCount(); ++l) {
        const std::string& s = d->buf.line(l);
        if (wrapCols > 8 && colX(s, static_cast<int>(s.size())) > static_cast<float>(wrapCols) * m_charW) {
            int start = 0;
            while (start < static_cast<int>(s.size())) {
                int end = start, vx = 0, lastSpace = -1;
                while (end < static_cast<int>(s.size()) && vx < wrapCols) {
                    if ((static_cast<unsigned char>(s[static_cast<size_t>(end)]) & 0xC0) != 0x80) ++vx;
                    if (s[static_cast<size_t>(end)] == ' ') lastSpace = end;
                    ++end;
                }
                if (end < static_cast<int>(s.size()) && lastSpace > start + wrapCols / 2) end = lastSpace + 1;
                rows.push_back({l, start, end});
                start = end;
            }
        } else {
            rows.push_back({l, 0, static_cast<int>(s.size())});
        }
        if (d->folded.count(l))
            for (auto& f : d->folds)
                if (f.startLine == l) {
                    l = f.endLine;
                    break;
                }
    }
    return rows;
}

// ============================================================================== editing helpers ===
void CodeWorkspace::typeText(Doc* d, const std::string& s) {
    if (d->readOnly) return;
    d->lastEdit = now();
    if (s.size() == 1) d->buf.typeChar(s[0]);
    else d->buf.insert(s);
    d->reveal = true;
    char c = s.back();
    if (c == '(' || c == ',') {
        if (m_lsp.state() == LspClient::State::Ready && d->lang == Lang::Cpp) {
            lspSync(d);
            m_lsp.signatureHelp(d->path, d->buf.cursors[0].pos, [this](std::optional<SignatureInfo> si) { m_sig = si; });
        }
    } else if (c == ')' || c == ';') {
        m_sig.reset();
    }
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') triggerCompletion(d, false);
    else if (c == '.' || c == '>' || c == ':') {
        std::string l = d->buf.line(d->buf.cursors[0].pos.line);
        int col = d->buf.cursors[0].pos.col;
        bool member = c == '.' || (col >= 2 && ((c == '>' && l[static_cast<size_t>(col - 2)] == '-') || (c == ':' && l[static_cast<size_t>(col - 2)] == ':')));
        if (member && m_lsp.state() == LspClient::State::Ready) triggerCompletion(d, true);
        else m_comp.open = false;
    } else {
        m_comp.open = false;
    }
}

void CodeWorkspace::triggerCompletion(Doc* d, bool explicitly) {
    Pos s, e;
    Pos cur = d->buf.cursors[0].pos;
    std::string word = wordAt(d->buf, {cur.line, cur.col}, &s, &e);
    word = word.substr(0, static_cast<size_t>(std::max(0, cur.col - s.col)));
    if (!explicitly && word.empty()) {
        m_comp.open = false;
        return;
    }
    m_comp.start = {cur.line, s.col};
    if (m_lsp.state() == LspClient::State::Ready && d->lang == Lang::Cpp) {
        if (m_comp.open && m_comp.fromLsp && !explicitly) return;  // filter the open list client-side
        lspSync(d);
        m_comp.fromLsp = true;
        m_comp.requested = now();
        m_lsp.completion(d->path, cur, [this](std::vector<CompletionItem> items) {
            m_comp.items = std::move(items);
            m_comp.open = !m_comp.items.empty();
            m_comp.sel = 0;
        });
        return;
    }
    m_comp.fromLsp = false;
    m_comp.items.clear();
    for (auto* e2 : m_index.complete(word, 40)) m_comp.items.push_back({e2->name, e2->kind + "  " + narrow(fs::path(widen(e2->file)).filename().wstring()), e2->name, 0});
    m_comp.open = !m_comp.items.empty();
    m_comp.sel = 0;
}

void CodeWorkspace::acceptCompletion(Doc* d) {
    Pos cur = d->buf.cursors[0].pos;
    std::string prefix = d->buf.textIn(m_comp.start, cur);
    std::vector<const CompletionItem*> vis;
    for (auto& it : m_comp.items)
        if (prefix.empty() || fuzzyScore(prefix, it.label) >= 0) vis.push_back(&it);
    if (vis.empty()) {
        m_comp.open = false;
        return;
    }
    const CompletionItem& c = *vis[static_cast<size_t>(std::clamp(m_comp.sel, 0, static_cast<int>(vis.size()) - 1))];
    std::string ins = c.insertText.empty() ? c.label : c.insertText;
    Pos s, e;
    wordAt(d->buf, cur, &s, &e);
    d->buf.replaceRange({cur.line, m_comp.start.col}, {cur.line, std::max(cur.col, e.col)}, ins);
    d->lastEdit = now();
    m_comp.open = false;
}

void CodeWorkspace::goToDefinition(Doc* d) {
    Pos cur = d->buf.cursors[0].pos;
    std::string w = wordAt(d->buf, cur);
    if (m_lsp.state() == LspClient::State::Ready && d->lang == Lang::Cpp) {
        lspSync(d);
        m_lsp.definition(d->path, cur, [this, w](std::vector<Location> locs) {
            if (locs.empty()) {
                m_app.toast(ToastLevel::Info, "No definition found for '" + w + "'");
                return;
            }
            jumpTo(locs[0].file, locs[0].start);
        });
        return;
    }
    if (auto* e = m_index.definition(w)) jumpTo(pathFromUtf8(e->file), {e->line, 0});
    else m_app.toast(ToastLevel::Info, "No definition found for '" + w + "' (basic index)");
}

void CodeWorkspace::findReferences(Doc* d) {
    Pos cur = d->buf.cursors[0].pos;
    std::string w = wordAt(d->buf, cur);
    if (w.empty()) return;
    m_refsTitle = "References: " + w;
    bottomTab = "references";
    showBottom = true;
    if (m_lsp.state() == LspClient::State::Ready && d->lang == Lang::Cpp) {
        lspSync(d);
        m_lsp.references(d->path, cur, [this](std::vector<Location> locs) { m_refs = std::move(locs); });
        return;
    }
    m_refsTitle += "  (basic index: whole-word text search)";
    m_refs.clear();
    SearchOptions o{false, true, true};
    for (auto& rel : projectFiles()) {
        fs::path p = m_root / pathFromUtf8(rel);
        Doc* open = findDoc(p);
        TextBuffer tmp;
        if (!open) {
            auto t = readFileText(p);
            if (!t) continue;
            tmp.setText(*t);
        }
        if (auto m = findAll(open ? open->buf : tmp, w, o))
            for (auto& x : *m) m_refs.push_back({p, x.start, x.end});
    }
}

void CodeWorkspace::startRename(Doc* d) {
    m_renameText = wordAt(d->buf, d->buf.cursors[0].pos);
    if (m_renameText.empty()) return;
    m_renameOpen = true;
}

void CodeWorkspace::formatDoc(Doc* d, std::function<void()> then) {
    if (m_lsp.state() != LspClient::State::Ready || d->lang != Lang::Cpp) {
        fs::path cf = findOnPath("clang-format.exe");
        if (!cf.empty()) {
            auto r = runCapture(quoteArg(narrow(cf.wstring())) + " --assume-filename=" + quoteArg(narrow(d->path.wstring())), d->path.parent_path(), 15000,
                                d->buf.text());
            if (r.exitCode == 0 && !r.output.empty()) {
                Pos keep = d->buf.cursors[0].pos;
                d->buf.replaceRange({0, 0}, {d->buf.lineCount() - 1, static_cast<int>(d->buf.line(d->buf.lineCount() - 1).size())}, r.output);
                d->buf.setCursor(d->buf.clamp(keep));
            }
        } else {
            m_app.toast(ToastLevel::Warning, "Formatting needs clangd (C++) or clang-format on PATH");
        }
        if (then) then();
        return;
    }
    lspSync(d);
    fs::path path = d->path;
    m_lsp.formatting(path, settings.tabSize, [this, path, then](std::vector<TextEdit> edits) {
        if (Doc* dd = findDoc(path)) {
            applyEdits(dd->buf, edits);
            dd->lastEdit = now();
        }
        if (then) then();
    });
}

// ============================================================================== drawing ===
void CodeWorkspace::draw() {
    float s = m_app.effectiveScale();
    m_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    m_drawnFrame = ImGui::GetFrameCount();
    drawUnsavedPrompt();
    if (m_root.empty()) {
        ImGui::TextDisabled("Open a project to use the Code workspace.");
        return;
    }
    // ---- top bar: run / build / debug
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6 * s, 4 * s});
    bool playing = m_app.m_session.playing();
    if (ImGui::Button(playing ? ICON_STOP " Stop" : ICON_PLAY " Run")) {
        saveAll();
        m_app.m_commands.execute(playing ? "play.stop" : "play.play");
    }
    m_app.trackRect("code.run");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play the game in the editor (F5). Scripts are saved and hot reloaded.");
    ImGui::SameLine();
    ImGui::BeginDisabled(m_app.m_scriptBuildRunning);
    if (ImGui::Button(ICON_BOLT " Build")) {
        saveAll();
        m_app.startScriptBuild(false, true);
        bottomTab = "build";
        showBottom = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!debugging()) {
        ImGui::BeginDisabled(!m_app.m_session.hasProject() || !m_app.m_session.project().hasScripts());
        if (ImGui::Button(ICON_BUG " Debug")) startDebugging();
        ImGui::EndDisabled();
        m_app.trackRect("code.debug");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Debug the game in a separate process under gdb (Ctrl+F5): scripts are built with -O0 -g, breakpoints (F9 / gutter "
                              "click) pause the game, the Breakpoints panel shows the call stack and locals.");
        ImGui::SameLine();
        if (ImGui::Button(ICON_PLAY " Separate process")) playExternal();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play the current scene (saved or not) in fledge_runtime, outside the editor.");
    } else {
        if (ImGui::Button(ICON_STOP " Stop debugging")) stopDebugging();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Kill the game and gdb (Shift+F5)");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_dbgStatus.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_COLUMNS)) {
        m_split = !m_split;
        if (m_split && active() && m_groups[1].tabs.empty()) {
            m_groups[1].tabs.push_back(active());
            m_groups[1].active = active();
        }
        if (!m_split) {
            for (Doc* d : m_groups[1].tabs)
                if (std::find(m_groups[0].tabs.begin(), m_groups[0].tabs.end(), d) == m_groups[0].tabs.end()) m_groups[0].tabs.push_back(d);
            m_groups[1] = {};
            m_activeGroup = 0;
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Split editor (Ctrl+\\)");
    ImGui::SameLine();
    if (ImGui::Button(ICON_TERMINAL)) showBottom = !showBottom;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle panel (Ctrl+`)");
    ImGui::SameLine();
    if (ImGui::Button(ICON_EXTERNAL)) {
        Doc* d = active();
        std::string cmd = m_app.m_externalEditor;
        fs::path f = d ? d->path : m_root;
#ifdef _WIN32
        if (cmd.empty()) {
            ShellExecuteW(nullptr, L"open", f.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else {
            auto rep = [&](const std::string& k, const std::string& v) {
                for (size_t p = cmd.find(k); p != std::string::npos; p = cmd.find(k, p + v.size())) cmd.replace(p, k.size(), v);
            };
            rep("{file}", narrow(f.wstring()));
            rep("{line}", std::to_string(d ? d->buf.cursors[0].pos.line + 1 : 1));
            rep("{scripts}", narrow(m_app.m_session.project().scriptsDir().wstring()));
            rep("{project}", narrow(m_root.wstring()));
            std::wstring full = L"cmd.exe /d /c " + widen(cmd);
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, full.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
#endif
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open in the external editor (Settings > Scripts > External editor command)");
    ImGui::SameLine();
    ImGui::TextDisabled("%s  %s", ICON_LIGHTBULB, lspStatus().c_str());
    ImGui::PopStyleVar();

    // ---- main area
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float statusH = ImGui::GetFrameHeight();
    float actW = 44 * s;
    float bottomH = showBottom ? std::clamp(m_bottomH, 80 * s, avail.y * 0.7f) : 0.0f;
    float sp = ImGui::GetStyle().ItemSpacing.y;
    float mainH = avail.y - statusH - bottomH - (showBottom ? 4 * s + 2 * sp : 0) - sp;
    ImGui::BeginChild("##codemain", {avail.x, mainH}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    drawActivityBar(actW);
    ImGui::SameLine(0, 0);
    ImGui::BeginChild("##sidebar", {m_sidebarW * s, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    m_sidebarW = ImGui::GetWindowWidth() / s;
    drawSidebar();
    ImGui::EndChild();
    ImGui::SameLine(0, 2 * s);
    ImVec2 ed = ImGui::GetContentRegionAvail();
    if (m_split) {
        float w = (ed.x - 4 * s) * 0.5f;
        drawGroup(0, {w, ed.y});
        ImGui::SameLine(0, 4 * s);
        drawGroup(1, {w, ed.y});
    } else {
        drawGroup(0, ed);
    }
    ImGui::EndChild();
    if (showBottom) {
        // splitter
        ImGui::InvisibleButton("##hsplit", {avail.x, 4 * s});
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive()) m_bottomH = std::max(80 * s, m_bottomH - ImGui::GetIO().MouseDelta.y);
        ImGui::BeginChild("##bottom", {avail.x, bottomH}, ImGuiChildFlags_Borders);
        drawBottom();
        ImGui::EndChild();
    }
    drawStatus();
    drawPopups();
    if (m_editorFocused && m_focused) ImGui::GetCurrentContext()->WantTextInputNextFrame = 1;  // global single-key shortcuts stay quiet
}

void CodeWorkspace::drawActivityBar(float w) {
    float s = m_app.effectiveScale();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
    ImGui::BeginChild("##activity", {w, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    struct A {
        const char* id;
        const char* icon;
        const char* tip;
    };
    std::vector<A> acts = {{"explorer", ICON_FILES, "Explorer (Ctrl+Shift+E)"}, {"search", ICON_SEARCH, "Search (Ctrl+Shift+F)"}};
    if (gitAvailable()) acts.push_back({"scm", ICON_BRANCH, "Source Control"});
    acts.push_back({"problems", ICON_WARNING, "Problems"});
    acts.push_back({"outline", ICON_SITEMAP, "Outline"});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4 * s);
    for (auto& a : acts) {
        bool on = activity == a.id;
        ImGui::SetCursorPosX(4 * s);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, on ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Button(a.icon, {w - 8 * s, w - 8 * s})) activity = a.id;
        ImGui::PopStyleColor(2);
        m_app.trackRect(std::string("code.activity.") + a.id);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a.tip);
        if (on) {
            ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled({mn.x - 4 * s, mn.y}, {mn.x - 2 * s, mx.y}, ImGui::GetColorU32(ImGuiCol_CheckMark));
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void CodeWorkspace::drawSidebar() {
    if (activity == "scm" && !gitAvailable()) activity = "explorer";
    if (activity == "explorer") drawExplorer();
    else if (activity == "search") drawSearch();
    else if (activity == "scm") drawScm();
    else if (activity == "problems") drawProblems(true);
    else drawOutline();
}

// ---------------------------------------------------------------------------- explorer ---
void CodeWorkspace::drawExplorer() {
    ImGui::TextDisabled("EXPLORER");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight() * 2.2f);
    static std::string newName;
    static bool newOpen = false;
    if (ImGui::SmallButton(ICON_PLUS)) {
        newOpen = true;
        newName = "Scripts/NewFile.cpp";
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("New file");
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_ROTATE)) m_fileCacheTime = -100;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Refresh");
    if (newOpen) {
        ImGui::SetNextItemWidth(-1);
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##newfile", &newName, ImGuiInputTextFlags_EnterReturnsTrue)) {
            fs::path p = (m_root / pathFromUtf8(newName)).lexically_normal();
            std::string rel = relOf(m_root, p);
            std::error_code ec;
            if (newName.empty() || startsWith(rel, "..") || fs::path(widen(newName)).is_absolute()) {
                m_app.toast(ToastLevel::Error, "New files must be inside the project folder");
            } else if (fs::exists(p, ec)) {
                openFile(p);
            } else {
                fs::create_directories(p.parent_path(), ec);
                if (writeFileText(p, "")) {
                    m_fileCacheTime = -100;
                    openFile(p);
                }
            }
            newOpen = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) newOpen = false;
    }
    ImGui::Separator();
    std::string proj = narrow(m_root.filename().wstring());
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNodeEx(("##root" + proj).c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen, "%s %s", ICON_FOLDER_OPEN,
                          proj.c_str())) {
        drawExplorerDir(m_root, 0);
        ImGui::TreePop();
    }
}

void CodeWorkspace::drawExplorerDir(const fs::path& dir, int depth) {
    if (depth > 12) return;
    std::vector<fs::path> dirs, files;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec); it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::string name = narrow(it->path().filename().wstring());
        if (it->is_directory(ec)) {
            if (!skipDir(name)) dirs.push_back(it->path());
        } else if (it->is_regular_file(ec)) {
            files.push_back(it->path());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    std::sort(files.begin(), files.end());
    for (auto& d : dirs) {
        std::string rel = relOf(m_root, d);
        bool open = m_expanded.count(rel) > 0;
        ImGui::SetNextItemOpen(open);
        bool nowOpen = ImGui::TreeNodeEx(rel.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth, "%s %s", open ? ICON_FOLDER_OPEN : ICON_FOLDER,
                                         narrow(d.filename().wstring()).c_str());
        m_app.trackRect("code.explorer." + rel);
        if (nowOpen != open) {
            if (nowOpen) m_expanded.insert(rel);
            else m_expanded.erase(rel);
        }
        if (nowOpen) {
            drawExplorerDir(d, depth + 1);
            ImGui::TreePop();
        }
    }
    Doc* act = active();
    for (auto& f : files) {
        std::string rel = relOf(m_root, f);
        Lang l = langForPath(narrow(f.filename().wstring()));
        bool sel = act && act->path == f.lexically_normal();
        ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (sel) fl |= ImGuiTreeNodeFlags_Selected;
        ImGui::TreeNodeEx(rel.c_str(), fl, "%s %s", fileIcon(l), narrow(f.filename().wstring()).c_str());
        m_app.trackRect("code.explorer." + rel);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            bool text = isTextFile(f);
            if (text) openFile(f, 0, 0, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
            else m_app.toast(ToastLevel::Info, rel + " is not a text file");
        }
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Open")) openFile(f);
            if (ImGui::MenuItem("Open to the Side")) {
                m_split = true;
                m_activeGroup = 1;
                openFile(f);
            }
            if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(narrow(f.wstring()).c_str());
            if (ImGui::MenuItem("Copy Relative Path")) ImGui::SetClipboardText(rel.c_str());
#ifdef _WIN32
            if (ImGui::MenuItem("Reveal in File Explorer")) {
                std::wstring args = L"/select,\"" + f.wstring() + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            }
#endif
            ImGui::EndPopup();
        }
    }
}

// ---------------------------------------------------------------------------- search ---
void CodeWorkspace::drawSearch() {
    float s = m_app.effectiveScale();
    ImGui::TextDisabled("SEARCH");
    bool run = false;
    ImGui::SetNextItemWidth(-90 * s);
    run |= ImGui::InputTextWithHint("##sq", "Search", &m_searchQuery, ImGuiInputTextFlags_EnterReturnsTrue);
    m_app.trackRect("code.search.input");
    auto toggle = [&](const char* label, bool& v, const char* tip) {
        ImGui::SameLine(0, 2 * s);
        ImGui::PushStyleColor(ImGuiCol_Button, v ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        if (ImGui::SmallButton(label)) {
            v = !v;
            run = true;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    toggle("Aa", m_searchOpt.matchCase, "Match case");
    toggle("ab", m_searchOpt.wholeWord, "Match whole word");
    toggle(".*", m_searchOpt.regex, "Use regular expression");
    ImGui::SetNextItemWidth(-90 * s);
    ImGui::InputTextWithHint("##sr", "Replace", &m_replaceQuery);
    ImGui::SameLine(0, 2 * s);
    bool replaceAllNow = ImGui::SmallButton("Replace All");
    if (run && !m_searchQuery.empty()) {
        m_hits.clear();
        m_searchError.clear();
        for (auto& rel : projectFiles()) {
            fs::path p = m_root / pathFromUtf8(rel);
            Doc* open = findDoc(p);
            TextBuffer tmp;
            if (!open) {
                auto t = readFileText(p);
                if (!t || t->size() > 2 * 1024 * 1024) continue;
                tmp.setText(*t);
            }
            const TextBuffer& b = open ? open->buf : tmp;
            auto m = findAll(b, m_searchQuery, m_searchOpt);
            if (!m) {
                m_searchError = m.error();
                break;
            }
            for (auto& x : *m) m_hits.push_back({rel, x.start.line, x.start.col, x.end.col - x.start.col, b.line(x.start.line)});
            if (m_hits.size() > 5000) break;
        }
    }
    if (replaceAllNow && !m_searchQuery.empty()) {
        int total = 0, files = 0;
        std::set<std::string> rels;
        for (auto& h : m_hits) rels.insert(h.rel);
        for (auto& rel : rels) {
            fs::path p = m_root / pathFromUtf8(rel);
            Doc* d = findDoc(p);
            if (!d) d = openDoc(p, true);
            if (!d) continue;
            auto n = replaceAll(d->buf, m_searchQuery, m_replaceQuery, m_searchOpt);
            if (n && *n > 0) {
                total += *n;
                ++files;
                d->lastEdit = now();
            }
        }
        m_app.toast(ToastLevel::Success, std::format("Replaced {} occurrence{} in {} file{} (unsaved - review, then Save All)", total, total == 1 ? "" : "s",
                                                     files, files == 1 ? "" : "s"));
        m_hits.clear();
    }
    if (!m_searchError.empty()) ImGui::TextColored({1, 0.45f, 0.45f, 1}, "%s", m_searchError.c_str());
    else if (!m_searchQuery.empty()) ImGui::TextDisabled("%zu results", m_hits.size());
    ImGui::BeginChild("##hits");
    std::string cur;
    bool open = true;
    for (size_t i = 0; i < m_hits.size(); ++i) {
        auto& h = m_hits[i];
        if (h.rel != cur) {
            cur = h.rel;
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            open = ImGui::CollapsingHeader((h.rel + "##h" + std::to_string(i)).c_str());
        }
        if (!open) continue;
        ImGui::PushID(static_cast<int>(i));
        std::string t = h.text;
        size_t st = t.find_first_not_of(" \t");
        t = st == std::string::npos ? t : t.substr(st);
        if (t.size() > 120) t = t.substr(0, 120);
        if (ImGui::Selectable(std::format("{:>5}  {}", h.line + 1, t).c_str())) jumpTo(m_root / pathFromUtf8(h.rel), {h.line, h.col});
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------- source control ---
void CodeWorkspace::drawScm() {
    float s = m_app.effectiveScale();
    ImGui::TextDisabled("SOURCE CONTROL");
    if (now() - m_gitTime > 3.0) {
        m_gitTime = now();
        m_git = gitStatus(m_root);
    }
    if (!m_git.isRepo) {
        ImGui::TextWrapped("This project folder is not a git repository.");
        if (ImGui::Button(ICON_BRANCH " Initialize Repository")) {
            auto r = gitRun(m_root, "init");
            m_output += "[git] " + r.output;
            m_gitTime = -100;
        }
        return;
    }
    ImGui::Text("%s %s", ICON_BRANCH, m_git.branch.c_str());
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextMultiline("##msg", &m_commitMsg, {-1, ImGui::GetTextLineHeight() * 3});
    bool anyStaged = std::any_of(m_git.files.begin(), m_git.files.end(), [](auto& f) { return f.index != ' ' && f.index != '?'; });
    ImGui::BeginDisabled(m_commitMsg.empty() || m_git.files.empty());
    if (ImGui::Button(anyStaged ? ICON_CHECK " Commit" : ICON_CHECK " Commit All", {-1, 0})) {
        saveAll();
        if (!anyStaged) gitRun(m_root, "add -A");
        fs::path msgFile = m_root / "Intermediate" / "Editor" / "COMMIT_MSG.txt";
        std::error_code ec;
        fs::create_directories(msgFile.parent_path(), ec);
        writeFileText(msgFile, m_commitMsg);
        auto r = gitRun(m_root, "commit -F " + quoteArg(narrow(msgFile.wstring())));
        m_output += "[git] " + r.output;
        if (r.exitCode == 0) {
            m_commitMsg.clear();
            m_app.toast(ToastLevel::Success, "Committed");
        } else {
            m_app.toast(ToastLevel::Error, "git commit failed (see Output)");
            bottomTab = "output";
            showBottom = true;
        }
        m_gitTime = -100;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !anyStaged) ImGui::SetTooltip("Nothing staged: stages every change, then commits");
    ImGui::Separator();
    ImGui::TextDisabled("CHANGES (%zu)", m_git.files.size());
    ImGui::BeginChild("##changes");
    for (size_t i = 0; i < m_git.files.size(); ++i) {
        auto& f = m_git.files[i];
        ImGui::PushID(static_cast<int>(i));
        char st = f.work != ' ' ? f.work : f.index;
        ImVec4 col = st == 'M' ? ImVec4(0.9f, 0.75f, 0.3f, 1) : st == '?' || st == 'A' ? ImVec4(0.4f, 0.85f, 0.5f, 1) : st == 'D' ? ImVec4(1, 0.45f, 0.45f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1);
        bool staged = f.index != ' ' && f.index != '?';
        if (ImGui::SmallButton(staged ? "-" : "+")) {
            gitRun(m_root, (staged ? "reset -q HEAD -- " : "add -- ") + quoteArg(f.path));
            m_gitTime = -100;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(staged ? "Unstage" : "Stage");
        ImGui::SameLine();
        if (ImGui::Selectable(f.path.c_str(), false, ImGuiSelectableFlags_AllowOverlap, {ImGui::GetContentRegionAvail().x - 24 * s, 0})) {
            std::string diff = f.index == '?' ? "(untracked file)\n" : gitDiff(m_root, f.path);
            if (diff.empty()) diff = gitRun(m_root, "diff --cached -- " + quoteArg(f.path)).output;
            auto doc = std::make_unique<Doc>();
            doc->virtualName = "Diff: " + f.path;
            doc->rel = doc->virtualName;
            doc->path = m_root / pathFromUtf8(f.path);
            doc->readOnly = true;
            doc->buf.setText(diff);
            Doc* d = doc.get();
            m_docs.push_back(std::move(doc));
            m_groups[m_activeGroup].tabs.push_back(d);
            m_groups[m_activeGroup].active = d;
        }
        ImGui::SameLine();
        ImGui::TextColored(col, "%c", st);
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------- problems / outline ---
void CodeWorkspace::drawProblems(bool compact) {
    if (compact) ImGui::TextDisabled("PROBLEMS");
    struct P {
        fs::path file;
        code::Diagnostic d;
    };
    std::vector<P> all;
    for (auto& [f, ds] : m_lsp.diagnostics())
        for (auto& d : ds) all.push_back({f, d});
    if (m_app.m_session.hasProject()) {
        fs::path scripts = m_app.m_session.project().scriptsDir();
        for (auto& d : m_app.m_session.lastScriptBuild().diagnostics) {
            if (d.severity == fledge::Diagnostic::Severity::Note) continue;
            code::Diagnostic cd;
            cd.start = {std::max(0, d.line - 1), std::max(0, d.column - 1)};
            cd.severity = d.severity == fledge::Diagnostic::Severity::Error ? 1 : 2;
            cd.message = d.message;
            cd.source = "build (g++)";
            all.push_back({scripts / pathFromUtf8(d.file), cd});
        }
    }
    if (all.empty()) ImGui::TextDisabled("No problems have been detected.");
    ImGui::BeginChild("##problems");
    for (size_t i = 0; i < all.size(); ++i) {
        auto& p = all[i];
        ImGui::PushID(static_cast<int>(i));
        const char* icon = p.d.severity == 1 ? ICON_ERROR : p.d.severity == 2 ? ICON_WARNING : ICON_INFO;
        ImVec4 col = p.d.severity == 1 ? ImVec4(1, 0.45f, 0.45f, 1) : p.d.severity == 2 ? ImVec4(1, 0.78f, 0.3f, 1) : ImVec4(0.5f, 0.75f, 1, 1);
        ImGui::TextColored(col, "%s", icon);
        ImGui::SameLine();
        std::string label = std::format("{}  {}:{}  [{}]", p.d.message, relOf(m_root, p.file), p.d.start.line + 1, p.d.source.empty() ? "clangd" : p.d.source);
        if (ImGui::Selectable(label.c_str())) jumpTo(p.file, p.d.start);
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void CodeWorkspace::drawOutline() {
    ImGui::TextDisabled("OUTLINE");
    Doc* d = active();
    if (!d) {
        ImGui::TextDisabled("No editor is active.");
        return;
    }
    if (d->outlineVersion != d->buf.version()) {
        d->symbols = outline(d->buf, d->lang);
        d->outlineVersion = d->buf.version();
    }
    ImGui::TextDisabled("%s", d->rel.c_str());
    if (d->symbols.empty()) ImGui::TextDisabled("No symbols found in this document.");
    ImGui::BeginChild("##outline");
    for (size_t i = 0; i < d->symbols.size(); ++i) {
        auto& sy = d->symbols[i];
        ImGui::PushID(static_cast<int>(i));
        const char* icon = sy.kind == "class" || sy.kind == "struct" ? ICON_CUBE : sy.kind == "namespace" ? ICON_LAYERS : sy.kind == "heading" ? ICON_BOOK : ICON_CODE;
        bool indent = sy.kind == "method";
        if (indent) ImGui::Indent();
        if (ImGui::Selectable(std::format("{} {}  ", icon, sy.name).c_str())) jumpTo(d->path, {sy.line, 0});
        ImGui::SameLine();
        ImGui::TextDisabled("%s", sy.kind.c_str());
        if (indent) ImGui::Unindent();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------- editor groups ---
void CodeWorkspace::drawGroup(int g, ImVec2 size) {
    ImGui::PushID(g);
    ImGui::BeginChild("##group", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawTabs(g);
    Doc* d = m_groups[g].active;
    if (!d) {
        ImVec2 a = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos({ImGui::GetCursorPosX() + a.x * 0.5f - 150, ImGui::GetCursorPosY() + a.y * 0.35f});
        ImGui::BeginGroup();
        ImGui::TextDisabled("Open a file from the Explorer");
        ImGui::TextDisabled("Ctrl+P  Go to file / anything");
        ImGui::TextDisabled("Ctrl+Shift+F  Search in files");
        ImGui::TextDisabled("Ctrl+`  Terminal");
        ImGui::EndGroup();
    } else {
        drawBreadcrumbs(d);
        drawEditor(d, g, ImGui::GetContentRegionAvail());
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void CodeWorkspace::drawTabs(int g) {
    Group& grp = m_groups[g];
    if (!ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_AutoSelectNewTabs)) return;
    Doc* closeMe = nullptr;
    // pinned tabs first
    std::stable_sort(grp.tabs.begin(), grp.tabs.end(), [](Doc* a, Doc* b) { return a->pinned && !b->pinned; });
    for (Doc* d : grp.tabs) {
        std::string name = d->virtualName.empty() ? narrow(d->path.filename().wstring()) : d->virtualName;
        std::string label = std::string(d->pinned ? ICON_THUMBTACK " " : "") + fileIcon(d->lang) + " " + name;
        if (d->preview) label += " (preview)";
        label += "###tab" + std::to_string(reinterpret_cast<uintptr_t>(d));
        bool open = true;
        ImGuiTabItemFlags fl = d->buf.dirty() ? ImGuiTabItemFlags_UnsavedDocument : 0;
        static Doc* lastSelected[2] = {nullptr, nullptr};
        if (grp.active == d && lastSelected[g] != d) fl |= ImGuiTabItemFlags_SetSelected;
        if (ImGui::BeginTabItem(label.c_str(), d->pinned ? nullptr : &open, fl)) {
            if (grp.active != d && lastSelected[g] == grp.active) grp.active = d;
            lastSelected[g] = grp.active == d ? d : lastSelected[g];
            if (grp.active == d) lastSelected[g] = d;
            ImGui::EndTabItem();
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) activate(d, g);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) d->preview = false;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Middle) && !d->pinned) closeMe = d;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d->virtualName.empty() ? narrow(d->path.wstring()).c_str() : d->virtualName.c_str());
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem(d->pinned ? "Unpin" : "Pin")) d->pinned = !d->pinned;
            if (ImGui::MenuItem("Close", nullptr, false, !d->pinned)) closeMe = d;
            if (ImGui::MenuItem("Close Others")) {
                std::vector<Doc*> others;
                for (Doc* o : grp.tabs)
                    if (o != d && !o->pinned && !o->buf.dirty()) others.push_back(o);
                for (Doc* o : others) closeDoc(o, g);
                ImGui::EndPopup();
                break;
            }
            if (ImGui::MenuItem("Save", nullptr, false, d->buf.dirty())) saveDoc(d);
            if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(narrow(d->path.wstring()).c_str());
            ImGui::EndPopup();
        }
        if (!open) closeMe = d;
    }
    ImGui::EndTabBar();
    if (closeMe) requestClose(closeMe, g);
}

void CodeWorkspace::drawBreadcrumbs(Doc* d) {
    std::string crumbs;
    std::string rel = d->rel;
    for (size_t p = 0, q; p < rel.size(); p = q + 1) {
        q = rel.find('/', p);
        if (q == std::string::npos) q = rel.size();
        if (!crumbs.empty()) crumbs += "  " ICON_CHEVRON_RIGHT "  ";
        crumbs += rel.substr(p, q - p);
    }
    if (d->outlineVersion != d->buf.version()) {
        d->symbols = outline(d->buf, d->lang);
        d->outlineVersion = d->buf.version();
    }
    int line = d->buf.cursors[0].pos.line;
    const Symbol* best = nullptr;
    for (auto& sy : d->symbols)
        if (sy.line <= line) best = &sy;
    if (best) crumbs += "  " ICON_CHEVRON_RIGHT "  " + best->name;
    ImGui::TextDisabled("%s", crumbs.c_str());
}

void CodeWorkspace::drawEditor(Doc* d, int g, ImVec2 size) {
    ImGuiIO& io = ImGui::GetIO();
    float s = m_app.effectiveScale();
    bool lt = light();
    ImFont* f = font ? font : ImGui::GetFont();
    m_fontPx = std::round(settings.fontSize * s);
    m_charW = f->CalcTextSizeA(m_fontPx, FLT_MAX, 0, "M").x;
    m_lineH = std::round(m_fontPx * 1.4f);
    float lineH = m_lineH;
    ImU32 bg = lt ? IM_COL32(255, 255, 255, 255) : IM_COL32(31, 31, 31, 255);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::BeginChild("##ed", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImVec2 sz = ImGui::GetContentRegionAvail();
    float mmW = settings.minimap ? 90 * s : 0;
    int digits = std::max(3, static_cast<int>(std::to_string(d->buf.lineCount()).size()));
    float gutterW = (settings.lineNumbers ? m_charW * static_cast<float>(digits) + 10 * s : 4 * s) + 28 * s;  // breakpoints + numbers + fold chevrons
    float textX0 = o.x + gutterW;
    float textW = sz.x - gutterW - mmW - 10 * s;
    int wrapCols = settings.wordWrap ? std::max(10, static_cast<int>(textW / m_charW)) : 0;
    auto rows = layoutRows(d, wrapCols);
    int nRows = static_cast<int>(rows.size());
    float contentH = static_cast<float>(nRows) * lineH + sz.y * 0.6f;
    float maxScroll = std::max(0.0f, contentH - sz.y);
    // invisible button = the editor's input item
    ImGui::InvisibleButton("##text", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    ImGuiID id = ImGui::GetItemID();
    bool hovered = ImGui::IsItemHovered();
    m_app.trackRect(g == 0 ? "code.editor" : "code.editor2");
    if (ImGui::IsItemActivated()) {
        activate(d, g);
        m_editorFocused = true;
    }
    bool focusedHere = ImGui::IsWindowFocused() && m_activeGroup == g && m_editorFocused;
    if (ImGui::IsWindowFocused() && m_activeGroup == g) m_editorFocused = m_editorFocused || ImGui::IsItemActive();
    if (m_editorFocused && m_activeGroup == g && !ImGui::IsWindowFocused() && ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))
        m_editorFocused = false;
    // scroll
    if (hovered && io.MouseWheel != 0) {
        if (io.KeyCtrl) {
            settings.fontSize = std::clamp(settings.fontSize + io.MouseWheel, 8.0f, 32.0f);
            m_app.m_fontsDirty = true;
        } else {
            d->scrollY -= io.MouseWheel * lineH * 3;
        }
    }
    if (hovered && io.MouseWheelH != 0) d->scrollX -= io.MouseWheelH * m_charW * 6;
    if (hovered && io.KeyShift && io.MouseWheel != 0 && !io.KeyCtrl) {
        d->scrollX -= io.MouseWheel * m_charW * 6;
        d->scrollY += io.MouseWheel * lineH * 3;
    }
    // caret visibility
    auto rowOf = [&](Pos p) {
        int lo = 0, hi = nRows - 1, best = 0;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            const Row& r = rows[static_cast<size_t>(mid)];
            if (r.line < p.line || (r.line == p.line && r.start <= p.col)) {
                best = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        return best;
    };
    if (d->reveal) {
        d->reveal = false;
        Pos p = d->buf.cursors[0].pos;
        float y = static_cast<float>(rowOf(p)) * lineH;
        if (y < d->scrollY) d->scrollY = std::max(0.0f, y - lineH * 2);
        else if (y + lineH * 2 > d->scrollY + sz.y) d->scrollY = y + lineH * 3 - sz.y;
        if (!settings.wordWrap) {
            float x = colX(d->buf.line(p.line), p.col);
            if (x < d->scrollX) d->scrollX = std::max(0.0f, x - m_charW * 4);
            else if (x > d->scrollX + textW - m_charW * 2) d->scrollX = x - textW + m_charW * 8;
        }
    }
    d->scrollY = std::clamp(d->scrollY, 0.0f, maxScroll);
    d->scrollX = settings.wordWrap ? 0.0f : std::max(0.0f, d->scrollX);

    auto posAt = [&](ImVec2 m) -> Pos {
        int r = std::clamp(static_cast<int>((m.y - o.y + d->scrollY) / lineH), 0, std::max(0, nRows - 1));
        if (rows.empty()) return {};
        const Row& row = rows[static_cast<size_t>(r)];
        const std::string& line = d->buf.line(row.line);
        float base = colX(line, row.start);
        int col = xToCol(line, m.x - textX0 + d->scrollX + base);
        return {row.line, std::clamp(col, row.start, row.end)};
    };

    // ---- mouse
    ImVec2 mp = io.MousePos;
    bool inText = mp.x >= textX0 && mp.x < o.x + sz.x - mmW && mp.y >= o.y && mp.y < o.y + sz.y;
    bool inGutter = mp.x >= o.x && mp.x < textX0 && mp.y >= o.y && mp.y < o.y + sz.y;
    bool inMinimap = settings.minimap && mp.x >= o.x + sz.x - mmW && mp.x < o.x + sz.x && mp.y >= o.y && mp.y < o.y + sz.y;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && inText) {
        Pos p = posAt(mp);
        if (io.KeyAlt) d->buf.addCursor(p);
        else if (io.KeyCtrl) {
            d->buf.setCursor(p);
            goToDefinition(d);
        } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            Pos ws, we;
            wordAt(d->buf, p, &ws, &we);
            d->buf.setCursor(ws);
            d->buf.setCursor(we, true);
        } else {
            d->buf.setCursor(p, io.KeyShift);
            m_dragSelecting = true;
            m_dragGroup = g;
        }
        m_comp.open = false;
        m_editorFocused = true;
        activate(d, g);
    }
    if (m_dragSelecting && m_dragGroup == g) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            d->buf.setCursor(posAt(mp), true);
            if (mp.y > o.y + sz.y) d->scrollY += lineH;
            if (mp.y < o.y) d->scrollY -= lineH;
        } else {
            m_dragSelecting = false;
        }
    }
    float mmScroll = 0, mmRowH = 2 * s;
    if (settings.minimap) {
        float mmContent = static_cast<float>(nRows) * mmRowH;
        mmScroll = maxScroll > 0 ? (d->scrollY / maxScroll) * std::max(0.0f, mmContent - sz.y) : 0;
        if (hovered && inMinimap && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float row = (mp.y - o.y + mmScroll) / mmRowH;
            d->scrollY = std::clamp(row * lineH - sz.y * 0.5f, 0.0f, maxScroll);
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && inGutter) {
        Pos p = posAt({textX0, mp.y});
        float gx = mp.x - o.x;
        if (gx < 14 * s) {  // breakpoint lane (stub)
            if (!d->breakpoints.erase(p.line)) d->breakpoints.insert(p.line);
        } else if (gx > gutterW - 16 * s) {  // fold chevron
            for (auto& fr : d->folds)
                if (fr.startLine == p.line) {
                    if (!d->folded.erase(p.line)) d->folded.insert(p.line);
                    break;
                }
        } else {  // line number: select the line
            d->buf.setCursor({p.line, 0});
            d->buf.setCursor(p.line + 1 < d->buf.lineCount() ? Pos{p.line + 1, 0} : Pos{p.line, static_cast<int>(d->buf.line(p.line).size())}, true);
        }
        m_editorFocused = true;
        activate(d, g);
    }
    if (hovered && (inText || inGutter)) ImGui::SetMouseCursor(inText ? ImGuiMouseCursor_TextInput : ImGuiMouseCursor_Arrow);

    // ---- keyboard
    if (focusedHere) {
        for (ImGuiKey k : {ImGuiKey_Tab, ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_PageUp, ImGuiKey_PageDown,
                           ImGuiKey_Home, ImGuiKey_End, ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Backspace, ImGuiKey_Delete, ImGuiKey_Escape, ImGuiKey_Space})
            ImGui::SetKeyOwner(k, id);
        int pageRows = std::max(1, static_cast<int>(sz.y / lineH) - 2);
        // completion navigation first
        if (m_comp.open) {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat, id)) ++m_comp.sel;
            else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat, id)) m_comp.sel = std::max(0, m_comp.sel - 1);
            else if (ImGui::IsKeyPressed(ImGuiKey_Enter, 0, id) || ImGui::IsKeyPressed(ImGuiKey_Tab, 0, id)) acceptCompletion(d);
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape, 0, id)) m_comp.open = false;
            else handleKeys(d);
        } else {
            handleKeys(d);
        }
        bool sh = io.KeyShift, ctrl = io.KeyCtrl;
        if (!m_comp.open && !io.KeyAlt) {
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat, id) && !ctrl) d->buf.moveCursors(-1, 0, sh), d->reveal = true;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat, id) && !ctrl) d->buf.moveCursors(1, 0, sh), d->reveal = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat, id) && ctrl && !io.KeyAlt) d->scrollY -= lineH;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat, id) && ctrl && !io.KeyAlt) d->scrollY += lineH;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, ImGuiInputFlags_Repeat, id) && !io.KeyAlt) d->buf.moveCursors(0, -1, sh, ctrl), d->reveal = true, m_comp.open = false;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, ImGuiInputFlags_Repeat, id) && !io.KeyAlt) d->buf.moveCursors(0, 1, sh, ctrl), d->reveal = true, m_comp.open = false;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, ImGuiInputFlags_Repeat, id)) d->buf.moveCursors(-pageRows, 0, sh), d->reveal = true;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, ImGuiInputFlags_Repeat, id)) d->buf.moveCursors(pageRows, 0, sh), d->reveal = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Home, ImGuiInputFlags_Repeat, id)) {
            if (ctrl) d->buf.setCursor({0, 0}, sh);
            else d->buf.home(sh);
            d->reveal = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_End, ImGuiInputFlags_Repeat, id)) {
            if (ctrl) d->buf.setCursor({d->buf.lineCount() - 1, static_cast<int>(d->buf.line(d->buf.lineCount() - 1).size())}, sh);
            else d->buf.end(sh);
            d->reveal = true;
        }
        if (!d->readOnly) {
            if (!m_comp.open && (ImGui::IsKeyPressed(ImGuiKey_Enter, ImGuiInputFlags_Repeat, id) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, ImGuiInputFlags_Repeat, id))) {
                d->buf.newline();
                d->lastEdit = now();
                d->reveal = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Backspace, ImGuiInputFlags_Repeat, id)) {
                if (ctrl && !d->buf.cursors[0].hasSelection()) d->buf.moveCursors(0, -1, true, true);
                d->buf.backspace();
                d->lastEdit = now();
                d->reveal = true;
                if (m_comp.open) triggerCompletion(d, false);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, ImGuiInputFlags_Repeat, id)) {
                if (ctrl && !d->buf.cursors[0].hasSelection()) d->buf.moveCursors(0, 1, true, true);
                d->buf.del();
                d->lastEdit = now();
            }
            if (!m_comp.open && ImGui::IsKeyPressed(ImGuiKey_Tab, ImGuiInputFlags_Repeat, id)) {
                if (sh) d->buf.outdent();
                else d->buf.indent();
                d->lastEdit = now();
            }
            if (!ctrl && !io.KeyAlt)
                for (ImWchar c : io.InputQueueCharacters) {
                    if (c < 32 || c == 127) continue;
                    typeText(d, utf8(c));
                }
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Space, 0, id)) triggerCompletion(d, true);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, 0, id)) {
            if (m_findOpen) m_findOpen = false;
            else if (d->buf.cursors.size() > 1) d->buf.cursors.resize(1);
            m_sig.reset();
        }
        io.InputQueueCharacters.resize(0);
    }
    d->scrollY = std::clamp(d->scrollY, 0.0f, maxScroll);
    if (d->buf.version() != d->foldsVersion) rows = layoutRows(d, wrapCols), nRows = static_cast<int>(rows.size());

    // ---- tokenizer states
    if (d->statesVersion != d->buf.version()) {
        d->lineStates.assign(static_cast<size_t>(d->buf.lineCount()) + 1, 0);
        int st = 0;
        for (int l = 0; l < d->buf.lineCount(); ++l) {
            d->lineStates[static_cast<size_t>(l)] = st;
            if (d->lang != Lang::Plain) tokenizeLine(d->lang, d->buf.line(l), st);
        }
        d->statesVersion = d->buf.version();
    }

    // ---- render
    dl->PushClipRect(o, {o.x + sz.x, o.y + sz.y}, true);
    ImU32 lineNo = lt ? IM_COL32(110, 118, 129, 255) : IM_COL32(110, 118, 129, 255);
    ImU32 lineNoCur = lt ? IM_COL32(23, 23, 23, 255) : IM_COL32(204, 204, 204, 255);
    ImU32 curLine = lt ? IM_COL32(0, 0, 0, 14) : IM_COL32(255, 255, 255, 12);
    ImU32 selCol = lt ? IM_COL32(173, 214, 255, 255) : IM_COL32(38, 79, 120, 255);
    ImU32 guide = lt ? IM_COL32(0, 0, 0, 30) : IM_COL32(255, 255, 255, 26);
    ImU32 caretCol = lt ? IM_COL32(0, 0, 0, 255) : IM_COL32(174, 175, 173, 255);
    ImU32 findCol = lt ? IM_COL32(234, 92, 0, 80) : IM_COL32(234, 92, 0, 90);
    int first = std::max(0, static_cast<int>(d->scrollY / lineH));
    int last = std::min(nRows - 1, static_cast<int>((d->scrollY + sz.y) / lineH) + 1);
    auto diags = diagnosticsFor(d->path);
    std::vector<Match> findHits;
    if (m_findOpen && !m_findQuery.empty() && m_groups[m_activeGroup].active == d)
        if (auto fh = findAll(d->buf, m_findQuery, m_findOpt)) findHits = *fh;
    std::optional<Pos> bm = matchBracket(d->buf, d->buf.cursors[0].pos);
    std::optional<Pos> bmFrom;
    if (bm) {
        Pos c = d->buf.cursors[0].pos;
        const std::string& cl = d->buf.line(c.line);
        bmFrom = (c.col < static_cast<int>(cl.size()) && std::strchr("()[]{}", cl[static_cast<size_t>(c.col)])) ? c : Pos{c.line, c.col - 1};
    }
    int primLine = d->buf.cursors[0].pos.line;
    bool blinkOn = std::fmod(now() - d->lastEdit, 1.0) < 0.6 || now() - d->lastEdit < 0.5;
    std::vector<ImU32> colors;
    for (int r = first; r <= last; ++r) {
        const Row& row = rows[static_cast<size_t>(r)];
        const std::string& line = d->buf.line(row.line);
        float y = o.y + static_cast<float>(r) * lineH - d->scrollY;
        float ty = y + (lineH - m_fontPx) * 0.5f;
        float base = colX(line, row.start);
        auto xOf = [&](int col) { return textX0 - d->scrollX + colX(line, col) - base; };
        if (row.line == primLine) dl->AddRectFilled({o.x, y}, {o.x + sz.x - mmW, y + lineH}, curLine);
        // selections
        for (auto& c : d->buf.cursors) {
            if (!c.hasSelection()) continue;
            Pos a = c.selStart(), b = c.selEnd();
            if (row.line < a.line || row.line > b.line) continue;
            int c0 = row.line == a.line ? std::max(a.col, row.start) : row.start;
            int c1 = row.line == b.line ? std::min(b.col, row.end) : row.end;
            if (row.line == b.line && b.col < row.start) continue;
            if (row.line == a.line && a.col > row.end) continue;
            float x1 = xOf(c1) + ((row.line < b.line && c1 == row.end) ? m_charW * 0.5f : 0);
            dl->AddRectFilled({xOf(c0), y}, {std::max(x1, xOf(c0) + 2), y + lineH}, selCol);
        }
        for (auto& m : findHits)
            if (m.start.line == row.line && m.start.col >= row.start && m.start.col < std::max(row.end, row.start + 1))
                dl->AddRectFilled({xOf(m.start.col), y}, {xOf(m.end.col), y + lineH}, findCol);
        if (bm && bmFrom)
            for (Pos p : {*bm, *bmFrom})
                if (p.line == row.line && p.col >= row.start && p.col < row.end)
                    dl->AddRect({xOf(p.col), y}, {xOf(p.col + 1), y + lineH}, lt ? IM_COL32(0, 0, 0, 90) : IM_COL32(255, 255, 255, 90));
        // indent guides
        if (settings.indentGuides && row.start == 0) {
            int ind = 0;
            const std::string* src = &line;
            if (line.find_first_not_of(" \t") == std::string::npos)  // blank: use the next non-blank line
                for (int k = row.line + 1; k < d->buf.lineCount() && k < row.line + 50; ++k)
                    if (d->buf.line(k).find_first_not_of(" \t") != std::string::npos) {
                        src = &d->buf.line(k);
                        break;
                    }
            for (char ch : *src) {
                if (ch == ' ') ++ind;
                else if (ch == '\t') ind = (ind / settings.tabSize + 1) * settings.tabSize;
                else break;
            }
            for (int k = settings.tabSize; k < ind || (k == ind && src != &line && k < ind); k += settings.tabSize) {
                float gx = textX0 - d->scrollX + static_cast<float>(k - settings.tabSize) * m_charW;
                if (k - settings.tabSize > 0 || true) dl->AddLine({gx + 0.5f, y}, {gx + 0.5f, y + lineH}, guide);
            }
        }
        // text
        colors.assign(line.size(), tokColor(Tok::Text));
        if (!d->virtualName.empty()) {  // diff view
            ImU32 c = tokColor(Tok::Text);
            if (startsWith(line, "+")) c = lt ? IM_COL32(20, 130, 40, 255) : IM_COL32(110, 200, 120, 255);
            else if (startsWith(line, "-")) c = lt ? IM_COL32(190, 30, 30, 255) : IM_COL32(240, 110, 110, 255);
            else if (startsWith(line, "@@")) c = tokColor(Tok::Preprocessor);
            std::fill(colors.begin(), colors.end(), c);
        } else if (d->lang != Lang::Plain) {
            int st = d->lineStates[static_cast<size_t>(row.line)];
            for (auto& t : tokenizeLine(d->lang, line, st))
                for (int k = t.start; k < t.start + t.len && k < static_cast<int>(colors.size()); ++k) colors[static_cast<size_t>(k)] = tokColor(t.kind);
        }
        int k = row.start;
        while (k < row.end) {
            int e = k;
            while (e < row.end && colors[static_cast<size_t>(e)] == colors[static_cast<size_t>(k)] && line[static_cast<size_t>(e)] != '\t') ++e;
            if (e == k) {  // a tab
                ++k;
                continue;
            }
            dl->AddText(f, m_fontPx, {xOf(k), ty}, colors[static_cast<size_t>(k)], line.data() + k, line.data() + e);
            k = e;
        }
        // whitespace
        if (settings.whitespace > 0) {
            for (int c = row.start; c < row.end; ++c) {
                char ch = line[static_cast<size_t>(c)];
                if (ch != ' ' && ch != '\t') continue;
                bool show = settings.whitespace == 2;
                if (!show)
                    for (auto& cur : d->buf.cursors)
                        if (cur.hasSelection() && Pos{row.line, c} >= cur.selStart() && Pos{row.line, c} < cur.selEnd()) show = true;
                if (!show) continue;
                float cx = xOf(c);
                if (ch == ' ') dl->AddCircleFilled({cx + m_charW * 0.5f, y + lineH * 0.5f}, 1.2f * s, guide);
                else dl->AddLine({cx + 2, y + lineH * 0.5f}, {xOf(c + 1) - 2, y + lineH * 0.5f}, guide);
            }
        }
        // folded marker
        if (d->folded.count(row.line) && row.end == static_cast<int>(line.size())) {
            float x = xOf(row.end) + m_charW;
            dl->AddRectFilled({x, y + 2}, {x + m_charW * 3, y + lineH - 2}, lt ? IM_COL32(0, 0, 0, 25) : IM_COL32(255, 255, 255, 30), 3);
            dl->AddText(f, m_fontPx, {x, ty}, lineNo, "...");
        }
        // diagnostics
        for (auto& dg : diags) {
            if (dg.start.line != row.line) continue;
            int c0 = std::clamp(dg.start.col, row.start, row.end), c1 = dg.end.line == row.line ? std::clamp(dg.end.col, row.start, row.end) : row.end;
            if (c1 <= c0) {
                Pos ws, we;
                wordAt(d->buf, {row.line, c0}, &ws, &we);
                c1 = std::max(c0 + 1, we.col);
            }
            ImU32 col = dg.severity == 1 ? IM_COL32(241, 76, 76, 255) : dg.severity == 2 ? IM_COL32(204, 167, 0, 255) : IM_COL32(55, 148, 255, 255);
            float x0 = xOf(c0), x1 = std::max(xOf(c1), x0 + m_charW), yy = y + lineH - 3;
            for (float x = x0; x < x1; x += 3 * s) dl->AddLine({x, yy + ((static_cast<int>((x - x0) / (3 * s)) & 1) ? 1.5f : -0.5f)}, {x + 3 * s, yy + ((static_cast<int>((x - x0) / (3 * s)) & 1) ? -0.5f : 1.5f)}, col);
        }
        // carets
        if (blinkOn || !focusedHere)
            for (auto& c : d->buf.cursors) {
                if (c.pos.line != row.line || c.pos.col < row.start || (c.pos.col > row.end) || (c.pos.col == row.end && row.end != static_cast<int>(line.size()) && r + 1 < nRows && rows[static_cast<size_t>(r + 1)].line == row.line)) continue;
                if (!focusedHere && &c != &d->buf.cursors[0]) continue;
                float x = xOf(c.pos.col);
                dl->AddRectFilled({x, y + 1}, {x + 2 * s, y + lineH - 1}, focusedHere ? caretCol : (lt ? IM_COL32(0, 0, 0, 60) : IM_COL32(255, 255, 255, 50)));
            }
        // gutter (drawn over horizontally scrolled text)
        dl->AddRectFilled({o.x, y}, {textX0 - 2, y + lineH}, bg);
        if (row.line == primLine) dl->AddRectFilled({o.x, y}, {textX0 - 2, y + lineH}, curLine);
        if (row.start == 0) {
            if (m_dbgLine == row.line && !m_dbgFile.empty() && samePath(m_dbgFile, d->path)) {  // execution paused here
                dl->AddRectFilled({o.x, y}, {o.x + ImGui::GetWindowWidth(), y + lineH}, IM_COL32(255, 210, 0, lt ? 70 : 45));
                dl->AddTriangleFilled({o.x + 2 * s, y + 3 * s}, {o.x + 2 * s, y + lineH - 3 * s}, {o.x + 13 * s, y + lineH * 0.5f}, IM_COL32(255, 200, 0, 255));
            } else if (d->breakpoints.count(row.line)) {
                dl->AddCircleFilled({o.x + 7 * s, y + lineH * 0.5f}, 4.5f * s, IM_COL32(229, 20, 0, 255));
            }
            if (settings.lineNumbers) {
                std::string num = std::to_string(row.line + 1);
                float nw = f->CalcTextSizeA(m_fontPx, FLT_MAX, 0, num.c_str()).x;
                dl->AddText(f, m_fontPx, {textX0 - 18 * s - nw, ty}, row.line == primLine ? lineNoCur : lineNo, num.c_str());
            }
            for (auto& fr : d->folds)
                if (fr.startLine == row.line) {
                    bool closed = d->folded.count(row.line) > 0;
                    if (closed || (hovered && inGutter) || (mp.y >= y && mp.y < y + lineH))
                        dl->AddText(ImGui::GetFont(), m_fontPx * 0.7f, {textX0 - 14 * s, y + (lineH - m_fontPx * 0.7f) * 0.5f}, lineNo, closed ? ICON_CHEVRON_RIGHT : ICON_CHEVRON_DOWN);
                    break;
                }
        }
    }
    // minimap
    if (settings.minimap) {
        float mx0 = o.x + sz.x - mmW;
        dl->AddRectFilled({mx0, o.y}, {o.x + sz.x, o.y + sz.y}, lt ? IM_COL32(0, 0, 0, 6) : IM_COL32(255, 255, 255, 5));
        int r0 = static_cast<int>(mmScroll / mmRowH), r1 = std::min(nRows - 1, r0 + static_cast<int>(sz.y / mmRowH) + 1);
        float cw = std::max(0.6f, mmW / 120.0f);
        for (int r = r0; r <= r1; ++r) {
            const Row& row = rows[static_cast<size_t>(r)];
            const std::string& line = d->buf.line(row.line);
            float y = o.y + static_cast<float>(r) * mmRowH - mmScroll;
            int st = d->lineStates[static_cast<size_t>(row.line)];
            auto toks = d->lang != Lang::Plain && d->virtualName.empty() ? tokenizeLine(d->lang, line, st) : std::vector<Token>{};
            size_t ti = 0;
            for (int c = row.start; c < row.end && c < 120 + row.start;) {
                if (line[static_cast<size_t>(c)] == ' ' || line[static_cast<size_t>(c)] == '\t') {
                    ++c;
                    continue;
                }
                int e = c;
                while (e < row.end && line[static_cast<size_t>(e)] != ' ') ++e;
                while (ti < toks.size() && toks[ti].start + toks[ti].len <= c) ++ti;
                ImU32 col = ti < toks.size() && toks[ti].start <= c ? tokColor(toks[ti].kind) : tokColor(Tok::Text);
                col = (col & 0x00FFFFFF) | 0x99000000;
                dl->AddRectFilled({mx0 + 4 + static_cast<float>(c - row.start) * cw, y}, {mx0 + 4 + static_cast<float>(e - row.start) * cw, y + mmRowH * 0.8f}, col);
                c = e;
            }
        }
        float vy0 = o.y + (d->scrollY / lineH) * mmRowH - mmScroll, vy1 = vy0 + (sz.y / lineH) * mmRowH;
        dl->AddRectFilled({mx0, vy0}, {o.x + sz.x, vy1}, lt ? IM_COL32(0, 0, 0, 22) : IM_COL32(255, 255, 255, 18));
    }
    // vertical scrollbar indicator
    if (maxScroll > 0) {
        float th = std::max(20 * s, sz.y * sz.y / contentH), ty0 = o.y + (sz.y - th) * (d->scrollY / maxScroll);
        dl->AddRectFilled({o.x + sz.x - 6 * s, ty0}, {o.x + sz.x - 1, ty0 + th}, lt ? IM_COL32(0, 0, 0, 40) : IM_COL32(255, 255, 255, 35), 3);
    }
    dl->PopClipRect();

    // ---- find widget
    if (m_findOpen && m_activeGroup == g) drawFindWidget(d, {o.x + sz.x - mmW - 420 * s, o.y + 2}, 410 * s);

    // ---- hover: diagnostics + LSP hover
    if (hovered && inText && !ImGui::IsMouseDown(0)) {
        Pos p = posAt(mp);
        std::string tip;
        for (auto& dg : diags)
            if (dg.start.line == p.line && (p.col >= dg.start.col - 1 && (dg.end.line > p.line || p.col <= std::max(dg.end.col, dg.start.col + 1) + 1)))
                tip += std::string(dg.severity == 1 ? "error: " : dg.severity == 2 ? "warning: " : "info: ") + dg.message + "\n";
        if (p != m_hoverPos) {
            m_hoverPos = p;
            m_hoverStart = now();
            m_hoverRequested = false;
            m_hoverText.clear();
        } else if (!m_hoverRequested && now() - m_hoverStart > 0.5) {
            m_hoverRequested = true;
            std::string w = wordAt(d->buf, p);
            if (!w.empty()) {
                if (m_lsp.state() == LspClient::State::Ready && d->lang == Lang::Cpp) {
                    lspSync(d);
                    m_lsp.hover(d->path, p, [this, p](std::string t) {
                        if (p == m_hoverPos) m_hoverText = t;
                    });
                } else if (auto* e = m_index.definition(w)) {
                    m_hoverText = std::format("{} {}\n{}:{}  (basic index)", e->kind, e->name, narrow(fs::path(widen(e->file)).filename().wstring()), e->line + 1);
                }
            }
        }
        if (!m_hoverText.empty()) tip += (tip.empty() ? "" : "\n") + m_hoverText;
        if (!tip.empty() && now() - m_hoverStart > 0.5) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40);
            ImGui::TextUnformatted(tip.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    // ---- completion + signature popups (anchored at the caret)
    if (m_activeGroup == g && (m_comp.open || m_sig)) {
        Pos c = d->buf.cursors[0].pos;
        int r = rowOf(c);
        const Row& row = rows.empty() ? Row{0, 0, 0} : rows[static_cast<size_t>(std::clamp(r, 0, nRows - 1))];
        float x = textX0 - d->scrollX + colX(d->buf.line(c.line), c.col) - colX(d->buf.line(c.line), row.start);
        float y = o.y + static_cast<float>(r) * lineH - d->scrollY;
        if (m_sig) {
            ImGui::SetNextWindowPos({x, y - 4}, ImGuiCond_Always, {0, 1});
            ImGui::BeginTooltip();
            const auto& sg = *m_sig;
            if (sg.paramStart >= 0 && sg.paramEnd <= static_cast<int>(sg.label.size())) {
                ImGui::TextUnformatted(sg.label.c_str(), sg.label.c_str() + sg.paramStart);
                ImGui::SameLine(0, 0);
                ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%.*s", sg.paramEnd - sg.paramStart, sg.label.c_str() + sg.paramStart);
                ImGui::SameLine(0, 0);
                ImGui::TextUnformatted(sg.label.c_str() + sg.paramEnd);
            } else {
                ImGui::TextUnformatted(sg.label.c_str());
            }
            ImGui::EndTooltip();
        }
        if (m_comp.open) {
            std::string prefix = d->buf.textIn(m_comp.start, c);
            std::vector<std::pair<int, const CompletionItem*>> vis;
            for (auto& it : m_comp.items) {
                int sc = prefix.empty() ? 0 : fuzzyScore(prefix, it.label);
                if (sc >= 0) vis.push_back({sc, &it});
            }
            if (!m_comp.fromLsp || !prefix.empty())
                std::stable_sort(vis.begin(), vis.end(), [](auto& a, auto& b) { return a.first > b.first; });
            if (vis.empty() || c.line != m_comp.start.line || c.col < m_comp.start.col) {
                m_comp.open = false;
            } else {
                // keep the filtered order for acceptCompletion
                std::vector<CompletionItem> sorted;
                for (auto& v : vis) sorted.push_back(*v.second);
                m_comp.items = std::move(sorted);
                m_comp.sel = std::clamp(m_comp.sel, 0, static_cast<int>(m_comp.items.size()) - 1);
                ImGui::SetNextWindowPos({x, y + lineH});
                ImGui::SetNextWindowSize({380 * s, std::min(10, static_cast<int>(m_comp.items.size())) * ImGui::GetTextLineHeightWithSpacing() + 34 * s});
                ImGui::Begin("##completion", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_Tooltip);
                for (int i = 0; i < static_cast<int>(m_comp.items.size()) && i < 200; ++i) {
                    auto& it = m_comp.items[static_cast<size_t>(i)];
                    bool sel = i == m_comp.sel;
                    if (ImGui::Selectable((it.label + "##c" + std::to_string(i)).c_str(), sel)) {
                        m_comp.sel = i;
                        acceptCompletion(d);
                        break;
                    }
                    if (sel) ImGui::SetScrollHereY();
                    if (!it.detail.empty()) {
                        ImGui::SameLine(200 * s);
                        ImGui::TextDisabled("%s", it.detail.substr(0, 40).c_str());
                    }
                }
                ImGui::TextDisabled(m_comp.fromLsp ? "clangd" : "basic index");
                ImGui::End();
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void CodeWorkspace::handleKeys(Doc* d) {
    ImGuiIO& io = ImGui::GetIO();
    TextBuffer& b = d->buf;
    bool ro = d->readOnly;
    auto edited = [&] {
        d->lastEdit = now();
        d->reveal = true;
    };
    if (keyPressed("undo") && !ro) {
        b.undo();
        edited();
    } else if ((keyPressed("redo") || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) && !ro) {
        b.redo();
        edited();
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) b.selectAll();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Insert)) {
        std::string clip;
        if (!b.cursors[0].hasSelection()) clip = b.line(b.cursors[0].pos.line) + "\n";
        else
            for (size_t i = 0; i < b.cursors.size(); ++i) clip += (i ? "\n" : "") + b.textIn(b.cursors[i].selStart(), b.cursors[i].selEnd());
        ImGui::SetClipboardText(clip.c_str());
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_X) && !ro) {
        std::string clip;
        if (!b.cursors[0].hasSelection()) {
            int l = b.cursors[0].pos.line;
            clip = b.line(l) + "\n";
            b.setCursor({l, 0});
            b.setCursor(l + 1 < b.lineCount() ? Pos{l + 1, 0} : Pos{l, static_cast<int>(b.line(l).size())}, true);
        } else {
            for (size_t i = 0; i < b.cursors.size(); ++i) clip += (i ? "\n" : "") + b.textIn(b.cursors[i].selStart(), b.cursors[i].selEnd());
        }
        ImGui::SetClipboardText(clip.c_str());
        b.insert("");
        edited();
    }
    if ((ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V) || ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_Insert)) && !ro) {
        const char* c = ImGui::GetClipboardText();
        std::string clip = c ? c : "";
        clip.erase(std::remove(clip.begin(), clip.end(), '\r'), clip.end());
        // one line per cursor when the counts match (multi-cursor copy/paste)
        std::vector<std::string> parts;
        for (size_t p = 0, q; p <= clip.size(); p = q + 1) {
            q = clip.find('\n', p);
            if (q == std::string::npos) q = clip.size();
            parts.push_back(clip.substr(p, q - p));
        }
        if (b.cursors.size() > 1 && parts.size() == b.cursors.size()) {
            for (size_t i = b.cursors.size(); i-- > 0;) {
                Cursor c2 = b.cursors[i];
                b.replaceRange(c2.selStart(), c2.selEnd(), parts[i]);
            }
        } else {
            b.insert(clip);
        }
        edited();
    }
    if (keyPressed("comment") && !ro) {
        std::string pre = commentPrefix(d->lang);
        if (!pre.empty()) b.toggleComment(pre);
        edited();
    }
    if (keyPressed("selectNext")) b.addNextOccurrence(), d->reveal = true;
    if (keyPressed("cursorAbove") || keyPressed("cursorBelow")) {
        Cursor last = b.cursors.back();
        int dl = keyPressed("cursorAbove") ? -1 : 1;
        Pos p{last.pos.line + dl, last.pos.col};
        if (p.line >= 0 && p.line < b.lineCount()) b.addCursor(b.clamp(p));
    }
    if (keyPressed("lineUp") && !ro) b.moveLines(-1), edited();
    if (keyPressed("lineDown") && !ro) b.moveLines(1), edited();
    if (keyPressed("deleteLine") && !ro) {
        int l = b.cursors[0].pos.line;
        b.setCursor({l, 0});
        b.setCursor(l + 1 < b.lineCount() ? Pos{l + 1, 0} : Pos{l, static_cast<int>(b.line(l).size())}, true);
        b.insert("");
        edited();
    }
    if (keyPressed("find") || keyPressed("replace")) {
        m_findOpen = true;
        m_findReplace = keyPressed("replace");
        m_findFocus = true;
        if (b.cursors[0].hasSelection() && b.cursors[0].selStart().line == b.cursors[0].selEnd().line) m_findQuery = b.selectedText();
    }
    if (keyPressed("search")) {
        activity = "search";
        if (b.cursors[0].hasSelection()) m_searchQuery = b.selectedText();
    }
    if (keyPressed("gotoLine")) {
        m_gotoLine = true;
        m_gotoQuery.clear();
    }
    if (keyPressed("gotoSymbol")) {
        m_gotoSymbol = true;
        m_gotoQuery.clear();
    }
    if (keyPressed("quickOpen")) openQuickOpen();
    if (keyPressed("format") && !ro) formatDoc(d);
    if (keyPressed("definition")) goToDefinition(d);
    if (keyPressed("references")) findReferences(d);
    if (keyPressed("rename") && !ro) startRename(d);
    if (keyPressed("wordWrap")) settings.wordWrap = !settings.wordWrap;
    if (keyPressed("zoomIn") || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd)) {
        settings.fontSize = std::min(32.0f, settings.fontSize + 1);
        m_app.m_fontsDirty = true;
    }
    if (keyPressed("zoomOut") || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract)) {
        settings.fontSize = std::max(8.0f, settings.fontSize - 1);
        m_app.m_fontsDirty = true;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_0)) {
        settings.fontSize = 14;
        m_app.m_fontsDirty = true;
    }
    if (keyPressed("split")) {
        m_split = !m_split;
        if (m_split) {
            m_groups[1].tabs = {d};
            m_groups[1].active = d;
            m_activeGroup = 1;
        } else {
            m_groups[1] = {};
            m_activeGroup = 0;
        }
    }
    if (keyPressed("closeTab") && !d->pinned) {
        requestClose(d, m_activeGroup);
        return;
    }
    if (keyPressed("terminal")) {
        showBottom = !showBottom || bottomTab != "terminal";
        bottomTab = "terminal";
        if (showBottom && m_terms.empty()) newTerminal();
    }
    if (m_dbg && m_dbg->state() == dbg::GdbSession::State::Stopped) {  // while paused F5 / F10 / F11 drive the debugger
        if (keyPressed("debugContinue")) debugCommand("continue");
        else if (keyPressed("stepOver")) debugCommand("next");
        else if (keyPressed("stepInto")) debugCommand("step");
        else if (keyPressed("stepOut")) debugCommand("finish");
    } else if (keyPressed("debug") && !debugging()) {
        startDebugging();
    } else if (keyPressed("debugStop") && debugging()) {
        stopDebugging();
    } else if (keyPressed("run")) {
        saveAll();
        m_app.m_commands.execute(m_app.m_session.playing() ? "play.stop" : "play.play");
    }
    if (keyPressed("breakpoint")) {
        int l = b.cursors[0].pos.line;
        if (!d->breakpoints.erase(l)) d->breakpoints.insert(l);
    }
    if (keyPressed("fold") || keyPressed("unfold")) {
        int l = b.cursors[0].pos.line;
        const FoldRange* best = nullptr;
        for (auto& fr : d->folds)
            if (fr.startLine <= l && fr.endLine >= l && (!best || fr.startLine > best->startLine)) best = &fr;
        if (best) {
            if (keyPressed("fold")) {
                d->folded.insert(best->startLine);
                b.setCursor({best->startLine, static_cast<int>(b.line(best->startLine).size())});
            } else {
                d->folded.erase(best->startLine);
            }
        }
    }
    (void)io;
}

void CodeWorkspace::drawFindWidget(Doc* d, ImVec2 pos, float width) {
    float s = m_app.effectiveScale();
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize({width, 0});
    ImGui::Begin("##find", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking);
    if (m_findFocus) {
        ImGui::SetKeyboardFocusHere();
        m_findFocus = false;
    }
    ImGui::SetNextItemWidth(width - 190 * s);
    bool enter = ImGui::InputTextWithHint("##fq", "Find", &m_findQuery, ImGuiInputTextFlags_EnterReturnsTrue);
    m_app.trackRect("code.find.input");
    auto toggle = [&](const char* label, bool& v, const char* tip) {
        ImGui::SameLine(0, 2 * s);
        ImGui::PushStyleColor(ImGuiCol_Button, v ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        if (ImGui::SmallButton(label)) v = !v;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    toggle("Aa", m_findOpt.matchCase, "Match case");
    toggle("ab", m_findOpt.wholeWord, "Match whole word");
    toggle(".*", m_findOpt.regex, "Use regular expression");
    auto hits = findAll(d->buf, m_findQuery, m_findOpt);
    int n = hits ? static_cast<int>(hits->size()) : 0;
    ImGui::SameLine();
    if (!hits) ImGui::TextColored({1, 0.45f, 0.45f, 1}, "bad regex");
    else ImGui::TextDisabled(n ? "%d of %d" : "No results", std::min(m_findIndex + 1, n), n);
    ImGui::SameLine();
    bool next = enter || ImGui::SmallButton(ICON_ARROW_RIGHT);
    if (next && n > 0) {
        Pos cur = d->buf.cursors[0].selEnd();
        int i = 0;
        while (i < n && (*hits)[static_cast<size_t>(i)].start < cur) ++i;
        if (ImGui::GetIO().KeyShift) i = (i - 2 + 2 * n) % n;
        m_findIndex = i % n;
        auto& m = (*hits)[static_cast<size_t>(m_findIndex)];
        d->buf.setCursor(m.start);
        d->buf.setCursor(m.end, true);
        d->reveal = true;
        if (enter) ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_XMARK)) m_findOpen = false;
    if (ImGui::SmallButton(m_findReplace ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT)) m_findReplace = !m_findReplace;
    ImGui::SameLine();
    if (m_findReplace) {
        ImGui::SetNextItemWidth(width - 190 * s);
        ImGui::InputTextWithHint("##fr", "Replace", &m_findRepl);
        ImGui::SameLine();
        ImGui::BeginDisabled(d->readOnly || n == 0);
        if (ImGui::SmallButton("Replace")) {
            auto& c = d->buf.cursors[0];
            if (c.hasSelection() && d->buf.selectedText().size() > 0) {
                auto sel = findAll(TextBuffer(d->buf.selectedText()), m_findQuery, m_findOpt);
                if (sel && !sel->empty()) d->buf.insert(m_findOpt.regex ? std::regex_replace(d->buf.selectedText(), std::regex(m_findQuery), m_findRepl) : m_findRepl);
            }
            d->lastEdit = now();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("All")) {
            auto r = replaceAll(d->buf, m_findQuery, m_findRepl, m_findOpt);
            if (r) m_app.toast(ToastLevel::Info, std::format("Replaced {} occurrence{}", *r, *r == 1 ? "" : "s"));
            d->lastEdit = now();
        }
        ImGui::EndDisabled();
    } else {
        ImGui::TextDisabled("Enter next, Shift+Enter previous, Esc close");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && ImGui::IsWindowFocused()) {
        m_findOpen = false;
        m_editorFocused = true;
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------- bottom panel ---
void CodeWorkspace::newTerminal() {
    auto t = std::make_unique<Terminal>();
    std::string shell = settings.shell;
    if (shell.empty()) shell = findOnPath("powershell.exe").empty() ? "cmd.exe" : "powershell.exe -NoLogo";
    std::string err;
    if (!t->start(shell, m_root.empty() ? fs::current_path() : m_root, 120, 30, &err)) {
        m_app.toast(ToastLevel::Error, "Cannot start the terminal: " + err);
        return;
    }
    t->name = shell.substr(0, shell.find(' '));
    m_terms.push_back(std::move(t));
    m_activeTerm = static_cast<int>(m_terms.size()) - 1;
    bottomTab = "terminal";
    showBottom = true;
}

void CodeWorkspace::drawBottom() {
    float s = m_app.effectiveScale();
    int errs = 0, warns = 0;
    for (auto& [f, ds] : m_lsp.diagnostics())
        for (auto& d : ds) (d.severity == 1 ? errs : warns) += d.severity <= 2 ? 1 : 0;
    struct T {
        const char* id;
        std::string label;
    };
    std::vector<T> tabs = {{"problems", std::format("PROBLEMS {}", errs + warns ? std::to_string(errs + warns) : "")},
                           {"output", "OUTPUT"},
                           {"build", "BUILD OUTPUT"},
                           {"terminal", "TERMINAL"},
                           {"references", "REFERENCES"},
                           {"breakpoints", "BREAKPOINTS"}};
    for (auto& t : tabs) {
        bool on = bottomTab == t.id;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, on ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::SmallButton(t.label.c_str())) {
            bottomTab = t.id;
            if (bottomTab == "terminal" && m_terms.empty()) newTerminal();
        }
        ImGui::PopStyleColor(2);
        m_app.trackRect(std::string("code.bottom.") + t.id);
        if (on) {
            ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine({mn.x, mx.y}, {mx.x, mx.y}, ImGui::GetColorU32(ImGuiCol_CheckMark), 2 * s);
        }
        ImGui::SameLine(0, 10 * s);
    }
    if (bottomTab == "terminal") {
        for (int i = 0; i < static_cast<int>(m_terms.size()); ++i) {
            ImGui::SameLine();
            ImGui::PushID(i);
            bool alive = m_terms[static_cast<size_t>(i)]->running();
            std::string l = std::format("{} {}{}", ICON_TERMINAL, m_terms[static_cast<size_t>(i)]->name, alive ? "" : " (exited)");
            if (ImGui::Selectable(l.c_str(), i == m_activeTerm, 0, {ImGui::CalcTextSize(l.c_str()).x, 0})) m_activeTerm = i;
            ImGui::PopID();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_PLUS)) newTerminal();
        m_app.trackRect("code.terminal.new");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("New terminal");
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_TRASH) && !m_terms.empty()) {
            m_terms[static_cast<size_t>(m_activeTerm)]->stop();
            m_terms.erase(m_terms.begin() + m_activeTerm);
            m_activeTerm = std::max(0, m_activeTerm - 1);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Kill terminal");
    }
    ImGui::Separator();
    if (bottomTab == "terminal" && m_terms.empty() && !m_termAutoStarted) {  // first look at the terminal starts one
        m_termAutoStarted = true;
        newTerminal();
    }
    if (bottomTab == "problems") drawProblems(false);
    else if (bottomTab == "output") {
        ImGui::BeginChild("##out");
        ImGui::PushFont(font ? font : ImGui::GetFont());
        ImGui::TextUnformatted(m_output.c_str());
        ImGui::TextDisabled("Language features: %s", lspStatus().c_str());
        ImGui::PopFont();
        ImGui::EndChild();
    } else if (bottomTab == "build") {
        const auto& b = m_app.m_session.lastScriptBuild();
        if (m_app.m_scriptBuildRunning) ImGui::TextDisabled("%s Building scripts...", ICON_HOURGLASS);
        else if (b.log.empty()) ImGui::TextDisabled("No script build yet. Press Build (Ctrl+B).");
        else ImGui::TextColored(b.ok ? ImVec4(0.4f, 0.85f, 0.5f, 1) : ImVec4(1, 0.45f, 0.45f, 1), "%s  %d error(s), %d warning(s), %.1fs", b.ok ? "Build succeeded" : "Build failed",
                                b.errors(), b.warnings(), b.seconds);
        ImGui::BeginChild("##buildlog");
        fs::path scripts = m_app.m_session.hasProject() ? m_app.m_session.project().scriptsDir() : fs::path();
        for (size_t i = 0; i < b.diagnostics.size(); ++i) {
            auto& dg = b.diagnostics[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(std::format("{}:{}:{}: {}: {}", dg.file, dg.line, dg.column, toString(dg.severity), dg.message).c_str()))
                jumpTo(scripts / pathFromUtf8(dg.file), {std::max(0, dg.line - 1), std::max(0, dg.column - 1)});
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::PushFont(font ? font : ImGui::GetFont());
        ImGui::TextUnformatted(b.log.c_str());
        ImGui::PopFont();
        ImGui::EndChild();
    } else if (bottomTab == "terminal") {
        if (m_terms.empty()) ImGui::TextDisabled("No terminal. Press + to start one.");
        else drawTerminal(std::clamp(m_activeTerm, 0, static_cast<int>(m_terms.size()) - 1));
    } else if (bottomTab == "references") {
        ImGui::TextDisabled("%s (%zu)", m_refsTitle.c_str(), m_refs.size());
        ImGui::BeginChild("##refs");
        for (size_t i = 0; i < m_refs.size(); ++i) {
            auto& r = m_refs[i];
            ImGui::PushID(static_cast<int>(i));
            std::string line;
            if (Doc* od = findDoc(r.file)) line = r.start.line < od->buf.lineCount() ? od->buf.line(r.start.line) : "";
            size_t st = line.find_first_not_of(" \t");
            line = st == std::string::npos ? "" : line.substr(st);
            if (ImGui::Selectable(std::format("{}:{}  {}", relOf(m_root, r.file), r.start.line + 1, line).c_str())) jumpTo(r.file, r.start);
            ImGui::PopID();
        }
        ImGui::EndChild();
    } else if (bottomTab == "breakpoints") {
        drawDebugPanel();
    }
}

void CodeWorkspace::drawTerminal(int i) {
    Terminal& t = *m_terms[static_cast<size_t>(i)];
    float s = m_app.effectiveScale();
    ImFont* f = font ? font : ImGui::GetFont();
    float px = std::round(13 * s);
    float cw = f->CalcTextSizeA(px, FLT_MAX, 0, "M").x, lh = std::round(px * 1.3f);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    int cols = std::max(20, static_cast<int>((avail.x - 16 * s) / cw)), rowsN = std::max(5, static_cast<int>(avail.y / lh) - 1);
    t.resize(cols, rowsN);
    bool lt = light();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, lt ? IM_COL32(255, 255, 255, 255) : IM_COL32(24, 24, 24, 255));
    ImGui::BeginChild("##term", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    const auto& sb = t.screen.scrollback();
    const auto& sc = t.screen.screen();
    int total = static_cast<int>(sb.size() + sc.size());
    ImGui::Dummy({static_cast<float>(cols) * cw, static_cast<float>(total) * lh});
    static size_t lastTotal = 0;
    if (static_cast<size_t>(total) != lastTotal) {
        ImGui::SetScrollY(ImGui::GetScrollMaxY() + lh * 2);
        lastTotal = static_cast<size_t>(total);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    float sy = ImGui::GetScrollY();
    int first = std::max(0, static_cast<int>(sy / lh)), last = std::min(total - 1, static_cast<int>((sy + ImGui::GetWindowHeight()) / lh) + 1);
    ImU32 deff = lt ? IM_COL32(30, 30, 30, 255) : IM_COL32(204, 204, 204, 255);
    for (int r = first; r <= last; ++r) {
        const AnsiScreen::Row& row = r < static_cast<int>(sb.size()) ? sb[static_cast<size_t>(r)] : sc[static_cast<size_t>(r - static_cast<int>(sb.size()))];
        float y = o.y + static_cast<float>(r) * lh - sy;
        for (int c = 0; c < static_cast<int>(row.size()); ++c) {
            const auto& cell = row[static_cast<size_t>(c)];
            float x = o.x + 6 * s + static_cast<float>(c) * cw;
            if (cell.bg) dl->AddRectFilled({x, y}, {x + cw, y + lh}, rgb(cell.bg));
            if (cell.ch == U' ' || cell.ch == 0) continue;
            std::string g = utf8(static_cast<unsigned int>(cell.ch));
            dl->AddText(f, px, {x, y + (lh - px) * 0.5f}, cell.fg ? rgb(cell.fg) : deff, g.c_str());
        }
    }
    bool focused = ImGui::IsWindowFocused();
    if (focused && t.running()) {
        int cr = static_cast<int>(sb.size()) + t.screen.cursorRow();
        float x = o.x + 6 * s + static_cast<float>(t.screen.cursorCol()) * cw, y = o.y + static_cast<float>(cr) * lh - sy;
        if (std::fmod(now(), 1.0) < 0.6) dl->AddRectFilled({x, y + 1}, {x + cw, y + lh - 1}, lt ? IM_COL32(0, 0, 0, 120) : IM_COL32(255, 255, 255, 140));
        ImGuiIO& io = ImGui::GetIO();
        ImGuiID id = ImGui::GetID("##termkeys");
        for (ImGuiKey k : {ImGuiKey_Tab, ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_Enter, ImGuiKey_Backspace,
                           ImGuiKey_Escape, ImGuiKey_Delete, ImGuiKey_Home, ImGuiKey_End})
            ImGui::SetKeyOwner(k, id);
        struct K {
            ImGuiKey k;
            const char* seq;
        };
        static const K keys[] = {{ImGuiKey_Enter, "\r"},       {ImGuiKey_KeypadEnter, "\r"}, {ImGuiKey_Backspace, "\x7f"}, {ImGuiKey_Tab, "\t"},
                                 {ImGuiKey_Escape, "\x1b"},    {ImGuiKey_UpArrow, "\x1b[A"}, {ImGuiKey_DownArrow, "\x1b[B"}, {ImGuiKey_RightArrow, "\x1b[C"},
                                 {ImGuiKey_LeftArrow, "\x1b[D"}, {ImGuiKey_Home, "\x1b[H"},  {ImGuiKey_End, "\x1b[F"},     {ImGuiKey_Delete, "\x1b[3~"}};
        for (auto& k : keys)
            if (ImGui::IsKeyPressed(k.k, ImGuiInputFlags_Repeat, id)) t.write(k.seq);
        if (io.KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_C, false)) t.write("\x03");
            if (ImGui::IsKeyPressed(ImGuiKey_V, false))
                if (const char* c = ImGui::GetClipboardText()) t.write(c);
        } else {
            std::string in;
            for (ImWchar c : io.InputQueueCharacters)
                if (c >= 32) in += utf8(c);
            if (!in.empty()) t.write(in);
        }
        io.InputQueueCharacters.resize(0);
        ImGui::GetCurrentContext()->WantTextInputNextFrame = 1;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void CodeWorkspace::drawStatus() {
    float s = m_app.effectiveScale();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    dl->AddRectFilled(p, {p.x + ImGui::GetContentRegionAvail().x, p.y + h}, ImGui::GetColorU32(ImGuiCol_MenuBarBg));
    ImGui::SetCursorScreenPos({p.x + 6 * s, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f});
    int errs = 0, warns = 0;
    for (auto& [f, ds] : m_lsp.diagnostics())
        for (auto& d : ds) (d.severity == 1 ? errs : warns) += d.severity <= 2 ? 1 : 0;
    if (gitAvailable() && m_git.isRepo) {
        ImGui::TextDisabled("%s %s", ICON_BRANCH, m_git.branch.c_str());
        ImGui::SameLine(0, 16 * s);
    }
    ImGui::TextDisabled("%s %d  %s %d", ICON_ERROR, errs, ICON_WARNING, warns);
    if (ImGui::IsItemClicked()) {
        bottomTab = "problems";
        showBottom = true;
    }
    if (Doc* d = active()) {
        Pos c = d->buf.cursors[0].pos;
        std::string right = std::format("Ln {}, Col {}{}    Spaces: {}    UTF-8    {}    {}    {}", c.line + 1, c.col + 1,
                                        d->buf.cursors.size() > 1 ? std::format(" ({} cursors)", d->buf.cursors.size()) : "", settings.tabSize, d->crlf ? "CRLF" : "LF",
                                        langName(d->lang), settings.wordWrap ? "Wrap" : "");
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20, ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(right.c_str()).x - 8 * s));
        ImGui::TextDisabled("%s", right.c_str());
        if (ImGui::IsItemClicked()) m_gotoLine = true;
    }
}

void CodeWorkspace::drawPopups() {
    float s = m_app.effectiveScale();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    auto center = [&](float w) {
        ImGui::SetNextWindowPos({vp->WorkPos.x + (vp->WorkSize.x - w) * 0.5f, vp->WorkPos.y + 60 * s});
        ImGui::SetNextWindowSize({w, 0});
    };
    // quick open (Ctrl+E; Ctrl+P opens the global palette, which lists files too)
    if (m_quickOpen) {
        ImGui::OpenPopup("##codequick");
        m_quickOpen = false;
        m_quickSel = 0;
    }
    center(560 * s);
    if (ImGui::BeginPopup("##codequick")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        bool enter = ImGui::InputTextWithHint("##q", "Search files by name", &m_quickQuery, ImGuiInputTextFlags_EnterReturnsTrue);
        std::vector<std::pair<int, std::string>> items;
        for (auto& f : projectFiles()) {
            int sc = fuzzyScore(m_quickQuery, f);
            if (sc >= 0) items.push_back({sc, f});
        }
        std::stable_sort(items.begin(), items.end(), [](auto& a, auto& b) { return a.first > b.first; });
        if (items.size() > 100) items.resize(100);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ++m_quickSel;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) --m_quickSel;
        m_quickSel = std::clamp(m_quickSel, 0, std::max(0, static_cast<int>(items.size()) - 1));
        for (int i = 0; i < static_cast<int>(items.size()); ++i)
            if (ImGui::Selectable(items[static_cast<size_t>(i)].second.c_str(), i == m_quickSel) || (enter && i == m_quickSel)) {
                openFile(m_root / pathFromUtf8(items[static_cast<size_t>(i)].second));
                ImGui::CloseCurrentPopup();
                break;
            }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // go to line
    if (m_gotoLine) {
        ImGui::OpenPopup("##gotoline");
        m_gotoLine = false;
    }
    center(360 * s);
    if (ImGui::BeginPopup("##gotoline")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        Doc* d = active();
        ImGui::SetNextItemWidth(-1);
        std::string hint = d ? std::format("Line number (1 - {}), or line:column", d->buf.lineCount()) : "No editor";
        if (ImGui::InputTextWithHint("##gl", hint.c_str(), &m_gotoQuery, ImGuiInputTextFlags_EnterReturnsTrue) && d) {
            int line = std::atoi(m_gotoQuery.c_str()), col = 1;
            if (auto c = m_gotoQuery.find(':'); c != std::string::npos) col = std::atoi(m_gotoQuery.c_str() + c + 1);
            jumpTo(d->path, {std::max(0, line - 1), std::max(0, col - 1)});
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // go to symbol
    if (m_gotoSymbol) {
        ImGui::OpenPopup("##gotosym");
        m_gotoSymbol = false;
        m_quickSel = 0;
    }
    center(460 * s);
    if (ImGui::BeginPopup("##gotosym")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        Doc* d = active();
        ImGui::SetNextItemWidth(-1);
        bool enter = ImGui::InputTextWithHint("##gs", "Go to symbol in file", &m_gotoQuery, ImGuiInputTextFlags_EnterReturnsTrue);
        if (d) {
            auto syms = outline(d->buf, d->lang);
            std::vector<const Symbol*> vis;
            for (auto& sy : syms)
                if (fuzzyScore(m_gotoQuery, sy.name) >= 0) vis.push_back(&sy);
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ++m_quickSel;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) --m_quickSel;
            m_quickSel = std::clamp(m_quickSel, 0, std::max(0, static_cast<int>(vis.size()) - 1));
            for (int i = 0; i < static_cast<int>(vis.size()); ++i)
                if (ImGui::Selectable(std::format("{}  ({})##{}", vis[static_cast<size_t>(i)]->name, vis[static_cast<size_t>(i)]->kind, i).c_str(), i == m_quickSel) ||
                    (enter && i == m_quickSel)) {
                    jumpTo(d->path, {vis[static_cast<size_t>(i)]->line, 0});
                    ImGui::CloseCurrentPopup();
                    break;
                }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // rename
    if (m_renameOpen) {
        ImGui::OpenPopup("##rename");
        m_renameOpen = false;
    }
    center(360 * s);
    if (ImGui::BeginPopup("##rename")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        Doc* d = active();
        bool lsp = m_lsp.state() == LspClient::State::Ready && d && d->lang == Lang::Cpp;
        ImGui::TextDisabled(lsp ? "Rename symbol (clangd, all files)" : "Rename in this file only: clangd is not running (whole-word replace, not semantic)");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##rn", &m_renameText, ImGuiInputTextFlags_EnterReturnsTrue) && d && !m_renameText.empty()) {
            if (lsp) {
                lspSync(d);
                m_lsp.rename(d->path, d->buf.cursors[0].pos, m_renameText, [this](std::map<fs::path, std::vector<TextEdit>> edits, std::string err) {
                    if (!err.empty()) {
                        m_app.toast(ToastLevel::Error, "Rename failed: " + err);
                        return;
                    }
                    int files = 0;
                    for (auto& [file, e] : edits) {
                        Doc* od = findDoc(file);
                        if (!od) od = openDoc(file, true);
                        if (!od) continue;
                        applyEdits(od->buf, e);
                        od->lastEdit = now();
                        ++files;
                    }
                    m_app.toast(ToastLevel::Success, std::format("Renamed in {} file{} (unsaved)", files, files == 1 ? "" : "s"));
                });
            } else {
                std::string old = wordAt(d->buf, d->buf.cursors[0].pos);
                auto n = replaceAll(d->buf, old, m_renameText, {false, true, true});
                d->lastEdit = now();
                if (n) m_app.toast(ToastLevel::Info, std::format("Renamed {} occurrence{} in this file", *n, *n == 1 ? "" : "s"));
            }
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------- settings page ---
void CodeWorkspace::drawSettings() {
    float s = m_app.effectiveScale();
    bool changed = false;
    ImGui::SeparatorText("Editor");
    ImGui::SetNextItemWidth(160 * s);
    if (ImGui::SliderFloat("Font size", &settings.fontSize, 8, 32, "%.0f px")) {
        changed = true;
        m_app.m_fontsDirty = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(JetBrains Mono)");
    ImGui::SetNextItemWidth(160 * s);
    changed |= ImGui::SliderInt("Tab size", &settings.tabSize, 1, 8);
    const char* themes[] = {"Follow editor theme", "Dark Modern", "Light Modern"};
    ImGui::SetNextItemWidth(220 * s);
    changed |= ImGui::Combo("Color theme", &settings.theme, themes, 3);
    changed |= ImGui::Checkbox("Word wrap (Alt+Z)", &settings.wordWrap);
    changed |= ImGui::Checkbox("Minimap", &settings.minimap);
    changed |= ImGui::Checkbox("Line numbers", &settings.lineNumbers);
    changed |= ImGui::Checkbox("Indent guides", &settings.indentGuides);
    changed |= ImGui::Checkbox("Auto-close brackets and quotes", &settings.autoClose);
    const char* ws[] = {"None", "Selection", "All"};
    ImGui::SetNextItemWidth(160 * s);
    changed |= ImGui::Combo("Render whitespace", &settings.whitespace, ws, 3);
    ImGui::SeparatorText("Files");
    const char* as[] = {"Off", "After delay (1 s)", "On focus change"};
    ImGui::SetNextItemWidth(220 * s);
    changed |= ImGui::Combo("Auto save", &settings.autoSave, as, 3);
    changed |= ImGui::Checkbox("Format on save (C++: clangd's built-in clang-format; others: clang-format on PATH)", &settings.formatOnSave);
    ImGui::SeparatorText("Language features");
    changed |= ImGui::Checkbox("Use clangd (bundled, or PATH)", &settings.useClangd);
    ImGui::SetNextItemWidth(-1);
    changed |= ImGui::InputTextWithHint("##clangd", "clangd.exe path (empty = bundled resources/clangd, then PATH)", &settings.clangdPath);
    ImGui::TextDisabled("Now: %s", lspStatus().c_str());
    if (ImGui::Button(ICON_ROTATE " Restart language server")) {
        m_lsp.shutdown();
        for (auto& d : m_docs) d->lspOpen = false;
        startLsp();
    }
    ImGui::SeparatorText("Terminal");
    ImGui::SetNextItemWidth(-1);
    changed |= ImGui::InputTextWithHint("##shell", "Shell (empty = powershell.exe -NoLogo, else cmd.exe)", &settings.shell);
    ImGui::SeparatorText("External editor command");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##ext", "code -g \"{file}:{line}\"   (empty = the system default app)", &m_app.m_externalEditor)) m_app.saveSettings();
    ImGui::SeparatorText("Keyboard shortcuts (click to rebind, then press the new keys; Esc cancels)");
    if (ImGui::BeginTable("##keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthFixed, 170 * s);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60 * s);
        for (auto& k : codeKeyDefs()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(k.title);
            ImGui::TableNextColumn();
            ImGui::PushID(k.id);
            bool rec = m_recordingKey == k.id;
            std::string chord = rec ? "press keys..." : CommandRegistry::chordName(settings.keys[k.id]);
            if (ImGui::Button((chord.empty() ? std::string("(none)") : chord).c_str(), {160 * s, 0})) m_recordingKey = k.id;
            if (rec) {
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) m_recordingKey.clear();
                for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
                    ImGuiKey kk = static_cast<ImGuiKey>(key);
                    if (kk == ImGuiKey_Escape || ImGui::IsLRModKey(kk) || kk >= ImGuiKey_GamepadStart) continue;
                    if (ImGui::IsKeyPressed(kk, false)) {
                        ImGuiIO& io = ImGui::GetIO();
                        settings.keys[k.id] = key | (io.KeyCtrl ? ImGuiMod_Ctrl : 0) | (io.KeyShift ? ImGuiMod_Shift : 0) | (io.KeyAlt ? ImGuiMod_Alt : 0);
                        m_recordingKey.clear();
                        changed = true;
                        break;
                    }
                }
            }
            ImGui::TableNextColumn();
            if (settings.keys[k.id] != k.def && ImGui::SmallButton("Reset")) {
                settings.keys[k.id] = k.def;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Fixed: Ctrl+C/X/V copy/cut/paste, Ctrl+A select all, Tab/Shift+Tab indent, Alt+click add cursor, Ctrl+click definition,");
    ImGui::TextDisabled("Ctrl+P quick open (global palette, files included), Ctrl+S save, Ctrl+B build scripts.");
    if (changed) saveSettings();
}

// ============================================================================== debugger ===
bool CodeWorkspace::playExternal() {
    EditorSession& ses = m_app.m_session;
    if (!ses.hasProject()) return false;
    saveAll();
    fs::path module = ses.hasScriptBuild() && ses.lastScriptBuild().ok ? ses.lastScriptBuild().module : fs::path();
    fs::path scene = pathFromUtf8(ses.world().currentScenePath());
    auto l = dbg::preparePlaySession(ses.project(), ses.scene().toJson(), narrow(scene.stem().wstring()), ses.toolsDir, module);
    if (!l) {
        m_app.toast(ToastLevel::Error, l.error());
        return false;
    }
#ifdef _WIN32
    std::wstring cmd = widen(l->commandLine());
    std::wstring dir = l->workingDir.wstring();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        m_app.toast(ToastLevel::Error, "Cannot start fledge_runtime.exe");
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#endif
    m_app.toast(ToastLevel::Info, "Playing the current scene in a separate process");
    return true;
}

bool CodeWorkspace::startDebugging() {
    EditorSession& ses = m_app.m_session;
    if (debugging() || !ses.hasProject()) return false;
    if (dbg::GdbSession::findGdb(ses.scriptSettings.compiler).empty()) {
        m_app.toast(ToastLevel::Error, "gdb not found: install a MinGW-w64 toolchain with gdb (Settings > Scripts > Install C++ toolchain) or set FLEDGE_GDB");
        return false;
    }
    saveAll();
    ScriptBuildOptions opt = ses.scriptBuildOptions();
    opt.debug = true;
    opt.release = false;
    Project project = ses.project();
    m_dbgStatus = "Building scripts (-O0 -g)...";
    m_dbgOutput.clear();
    bottomTab = "breakpoints";
    showBottom = true;
    m_dbgBuild = std::async(std::launch::async, [project, opt] { return buildScripts(project, opt); });
    return true;
}

void CodeWorkspace::launchDebugger(const ScriptBuildResult& build) {
    EditorSession& ses = m_app.m_session;
    if (!build.ok) {
        m_dbgStatus = std::format("Debug build failed ({} errors): see Build Output", build.errors());
        ses.applyScriptBuild(build);  // diagnostics show in the Problems / Build Output views
        bottomTab = "build";
        return;
    }
    fs::path scene = pathFromUtf8(ses.world().currentScenePath());
    auto l = dbg::preparePlaySession(ses.project(), ses.scene().toJson(), narrow(scene.stem().wstring()), ses.toolsDir, build.module);
    if (!l) {
        m_dbgStatus = l.error();
        return;
    }
    auto session = std::make_unique<dbg::GdbSession>();
    std::string err;
    if (!session->start(dbg::GdbSession::findGdb(ses.scriptSettings.compiler), l->runtime, l->args, l->workingDir, &err)) {
        m_dbgStatus = "gdb failed to start: " + err;
        return;
    }
    m_dbg = std::move(session);
    m_dbgBps.clear();
    syncBreakpoints();
    if (!m_dbg->run()) {
        m_dbgStatus = "gdb could not start the game: " + m_dbg->lastError();
        m_dbg.reset();
        return;
    }
    m_dbgStatus = std::format("Running under gdb ({} breakpoint{})", m_dbgBps.size(), m_dbgBps.size() == 1 ? "" : "s");
    m_app.toast(ToastLevel::Info, "Debugging the game in a separate process");
}

void CodeWorkspace::stopDebugging() {
    if (m_dbgBuild.valid()) m_dbgBuild.wait();  // a build in flight finishes (it writes files); nothing is launched
    m_dbgBuild = {};
    if (m_dbg) m_dbg->stop();
    m_dbg.reset();
    m_dbgBps.clear();
    m_dbgStack.clear();
    m_dbgLocals.clear();
    m_dbgFile.clear();
    m_dbgLine = -1;
    if (!m_dbgStatus.empty()) m_dbgStatus = "Debugging stopped";
}

void CodeWorkspace::syncBreakpoints() {
    if (!m_dbg) return;
    std::set<std::pair<std::string, int>> want;
    std::map<std::pair<std::string, int>, fs::path> paths;
    for (auto& d : m_docs)
        if (d->virtualName.empty())
            for (int l : d->breakpoints) {
                auto key = std::make_pair(pathKey(d->path), l);
                want.insert(key);
                paths[key] = d->path;
            }
    for (auto it = m_dbgBps.begin(); it != m_dbgBps.end();)
        if (!want.count(it->first)) {
            m_dbg->removeBreakpoint(it->second);
            it = m_dbgBps.erase(it);
        } else {
            ++it;
        }
    for (auto& key : want)
        if (!m_dbgBps.count(key))
            if (int n = m_dbg->addBreakpoint(paths[key], key.second + 1)) m_dbgBps[key] = n;
}

void CodeWorkspace::debugCommand(const char* what) {
    if (!m_dbg || m_dbg->state() != dbg::GdbSession::State::Stopped) return;
    std::string w = what;
    bool ok = w == "continue" ? m_dbg->continueExec() : w == "next" ? m_dbg->next() : w == "step" ? m_dbg->step() : m_dbg->finish();
    if (ok) {
        m_dbgLine = -1;
        m_dbgStack.clear();
        m_dbgLocals.clear();
        m_dbgStatus = "Running";
    } else {
        m_dbgStatus = "gdb: " + m_dbg->lastError();
    }
}

void CodeWorkspace::updateDebugger() {
    if (m_dbgBuild.valid() && m_dbgBuild.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        ScriptBuildResult r = m_dbgBuild.get();
        m_dbgBuild = {};
        launchDebugger(r);
    }
    if (!m_dbg) return;
    static double lastSync = 0;
    if (now() - lastSync > 0.25) {  // breakpoints toggled while debugging (F9 / gutter) reach gdb
        lastSync = now();
        if (m_dbg->state() == dbg::GdbSession::State::Stopped) syncBreakpoints();
    }
    if (!m_dbg->poll()) return;
    using S = dbg::GdbSession::State;
    switch (m_dbg->state()) {
        case S::Stopped: {
            const dbg::StopInfo& st = m_dbg->lastStop();
            m_dbgStack = m_dbg->stack();
            m_dbgLocals = m_dbg->locals();
            m_dbgFile = pathFromUtf8(st.frame.fullname.empty() ? st.frame.file : st.frame.fullname);
            m_dbgLine = st.frame.line - 1;
            m_dbgStatus = std::format("Paused ({}) at {}:{}", st.reason.empty() ? "interrupted" : st.reason, narrow(m_dbgFile.filename().wstring()),
                                      st.frame.line);
            if (!st.signal.empty()) m_dbgStatus += " - " + st.signal;
            std::error_code ec;
            if (st.frame.line > 0 && fs::is_regular_file(m_dbgFile, ec)) jumpTo(m_dbgFile, {m_dbgLine, 0});
            bottomTab = "breakpoints";
            showBottom = true;
            break;
        }
        case S::Exited:
            m_dbgStatus = std::format("The game exited (code {})", m_dbg->lastStop().exitCode);
            m_dbgOutput = m_dbg->programOutput();
            m_dbg->stop();
            m_dbg.reset();
            m_dbgLine = -1;
            break;
        case S::Failed:
            m_dbgStatus = "gdb stopped unexpectedly: " + m_dbg->lastError();
            m_dbg.reset();
            m_dbgLine = -1;
            break;
        default: break;
    }
}

void CodeWorkspace::drawDebugPanel() {
    float s = m_app.effectiveScale();
    using S = dbg::GdbSession::State;
    bool stopped = m_dbg && m_dbg->state() == S::Stopped;
    if (!debugging()) {
        ImGui::BeginDisabled(!m_app.m_session.hasProject());
        if (ImGui::Button(ICON_BUG " Start debugging")) startDebugging();
        ImGui::EndDisabled();
    } else {
        ImGui::BeginDisabled(!stopped);
        if (ImGui::Button(ICON_PLAY " Continue")) debugCommand("continue");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("F5");
        ImGui::SameLine();
        if (ImGui::Button("Step over")) debugCommand("next");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("F10");
        ImGui::SameLine();
        if (ImGui::Button("Step into")) debugCommand("step");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("F11");
        ImGui::SameLine();
        if (ImGui::Button("Step out")) debugCommand("finish");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Shift+F11");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(ICON_STOP " Stop")) stopDebugging();
    }
    ImGui::SameLine(0, 16 * s);
    ImGui::TextDisabled("%s", m_dbgStatus.empty() ? "Breakpoints pause the game when it runs under the debugger (Ctrl+F5)." : m_dbgStatus.c_str());
    float colW = ImGui::GetContentRegionAvail().x / 3.0f;
    ImGui::BeginChild("##bps", {colW, 0}, ImGuiChildFlags_Borders);
    ImGui::TextDisabled("BREAKPOINTS");
    for (auto& d : m_docs)
        for (int l : std::set<int>(d->breakpoints)) {
            ImGui::PushID(d.get());
            ImGui::PushID(l);
            bool bound = m_dbgBps.count({pathKey(d->path), l}) > 0;
            if (ImGui::SmallButton(ICON_TRASH)) d->breakpoints.erase(l);
            ImGui::SameLine();
            if (ImGui::Selectable(std::format("{} {}:{}{}", ICON_CIRCLE, d->rel, l + 1, m_dbg && !bound ? " (not set)" : "").c_str())) jumpTo(d->path, {l, 0});
            ImGui::PopID();
            ImGui::PopID();
        }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##stack", {colW, 0}, ImGuiChildFlags_Borders);
    ImGui::TextDisabled("CALL STACK");
    for (size_t i = 0; i < m_dbgStack.size(); ++i) {
        const auto& f = m_dbgStack[i];
        ImGui::PushID(static_cast<int>(i));
        std::string where = f.file.empty() ? f.address : std::format("{}:{}", f.file, f.line);
        if (ImGui::Selectable(std::format("{}  {}", f.function.empty() ? "??" : f.function, where).c_str(), i == 0) && !f.fullname.empty())
            jumpTo(pathFromUtf8(f.fullname), {std::max(0, f.line - 1), 0});
        ImGui::PopID();
    }
    if (m_dbgStack.empty()) ImGui::TextDisabled(stopped ? "(no frames)" : "(running)");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##locals", {0, 0}, ImGuiChildFlags_Borders);
    ImGui::TextDisabled("LOCALS");
    if (ImGui::BeginTable("##vars", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        for (auto& v : m_dbgLocals) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(v.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(v.value.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", v.type.c_str());
        }
        ImGui::EndTable();
    }
    if (!m_dbgOutput.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("GAME OUTPUT");
        ImGui::TextUnformatted(m_dbgOutput.c_str());
    }
    ImGui::EndChild();
}

} // namespace fledge::editor

namespace fledge::editor {
void EditorApp::drawCodePanel() {
    PanelDef* pd = panel("Code");
    ImGui::SetNextWindowSize({1100, 700}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Code", pd ? &pd->open : nullptr)) {
        ImGui::End();
        return;
    }
    m_code->draw();
    ImGui::End();
}
} // namespace fledge::editor
