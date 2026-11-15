#include "fledge/editor/code_services.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "fledge/core/log.h"

namespace fledge::code {

#ifdef _WIN32
namespace {
// ConPTY entry points are resolved at run time (older Windows / SDK headers built for an older _WIN32_WINNT).
using PcHandle = void*;
using CreatePcFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, PcHandle*);
using ResizePcFn = HRESULT(WINAPI*)(PcHandle, COORD);
using ClosePcFn = void(WINAPI*)(PcHandle);
constexpr DWORD_PTR kAttrPseudoConsole = 0x00020016;  // PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
struct ConPtyApi {
    CreatePcFn create = nullptr;
    ResizePcFn resize = nullptr;
    ClosePcFn close = nullptr;
};
const ConPtyApi& conpty() {
    static ConPtyApi api = [] {
        ConPtyApi a;
        if (HMODULE k = GetModuleHandleW(L"kernel32.dll")) {
            a.create = reinterpret_cast<CreatePcFn>(reinterpret_cast<void*>(GetProcAddress(k, "CreatePseudoConsole")));
            a.resize = reinterpret_cast<ResizePcFn>(reinterpret_cast<void*>(GetProcAddress(k, "ResizePseudoConsole")));
            a.close = reinterpret_cast<ClosePcFn>(reinterpret_cast<void*>(GetProcAddress(k, "ClosePseudoConsole")));
        }
        return a;
    }();
    return api;
}
} // namespace
#endif

// ============================================================================== processes ===
fs::path findOnPath(const std::string& exeName) {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 2];
    DWORD n = SearchPathW(nullptr, widen(exeName).c_str(), nullptr, static_cast<DWORD>(std::size(buf)), buf, nullptr);
    if (n > 0 && n < std::size(buf)) return fs::path(std::wstring(buf, n));
#endif
    (void)exeName;
    return {};
}

std::string quoteArg(const std::string& s) {
    if (!s.empty() && s.find_first_of(" \t\"") == std::string::npos) return s;
    std::string out = "\"";
    int bs = 0;
    for (char c : s) {
        if (c == '\\') {
            ++bs;
            continue;
        }
        if (c == '"') out.append(static_cast<size_t>(bs * 2 + 1), '\\');
        else out.append(static_cast<size_t>(bs), '\\');
        bs = 0;
        out += c;
    }
    out.append(static_cast<size_t>(bs * 2), '\\');
    return out + "\"";
}

#ifdef _WIN32
namespace {
bool spawn(const std::string& commandLine, const fs::path& cwd, HANDLE in, HANDLE out, PROCESS_INFORMATION& pi, std::string* error) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = out;
    si.hStdError = out;
    std::wstring cmd = widen(commandLine);
    std::wstring dir = cwd.empty() ? std::wstring() : cwd.wstring();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        if (error) *error = "cannot start: " + commandLine + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}
} // namespace

CaptureResult runCapture(const std::string& commandLine, const fs::path& cwd, int timeoutMs, const std::string& stdinText) {
    CaptureResult r;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outR, outW, inR, inW;
    if (!CreatePipe(&outR, &outW, &sa, 0)) return r;
    if (!CreatePipe(&inR, &inW, &sa, 0)) {
        CloseHandle(outR);
        CloseHandle(outW);
        return r;
    }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    PROCESS_INFORMATION pi{};
    std::string err;
    bool ok = spawn(commandLine, cwd, inR, outW, pi, &err);
    CloseHandle(outW);
    CloseHandle(inR);
    if (!ok) {
        CloseHandle(outR);
        CloseHandle(inW);
        r.output = err;
        return r;
    }
    std::thread writer([inW, stdinText] {
        DWORD w = 0;
        if (!stdinText.empty()) WriteFile(inW, stdinText.data(), static_cast<DWORD>(stdinText.size()), &w, nullptr);
        CloseHandle(inW);
    });
    std::thread reader([outR, &r] {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(outR, buf, sizeof(buf), &n, nullptr) && n > 0) r.output.append(buf, n);
    });
    if (WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeoutMs)) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        r.timedOut = true;
        WaitForSingleObject(pi.hProcess, 2000);
    }
    writer.join();
    reader.join();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exitCode = static_cast<int>(code);
    CloseHandle(outR);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
}

ChildProcess::~ChildProcess() { stop(); }

bool ChildProcess::start(const std::string& commandLine, const fs::path& cwd, std::string* error) {
    stop();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outR, outW, inR, inW;
    if (!CreatePipe(&outR, &outW, &sa, 0)) return false;
    if (!CreatePipe(&inR, &inW, &sa, 0)) {
        CloseHandle(outR);
        CloseHandle(outW);
        return false;
    }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    // stderr goes to NUL for servers: clangd logs heavily there and would interleave with the protocol.
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = nul;
    std::wstring cmd = widen(commandLine);
    std::wstring dir = cwd.wstring();
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    DWORD le = GetLastError();
    CloseHandle(outW);
    CloseHandle(inR);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(outR);
        CloseHandle(inW);
        if (error) *error = "cannot start " + commandLine + " (error " + std::to_string(le) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    m_process = pi.hProcess;
    m_stdinWrite = inW;
    m_stdoutRead = outR;
    m_exited = false;
    m_reader = std::thread([this, outR] {
        char buf[8192];
        DWORD n = 0;
        while (ReadFile(outR, buf, sizeof(buf), &n, nullptr) && n > 0) {
            std::lock_guard l(m_mutex);
            m_buffer.append(buf, n);
        }
        m_exited = true;
    });
    return true;
}

bool ChildProcess::write(std::string_view data) {
    if (!m_stdinWrite) return false;
    DWORD w = 0;
    return WriteFile(static_cast<HANDLE>(m_stdinWrite), data.data(), static_cast<DWORD>(data.size()), &w, nullptr) && w == data.size();
}

std::string ChildProcess::read() {
    std::lock_guard l(m_mutex);
    std::string s;
    s.swap(m_buffer);
    return s;
}

bool ChildProcess::running() const { return m_process && !m_exited && WaitForSingleObject(static_cast<HANDLE>(m_process), 0) == WAIT_TIMEOUT; }

void ChildProcess::stop() {
    if (!m_process) return;
    if (m_stdinWrite) CloseHandle(static_cast<HANDLE>(m_stdinWrite));
    m_stdinWrite = nullptr;
    if (WaitForSingleObject(static_cast<HANDLE>(m_process), 500) == WAIT_TIMEOUT) TerminateProcess(static_cast<HANDLE>(m_process), 1);
    WaitForSingleObject(static_cast<HANDLE>(m_process), 2000);
    if (m_reader.joinable()) m_reader.join();
    CloseHandle(static_cast<HANDLE>(m_stdoutRead));
    CloseHandle(static_cast<HANDLE>(m_process));
    m_process = m_stdoutRead = nullptr;
}
#else
CaptureResult runCapture(const std::string&, const fs::path&, int, const std::string&) { return {}; }
ChildProcess::~ChildProcess() = default;
bool ChildProcess::start(const std::string&, const fs::path&, std::string* error) {
    if (error) *error = "not supported on this platform";
    return false;
}
bool ChildProcess::write(std::string_view) { return false; }
std::string ChildProcess::read() { return {}; }
bool ChildProcess::running() const { return false; }
void ChildProcess::stop() {}
#endif

// ============================================================================== LSP framing ===
std::string lspFrame(const Json& message) {
    std::string body = message.dump(-1, ' ', false, Json::error_handler_t::replace);
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

void LspDecoder::feed(std::string_view bytes) { m_buf.append(bytes); }

std::optional<Json> LspDecoder::next() {
    while (true) {
        size_t hdrEnd = m_buf.find("\r\n\r\n");
        if (hdrEnd == std::string::npos) return std::nullopt;
        size_t len = std::string::npos;
        std::string headers = toLower(m_buf.substr(0, hdrEnd));
        size_t p = headers.find("content-length:");
        if (p != std::string::npos) len = std::strtoull(headers.c_str() + p + 15, nullptr, 10);
        if (len == std::string::npos) {  // garbage before a header: resync after it
            m_error = true;
            m_buf.erase(0, hdrEnd + 4);
            continue;
        }
        if (m_buf.size() < hdrEnd + 4 + len) return std::nullopt;
        std::string body = m_buf.substr(hdrEnd + 4, len);
        m_buf.erase(0, hdrEnd + 4 + len);
        Json j = Json::parse(body, nullptr, false);
        if (j.is_discarded()) {
            m_error = true;
            continue;
        }
        return j;
    }
}

std::string pathToUri(const fs::path& p) {
    std::string s = narrow(fs::absolute(p).lexically_normal().generic_wstring());
    if (s.size() > 1 && s[1] == ':') s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    std::string out = "file:///";
    for (unsigned char c : s) {
        if (std::isalnum(c) || std::strchr("/-_.~:", c)) out += static_cast<char>(c);
                else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

fs::path uriToPath(const std::string& uri) {
    std::string s = uri;
    if (startsWith(s, "file:///")) s = s.substr(8);
    else if (startsWith(s, "file://")) s = s.substr(7);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    if (out.size() > 1 && out[1] == ':') out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return pathFromUtf8(out).lexically_normal();
}

Pos lspPos(const Json& j) { return {j.value("line", 0), j.value("character", 0)}; }

std::vector<TextEdit> parseTextEdits(const Json& arr) {
    std::vector<TextEdit> out;
    if (!arr.is_array()) return out;
    for (auto& e : arr) {
        if (!e.contains("range")) continue;
        out.push_back({lspPos(e["range"]["start"]), lspPos(e["range"]["end"]), e.value("newText", "")});
    }
    return out;
}

void applyEdits(TextBuffer& b, std::vector<TextEdit> edits) {
    if (edits.empty()) return;
    std::sort(edits.begin(), edits.end(), [](const TextEdit& x, const TextEdit& y) { return y.start < x.start; });  // back to front
    // Build the new text in one go so the whole format/rename is one undo step.
    std::string text = b.text();
    auto offset = [&](Pos p) {
        p = b.clamp(p);
        size_t o = 0;
        for (int l = 0; l < p.line; ++l) o += b.line(l).size() + 1;
        return o + static_cast<size_t>(p.col);
    };
    Pos keep = b.cursors.empty() ? Pos{} : b.cursors[0].pos;
    for (auto& e : edits) {
        size_t a = offset(e.start), z = offset(e.end);
        text.replace(a, z - a, e.newText);
    }
    Pos endPos{b.lineCount() - 1, static_cast<int>(b.line(b.lineCount() - 1).size())};
    b.replaceRange({0, 0}, endPos, text);
    b.setCursor(b.clamp(keep));
}

// ============================================================================== LSP client ===
LspClient::~LspClient() { shutdown(); }

bool LspClient::startProcess(const std::string& commandLine, const fs::path& root) {
    m_proc = std::make_unique<ChildProcess>();
    std::string err;
    if (!m_proc->start(commandLine, root, &err)) {
        m_state = State::Failed;
        m_status = err;
        m_proc.reset();
        return false;
    }
    m_root = root;
    m_writer = [this](const std::string& s) {
        if (m_proc) m_proc->write(s);
    };
    initialize();
    return true;
}

void LspClient::attach(Writer writer, const fs::path& root) {
    m_writer = std::move(writer);
    m_root = root;
    initialize();
}

void LspClient::initialize() {
    m_state = State::Starting;
    m_status = "starting";
    Json caps = {
        {"general", {{"positionEncodings", {"utf-8"}}}},
        {"textDocument",
         {{"publishDiagnostics", {{"relatedInformation", false}}},
          {"hover", {{"contentFormat", {"plaintext", "markdown"}}}},
          {"completion", {{"completionItem", {{"snippetSupport", false}}}}},
          {"signatureHelp", {{"signatureInformation", {{"parameterInformation", {{"labelOffsetSupport", true}}}}}}},
          {"synchronization", {{"didSave", true}}}}},
        {"offsetEncoding", {"utf-8"}},  // clangd extension (older servers)
    };
    request("initialize",
            {{"processId", nullptr}, {"rootUri", pathToUri(m_root)}, {"capabilities", caps}, {"clientInfo", {{"name", "Fledge"}}}},
            [this](const Json& result, const std::string& error) {
                if (!error.empty()) {
                    m_state = State::Failed;
                    m_status = "initialize failed: " + error;
                    return;
                }
                if (result.contains("serverInfo")) m_serverName = result["serverInfo"].value("name", "") + " " + result["serverInfo"].value("version", "");
                m_state = State::Ready;
                m_status = "ready";
                notify("initialized", Json::object());
                auto q = std::move(m_queued);
                m_queued.clear();
                for (auto& m : q) send(m);
            });
}

void LspClient::send(const Json& msg) {
    if (!m_writer) return;
    if (m_state == State::Starting && msg.value("method", "") != "initialize" && msg.value("method", "") != "initialized" && !msg.contains("id")) {
        m_queued.push_back(msg);  // didOpen/didChange before the handshake finished
        return;
    }
    m_writer(lspFrame(msg));
}

int LspClient::request(const std::string& method, Json params, std::function<void(const Json&, const std::string&)> cb) {
    int id = m_nextId++;
    if (cb) m_pending[id] = std::move(cb);
    Json msg = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
    if (m_state == State::Starting && method != "initialize") {
        m_queued.push_back(msg);
        return id;
    }
    if (m_writer) m_writer(lspFrame(msg));
    return id;
}

void LspClient::notify(const std::string& method, Json params) { send({{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}}); }

void LspClient::feed(std::string_view bytes) {
    m_decoder.feed(bytes);
    while (auto m = m_decoder.next()) dispatch(*m);
}

void LspClient::poll() {
    if (!m_proc) return;
    std::string s = m_proc->read();
    if (!s.empty()) feed(s);
    if (!m_proc->running() && m_state != State::Failed) {
        m_state = State::Failed;
        m_status = "language server exited";
    }
}

void LspClient::shutdown() {
    if (m_proc && m_proc->running() && m_state == State::Ready) {
        m_writer(lspFrame({{"jsonrpc", "2.0"}, {"id", m_nextId++}, {"method", "shutdown"}, {"params", nullptr}}));
        m_writer(lspFrame({{"jsonrpc", "2.0"}, {"method", "exit"}}));
    }
    if (m_proc) m_proc->stop();
    m_proc.reset();
    m_pending.clear();
    m_state = State::Off;
}

void LspClient::dispatch(const Json& msg) {
    if (msg.contains("id") && (msg.contains("result") || msg.contains("error")) && !msg.contains("method")) {
        int id = msg["id"].is_number() ? msg["id"].get<int>() : -1;
        auto it = m_pending.find(id);
        if (it == m_pending.end()) return;
        auto cb = std::move(it->second);
        m_pending.erase(it);
        std::string err = msg.contains("error") ? msg["error"].value("message", "error") : "";
        cb(msg.contains("result") ? msg["result"] : Json(), err);
        return;
    }
    std::string method = msg.value("method", "");
    if (method == "textDocument/publishDiagnostics") {
        const Json& p = msg["params"];
        std::vector<Diagnostic> ds;
        for (auto& d : p.value("diagnostics", Json::array()))
            ds.push_back({lspPos(d["range"]["start"]), lspPos(d["range"]["end"]), d.value("severity", 1), d.value("message", ""), d.value("source", "")});
        m_diags[uriToPath(p.value("uri", ""))] = std::move(ds);
        ++m_diagVersion;
    } else if (msg.contains("id") && !method.empty()) {
        // server -> client requests (workDoneProgress/create, configuration, ...): answer null
        m_writer(lspFrame({{"jsonrpc", "2.0"}, {"id", msg["id"]}, {"result", nullptr}}));
    }
}

Json LspClient::docPos(const fs::path& f, Pos p) const {
    return {{"textDocument", {{"uri", pathToUri(f)}}}, {"position", {{"line", p.line}, {"character", p.col}}}};
}

void LspClient::didOpen(const fs::path& file, const std::string& text, const std::string& languageId) {
    m_docVersions[file] = 1;
    notify("textDocument/didOpen", {{"textDocument", {{"uri", pathToUri(file)}, {"languageId", languageId}, {"version", 1}, {"text", text}}}});
}
void LspClient::didChange(const fs::path& file, const std::string& text) {
    int v = ++m_docVersions[file];
    notify("textDocument/didChange", {{"textDocument", {{"uri", pathToUri(file)}, {"version", v}}}, {"contentChanges", Json::array({{{"text", text}}})}});
}
void LspClient::didSave(const fs::path& file) { notify("textDocument/didSave", {{"textDocument", {{"uri", pathToUri(file)}}}}); }
void LspClient::didClose(const fs::path& file) {
    m_docVersions.erase(file);
    notify("textDocument/didClose", {{"textDocument", {{"uri", pathToUri(file)}}}});
}

void LspClient::hover(const fs::path& f, Pos p, std::function<void(std::string)> cb) {
    request("textDocument/hover", docPos(f, p), [cb](const Json& r, const std::string&) {
        std::string s;
        if (r.is_object() && r.contains("contents")) {
            const Json& c = r["contents"];
            if (c.is_string()) s = c.get<std::string>();
            else if (c.is_object()) s = c.value("value", "");
            else if (c.is_array())
                for (auto& x : c) s += (x.is_string() ? x.get<std::string>() : x.value("value", "")) + "\n";
        }
        cb(s);
    });
}

namespace {
std::vector<Location> parseLocations(const Json& r) {
    std::vector<Location> out;
    auto one = [&](const Json& l) {
        if (l.contains("uri")) out.push_back({uriToPath(l["uri"]), lspPos(l["range"]["start"]), lspPos(l["range"]["end"])});
        else if (l.contains("targetUri")) out.push_back({uriToPath(l["targetUri"]), lspPos(l["targetSelectionRange"]["start"]), lspPos(l["targetSelectionRange"]["end"])});
    };
    if (r.is_array())
        for (auto& l : r) one(l);
    else if (r.is_object()) one(r);
    return out;
}
} // namespace

void LspClient::definition(const fs::path& f, Pos p, std::function<void(std::vector<Location>)> cb) {
    request("textDocument/definition", docPos(f, p), [cb](const Json& r, const std::string&) { cb(parseLocations(r)); });
}
void LspClient::references(const fs::path& f, Pos p, std::function<void(std::vector<Location>)> cb) {
    Json params = docPos(f, p);
    params["context"] = {{"includeDeclaration", true}};
    request("textDocument/references", params, [cb](const Json& r, const std::string&) { cb(parseLocations(r)); });
}
void LspClient::rename(const fs::path& f, Pos p, const std::string& newName, std::function<void(std::map<fs::path, std::vector<TextEdit>>, std::string)> cb) {
    Json params = docPos(f, p);
    params["newName"] = newName;
    request("textDocument/rename", params, [cb](const Json& r, const std::string& err) {
        std::map<fs::path, std::vector<TextEdit>> out;
        if (r.is_object()) {
            if (r.contains("changes"))
                for (auto& [uri, edits] : r["changes"].items()) out[uriToPath(uri)] = parseTextEdits(edits);
            if (r.contains("documentChanges"))
                for (auto& dc : r["documentChanges"])
                    if (dc.contains("textDocument")) out[uriToPath(dc["textDocument"].value("uri", ""))] = parseTextEdits(dc["edits"]);
        }
        cb(std::move(out), err);
    });
}
void LspClient::signatureHelp(const fs::path& f, Pos p, std::function<void(std::optional<SignatureInfo>)> cb) {
    request("textDocument/signatureHelp", docPos(f, p), [cb](const Json& r, const std::string&) {
        if (!r.is_object() || !r.contains("signatures") || r["signatures"].empty()) return cb(std::nullopt);
        int active = r.value("activeSignature", 0);
        const Json& sigs = r["signatures"];
        const Json& s = sigs[static_cast<size_t>(std::clamp(active, 0, static_cast<int>(sigs.size()) - 1))];
        SignatureInfo info;
        info.label = s.value("label", "");
        info.activeParam = s.value("activeParameter", r.value("activeParameter", 0));
        if (s.contains("parameters") && info.activeParam >= 0 && info.activeParam < static_cast<int>(s["parameters"].size())) {
            const Json& lab = s["parameters"][static_cast<size_t>(info.activeParam)]["label"];
            if (lab.is_array() && lab.size() == 2) {
                info.paramStart = lab[0].get<int>();
                info.paramEnd = lab[1].get<int>();
            } else if (lab.is_string()) {
                size_t f2 = info.label.find(lab.get<std::string>());
                if (f2 != std::string::npos) {
                    info.paramStart = static_cast<int>(f2);
                    info.paramEnd = static_cast<int>(f2 + lab.get<std::string>().size());
                }
            }
        }
        cb(info);
    });
}
void LspClient::completion(const fs::path& f, Pos p, std::function<void(std::vector<CompletionItem>)> cb) {
    request("textDocument/completion", docPos(f, p), [cb](const Json& r, const std::string&) {
        std::vector<CompletionItem> out;
        const Json& items = r.is_object() ? r.value("items", Json::array()) : r;
        if (items.is_array())
            for (auto& it : items) {
                CompletionItem c;
                c.label = it.value("label", "");
                c.detail = it.value("detail", "");
                c.kind = it.value("kind", 0);
                c.insertText = it.contains("textEdit") ? it["textEdit"].value("newText", c.label) : it.value("insertText", c.label);
                // clangd prefixes labels with a space / bullet for include-insertion; keep it readable
                while (!c.label.empty() && (c.label[0] == ' ' || static_cast<unsigned char>(c.label[0]) == 0xE2)) {
                    c.label.erase(0, static_cast<unsigned char>(c.label[0]) == 0xE2 ? 3 : 1);
                }
                out.push_back(std::move(c));
            }
        cb(std::move(out));
    });
}
void LspClient::formatting(const fs::path& f, int tabSize, std::function<void(std::vector<TextEdit>)> cb) {
    request("textDocument/formatting", {{"textDocument", {{"uri", pathToUri(f)}}}, {"options", {{"tabSize", tabSize}, {"insertSpaces", true}}}},
            [cb](const Json& r, const std::string&) { cb(parseTextEdits(r)); });
}

// ============================================================================== ANSI screen ===
namespace {
uint32_t xtermColor(int n) {
    static const uint32_t base[16] = {0x1e1e1e, 0xcd3131, 0x0dbc79, 0xe5e510, 0x2472c8, 0xbc3fbc, 0x11a8cd, 0xe5e5e5,
                                      0x666666, 0xf14c4c, 0x23d18b, 0xf5f543, 0x3b8eea, 0xd670d6, 0x29b8db, 0xffffff};
    if (n < 16) return base[n] | 0x1000000;
    if (n < 232) {
        n -= 16;
        auto c = [](int v) { return v ? 55 + v * 40 : 0; };
        return static_cast<uint32_t>((c(n / 36) << 16) | (c((n / 6) % 6) << 8) | c(n % 6)) | 0x1000000;
    }
    int g = 8 + (n - 232) * 10;
    return static_cast<uint32_t>((g << 16) | (g << 8) | g) | 0x1000000;
}
} // namespace

AnsiScreen::AnsiScreen(int cols, int rows) : m_cols(std::max(cols, 2)), m_rows(std::max(rows, 2)) {
    m_screen.assign(static_cast<size_t>(m_rows), Row(static_cast<size_t>(m_cols)));
}

AnsiScreen::Cell AnsiScreen::blank() const {
    Cell c;
    c.bg = m_attr.bg;
    return c;
}

void AnsiScreen::resize(int cols, int rows) {
    cols = std::max(cols, 2);
    rows = std::max(rows, 2);
    if (cols == m_cols && rows == m_rows) return;
    while (static_cast<int>(m_screen.size()) > rows) {
        if (m_cy > 0) {
            m_scrollback.push_back(m_screen.front());
            m_screen.erase(m_screen.begin());
            --m_cy;
        } else {
            m_screen.pop_back();
        }
    }
    while (static_cast<int>(m_screen.size()) < rows) m_screen.emplace_back(static_cast<size_t>(cols));
    for (auto& r : m_screen) r.resize(static_cast<size_t>(cols));
    m_cols = cols;
    m_rows = rows;
    m_cx = std::min(m_cx, m_cols - 1);
    m_cy = std::min(m_cy, m_rows - 1);
}

void AnsiScreen::clear() {
    m_scrollback.clear();
    for (auto& r : m_screen) std::fill(r.begin(), r.end(), Cell{});
    m_cx = m_cy = 0;
}

void AnsiScreen::lineFeed() {
    if (m_cy + 1 < m_rows) {
        ++m_cy;
        return;
    }
    m_scrollback.push_back(std::move(m_screen.front()));
    if (m_scrollback.size() > maxScrollback) m_scrollback.erase(m_scrollback.begin());
    m_screen.erase(m_screen.begin());
    m_screen.emplace_back(static_cast<size_t>(m_cols), blank());
}

void AnsiScreen::put(char32_t c) {
    if (m_wrapPending) {
        m_wrapPending = false;
        m_cx = 0;
        lineFeed();
    }
    Cell cell = m_attr;
    cell.ch = c;
    m_screen[static_cast<size_t>(m_cy)][static_cast<size_t>(m_cx)] = cell;
    if (m_cx + 1 >= m_cols) m_wrapPending = true;
    else ++m_cx;
}

void AnsiScreen::sgr(const std::vector<int>& p) {
    if (p.empty()) {
        m_attr = Cell{};
        return;
    }
    for (size_t i = 0; i < p.size(); ++i) {
        int v = p[i];
        if (v == 0) m_attr = Cell{};
        else if (v == 1) m_attr.bold = true;
        else if (v == 22) m_attr.bold = false;
        else if (v >= 30 && v <= 37) m_attr.fg = xtermColor(v - 30);
        else if (v >= 90 && v <= 97) m_attr.fg = xtermColor(v - 90 + 8);
        else if (v >= 40 && v <= 47) m_attr.bg = xtermColor(v - 40);
        else if (v >= 100 && v <= 107) m_attr.bg = xtermColor(v - 100 + 8);
        else if (v == 39) m_attr.fg = 0;
        else if (v == 49) m_attr.bg = 0;
        else if (v == 7) std::swap(m_attr.fg, m_attr.bg);
        else if ((v == 38 || v == 48) && i + 1 < p.size()) {
            uint32_t col = 0;
            if (p[i + 1] == 5 && i + 2 < p.size()) {
                col = xtermColor(std::clamp(p[i + 2], 0, 255));
                i += 2;
            } else if (p[i + 1] == 2 && i + 4 < p.size()) {
                col = static_cast<uint32_t>((std::clamp(p[i + 2], 0, 255) << 16) | (std::clamp(p[i + 3], 0, 255) << 8) | std::clamp(p[i + 4], 0, 255)) | 0x1000000;
                i += 4;
            }
            (v == 38 ? m_attr.fg : m_attr.bg) = col;
        }
    }
}

void AnsiScreen::csi(char f, const std::string& params) {
    bool priv = !params.empty() && (params[0] == '?' || params[0] == '>');
    std::vector<int> p;
    {
        std::string cur;
        for (char c : priv ? params.substr(1) : params) {
            if (c == ';' || c == ':') {
                p.push_back(cur.empty() ? -1 : std::atoi(cur.c_str()));
                cur.clear();
            } else if (std::isdigit(static_cast<unsigned char>(c))) {
                cur += c;
            }
        }
        if (!cur.empty() || !p.empty()) p.push_back(cur.empty() ? -1 : std::atoi(cur.c_str()));
    }
    auto arg = [&](size_t i, int def) { return i < p.size() && p[i] > 0 ? p[i] : def; };
    auto arg0 = [&](size_t i) { return i < p.size() && p[i] >= 0 ? p[i] : 0; };
    if (priv) return;  // ?25h/l cursor visibility, ?1049 alt screen, ... : ignored
    m_wrapPending = false;
    Row& row = m_screen[static_cast<size_t>(m_cy)];
    switch (f) {
        case 'm': {
            std::vector<int> q;
            for (int v : p) q.push_back(v < 0 ? 0 : v);
            sgr(q);
            break;
        }
        case 'H':
        case 'f':
            m_cy = std::clamp(arg(0, 1) - 1, 0, m_rows - 1);
            m_cx = std::clamp(arg(1, 1) - 1, 0, m_cols - 1);
            break;
        case 'A': m_cy = std::max(0, m_cy - arg(0, 1)); break;
        case 'B': m_cy = std::min(m_rows - 1, m_cy + arg(0, 1)); break;
        case 'C': m_cx = std::min(m_cols - 1, m_cx + arg(0, 1)); break;
        case 'D': m_cx = std::max(0, m_cx - arg(0, 1)); break;
        case 'G': m_cx = std::clamp(arg(0, 1) - 1, 0, m_cols - 1); break;
        case 'd': m_cy = std::clamp(arg(0, 1) - 1, 0, m_rows - 1); break;
        case 'J': {
            int mode = arg0(0);
            if (mode == 2 || mode == 3) {
                for (auto& r : m_screen) std::fill(r.begin(), r.end(), blank());
                if (mode == 3) m_scrollback.clear();
            } else if (mode == 0) {
                std::fill(row.begin() + m_cx, row.end(), blank());
                for (int y = m_cy + 1; y < m_rows; ++y) std::fill(m_screen[static_cast<size_t>(y)].begin(), m_screen[static_cast<size_t>(y)].end(), blank());
            } else {
                for (int y = 0; y < m_cy; ++y) std::fill(m_screen[static_cast<size_t>(y)].begin(), m_screen[static_cast<size_t>(y)].end(), blank());
                std::fill(row.begin(), row.begin() + m_cx + 1, blank());
            }
            break;
        }
        case 'K': {
            int mode = arg0(0);
            if (mode == 0) std::fill(row.begin() + m_cx, row.end(), blank());
            else if (mode == 1) std::fill(row.begin(), row.begin() + m_cx + 1, blank());
            else std::fill(row.begin(), row.end(), blank());
            break;
        }
        case 'X': std::fill(row.begin() + m_cx, row.begin() + std::min(m_cols, m_cx + arg(0, 1)), blank()); break;
        case 'P': {
            int n = std::min(arg(0, 1), m_cols - m_cx);
            row.erase(row.begin() + m_cx, row.begin() + m_cx + n);
            row.insert(row.end(), static_cast<size_t>(n), blank());
            break;
        }
        case '@': {
            int n = std::min(arg(0, 1), m_cols - m_cx);
            row.insert(row.begin() + m_cx, static_cast<size_t>(n), blank());
            row.resize(static_cast<size_t>(m_cols));
            break;
        }
        case 'L':
        case 'M': {
            int n = std::min(arg(0, 1), m_rows - m_cy);
            for (int k = 0; k < n; ++k) {
                if (f == 'L') {
                    m_screen.pop_back();
                    m_screen.insert(m_screen.begin() + m_cy, Row(static_cast<size_t>(m_cols), blank()));
                } else {
                    m_screen.erase(m_screen.begin() + m_cy);
                    m_screen.emplace_back(static_cast<size_t>(m_cols), blank());
                }
            }
            break;
        }
        case 's':
            m_savedX = m_cx;
            m_savedY = m_cy;
            break;
        case 'u':
            m_cx = m_savedX;
            m_cy = m_savedY;
            break;
        default: break;  // 'r' scroll region, 'n' reports, 't' window ops: ignored
    }
}

void AnsiScreen::feed(std::string_view bytes) {
    for (unsigned char b : bytes) {
        switch (m_st) {
            case St::Ground:
                if (m_utf8Left > 0) {
                    if ((b & 0xC0) == 0x80) {
                        m_utf8 = (m_utf8 << 6) | (b & 0x3F);
                        if (--m_utf8Left == 0) put(static_cast<char32_t>(m_utf8));
                        continue;
                    }
                    m_utf8Left = 0;
                }
                if (b == 0x1B) {
                    m_st = St::Esc;
                } else if (b == '\r') {
                    m_cx = 0;
                    m_wrapPending = false;
                } else if (b == '\n') {
                    m_wrapPending = false;
                    lineFeed();
                } else if (b == '\b') {
                    m_cx = std::max(0, m_cx - 1);
                    m_wrapPending = false;
                } else if (b == '\t') {
                    m_cx = std::min(m_cols - 1, (m_cx / 8 + 1) * 8);
                } else if (b == 7 || b < 0x20) {
                    // BEL and other C0 controls: ignored
                } else if (b >= 0xC0) {
                    m_utf8Left = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : 1;
                    m_utf8 = b & (b >= 0xF0 ? 0x07 : b >= 0xE0 ? 0x0F : 0x1F);
                } else if (b < 0x80) {
                    put(b);
                }
                break;
            case St::Esc:
                if (b == '[') {
                    m_st = St::Csi;
                    m_seq.clear();
                } else if (b == ']') {
                    m_st = St::Osc;
                    m_seq.clear();
                } else if (b == '(' || b == ')') {
                    m_st = St::Charset;
                } else {
                    if (b == 'c') clear();
                    else if (b == '7') {
                        m_savedX = m_cx;
                        m_savedY = m_cy;
                    } else if (b == '8') {
                        m_cx = m_savedX;
                        m_cy = m_savedY;
                    } else if (b == 'M') {
                        m_cy = std::max(0, m_cy - 1);
                    }
                    m_st = St::Ground;
                }
                break;
            case St::Charset: m_st = St::Ground; break;
            case St::Csi:
                if (b >= 0x40 && b <= 0x7E) {
                    csi(static_cast<char>(b), m_seq);
                    m_st = St::Ground;
                } else {
                    m_seq += static_cast<char>(b);
                    if (m_seq.size() > 64) m_st = St::Ground;
                }
                break;
            case St::Osc:
                if (b == 7) {
                    if (startsWith(m_seq, "0;") || startsWith(m_seq, "2;")) title = m_seq.substr(2);
                    m_st = St::Ground;
                } else if (b == 0x1B) {
                    m_st = St::OscEsc;
                } else {
                    m_seq += static_cast<char>(b);
                    if (m_seq.size() > 512) m_st = St::Ground;
                }
                break;
            case St::OscEsc:
                if (startsWith(m_seq, "0;") || startsWith(m_seq, "2;")) title = m_seq.substr(2);
                m_st = St::Ground;
                break;
        }
    }
}

std::string AnsiScreen::rowText(const Row& r) {
    std::string s;
    for (auto& c : r) {
        char32_t ch = c.ch ? c.ch : U' ';
        if (ch < 0x80) s += static_cast<char>(ch);
        else if (ch < 0x800) {
            s += static_cast<char>(0xC0 | (ch >> 6));
            s += static_cast<char>(0x80 | (ch & 0x3F));
        } else if (ch < 0x10000) {
            s += static_cast<char>(0xE0 | (ch >> 12));
            s += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (ch & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (ch >> 18));
            s += static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (ch & 0x3F));
        }
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string AnsiScreen::text() const {
    std::string s;
    for (auto& r : m_scrollback) s += rowText(r) + "\n";
    for (auto& r : m_screen) s += rowText(r) + "\n";
    while (s.size() >= 2 && s[s.size() - 1] == '\n' && s[s.size() - 2] == '\n') s.pop_back();
    return s;
}

// ============================================================================== terminal ===
#ifdef _WIN32
Terminal::~Terminal() { stop(); }

bool Terminal::start(const std::string& commandLine, const fs::path& cwd, int cols, int rows, std::string* error) {
    stop();
    screen.resize(cols, rows);
    HANDLE inR, inW, outR, outW;
    if (!CreatePipe(&inR, &inW, nullptr, 0)) return false;
    if (!CreatePipe(&outR, &outW, nullptr, 0)) {
        CloseHandle(inR);
        CloseHandle(inW);
        return false;
    }
    if (!conpty().create) {
        CloseHandle(inR);
        CloseHandle(inW);
        CloseHandle(outR);
        CloseHandle(outW);
        if (error) *error = "ConPTY is not available (needs Windows 10 1809 or newer)";
        return false;
    }
    PcHandle pc = nullptr;
    HRESULT hr = conpty().create({static_cast<SHORT>(cols), static_cast<SHORT>(rows)}, inR, outW, 0, &pc);
    CloseHandle(inR);
    CloseHandle(outW);
    if (FAILED(hr)) {
        CloseHandle(inW);
        CloseHandle(outR);
        if (error) *error = "CreatePseudoConsole failed (needs Windows 10 1809 or newer)";
        return false;
    }
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrs, 0, kAttrPseudoConsole, pc, sizeof(pc), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;  // no inherited std handles: the pseudo console provides them
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = widen(commandLine);
    std::wstring dir = cwd.wstring();
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr, dir.empty() ? nullptr : dir.c_str(),
                             &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    if (!ok) {
        if (error) *error = "cannot start " + commandLine;
        conpty().close(pc);
        CloseHandle(inW);
        CloseHandle(outR);
        return false;
    }
    CloseHandle(pi.hThread);
    m_pc = pc;
    m_process = pi.hProcess;
    m_inWrite = inW;
    m_outRead = outR;
    m_exited = false;
    m_reader = std::thread([this, outR] {
        char buf[8192];
        DWORD n = 0;
        while (ReadFile(outR, buf, sizeof(buf), &n, nullptr) && n > 0) {
            std::lock_guard l(m_mutex);
            m_pending.append(buf, n);
        }
        m_exited = true;
    });
    return true;
}

void Terminal::write(std::string_view data) {
    if (!m_inWrite) return;
    DWORD w = 0;
    WriteFile(static_cast<HANDLE>(m_inWrite), data.data(), static_cast<DWORD>(data.size()), &w, nullptr);
}

void Terminal::resize(int cols, int rows) {
    if (cols == screen.cols() && rows == screen.rows()) return;
    screen.resize(cols, rows);
    if (m_pc) conpty().resize(m_pc, {static_cast<SHORT>(cols), static_cast<SHORT>(rows)});
}

void Terminal::poll() {
    std::string s;
    {
        std::lock_guard l(m_mutex);
        s.swap(m_pending);
    }
    if (!s.empty()) screen.feed(s);
}

bool Terminal::running() const { return m_process && WaitForSingleObject(static_cast<HANDLE>(m_process), 0) == WAIT_TIMEOUT; }

void Terminal::stop() {
    if (!m_process) return;
    if (WaitForSingleObject(static_cast<HANDLE>(m_process), 0) == WAIT_TIMEOUT) TerminateProcess(static_cast<HANDLE>(m_process), 0);
    // Closing the pseudo console ends the output pipe, which lets the reader thread finish.
    conpty().close(m_pc);
    m_pc = nullptr;
    if (m_reader.joinable()) m_reader.join();
    CloseHandle(static_cast<HANDLE>(m_inWrite));
    CloseHandle(static_cast<HANDLE>(m_outRead));
    CloseHandle(static_cast<HANDLE>(m_process));
    m_process = m_inWrite = m_outRead = nullptr;
}
#else
Terminal::~Terminal() = default;
bool Terminal::start(const std::string&, const fs::path&, int, int, std::string* error) {
    if (error) *error = "not supported";
    return false;
}
void Terminal::write(std::string_view) {}
void Terminal::resize(int, int) {}
void Terminal::poll() {}
bool Terminal::running() const { return false; }
void Terminal::stop() {}
#endif

// ============================================================================== git ===
bool gitAvailable() {
    static int cached = -1;
    if (cached < 0) cached = findOnPath("git.exe").empty() ? 0 : 1;
    return cached == 1;
}

CaptureResult gitRun(const fs::path& root, const std::string& args) {
    return runCapture(quoteArg(narrow(findOnPath("git.exe").wstring())) + " -c core.quotepath=off " + args, root, 15000);
}

GitStatus parseGitPorcelain(const std::string& output) {
    GitStatus s;
    s.available = true;
    s.isRepo = true;
    std::istringstream in(output);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (startsWith(line, "## ")) {
            std::string b = line.substr(3);
            if (startsWith(b, "No commits yet on ")) b = b.substr(18);
            s.branch = b.substr(0, b.find("..."));
            continue;
        }
        if (line.size() < 4) continue;
        GitFile f;
        f.index = line[0];
        f.work = line[1];
        f.path = line.substr(3);
        size_t arrow = f.path.find(" -> ");
        if (arrow != std::string::npos) f.path = f.path.substr(arrow + 4);
        if (f.path.size() >= 2 && f.path.front() == '"') f.path = f.path.substr(1, f.path.size() - 2);
        s.files.push_back(f);
    }
    return s;
}

GitStatus gitStatus(const fs::path& root) {
    GitStatus s;
    if (!gitAvailable()) return s;
    auto r = gitRun(root, "status --porcelain=v1 -b");
    if (r.exitCode != 0) {
        s.available = true;
        s.error = r.output;
        return s;
    }
    return parseGitPorcelain(r.output);
}

std::string gitDiff(const fs::path& root, const std::string& relPath) { return gitRun(root, "diff -- " + quoteArg(relPath)).output; }

} // namespace fledge::code
