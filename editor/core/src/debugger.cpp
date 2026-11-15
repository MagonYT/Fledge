#include "fledge/editor/debugger.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "fledge/packager/script_build.h"

namespace fledge::dbg {

// ------------------------------------------------------------------- GDB/MI parser --
namespace {
struct MiParser {
    std::string_view s;
    size_t i = 0;
    bool ok = true;
    bool eof() const { return i >= s.size(); }
    char peek() const { return eof() ? '\0' : s[i]; }
    std::string cstring() {
        std::string out;
        if (peek() != '"') {
            ok = false;
            return out;
        }
        ++i;
        while (!eof() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\' && !eof()) {
                char e = s[i++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    default:
                        if (e >= '0' && e <= '7') {  // octal escape (non-ASCII bytes)
                            int v = e - '0';
                            for (int k = 0; k < 2 && !eof() && s[i] >= '0' && s[i] <= '7'; ++k) v = v * 8 + (s[i++] - '0');
                            out += static_cast<char>(v);
                        } else {
                            out += e;
                        }
                }
            } else {
                out += c;
            }
        }
        if (peek() == '"') ++i;
        else ok = false;
        return out;
    }
    std::string name() {
        size_t b = i;
        while (!eof() && s[i] != '=' && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
        return std::string(s.substr(b, i - b));
    }
    Json value() {
        char c = peek();
        if (c == '"') return cstring();
        if (c == '{') {
            ++i;
            Json o = Json::object();
            while (ok && !eof() && peek() != '}') {
                std::string n = name();
                if (peek() != '=') {
                    ok = false;
                    break;
                }
                ++i;
                o[n] = value();
                if (peek() == ',') ++i;
            }
            if (peek() == '}') ++i;
            return o;
        }
        if (c == '[') {
            ++i;
            Json a = Json::array();
            while (ok && !eof() && peek() != ']') {
                if (peek() == '"' || peek() == '{' || peek() == '[') {
                    a.push_back(value());
                } else {  // name=value inside a list: keep the value
                    (void)name();
                    if (peek() != '=') {
                        ok = false;
                        break;
                    }
                    ++i;
                    a.push_back(value());
                }
                if (peek() == ',') ++i;
            }
            if (peek() == ']') ++i;
            return a;
        }
        ok = false;
        return {};
    }
};
} // namespace

std::optional<MiRecord> parseMiLine(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    MiRecord r;
    if (line.empty()) return std::nullopt;
    if (line.rfind("(gdb)", 0) == 0) {
        r.kind = MiRecord::Kind::Prompt;
        return r;
    }
    size_t i = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
    int token = i > 0 ? std::atoi(std::string(line.substr(0, i)).c_str()) : -1;
    char c = i < line.size() ? line[i] : '\0';
    auto other = [&] {
        MiRecord o;
        o.kind = MiRecord::Kind::Other;
        o.text = std::string(line);
        return o;
    };
    if (c == '~' || c == '@' || c == '&') {
        MiParser p{line, i + 1};
        r.kind = c == '~' ? MiRecord::Kind::Console : c == '@' ? MiRecord::Kind::Target : MiRecord::Kind::Log;
        r.text = p.cstring();
        if (!p.ok) return other();
        return r;
    }
    if (c != '^' && c != '*' && c != '+' && c != '=') return other();
    r.kind = c == '^' ? MiRecord::Kind::Result : c == '*' ? MiRecord::Kind::Exec : c == '+' ? MiRecord::Kind::Status : MiRecord::Kind::Notify;
    r.token = token;
    MiParser p{line, i + 1};
    r.cls = p.name();
    while (p.ok && p.peek() == ',') {
        ++p.i;
        std::string n = p.name();
        if (p.peek() != '=') {
            p.ok = false;
            break;
        }
        ++p.i;
        r.results[n] = p.value();
    }
    if (!p.ok) return other();
    return r;
}

std::string miQuote(std::string_view s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if (c == '\n') {
            o += "\\n";
            continue;
        }
        o += c;
    }
    return o + "\"";
}

Frame frameFromJson(const Json& j) {
    Frame f;
    if (!j.is_object()) return f;
    auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
    f.level = std::atoi(str("level").c_str());
    f.function = str("func");
    f.file = str("file");
    f.fullname = str("fullname");
    f.address = str("addr");
    f.line = std::atoi(str("line").c_str());
    return f;
}

const char* toString(GdbSession::State s) {
    switch (s) {
        case GdbSession::State::Ready: return "ready";
        case GdbSession::State::Running: return "running";
        case GdbSession::State::Stopped: return "stopped";
        case GdbSession::State::Exited: return "exited";
        case GdbSession::State::Failed: return "failed";
        default: return "idle";
    }
}

// ------------------------------------------------------------------------- session --
GdbSession::~GdbSession() { stop(); }

fs::path GdbSession::findGdb(const fs::path& compilerHint) {
    std::error_code ec;
    if (const char* env = std::getenv("FLEDGE_GDB"); env && *env && fs::is_regular_file(pathFromUtf8(env), ec)) return pathFromUtf8(env);
    if (auto tc = findToolchain(compilerHint)) {  // MinGW ships gdb next to g++
        fs::path g = tc->cxx.parent_path() / "gdb.exe";
        if (fs::is_regular_file(g, ec)) return g;
    }
    return code::findOnPath("gdb.exe");
}

bool GdbSession::start(const fs::path& gdb, const fs::path& program, const std::vector<std::string>& args, const fs::path& cwd, std::string* error) {
    stop();
    m_output.clear();
    m_error.clear();
    m_partial.clear();
    m_results.clear();
    m_stop = {};
    std::string cmd = code::quoteArg(narrow(gdb.wstring())) + " --interpreter=mi2 --nx --quiet --args " + code::quoteArg(narrow(program.wstring()));
    for (auto& a : args) cmd += " " + code::quoteArg(a);
    if (!m_proc.start(cmd, cwd, error)) {
        m_state = State::Failed;
        return false;
    }
    m_state = State::Ready;
    // non-blocking target I/O: async exec commands so the UI can poll; no confirmation prompts; no pager
    for (const char* c : {"-gdb-set mi-async on", "-gdb-set confirm off", "-gdb-set pagination off", "-gdb-set breakpoint pending on"})
        if (auto r = command(c); !r || r->cls == "error") {
            if (error) *error = "gdb did not accept '" + std::string(c) + "'" + (r ? ": " + r->results.value("msg", std::string()) : std::string(" (no answer)"));
            stop();
            m_state = State::Failed;
            return false;
        }
    return true;
}

void GdbSession::handle(const MiRecord& r) {
    switch (r.kind) {
        case MiRecord::Kind::Result: m_results.push_back(r); break;
        case MiRecord::Kind::Exec:
            if (r.cls == "running") {
                m_state = State::Running;
            } else if (r.cls == "stopped") {
                StopInfo s;
                s.reason = r.results.value("reason", std::string());
                if (r.results.contains("frame")) s.frame = frameFromJson(r.results["frame"]);
                s.breakpoint = std::atoi(r.results.value("bkptno", std::string("0")).c_str());
                s.signal = r.results.value("signal-name", std::string());
                if (s.reason == "exited-normally" || s.reason == "exited" || s.reason == "exited-signalled") {
                    s.exitCode = static_cast<int>(std::strtol(r.results.value("exit-code", std::string("0")).c_str(), nullptr, 8));  // octal
                    m_state = State::Exited;
                } else {
                    m_state = State::Stopped;
                }
                m_stop = s;
            }
            break;
        case MiRecord::Kind::Other: m_output += r.text + "\n"; break;
        default: break;
    }
}

bool GdbSession::poll() {
    State before = m_state;
    m_partial += m_proc.read();
    size_t nl;
    while ((nl = m_partial.find('\n')) != std::string::npos) {
        std::string line = m_partial.substr(0, nl);
        m_partial.erase(0, nl + 1);
        if (auto r = parseMiLine(line)) handle(*r);
    }
    if (!m_proc.running() && m_state != State::Idle && m_state != State::Exited) {
        m_state = State::Failed;
        if (m_error.empty()) m_error = "gdb exited";
    }
    return m_state != before;
}

bool GdbSession::waitFor(State s, int timeoutMs) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        poll();
        if (m_state == s) return true;
        if (m_state == State::Exited || m_state == State::Failed) return m_state == s;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

std::optional<MiRecord> GdbSession::command(const std::string& cmd, int timeoutMs) {
    if (m_state == State::Idle || m_state == State::Failed) return std::nullopt;
    int token = m_token++;
    if (!m_proc.write(std::to_string(token) + cmd + "\n")) return std::nullopt;
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        poll();
        for (size_t k = 0; k < m_results.size(); ++k)
            if (m_results[k].token == token) {
                MiRecord r = m_results[k];
                m_results.erase(m_results.begin() + static_cast<std::ptrdiff_t>(k));
                if (r.cls == "error") m_error = r.results.value("msg", std::string("gdb error"));
                return r;
            }
        if (m_state == State::Failed) return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    m_error = "gdb did not answer '" + cmd + "'";
    return std::nullopt;
}

bool GdbSession::execCommand(const std::string& cmd) {
    auto r = command(cmd);
    if (!r || r->cls == "error") return false;
    if (r->cls == "running") m_state = State::Running;  // *running follows; set it now so callers can wait for Stopped
    return true;
}

int GdbSession::addBreakpoint(const fs::path& file, int line) {
    std::string where = narrow(file.generic_wstring()) + ":" + std::to_string(line);
    auto r = command("-break-insert -f " + miQuote(where));
    if (!r || r->cls != "done" || !r->results.contains("bkpt")) return 0;
    return std::atoi(r->results["bkpt"].value("number", std::string("0")).c_str());
}

bool GdbSession::removeBreakpoint(int number) {
    auto r = command("-break-delete " + std::to_string(number));
    return r && r->cls == "done";
}

bool GdbSession::run() { return execCommand("-exec-run"); }
bool GdbSession::continueExec() { return execCommand("-exec-continue"); }
bool GdbSession::next() { return execCommand("-exec-next"); }
bool GdbSession::step() { return execCommand("-exec-step"); }
bool GdbSession::finish() { return execCommand("-exec-finish"); }

std::vector<Frame> GdbSession::stack(int timeoutMs) {
    std::vector<Frame> out;
    auto r = command("-stack-list-frames", timeoutMs);
    if (r && r->cls == "done" && r->results.contains("stack"))
        for (auto& f : r->results["stack"]) out.push_back(frameFromJson(f));
    return out;
}

std::vector<Variable> GdbSession::locals(int timeoutMs) {
    std::vector<Variable> out;
    auto r = command("-stack-list-variables --simple-values", timeoutMs);
    if (r && r->cls == "done" && r->results.contains("variables"))
        for (auto& v : r->results["variables"])
            out.push_back({v.value("name", std::string()), v.value("type", std::string()), v.value("value", std::string("..."))});
    return out;
}

std::optional<std::string> GdbSession::evaluate(const std::string& expression, int timeoutMs) {
    auto r = command("-data-evaluate-expression " + miQuote(expression), timeoutMs);
    if (!r || r->cls != "done") return std::nullopt;
    return r->results.value("value", std::string());
}

std::string PlayLaunch::commandLine() const {
    std::string s = code::quoteArg(narrow(runtime.wstring()));
    for (auto& a : args) s += " " + code::quoteArg(a);
    return s;
}

Result<PlayLaunch> preparePlaySession(const Project& project, const Json& sceneJson, const std::string& sceneName, const fs::path& toolsDir,
                                      const fs::path& scriptModule, const std::vector<std::string>& extraArgs) {
    PlayLaunch l;
    std::error_code ec;
    l.runtime = toolsDir / "fledge_runtime.exe";
    if (!fs::is_regular_file(l.runtime, ec)) return Result<PlayLaunch>::failure("fledge_runtime.exe not found next to the editor (" + narrow(toolsDir.wstring()) + ")");
    fs::path dir = project.rootDir() / "Intermediate" / "PlaySession";
    fs::create_directories(dir, ec);
    std::string name = sceneName.empty() ? "Session" : sceneName;
    for (char& c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '_';
    l.sceneFile = dir / pathFromUtf8(name + ".fscene");
    if (!writeFileText(l.sceneFile, sceneJson.dump(2))) return Result<PlayLaunch>::failure("cannot write " + narrow(l.sceneFile.wstring()));
    l.workingDir = project.rootDir();
    l.args = {"--project", narrow(project.file.wstring()), "--scene", narrow(l.sceneFile.wstring())};
    if (!scriptModule.empty()) l.args.insert(l.args.end(), {"--scripts", narrow(scriptModule.wstring())});
    l.args.insert(l.args.end(), extraArgs.begin(), extraArgs.end());
    return l;
}

void GdbSession::stop() {
    if (m_state == State::Idle) return;
    if (m_proc.running()) {
        if (m_state == State::Running) m_proc.write("-exec-interrupt\n");
        m_proc.write("kill\n");  // CLI command: kills the inferior (confirm is off)
        m_proc.write("-gdb-exit\n");
        for (int i = 0; i < 100 && m_proc.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    m_proc.stop();
    m_state = State::Idle;
}

} // namespace fledge::dbg
