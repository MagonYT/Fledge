#pragma once
// Out-of-process services of the Code workspace, all UI-free and unit-tested:
//   - LSP JSON-RPC framing + an LSP client (clangd), testable against a mock writer/feed
//   - child processes with pipes (clangd, git, clang-format) and one-shot capture
//   - a small VT/ANSI screen emulator and a ConPTY-backed integrated terminal
//   - git status/diff/commit helpers (Source Control view; hidden when git is not installed)
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/utility.h"
#include "fledge/editor/code_model.h"

namespace fledge::code {

// ---------------------------------------------------------------------------- processes ---
fs::path findOnPath(const std::string& exeName);  // "" if not found

struct CaptureResult {
    int exitCode = -1;
    std::string output;  // stdout + stderr
    bool timedOut = false;
};
// Runs a command line (UTF-8), waits up to timeoutMs. stdinText is written to the child's stdin.
CaptureResult runCapture(const std::string& commandLine, const fs::path& cwd, int timeoutMs = 15000, const std::string& stdinText = {});
std::string quoteArg(const std::string& s);

// Long-running child with stdin/stdout pipes; a reader thread buffers stdout.
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    bool start(const std::string& commandLine, const fs::path& cwd, std::string* error = nullptr);
    bool write(std::string_view data);
    std::string read();  // drains what arrived so far
    bool running() const;
    void stop();

private:
    void* m_process = nullptr;
    void* m_stdinWrite = nullptr;
    void* m_stdoutRead = nullptr;
    std::thread m_reader;
    std::mutex m_mutex;
    std::string m_buffer;
    std::atomic<bool> m_exited{false};
};

// ---------------------------------------------------------------------------- LSP ---
std::string lspFrame(const Json& message);  // "Content-Length: N\r\n\r\n<json>"

class LspDecoder {
public:
    void feed(std::string_view bytes);
    std::optional<Json> next();  // one complete message, if any
    bool error() const { return m_error; }

private:
    std::string m_buf;
    bool m_error = false;
};

std::string pathToUri(const fs::path& p);
fs::path uriToPath(const std::string& uri);

struct Diagnostic {
    Pos start, end;
    int severity = 1;  // 1 error, 2 warning, 3 info, 4 hint
    std::string message, source;
};
struct Location {
    fs::path file;
    Pos start, end;
};
struct TextEdit {
    Pos start, end;
    std::string newText;
};
struct CompletionItem {
    std::string label, detail, insertText;
    int kind = 0;
};
struct SignatureInfo {
    std::string label;
    int activeParam = -1;
    int paramStart = -1, paramEnd = -1;  // byte range of the active parameter in label
};

class LspClient {
public:
    using Writer = std::function<void(const std::string&)>;
    enum class State { Off, Starting, Ready, Failed };

    ~LspClient();
    // Real transport: spawns the server (e.g. clangd) and sends `initialize`.
    bool startProcess(const std::string& commandLine, const fs::path& root);
    // Test transport: bytes the client sends go to `writer`; incoming bytes come via feed().
    void attach(Writer writer, const fs::path& root);
    void feed(std::string_view bytes);
    void poll();  // pumps the process' stdout (real transport) and dispatches messages
    void shutdown();

    State state() const { return m_state; }
    const std::string& status() const { return m_status; }
    const std::string& serverName() const { return m_serverName; }

    // documents (positions are UTF-8 byte offsets: the client negotiates positionEncoding utf-8)
    void didOpen(const fs::path& file, const std::string& text, const std::string& languageId);
    void didChange(const fs::path& file, const std::string& text);
    void didSave(const fs::path& file);
    void didClose(const fs::path& file);

    void hover(const fs::path& f, Pos p, std::function<void(std::string)> cb);
    void definition(const fs::path& f, Pos p, std::function<void(std::vector<Location>)> cb);
    void references(const fs::path& f, Pos p, std::function<void(std::vector<Location>)> cb);
    void rename(const fs::path& f, Pos p, const std::string& newName, std::function<void(std::map<fs::path, std::vector<TextEdit>>, std::string error)> cb);
    void signatureHelp(const fs::path& f, Pos p, std::function<void(std::optional<SignatureInfo>)> cb);
    void completion(const fs::path& f, Pos p, std::function<void(std::vector<CompletionItem>)> cb);
    void formatting(const fs::path& f, int tabSize, std::function<void(std::vector<TextEdit>)> cb);

    const std::map<fs::path, std::vector<Diagnostic>>& diagnostics() const { return m_diags; }
    uint64_t diagnosticsVersion() const { return m_diagVersion; }

    // low level
    int request(const std::string& method, Json params, std::function<void(const Json& result, const std::string& error)> cb);
    void notify(const std::string& method, Json params);

private:
    void send(const Json& msg);
    void dispatch(const Json& msg);
    void initialize();
    Json docPos(const fs::path& f, Pos p) const;
    Writer m_writer;
    std::unique_ptr<ChildProcess> m_proc;
    LspDecoder m_decoder;
    fs::path m_root;
    State m_state = State::Off;
    std::string m_status = "off", m_serverName;
    int m_nextId = 1;
    std::map<int, std::function<void(const Json&, const std::string&)>> m_pending;
    std::map<fs::path, int> m_docVersions;
    std::map<fs::path, std::vector<Diagnostic>> m_diags;
    uint64_t m_diagVersion = 0;
    std::vector<Json> m_queued;  // document notifications sent before `initialized`
};

Pos lspPos(const Json& j);  // {"line","character"} -> Pos
std::vector<TextEdit> parseTextEdits(const Json& arr);
// Applies edits (any order) to a buffer as a single undo step.
void applyEdits(TextBuffer& b, std::vector<TextEdit> edits);

// ---------------------------------------------------------------------------- terminal ---
// VT100/xterm subset emulator: what ConPTY emits (cursor positioning, erase, SGR colours incl. 256/truecolor,
// scroll) plus plain-pipe output. Lines that scroll off the top go to the scrollback.
class AnsiScreen {
public:
    struct Cell {
        char32_t ch = U' ';
        uint32_t fg = 0, bg = 0;  // 0 = default; else 0xRRGGBB | 0x1000000
        bool bold = false;
    };
    using Row = std::vector<Cell>;

    AnsiScreen(int cols = 120, int rows = 30);
    void resize(int cols, int rows);
    void feed(std::string_view bytes);
    void clear();

    int cols() const { return m_cols; }
    int rows() const { return m_rows; }
    const std::vector<Row>& scrollback() const { return m_scrollback; }
    const std::vector<Row>& screen() const { return m_screen; }
    int cursorRow() const { return m_cy; }
    int cursorCol() const { return m_cx; }
    std::string title;
    // Plain-text dump (scrollback + screen, trailing spaces trimmed) for tests / copy.
    std::string text() const;
    static std::string rowText(const Row& r);
    size_t maxScrollback = 5000;

private:
    void put(char32_t c);
    void lineFeed();
    void csi(char final, const std::string& params);
    void sgr(const std::vector<int>& p);
    Cell blank() const;
    int m_cols, m_rows, m_cx = 0, m_cy = 0;
    std::vector<Row> m_screen, m_scrollback;
    Cell m_attr;
    bool m_wrapPending = false;
    int m_savedX = 0, m_savedY = 0;
    // parser state
    enum class St { Ground, Esc, Csi, Osc, OscEsc, Charset } m_st = St::Ground;
    std::string m_seq;
    uint32_t m_utf8 = 0;
    int m_utf8Left = 0;
};

// Integrated terminal: a shell on a Windows pseudo console (ConPTY, Windows 10 1809+).
class Terminal {
public:
    ~Terminal();
    bool start(const std::string& commandLine, const fs::path& cwd, int cols, int rows, std::string* error = nullptr);
    void write(std::string_view data);
    void resize(int cols, int rows);
    void poll();  // moves pending output into screen
    bool running() const;
    void stop();
    AnsiScreen screen;
    std::string name = "terminal";

private:
    void* m_pc = nullptr;  // HPCON
    void* m_process = nullptr;
    void* m_inWrite = nullptr;
    void* m_outRead = nullptr;
    std::thread m_reader;
    std::mutex m_mutex;
    std::string m_pending;
    std::atomic<bool> m_exited{false};
};

// ---------------------------------------------------------------------------- git ---
struct GitFile {
    std::string path;  // repo-relative, '/'
    char index = ' ', work = ' ';  // porcelain v1 XY
};
struct GitStatus {
    bool available = false;  // git.exe found
    bool isRepo = false;
    std::string branch;
    std::vector<GitFile> files;
    std::string error;
};
bool gitAvailable();
GitStatus parseGitPorcelain(const std::string& output);  // `git status --porcelain=v1 -b`
GitStatus gitStatus(const fs::path& root);
std::string gitDiff(const fs::path& root, const std::string& relPath);
CaptureResult gitRun(const fs::path& root, const std::string& args);

} // namespace fledge::code
