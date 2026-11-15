#include "fledge/ai/claude_code.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <thread>

#include "fledge/core/redact.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fledge::ai {

namespace {

constexpr const char* kServer = "fledge";
constexpr const char* kToolPrefix = "mcp__fledge__";

std::string envVar(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

bool isCmdShim(const std::string& exe) {
    std::string l = exe;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return l.ends_with(".cmd") || l.ends_with(".bat");
}

// CommandLineToArgvW rules: backslashes before a quote are doubled, the quote is escaped.
std::string quoteArg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string out = "\"";
    size_t slashes = 0;
    for (char c : a) {
        if (c == '\\') {
            ++slashes;
        } else if (c == '"') {
            out.append(slashes * 2 + 1, '\\');
            out += '"';
            slashes = 0;
            continue;
        } else {
            slashes = 0;
        }
        out += c;
    }
    out.append(slashes, '\\');
    out += '"';
    return out;
}

std::string blockText(const Json& content) {
    if (content.is_string()) return content.get<std::string>();
    std::string s;
    if (content.is_array())
        for (auto& b : content)
            if (b.is_object() && b.value("type", "") == "text" && b.contains("text") && b["text"].is_string()) {
                if (!s.empty()) s += "\n";
                s += b["text"].get<std::string>();
            }
    return s;
}

std::string displayTool(const std::string& name) { return name.rfind(kToolPrefix, 0) == 0 ? name.substr(std::strlen(kToolPrefix)) : name; }

class ClaudeCodeProvider final : public LlmProvider {
public:
    ClaudeCodeProvider(ClaudeCodeOptions o, ProcessRunner r) : m_opt(std::move(o)), m_run(std::move(r)) {}

    std::string name() const override { return m_opt.model.empty() ? "Claude Code" : "Claude Code (" + m_opt.model + ")"; }

    // Not an HTTP provider: there is no request body. Kept for the interface (tests look at the arguments instead).
    Json buildRequestBody(const ChatRequest&) const override { return Json::object(); }
    Result<ChatResponse> parseResponse(int, const std::string&) const override {
        return Result<ChatResponse>::failure("Claude Code has no HTTP response");
    }

    Result<ChatResponse> chat(const ChatRequest& req) override {
        ClaudeCodeStatus cli = findClaudeCode(m_opt.cliPath);
        if (!cli.found) return Result<ChatResponse>::failure(cli.message);
        if (!m_opt.model.empty() && !claudeCodeSafeToken(m_opt.model))
            return Result<ChatResponse>::failure("The model id has characters that cannot be passed to Claude Code.");

        // Claude Code keeps the conversation itself (--resume): only the newest user message is sent.
        std::string prompt;
        size_t userTurns = 0;
        for (auto& m : req.messages)
            if (m.role == ChatMessage::Role::User) {
                prompt = m.text;
                ++userTurns;
            }
        if (prompt.empty()) return Result<ChatResponse>::failure("nothing to send");
        if (userTurns <= 1) m_session.clear();  // a new chat in the panel starts a new Claude Code session
        if (m_session.empty() && !req.system.empty()) prompt = "[Instructions from the Fledge editor]\n" + req.system + "\n[/Instructions]\n\n" + prompt;

        fs::path cfg;
        if (!m_opt.mcpUrl.empty()) {
            std::error_code ec;
            fs::create_directories(m_opt.scratchDir, ec);
            cfg = m_opt.scratchDir / std::format("claude_code_mcp_{}.json", ++m_turn);
            std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
            f << claudeCodeMcpConfig(m_opt).dump(2);
            if (!f) return Result<ChatResponse>::failure("could not write the MCP config for Claude Code");
        }
        struct Cleanup {
            fs::path p;
            ~Cleanup() {
                std::error_code ec;
                if (!p.empty()) fs::remove(p, ec);  // it can hold the MCP bearer token
            }
        } cleanup{cfg};

        ClaudeStreamState st;
        std::string err;
        int code = m_run(cli.path, claudeCodeArgs(m_opt, m_session, cfg), m_opt.workDir, prompt, m_opt.timeoutSeconds,
                         [&](std::string_view line) { handleClaudeStreamLine(line, st, m_opt.showThinking, req.onActivity); }, err);
        if (!st.sessionId.empty() && claudeCodeSafeToken(st.sessionId)) m_session = st.sessionId;
        if (code == -1) return Result<ChatResponse>::failure(redact("Claude Code: " + err));
        if (!st.gotResult || st.isError) {
            std::string why = !st.error.empty() ? st.error : !st.lastNoise.empty() ? st.lastNoise : std::format("exit code {}", code);
            if (why.find("login") != std::string::npos || why.find("authenticat") != std::string::npos || why.find("API key") != std::string::npos)
                why += "  (Open a terminal, run `claude`, and sign in with /login. The editor cannot sign in for you.)";
            return Result<ChatResponse>::failure(redact("Claude Code: " + why));
        }
        ChatResponse r;
        r.text = st.finalText;
        r.stopReason = "end";
        r.inputTokens = st.inputTokens;
        r.outputTokens = st.outputTokens;
        r.activityStreamed = static_cast<bool>(req.onActivity);
        return r;
    }

private:
    ClaudeCodeOptions m_opt;
    ProcessRunner m_run;
    std::string m_session;
    int m_turn = 0;
};

#ifdef _WIN32
struct Handle {
    HANDLE h = nullptr;
    ~Handle() { close(); }
    void close() {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = nullptr;
    }
};

int runProcessWin32(const std::string& exe, const std::vector<std::string>& args, const fs::path& cwd, const std::string& stdinText,
                    int timeoutSeconds, const std::function<void(std::string_view)>& onLine, std::string& error) {
    std::string cmd = claudeCodeCommandLine(exe, args, error);
    if (cmd.empty()) return -1;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    Handle inR, inW, outR, outW;
    if (!CreatePipe(&inR.h, &inW.h, &sa, 0) || !CreatePipe(&outR.h, &outW.h, &sa, 1 << 16)) {
        error = "could not create pipes";
        return -1;
    }
    SetHandleInformation(inW.h, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR.h, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR.h;
    si.hStdOutput = outW.h;
    si.hStdError = outW.h;
    PROCESS_INFORMATION pi{};
    std::wstring wcmd = widen(cmd);
    std::wstring wcwd = cwd.empty() ? std::wstring() : cwd.wstring();
    // The job kills Claude Code (and anything it started) if the editor goes away mid-turn.
    Handle job;
    job.h = CreateJobObjectW(nullptr, nullptr);
    if (job.h) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi)) {
        error = std::format("could not start '{}' (Windows error {})", exe, GetLastError());
        return -1;
    }
    Handle proc{pi.hProcess}, thread{pi.hThread};
    if (job.h) AssignProcessToJobObject(job.h, proc.h);
    ResumeThread(thread.h);
    inR.close();
    outW.close();

    std::thread writer([&] {
        size_t off = 0;
        while (off < stdinText.size()) {
            DWORD n = 0;
            if (!WriteFile(inW.h, stdinText.data() + off, static_cast<DWORD>(std::min<size_t>(stdinText.size() - off, 1 << 15)), &n, nullptr)) break;
            off += n;
        }
        inW.close();  // end of input: the CLI starts working
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, timeoutSeconds));
    std::string buf;
    bool timedOut = false;
    auto emitLines = [&](bool flush) {
        size_t start = 0;
        for (size_t nl; (nl = buf.find('\n', start)) != std::string::npos; start = nl + 1) {
            std::string_view l(buf.data() + start, nl - start);
            if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
            if (!l.empty() && onLine) onLine(l);
        }
        buf.erase(0, start);
        if (flush && !buf.empty() && onLine) onLine(buf);
    };
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(outR.h, nullptr, 0, nullptr, &avail, nullptr)) break;  // pipe closed: the process ended
        if (avail == 0) {
            if (WaitForSingleObject(proc.h, 15) == WAIT_OBJECT_0 && (!PeekNamedPipe(outR.h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0)) break;
            if (std::chrono::steady_clock::now() > deadline) {
                timedOut = true;
                break;
            }
            continue;
        }
        char chunk[8192];
        DWORD n = 0;
        if (!ReadFile(outR.h, chunk, std::min<DWORD>(avail, sizeof(chunk)), &n, nullptr) || n == 0) break;
        buf.append(chunk, n);
        if (buf.size() > (8u << 20)) buf.erase(0, buf.size() - (1u << 20));  // a runaway line never grows without bound
        emitLines(false);
    }
    emitLines(true);
    if (timedOut) {
        if (job.h) TerminateJobObject(job.h, 1);
        else TerminateProcess(proc.h, 1);
    }
    inW.close();
    if (writer.joinable()) writer.join();
    WaitForSingleObject(proc.h, 5000);
    if (timedOut) {
        error = std::format("no answer after {} s; the run was stopped", timeoutSeconds);
        return -1;
    }
    DWORD code = 0;
    GetExitCodeProcess(proc.h, &code);
    return static_cast<int>(code);
}
#endif

} // namespace

bool claudeCodeSafeToken(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == ':' || c == '-' || c == '[' || c == ']'; });
}

ClaudeCodeStatus findClaudeCode(const std::string& cliPath) {
    ClaudeCodeStatus s;
    std::error_code ec;
    auto ok = [&](const fs::path& p) {
        if (p.empty() || !fs::is_regular_file(p, ec)) return false;
        s.found = true;
        s.path = narrow(p.wstring());
        return true;
    };
    if (!cliPath.empty()) {
        if (ok(pathFromUtf8(cliPath))) return s;
        s.message = "Claude Code was not found at '" + cliPath + "'. Fix the path in AI settings, or clear it to search automatically.";
        return s;
    }
    std::vector<fs::path> dirs;
    std::string path = envVar("PATH");
#ifdef _WIN32
    const char sep = ';';
    const std::vector<std::string> names = {"claude.exe", "claude.cmd"};
#else
    const char sep = ':';
    const std::vector<std::string> names = {"claude"};
#endif
    for (size_t pos = 0; pos <= path.size();) {
        size_t e = path.find(sep, pos);
        if (e == std::string::npos) e = path.size();
        if (e > pos) dirs.push_back(pathFromUtf8(path.substr(pos, e - pos)));
        pos = e + 1;
    }
    if (std::string h = envVar("USERPROFILE"); !h.empty()) dirs.push_back(pathFromUtf8(h) / ".local" / "bin");
    if (std::string a = envVar("APPDATA"); !a.empty()) dirs.push_back(pathFromUtf8(a) / "npm");
    for (auto& d : dirs)
        for (auto& n : names)
            if (ok(d / n)) return s;
    s.message = "Claude Code is not installed (no `claude` on PATH). Install it from claude.com/claude-code, run `claude` once in a "
                "terminal and sign in with /login, then try again.";
    return s;
}

Json claudeCodeMcpConfig(const ClaudeCodeOptions& o) {
    Json server = {{"type", "http"}, {"url", o.mcpUrl}};
    if (!o.mcpToken.empty()) server["headers"] = {{"Authorization", "Bearer " + o.mcpToken}};
    return {{"mcpServers", {{kServer, server}}}};
}

std::vector<std::string> claudeCodeArgs(const ClaudeCodeOptions& o, const std::string& sessionId, const fs::path& mcpConfig) {
    std::vector<std::string> a = {"-p", "--output-format", "stream-json", "--verbose"};
    // Only this editor's MCP server, whatever the user's own Claude Code configuration lists.
    a.push_back("--strict-mcp-config");
    if (!mcpConfig.empty()) {
        a.push_back("--mcp-config");
        a.push_back(narrow(mcpConfig.wstring()));
        a.push_back("--allowedTools");
        a.push_back(std::string("mcp__") + kServer);
    }
    // Nothing that writes files, runs programs or goes online from inside the editor: scene edits go through MCP tools.
    a.push_back("--disallowedTools");
    a.push_back("Bash,Edit,Write,MultiEdit,NotebookEdit,WebFetch,WebSearch,Task");
    if (!o.model.empty() && claudeCodeSafeToken(o.model)) {
        a.push_back("--model");
        a.push_back(o.model);
    }
    if (!sessionId.empty() && claudeCodeSafeToken(sessionId)) {
        a.push_back("--resume");
        a.push_back(sessionId);
    }
    return a;
}

std::string claudeCodeCommandLine(const std::string& exe, const std::vector<std::string>& args, std::string& error) {
    const bool shim = isCmdShim(exe);
    auto unsafe = [&](const std::string& s) {
        if (s.find_first_of("\r\n") != std::string::npos || s.find('\0') != std::string::npos) return true;
        // cmd.exe re-reads the line: these would change its meaning, and there is no reliable escape for all of them
        return shim && s.find_first_of("\"%^&|<>!()") != std::string::npos;
    };
    if (unsafe(exe)) {
        error = "the Claude Code path has characters that cannot be passed safely";
        return {};
    }
    std::string line = quoteArg(exe);
    for (auto& a : args) {
        if (unsafe(a)) {
            error = "an argument for Claude Code has characters that cannot be passed safely (check the project path)";
            return {};
        }
        line += " " + quoteArg(a);
    }
    if (!shim) return line;
    std::string comspec = envVar("ComSpec");
    if (comspec.empty()) comspec = "C:\\Windows\\System32\\cmd.exe";
    return quoteArg(comspec) + " /d /s /c \"" + line + "\"";
}

void handleClaudeStreamLine(std::string_view line, ClaudeStreamState& st, bool showThinking,
                            const std::function<void(const ProviderActivity&)>& onActivity) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    if (line.empty()) return;
    if (line.front() != '{') {
        st.lastNoise = std::string(line.substr(0, 400));
        return;
    }
    if (line.size() > (4u << 20)) return;
    Json j = Json::parse(line, nullptr, false);
    if (!j.is_object()) {
        st.lastNoise = std::string(line.substr(0, 400));
        return;
    }
    auto str = [](const Json& o, const char* k) { return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string(); };
    auto emit = [&](ProviderActivity a) {
        if (onActivity) onActivity(a);
    };
    std::string type = str(j, "type");
    if (std::string sid = str(j, "session_id"); !sid.empty()) st.sessionId = sid;

    if (type == "assistant" || type == "user") {
        if (!j.contains("message") || !j["message"].is_object()) return;
        const Json& msg = j["message"];
        if (!msg.contains("content") || !msg["content"].is_array()) return;
        for (auto& b : msg["content"]) {
            if (!b.is_object()) continue;
            std::string bt = str(b, "type");
            if (type == "assistant" && bt == "text") {
                std::string t = str(b, "text");
                if (!t.empty()) emit({ProviderActivity::Kind::Text, t, {}, {}, false});
            } else if (type == "assistant" && bt == "thinking") {
                std::string t = str(b, "thinking");
                if (showThinking && !t.empty()) emit({ProviderActivity::Kind::Thinking, t, {}, {}, false});
            } else if (type == "assistant" && bt == "tool_use") {
                std::string name = displayTool(str(b, "name"));
                st.toolNames[str(b, "id")] = name;
                if (st.toolNames.size() > 4096) st.toolNames.erase(st.toolNames.begin());
                emit({ProviderActivity::Kind::ToolCall, name, name, b.contains("input") ? b["input"] : Json::object(), false});
            } else if (type == "user" && bt == "tool_result") {
                auto it = st.toolNames.find(str(b, "tool_use_id"));
                std::string text = b.contains("content") ? blockText(b["content"]) : std::string();
                if (text.size() > 2000) text = text.substr(0, 2000) + "...";
                bool isErr = b.contains("is_error") && b["is_error"].is_boolean() && b["is_error"].get<bool>();
                emit({ProviderActivity::Kind::ToolResult, text, it != st.toolNames.end() ? it->second : std::string("tool"), {}, isErr});
            }
        }
    } else if (type == "result") {
        st.gotResult = true;
        st.isError = (j.contains("is_error") && j["is_error"].is_boolean() && j["is_error"].get<bool>()) || str(j, "subtype").rfind("error", 0) == 0;
        st.finalText = str(j, "result");
        if (st.isError) st.error = !st.finalText.empty() ? st.finalText : str(j, "subtype");
        if (j.contains("usage") && j["usage"].is_object()) {
            st.inputTokens = j["usage"].value("input_tokens", 0);
            st.outputTokens = j["usage"].value("output_tokens", 0);
        }
    }
}

ProcessRunner defaultProcessRunner() {
#ifdef _WIN32
    return runProcessWin32;
#else
    return [](const std::string&, const std::vector<std::string>&, const fs::path&, const std::string&, int,
              const std::function<void(std::string_view)>&, std::string& error) {
        error = "the Claude Code bridge is only implemented on Windows";
        return -1;
    };
#endif
}

std::unique_ptr<LlmProvider> createClaudeCodeProvider(ClaudeCodeOptions options, ProcessRunner runner) {
    return std::make_unique<ClaudeCodeProvider>(std::move(options), std::move(runner));
}

} // namespace fledge::ai
