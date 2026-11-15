#pragma once
// Fledge Script build pipeline (editor, fledgepack and MCP; never the runtime).
//
//   <project>/Scripts/**/*.cpp|h     gameplay sources (sandboxed: tools may only write here)
//   <project>/Intermediate/Scripts/  obj/ (incremental, -MMD dependency files), gen/script_module.cpp,
//                                    bin/<Game>Scripts_<N>.dll (unique name per build: hot reload never
//                                    fights a locked DLL), current.txt (latest module), build.log
//
// The compiler is MinGW-w64 g++ (C++20) found via Settings / FLEDGE_CXX / PATH / common install
// locations. Diagnostics are parsed from GCC/Clang text output into Diagnostic records.
// MSVC (cl.exe) is not supported yet (the C ABI boundary already allows it).
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/framework/project.h"

namespace fledge {

struct Diagnostic {
    enum class Severity { Error, Warning, Note };
    Severity severity = Severity::Error;
    std::string file;  // relative to Scripts/ when inside it, else as reported
    int line = 0, column = 0;
    std::string message;
    Json toJson() const;
};
const char* toString(Diagnostic::Severity s);

// Parses GCC / Clang / GNU ld text diagnostics. Paths inside scriptsDir become relative ("Player.cpp").
std::vector<Diagnostic> parseCompilerOutput(std::string_view output, const fs::path& scriptsDir = {});

struct Toolchain {
    fs::path cxx;            // g++.exe
    std::string family = "gcc";
    std::string version;     // "13.2.0"
};
// configured: explicit path (Settings / --compiler). Then $FLEDGE_CXX, PATH, common MinGW locations.
// The error message explains how to fix a missing compiler.
Result<Toolchain> findToolchain(const fs::path& configured = {});

struct ProcessResult {
    bool started = false;
    int exitCode = -1;
    std::string output;  // stdout + stderr
};
// Runs a program and captures its output. extraPathDir is prepended to PATH for the child.
ProcessResult runProcess(const std::vector<std::string>& args, const fs::path& workingDir = {}, const fs::path& extraPathDir = {});

// <install>/sdk/include, or the source tree's engine/scripting/sdk in a build tree.
fs::path scriptSdkIncludeDir();

struct ScriptBuildOptions {
    Toolchain toolchain;
    fs::path sdkInclude;     // default scriptSdkIncludeDir()
    bool release = false;    // -O2 into Intermediate/Scripts/release (packaging)
    bool debug = false;      // -O0 -g into Intermediate/Scripts/debug/<Game>Scripts_debug.dll (gdb sessions)
    bool force = false;      // rebuild everything
    int jobs = 0;            // 0 = hardware threads
};

struct ScriptBuildResult {
    bool ok = false;
    bool upToDate = false;   // nothing compiled or linked
    fs::path module;         // the DLL to load / ship
    std::vector<Diagnostic> diagnostics;
    std::string log;         // full compiler / linker output
    double seconds = 0;
    int sources = 0, compiled = 0;
    int errors() const;
    int warnings() const;
    Json toJson(size_t maxDiagnostics = 200) const;
};

ScriptBuildResult buildScripts(const Project& project, const ScriptBuildOptions& options,
                               const std::function<void(const std::string&)>& progress = {});

// Sandbox: resolves a path relative to <project>/Scripts. Rejects absolute paths, "..", drive
// letters, alternate data streams, hidden components, reserved device names, odd characters,
// non-source extensions (.cpp .h .hpp .inl only) and anything that resolves outside Scripts/
// (including through symlinks / junctions).
Result<fs::path> resolveScriptPath(const Project& project, std::string_view relativePath);

// New script from a template. kind: "behaviour" | "service" | "character" | "empty".
std::vector<std::string> scriptTemplateKinds();
bool isValidScriptClassName(std::string_view name);
std::string scriptTemplateSource(const std::string& kind, const std::string& className);
Result<fs::path> createScript(const Project& project, const std::string& className, const std::string& kind = "behaviour");

// compile_commands.json (clangd / VS Code / CLion), .clangd and a CMakeLists.txt hint in Scripts/.
Status writeScriptIdeFiles(const Project& project, const ScriptBuildOptions& options);

} // namespace fledge
