#pragma once
// Code workspace: a VS Code-style editor inside Fledge (activity bar, explorer, search, source control,
// problems, outline, tabbed + split editor groups, minimap, folding, multi-cursor, LSP via clangd or a
// basic symbol index, integrated ConPTY terminals, build output). UI on top of the unit-tested cores in
// fledge/editor/code_model.h and code_services.h. See docs/CODE_EDITOR.md for what is real vs stub.
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include "fledge/core/json.h"
#include "fledge/editor/code_model.h"
#include "fledge/editor/code_services.h"
#include "fledge/editor/debugger.h"
#include "fledge/packager/script_build.h"

namespace fledge::editor {

class EditorApp;

struct CodeSettings {
    float fontSize = 14.0f;  // at 100% UI scale
    int tabSize = 4;
    bool formatOnSave = false;
    int autoSave = 0;  // 0 off, 1 after delay (1 s), 2 on focus change
    int theme = 0;     // 0 follow editor, 1 Dark Modern, 2 Light Modern
    bool wordWrap = false, minimap = true, lineNumbers = true, indentGuides = true, autoClose = true;
    int whitespace = 1;  // 0 none, 1 selection, 2 all
    bool useClangd = true;
    std::string clangdPath;  // empty = bundled resources/clangd, then PATH
    std::string shell;       // empty = PowerShell if present, else cmd.exe
    std::map<std::string, ImGuiKeyChord> keys;  // command id -> chord (defaults filled in)
    Json toJson() const;
    void fromJson(const Json& j);
};

struct CodeKeyDef {
    const char* id;
    const char* title;
    ImGuiKeyChord def;
};
const std::vector<CodeKeyDef>& codeKeyDefs();

class CodeWorkspace {
public:
    explicit CodeWorkspace(EditorApp& app);
    ~CodeWorkspace();

    void draw();          // the "Code" panel
    bool unsavedPromptOpen() const { return m_pendingClose != nullptr; }
    void update();        // per frame (LSP poll, terminals, auto-save), also when hidden
    bool focused() const { return m_focused && m_drawnFrame >= ImGui::GetFrameCount() - 1; }  // stale once the panel is hidden
    bool saveActive();    // Ctrl+S
    void saveAll();
    bool anyDirty() const;
    void onProjectChanged();  // project opened / closed: persist + reload session
    void openFile(const fs::path& file, int line = 0, int col = 0, bool pin = true);
    void openQuickOpen() { m_quickOpen = true; m_quickQuery.clear(); }
    std::vector<std::string> projectFiles();  // relative to the project root ('/'), for the command palette
    fs::path root() const { return m_root; }
    void drawSettings();  // Settings > Code Editor page
    void saveSettings();
    void loadSettings();
    CodeSettings settings;
    ImFont* font = nullptr;  // JetBrains Mono (set by the theme code)
    // Code editor state and controls
    std::string activeText() const;
    std::string activeRel() const;
    code::TextBuffer* activeBuffer();
    int terminalCount() const { return static_cast<int>(m_terms.size()); }
    std::string terminalText(int i) const;
    void newTerminal();
    std::string lspStatus() const;
    std::string activity = "explorer";  // explorer | search | scm | problems | outline
    std::string bottomTab = "terminal";  // problems | output | build | terminal | references | breakpoints
    bool showBottom = true;
    void setFind(const std::string& q) {
        m_findOpen = true;
        m_findQuery = q;
    }
    std::vector<std::pair<int, int>> findMatchesActive();

    // Debugging (docs/CODE_EDITOR.md "Debugging"): out-of-process play under gdb. startDebugging() builds the scripts
    // with -O0 -g in the background, writes the current scene to Intermediate/PlaySession, then runs fledge_runtime
    // under gdb with every breakpoint of the open files. playExternal() is the same without gdb.
    bool startDebugging();
    void stopDebugging();
    bool playExternal();
    bool debugging() const { return m_dbg != nullptr || m_dbgBuild.valid(); }
    std::string debugStatus() const { return m_dbgStatus; }

private:
    struct Doc {
        fs::path path;
        std::string rel;
        code::TextBuffer buf;
        code::Lang lang = code::Lang::Plain;
        bool pinned = false, preview = false, readOnly = false;
        std::string virtualName;  // diff views etc.
        std::set<int> folded;  // start lines
        std::vector<code::FoldRange> folds;
        uint64_t foldsVersion = ~0ull, lspSent = 0, outlineVersion = ~0ull;
        std::vector<code::Symbol> symbols;
        float scrollY = 0, scrollX = 0;
        bool reveal = false;   // scroll the caret into view next frame
        bool lspOpen = false;
        double lastEdit = 0;
        std::set<int> breakpoints;
        std::vector<int> lineStates;  // tokenizer state at each line start
        uint64_t statesVersion = ~0ull;
        bool crlf = false;            // file used CRLF: saved back with CRLF
        fs::file_time_type mtime{};
    };
    struct Group {
        std::vector<Doc*> tabs;
        Doc* active = nullptr;
    };
    struct Row {
        int line, start, end;  // byte range of a visual row
    };
    struct Completion {
        bool open = false;
        std::vector<code::CompletionItem> items;
        int sel = 0;
        code::Pos start;  // word start
        bool fromLsp = false;
        double requested = 0;
    };

    // documents
    Doc* findDoc(const fs::path& p);
    Doc* openDoc(const fs::path& p, bool pin);
    void closeDoc(Doc* d, int group);
    // Closing a tab with unsaved changes asks Save / Don't Save / Cancel (modal, drawn by drawUnsavedPrompt).
    void requestClose(Doc* d, int group);
    void drawUnsavedPrompt();
    Doc* m_pendingClose = nullptr;
    int m_pendingCloseGroup = 0;
    bool saveDoc(Doc* d);
    void reloadIfChanged();
    Doc* active() { return m_groups[m_activeGroup].active; }
    const Doc* active() const { return m_groups[m_activeGroup].active; }
    void activate(Doc* d, int group);
    void lspSync(Doc* d);
    void jumpTo(const fs::path& file, code::Pos p);

    // ui pieces
    void drawActivityBar(float w);
    void drawSidebar();
    void drawExplorer();
    void drawExplorerDir(const fs::path& dir, int depth);
    void drawSearch();
    void drawScm();
    void drawProblems(bool compact);
    void drawOutline();
    void drawGroup(int g, ImVec2 size);
    void drawTabs(int g);
    void drawBreadcrumbs(Doc* d);
    void drawEditor(Doc* d, int g, ImVec2 size);
    void drawFindWidget(Doc* d, ImVec2 origin, float width);
    void drawBottom();
    void drawTerminal(int i);
    void drawStatus();
    void drawPopups();
    void handleKeys(Doc* d);
    void typeText(Doc* d, const std::string& s);
    void triggerCompletion(Doc* d, bool explicitly);
    void acceptCompletion(Doc* d);
    void goToDefinition(Doc* d);
    void findReferences(Doc* d);
    void startRename(Doc* d);
    void formatDoc(Doc* d, std::function<void()> then = {});
    std::vector<Row> layoutRows(Doc* d, int wrapCols);
    float colX(const std::string& line, int col) const;
    int xToCol(const std::string& line, float x) const;
    ImU32 tokColor(code::Tok t) const;
    bool light() const;
    std::string commentPrefix(code::Lang l) const;
    bool keyPressed(const char* id) const;

    // services
    void startLsp();
    void indexProject();
    std::vector<code::Diagnostic> diagnosticsFor(const fs::path& file);

    // persistence
    void saveSession();
    void loadSession();

    EditorApp& m_app;
    fs::path m_root;
    std::vector<std::unique_ptr<Doc>> m_docs;
    Group m_groups[2];
    int m_activeGroup = 0;
    bool m_split = false, m_focused = false, m_editorFocused = false;
    float m_sidebarW = 250, m_bottomH = 210, m_charW = 8, m_lineH = 16, m_fontPx = 14;
    std::set<std::string> m_expanded;
    // search
    std::string m_searchQuery, m_replaceQuery;
    code::SearchOptions m_searchOpt;
    struct SearchHit {
        std::string rel;
        int line, col, len;
        std::string text;
    };
    std::vector<SearchHit> m_hits;
    std::string m_searchError;
    // find widget
    bool m_findOpen = false, m_findReplace = false, m_findFocus = false;
    std::string m_findQuery, m_findRepl;
    code::SearchOptions m_findOpt;
    int m_findIndex = 0;
    // scm
    code::GitStatus m_git;
    double m_gitTime = -100;
    std::string m_commitMsg;
    // popups
    bool m_quickOpen = false, m_gotoLine = false, m_gotoSymbol = false, m_renameOpen = false;
    std::string m_quickQuery, m_gotoQuery, m_renameText;
    int m_quickSel = 0;
    Completion m_comp;
    std::optional<code::SignatureInfo> m_sig;
    std::string m_hoverText;
    code::Pos m_hoverPos{-1, -1};
    double m_hoverStart = 0;
    bool m_hoverRequested = false;
    std::vector<code::Location> m_refs;
    std::string m_refsTitle;
    // services
    code::LspClient m_lsp;
    bool m_lspTried = false;
    code::SymbolIndex m_index;
    double m_indexTime = -100;
    std::vector<std::unique_ptr<code::Terminal>> m_terms;
    int m_activeTerm = 0;
    std::string m_output;  // "Output" tab log
    std::vector<std::string> m_fileCache;
    double m_fileCacheTime = -100;
    std::string m_recordingKey;  // keybinding being recorded
    double m_lastAutoSave = 0;
    bool m_wasFocused = false, m_termAutoStarted = false;
    int m_drawnFrame = -10;
    bool m_dragSelecting = false;
    int m_dragGroup = -1;
    // debugger
    void updateDebugger();
    void syncBreakpoints();
    void launchDebugger(const ScriptBuildResult& build);
    void drawDebugPanel();
    void debugCommand(const char* what);  // continue | next | step | finish
    std::unique_ptr<dbg::GdbSession> m_dbg;
    std::future<ScriptBuildResult> m_dbgBuild;
    std::map<std::pair<std::string, int>, int> m_dbgBps;  // (generic path, 0-based line) -> gdb breakpoint number
    std::vector<dbg::Frame> m_dbgStack;
    std::vector<dbg::Variable> m_dbgLocals;
    fs::path m_dbgFile;  // stopped location
    int m_dbgLine = -1;  // 0-based, -1 = not stopped
    std::string m_dbgStatus, m_dbgOutput;
};

} // namespace fledge::editor
