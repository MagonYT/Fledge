#pragma once
// Script debugging (docs/CODE_EDITOR.md "Debugging"): out-of-process play under gdb's machine interface (GDB/MI).
//
//   editor (Code workspace)  --GDB/MI over pipes-->  gdb --interpreter=mi2  --runs-->  fledge_runtime --project ...
//                                                                                        --scripts <Game>Scripts_debug.dll
//
// The game runs in its own process, so a breakpoint pauses only the game and a crash in a script cannot take the
// editor down. Scripts are built with -O0 -g (ScriptBuildOptions::debug); breakpoints are inserted as pending
// (-break-insert -f) and resolve when the runtime loads the script DLL. UI-free and unit-tested: the MI parser on
// canned output, the session against real gdb + fledge_runtime (tests/test_code_editor.cpp).
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/utility.h"
#include "fledge/editor/code_services.h"
#include "fledge/framework/project.h"

namespace fledge::dbg {

// One line of GDB/MI output. Results / async records carry their key=value list as JSON: tuples are objects,
// lists are arrays (a list of name=value results keeps only the values, e.g. stack=[frame={..},..] -> [{..},..]),
// c-strings are strings.
struct MiRecord {
    enum class Kind { Result, Exec, Status, Notify, Console, Target, Log, Prompt, Other };
    Kind kind = Kind::Other;
    int token = -1;       // numeric prefix of result records (-1 = none)
    std::string cls;      // "done", "running", "error", "stopped", "breakpoint-modified", ...
    Json results = Json::object();
    std::string text;     // stream records: the unescaped text; Other: the raw line (the game's own stdout)
};
std::optional<MiRecord> parseMiLine(std::string_view line);
std::string miQuote(std::string_view s);  // "C string" for MI command arguments

struct Frame {
    int level = 0;
    std::string function, file, fullname, address;
    int line = 0;
};
struct Variable {
    std::string name, type, value;
};
struct StopInfo {
    std::string reason;  // breakpoint-hit | end-stepping-range | function-finished | signal-received | exited-normally | exited
    Frame frame;
    int breakpoint = 0;
    int exitCode = 0;
    std::string signal;  // signal-received: SIGSEGV ...
};

class GdbSession {
public:
    enum class State { Idle, Ready, Running, Stopped, Exited, Failed };
    ~GdbSession();
    // gdb.exe next to the C++ compiler (Settings / FLEDGE_CXX / PATH), $FLEDGE_GDB, or on PATH. Empty if none.
    static fs::path findGdb(const fs::path& compilerHint = {});

    // Starts gdb on `program args...` (not running yet: add breakpoints, then run()).
    bool start(const fs::path& gdb, const fs::path& program, const std::vector<std::string>& args, const fs::path& cwd, std::string* error = nullptr);
    // Pending breakpoint at file:line (resolves when the DLL loads). Returns gdb's number, 0 on failure.
    int addBreakpoint(const fs::path& file, int line);
    bool removeBreakpoint(int number);
    bool run();
    bool continueExec();
    bool next();    // step over
    bool step();    // step into
    bool finish();  // step out
    void stop();    // kills the game and gdb

    State state() const { return m_state; }
    bool poll();                                  // drains gdb output; true if the state changed
    bool waitFor(State s, int timeoutMs);         // polls until state == s (or Exited / Failed)
    const StopInfo& lastStop() const { return m_stop; }
    std::vector<Frame> stack(int timeoutMs = 10000);
    std::vector<Variable> locals(int timeoutMs = 10000);  // of the selected (top) frame
    std::optional<std::string> evaluate(const std::string& expression, int timeoutMs = 10000);
    const std::string& programOutput() const { return m_output; }  // lines that were not MI (the game's stdout)
    const std::string& lastError() const { return m_error; }

private:
    std::optional<MiRecord> command(const std::string& cmd, int timeoutMs = 10000);  // waits for the ^ record
    bool execCommand(const std::string& cmd);  // -exec-*: ^running, then *running / *stopped arrive later
    void handle(const MiRecord& r);
    code::ChildProcess m_proc;
    std::string m_partial, m_output, m_error;
    int m_token = 1;
    State m_state = State::Idle;
    StopInfo m_stop;
    std::vector<MiRecord> m_results;
};

const char* toString(GdbSession::State s);
Frame frameFromJson(const Json& j);

// Out-of-process play: the editor's current scene (saved or not) is written to
// <project>/Intermediate/PlaySession/<name>.fscene and the game runs it with
//   fledge_runtime --project <p> --scene <that file> [--scripts <module>] <extra...>
struct PlayLaunch {
    fs::path runtime, workingDir, sceneFile;
    std::vector<std::string> args;
    std::string commandLine() const;  // quoted, for CreateProcess / logs
};
Result<PlayLaunch> preparePlaySession(const Project& project, const Json& sceneJson, const std::string& sceneName, const fs::path& toolsDir,
                                      const fs::path& scriptModule = {}, const std::vector<std::string>& extraArgs = {});

} // namespace fledge::dbg
