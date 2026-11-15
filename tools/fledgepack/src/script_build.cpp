#include "fledge/packager/script_build.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <mutex>
#include <regex>
#include <thread>

#include "fledge/core/log.h"
#include "fledge/core/paths.h"
#include "fledge/script/abi.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fledge {

// ------------------------------------------------------------- diagnostics --
const char* toString(Diagnostic::Severity s) {
    switch (s) {
        case Diagnostic::Severity::Error: return "error";
        case Diagnostic::Severity::Warning: return "warning";
        case Diagnostic::Severity::Note: return "note";
    }
    return "?";
}

Json Diagnostic::toJson() const {
    Json j = {{"severity", toString(severity)}, {"message", message}};
    if (!file.empty()) j["file"] = file;
    if (line) j["line"] = line;
    if (column) j["column"] = column;
    return j;
}

namespace {
std::string relativeTo(const std::string& file, const fs::path& base) {
    if (base.empty() || file.empty()) return file;
    std::error_code ec;
    fs::path p = fs::weakly_canonical(pathFromUtf8(file), ec);
    fs::path b = fs::weakly_canonical(base, ec);
    if (ec) return file;
    auto rel = p.lexically_relative(b);
    if (rel.empty() || rel.begin()->string() == "..") return file;
    return narrow(rel.generic_wstring());
}
} // namespace

std::vector<Diagnostic> parseCompilerOutput(std::string_view output, const fs::path& scriptsDir) {
    static const std::regex withCol(R"(^((?:[A-Za-z]:)?[^:]+):(\d+):(\d+): (fatal error|error|warning|note): (.*)$)");
    static const std::regex noCol(R"(^((?:[A-Za-z]:)?[^:]+):(\d+): (fatal error|error|warning|note): (.*)$)");
    static const std::regex undefinedRef(R"(^(.*): undefined reference to (.*)$)");
    static const std::regex toolError(R"(^([^:]+(?:\.exe)?): (fatal error|error): (.*)$)");
    std::vector<Diagnostic> out;
    auto sev = [](const std::string& s) {
        if (s == "warning") return Diagnostic::Severity::Warning;
        if (s == "note") return Diagnostic::Severity::Note;
        return Diagnostic::Severity::Error;
    };
    size_t start = 0;
    while (start < output.size()) {
        size_t end = output.find('\n', start);
        if (end == std::string_view::npos) end = output.size();
        std::string line(output.substr(start, end - start));
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch m;
        Diagnostic d;
        if (std::regex_match(line, m, withCol)) {
            d.file = relativeTo(m[1], scriptsDir);
            d.line = std::stoi(m[2]);
            d.column = std::stoi(m[3]);
            d.severity = sev(m[4]);
            d.message = m[5];
        } else if (std::regex_match(line, m, noCol)) {
            d.file = relativeTo(m[1], scriptsDir);
            d.line = std::stoi(m[2]);
            d.severity = sev(m[3]);
            d.message = m[4];
        } else if (std::regex_match(line, m, undefinedRef)) {
            d.severity = Diagnostic::Severity::Error;
            std::string where = m[1];
            d.message = "undefined reference to " + std::string(m[2]) + " (linker)";
            // "...obj/Player.cpp.o:Player.cpp:(.text+0x12)" -> keep the source name when present
            std::smatch src;
            static const std::regex srcName(R"(([A-Za-z0-9_\-./\\]+\.(?:cpp|cc|h|hpp)):)");
            if (std::regex_search(where, src, srcName)) d.file = relativeTo(src[1], scriptsDir);
        } else if (std::regex_match(line, m, toolError)) {
            d.severity = Diagnostic::Severity::Error;
            d.message = std::string(m[1]) + ": " + std::string(m[3]);
        } else {
            continue;
        }
        out.push_back(std::move(d));
    }
    return out;
}

// ---------------------------------------------------------------- process --
#ifdef _WIN32
namespace {
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') q.append(backslashes * 2 + 1, L'\\');
        else q.append(backslashes, L'\\');
        backslashes = 0;
        q += c;
    }
    q.append(backslashes * 2, L'\\');
    q += L'"';
    return q;
}
} // namespace

ProcessResult runProcess(const std::vector<std::string>& args, const fs::path& workingDir, const fs::path& extraPathDir) {
    ProcessResult r;
    if (args.empty()) return r;
    std::wstring cmd;
    for (auto& a : args) {
        if (!cmd.empty()) cmd += L' ';
        cmd += quoteArg(widen(a));
    }
    // Environment: the current one with extraPathDir prepended to PATH.
    std::wstring env;
    {
        wchar_t* block = GetEnvironmentStringsW();
        bool pathDone = false;
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            std::wstring entry = p;
            if (!extraPathDir.empty() && _wcsnicmp(entry.c_str(), L"PATH=", 5) == 0) {
                entry = L"PATH=" + extraPathDir.wstring() + L";" + entry.substr(5);
                pathDone = true;
            }
            env += entry;
            env += L'\0';
        }
        if (!extraPathDir.empty() && !pathDone) {
            env += L"PATH=" + extraPathDir.wstring();
            env += L'\0';
        }
        env += L'\0';
        FreeEnvironmentStringsW(block);
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return r;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cwd = workingDir.empty() ? std::wstring() : workingDir.wstring();
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env.data(),
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(writeEnd);
    if (!ok) {
        CloseHandle(readEnd);
        r.output = std::format("cannot start {} (Windows error {})", args[0], GetLastError());
        return r;
    }
    r.started = true;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(readEnd, buf, sizeof buf, &n, nullptr) && n > 0) r.output.append(buf, n);
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exitCode = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
}
#else
ProcessResult runProcess(const std::vector<std::string>& args, const fs::path& workingDir, const fs::path& extraPathDir) {
    ProcessResult r;
    std::string cmd;
    if (!workingDir.empty()) cmd = "cd '" + workingDir.string() + "' && ";
    if (!extraPathDir.empty()) cmd += "PATH='" + extraPathDir.string() + "':$PATH ";
    for (auto& a : args) cmd += "'" + a + "' ";
    cmd += "2>&1";
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return r;
    r.started = true;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) r.output.append(buf, n);
    r.exitCode = pclose(f);
    return r;
}
#endif

// -------------------------------------------------------------- toolchain --
namespace {
std::string kToolchainHelp =
    "Fledge Script needs a C++20 compiler to build scripts: MinGW-w64 g++ 13 or newer.\n"
    "Fix: install one (e.g. Strawberry Perl ships C:\\Strawberry\\c\\bin\\g++.exe, or MSYS2: pacman -S mingw-w64-ucrt-x86_64-gcc),\n"
    "then either add its bin folder to PATH, set the FLEDGE_CXX environment variable to the full g++.exe path,\n"
    "or set Settings > Scripts > Compiler in the editor (fledgepack: --compiler <path>).\n"
    "Easiest: editor Settings > Scripts > Install C++ toolchain (a pinned portable MinGW-w64 into Fledge's data folder).";

std::optional<Toolchain> probe(const fs::path& cxx) {
    std::error_code ec;
    if (cxx.empty() || !fs::is_regular_file(cxx, ec)) return std::nullopt;
    auto r = runProcess({narrow(cxx.wstring()), "-dumpfullversion"}, {}, cxx.parent_path());
    if (!r.started || r.exitCode != 0) return std::nullopt;
    Toolchain t;
    t.cxx = cxx;
    t.version = r.output;
    while (!t.version.empty() && (t.version.back() == '\n' || t.version.back() == '\r')) t.version.pop_back();
    if (t.version.empty() || !std::isdigit(static_cast<unsigned char>(t.version[0]))) return std::nullopt;
    return t;
}
} // namespace

Result<Toolchain> findToolchain(const fs::path& configured) {
    if (!configured.empty()) {
        if (auto t = probe(configured)) return *t;
        return Result<Toolchain>::failure("the configured compiler does not run: " + narrow(configured.wstring()) + "\n" + kToolchainHelp);
    }
    if (const char* env = std::getenv("FLEDGE_CXX"); env && *env) {
        if (auto t = probe(pathFromUtf8(env))) return *t;
        return Result<Toolchain>::failure(std::string("FLEDGE_CXX does not point to a working compiler: ") + env + "\n" + kToolchainHelp);
    }
    std::vector<fs::path> candidates;
#ifdef _WIN32
    const wchar_t* exe = L"g++.exe";
#else
    const wchar_t* exe = L"g++";
#endif
    if (const char* path = std::getenv("PATH")) {
        std::string p = path;
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        size_t s = 0;
        while (s <= p.size()) {
            size_t e = p.find(sep, s);
            if (e == std::string::npos) e = p.size();
            if (e > s) candidates.push_back(pathFromUtf8(p.substr(s, e - s)) / exe);
            s = e + 1;
        }
    }
    {  // toolchains installed by the editor ("Install C++ toolchain") into the Fledge data folder
        std::error_code ec;
        fs::path root = enginePaths().userData / "Toolchains";
        if (fs::is_directory(root, ec))
            for (auto& d : fs::directory_iterator(root, ec))
                if (d.is_directory() && d.path().extension() != ".partial") candidates.push_back(d.path() / "mingw64" / "bin" / exe);
    }
#ifdef _WIN32
    for (const wchar_t* dir : {L"C:\\Strawberry\\c\\bin", L"C:\\msys64\\ucrt64\\bin", L"C:\\msys64\\mingw64\\bin", L"C:\\mingw64\\bin",
                               L"C:\\Program Files\\mingw64\\bin"})
        candidates.push_back(fs::path(dir) / exe);
#endif
    for (auto& c : candidates)
        if (auto t = probe(c)) return *t;
    return Result<Toolchain>::failure("no C++ compiler found.\n" + kToolchainHelp);
}

fs::path scriptSdkIncludeDir() { return enginePaths().sdk; }

// ------------------------------------------------------------------ build --
int ScriptBuildResult::errors() const {
    return static_cast<int>(std::count_if(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& d) { return d.severity == Diagnostic::Severity::Error; }));
}
int ScriptBuildResult::warnings() const {
    return static_cast<int>(std::count_if(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& d) { return d.severity == Diagnostic::Severity::Warning; }));
}
Json ScriptBuildResult::toJson(size_t maxDiagnostics) const {
    Json d = Json::array();
    for (size_t i = 0; i < diagnostics.size() && i < maxDiagnostics; ++i) d.push_back(diagnostics[i].toJson());
    return {{"ok", ok},
            {"upToDate", upToDate},
            {"module", narrow(module.filename().wstring())},
            {"errors", errors()},
            {"warnings", warnings()},
            {"diagnostics", d},
            {"truncated", diagnostics.size() > maxDiagnostics},
            {"seconds", seconds},
            {"sources", sources},
            {"compiled", compiled}};
}

namespace {
std::vector<std::string> compileFlags(const ScriptBuildOptions& o, const Project& p) {
    std::vector<std::string> f = {"-std=c++20", "-Wall", "-Wextra", "-Wno-unused-parameter", "-fdiagnostics-color=never",
                                  "-fno-diagnostics-show-caret", "-fvisibility=hidden"};
    if (o.release) {
        f.push_back("-O2");
        f.push_back("-DNDEBUG");
    } else if (o.debug) {  // debugger sessions: no optimisation, full DWARF
        f.push_back("-O0");
        f.push_back("-g");
    } else {
        f.push_back("-O1");
        f.push_back("-g0");
    }
    f.push_back("-I" + narrow(o.sdkInclude.wstring()));
    f.push_back("-I" + narrow(p.scriptsDir().wstring()));
    return f;
}

std::vector<fs::path> dependencies(const fs::path& depFile) {
    std::vector<fs::path> deps;
    auto text = readFileText(depFile);
    if (!text) return deps;
    std::string s = *text, cur;
    size_t colon = s.find(": ");
    if (colon == std::string::npos) return deps;
    for (size_t i = colon + 2; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size() && (s[i + 1] == '\n' || s[i + 1] == '\r')) continue;
        if (c == '\\' && i + 1 < s.size() && s[i + 1] == ' ') {
            cur += ' ';
            ++i;
            continue;
        }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            if (!cur.empty()) deps.push_back(pathFromUtf8(cur));
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) deps.push_back(pathFromUtf8(cur));
    return deps;
}

bool upToDate(const fs::path& obj, const fs::path& dep) {
    std::error_code ec;
    if (!fs::exists(obj, ec) || !fs::exists(dep, ec)) return false;
    auto objTime = fs::last_write_time(obj, ec);
    if (ec) return false;
    auto deps = dependencies(dep);
    if (deps.empty()) return false;
    for (auto& d : deps) {
        auto t = fs::last_write_time(d, ec);
        if (ec || t >= objTime) return false;  // >=: coarse file-time volumes (exFAT, 10 ms) must not hide an edit
    }
    return true;
}

std::string flagsKey(const std::vector<std::string>& flags, const Toolchain& tc) {
    std::string k = narrow(tc.cxx.wstring()) + "|" + tc.version;
    for (auto& f : flags) k += "|" + f;
    k += "|abi" + std::to_string(FSCRIPT_ABI_VERSION) + "|sdk" FSCRIPT_SDK_VERSION;
    return k;
}
} // namespace

ScriptBuildResult buildScripts(const Project& project, const ScriptBuildOptions& optIn, const std::function<void(const std::string&)>& progress) {
    auto t0 = std::chrono::steady_clock::now();
    ScriptBuildResult res;
    auto finish = [&]() -> ScriptBuildResult& {
        res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return res;
    };
    auto say = [&](const std::string& s) {
        if (progress) progress(s);
    };
    ScriptBuildOptions opt = optIn;
    if (opt.sdkInclude.empty()) opt.sdkInclude = scriptSdkIncludeDir();
    std::error_code ec;
    if (!fs::exists(opt.sdkInclude / "fledge" / "script" / "script.h", ec)) {
        res.diagnostics.push_back({Diagnostic::Severity::Error, "", 0, 0, "Fledge Script SDK headers not found in " + narrow(opt.sdkInclude.wstring())});
        return finish();
    }
    if (opt.toolchain.cxx.empty()) {
        auto tc = findToolchain();
        if (!tc) {
            res.diagnostics.push_back({Diagnostic::Severity::Error, "", 0, 0, tc.error()});
            res.log = tc.error();
            return finish();
        }
        opt.toolchain = *tc;
    }
    fs::path scripts = project.scriptsDir();
    if (opt.release) opt.debug = false;
    const bool fixedName = opt.release || opt.debug;  // dev builds get a unique name per build (hot reload)
    fs::path out = project.scriptBuildDir() / (opt.release ? "release" : opt.debug ? "debug" : "dev");
    fs::path objDir = out / "obj", binDir = fixedName ? out : project.scriptBuildDir() / "bin", genDir = project.scriptBuildDir() / "gen";
    const fs::path fixedDll = binDir / pathFromUtf8(project.scriptModuleName() + (opt.debug ? "_debug.dll" : ".dll"));
    fs::create_directories(objDir, ec);
    fs::create_directories(binDir, ec);
    fs::create_directories(genDir, ec);

    // Sources: Scripts/**/*.cpp + the generated module runtime TU.
    std::vector<fs::path> sources;
    if (fs::is_directory(scripts, ec))
        for (auto it = fs::recursive_directory_iterator(scripts, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file() && (it->path().extension() == ".cpp" || it->path().extension() == ".cc")) sources.push_back(it->path());
    std::sort(sources.begin(), sources.end());
    res.sources = static_cast<int>(sources.size());
    if (sources.empty()) {
        res.diagnostics.push_back({Diagnostic::Severity::Error, "", 0, 0, "no .cpp files in " + narrow(scripts.wstring())});
        return finish();
    }
    fs::path moduleTu = genDir / "script_module.cpp";
    const std::string moduleSrc =
        "// Generated by the Fledge Script build - do not edit. Compiles the Fledge Script module runtime and entry point once.\n"
        "#define FLEDGE_SCRIPT_IMPLEMENTATION\n#include <fledge/script/script.h>\n";
    if (readFileText(moduleTu).value_or("") != moduleSrc) writeFileText(moduleTu, moduleSrc);
    sources.push_back(moduleTu);

    std::vector<std::string> flags = compileFlags(opt, project);
    std::string key = flagsKey(flags, opt.toolchain);
    bool flagsChanged = readFileText(out / "flags.txt").value_or("") != key;
    std::string cxx = narrow(opt.toolchain.cxx.wstring());

    fs::path pchDir = out / "pch";
    fs::path pchHeader = pchDir / "script_pch.h", pchFile = pchDir / "script_pch.h.gch", pchDep = pchDir / "script_pch.d";
    bool pchStale = opt.force || flagsChanged || !upToDate(pchFile, pchDep);  // SDK header changed -> rebuild every script
    struct Unit {
        fs::path src, obj, dep;
        bool needed = false;
        ProcessResult result;
    };
    std::vector<Unit> units;
    for (auto& s : sources) {
        fs::path rel = s.lexically_relative(s == moduleTu ? genDir : scripts);
        std::string flat = narrow(rel.generic_wstring());
        std::replace(flat.begin(), flat.end(), '/', '_');
        Unit u{s, objDir / pathFromUtf8(flat + ".o"), objDir / pathFromUtf8(flat + ".d")};
        u.needed = opt.force || flagsChanged || (pchStale && s != moduleTu) || !upToDate(u.obj, u.dep);
        units.push_back(std::move(u));
    }
    int toCompile = static_cast<int>(std::count_if(units.begin(), units.end(), [](const Unit& u) { return u.needed; }));
    say(std::format("Compiling {} of {} files with g++ {}", toCompile, units.size(), opt.toolchain.version));

    // Precompiled header for <fledge/script/script.h> (most of every script's compile time).
    // The generated module TU is compiled without it (it defines FLEDGE_SCRIPT_IMPLEMENTATION first).
    fs::create_directories(pchDir, ec);
    const std::string pchSrc = "// Generated by the Fledge Script build: precompiled SDK header.\n#include <fledge/script/script.h>\n";
    if (readFileText(pchHeader).value_or("") != pchSrc) writeFileText(pchHeader, pchSrc);
    bool usePch = toCompile > 0;
    if (usePch && pchStale) {
        say("Precompiling the Fledge Script SDK header");
        std::vector<std::string> args = {cxx};
        args.insert(args.end(), flags.begin(), flags.end());
        args.insert(args.end(), {"-x", "c++-header", "-MMD", "-MF", narrow(pchDep.wstring()), narrow(pchHeader.wstring()), "-o",
                                 narrow(pchFile.wstring())});
        auto pr = runProcess(args, scripts, opt.toolchain.cxx.parent_path());
        if (!pr.started || pr.exitCode != 0) {
            usePch = false;  // still builds, just slower
            fs::remove(pchFile, ec);
            res.log += "warning: precompiled header failed, building without it\n" + pr.output;
        }
    }

    std::atomic<size_t> next{0};
    std::atomic<int> done{0};
    std::mutex sayMutex;
    int jobs = opt.jobs > 0 ? opt.jobs : std::max(1u, std::thread::hardware_concurrency());
    auto worker = [&] {
        for (;;) {
            size_t i = next++;
            if (i >= units.size()) return;
            Unit& u = units[i];
            if (!u.needed) continue;
            std::vector<std::string> args = {cxx};
            if (usePch && u.src != moduleTu) args.insert(args.end(), {"-I" + narrow(pchDir.wstring()), "-include", "script_pch.h", "-Winvalid-pch"});
            args.insert(args.end(), flags.begin(), flags.end());
            args.insert(args.end(), {"-MMD", "-MF", narrow(u.dep.wstring()), "-c", narrow(u.src.wstring()), "-o", narrow(u.obj.wstring())});
            u.result = runProcess(args, scripts, opt.toolchain.cxx.parent_path());
            int d = ++done;
            std::lock_guard l(sayMutex);
            say(std::format("[{}/{}] {}{}", d, toCompile, narrow(u.src.filename().wstring()), u.result.exitCode == 0 ? "" : " (failed)"));
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < std::min(jobs, std::max(1, toCompile)); ++i) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    bool compileOk = true;
    for (auto& u : units) {
        if (!u.needed) continue;
        ++res.compiled;
        res.log += u.result.output;
        if (!u.result.started) {
            res.diagnostics.push_back({Diagnostic::Severity::Error, "", 0, 0, u.result.output});
            compileOk = false;
        } else if (u.result.exitCode != 0) {
            compileOk = false;
            fs::remove(u.obj, ec);  // never link a stale object
        }
    }
    auto diags = parseCompilerOutput(res.log, scripts);
    res.diagnostics.insert(res.diagnostics.end(), diags.begin(), diags.end());
    if (!compileOk) {
        if (res.errors() == 0) res.diagnostics.push_back({Diagnostic::Severity::Error, "", 0, 0, "compilation failed (see build log)"});
        writeFileText(project.scriptBuildDir() / "build.log", res.log);
        return finish();
    }
    writeFileText(out / "flags.txt", key);

    // Link when anything changed or the module is missing.
    std::string objList;
    for (auto& u : units) objList += narrow(u.obj.wstring()) + "\n";
    bool objsChanged = readFileText(out / "objects.txt").value_or("") != objList;
    fs::path current = fixedName ? fixedDll : project.currentScriptModule();
    if (res.compiled == 0 && !objsChanged && !current.empty() && fs::exists(current, ec)) {
        res.ok = true;
        res.upToDate = true;
        res.module = current;
        say("Scripts are up to date");
        return finish();
    }
    fs::path dll;
    if (fixedName) {
        dll = fixedDll;
    } else {
        int n = std::atoi(readFileText(project.scriptBuildDir() / "counter.txt").value_or("0").c_str()) + 1;
        writeFileText(project.scriptBuildDir() / "counter.txt", std::to_string(n));
        dll = binDir / pathFromUtf8(std::format("{}_{}.dll", project.scriptModuleName(), n));
    }
    std::vector<std::string> link = {cxx, "-shared", "-static", "-o", narrow(dll.wstring())};
    for (auto& u : units) link.push_back(narrow(u.obj.wstring()));
    if (opt.release) link.push_back("-s");
    say("Linking " + narrow(dll.filename().wstring()));
    auto lr = runProcess(link, scripts, opt.toolchain.cxx.parent_path());
    res.log += lr.output;
    auto ldiags = parseCompilerOutput(lr.output, scripts);
    res.diagnostics.insert(res.diagnostics.end(), ldiags.begin(), ldiags.end());
    if (!lr.started || lr.exitCode != 0) {
        if (res.errors() == 0) res.diagnostics.push_back({Diagnostic::Severity::Error, "", 0, 0, "link failed: " + lr.output});
        writeFileText(project.scriptBuildDir() / "build.log", res.log);
        return finish();
    }
    writeFileText(out / "objects.txt", objList);
    if (!fixedName) {
        writeFileText(project.scriptBuildDir() / "current.txt", narrow(dll.filename().wstring()));
        // Old builds: delete what is not locked (the editor may still hold the previous one).
        for (auto& f : fs::directory_iterator(binDir, ec))
            if (f.path() != dll && f.path().extension() == ".dll") fs::remove(f.path(), ec);
    }
    writeFileText(project.scriptBuildDir() / "build.log", res.log);
    (void)writeScriptIdeFiles(project, opt);
    res.ok = true;
    res.module = dll;
    say(std::format("Built {} ({} compiled)", narrow(dll.filename().wstring()), res.compiled));
    return finish();
}

// ---------------------------------------------------------------- sandbox --
Result<fs::path> resolveScriptPath(const Project& project, std::string_view relIn) {
    auto bad = [&](const std::string& why) { return Result<fs::path>::failure("rejected script path '" + std::string(relIn) + "': " + why); };
    std::string rel(relIn);
    if (rel.empty() || rel.size() > 200) return bad("empty or too long");
    if (rel.starts_with("Scripts/") || rel.starts_with("Scripts\\")) rel = rel.substr(8);  // tolerate a "Scripts/" prefix
    for (unsigned char c : rel)
        if (c < 32 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return bad("contains a forbidden character (: * ? \" < > | or control)");
    if (rel[0] == '/' || rel[0] == '\\') return bad("absolute paths are not allowed; use a path relative to Scripts/");
    fs::path p = pathFromUtf8(rel);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return bad("absolute paths are not allowed");
    static const char* reserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
                                     "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    for (auto& part : p) {
        std::string s = narrow(part.wstring());
        if (s == ".." || s == ".") return bad("'..' and '.' components are not allowed");
        if (s.empty() || s[0] == '.') return bad("hidden components are not allowed");
        if (s.back() == ' ' || s.back() == '.') return bad("components may not end with a space or dot");
        std::string stem = toLower(narrow(fs::path(part).stem().wstring()));
        for (const char* r : reserved)
            if (stem == r) return bad("reserved device name");
    }
    std::string ext = toLower(narrow(p.extension().wstring()));
    if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".inl") return bad("only .cpp, .h, .hpp and .inl files may be written");
    std::error_code ec;
    // Links / junctions inside Scripts/ could point anywhere: refuse any existing reparse point on the way.
    fs::path walk = project.scriptsDir();
    for (auto& part : p) {
        walk /= part;
        if (!fs::exists(walk, ec)) break;
        if (fs::is_symlink(walk, ec)) return bad("symbolic links are not allowed");
#ifdef _WIN32
        DWORD attr = GetFileAttributesW(walk.wstring().c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return bad("links / junctions are not allowed");
#endif
    }
    fs::path root = fs::weakly_canonical(project.scriptsDir(), ec);
    fs::path full = fs::weakly_canonical(project.scriptsDir() / p, ec);
    if (ec) return bad("cannot resolve");
    auto r = full.lexically_relative(root);
    if (r.empty() || narrow(r.begin()->wstring()) == "..") return bad("resolves outside the project's Scripts folder");
    return full;
}

// -------------------------------------------------------------- templates --
std::vector<std::string> scriptTemplateKinds() { return {"behaviour", "service", "character", "empty"}; }

bool isValidScriptClassName(std::string_view n) {
    if (n.empty() || n.size() > 64 || !(std::isalpha(static_cast<unsigned char>(n[0])) || n[0] == '_')) return false;
    for (char c : n)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    static const char* keywords[] = {"class", "struct", "int", "float", "void", "new", "delete", "public", "private", "namespace",
                                     "template", "return", "Behaviour", "Service", "Object", "Entity", "Scene"};
    for (const char* k : keywords)
        if (n == k) return false;
    return true;
}

std::string scriptTemplateSource(const std::string& kind, const std::string& name) {
    std::string t;
    if (kind == "service") {
        t = R"(// {{Name}} - game-wide service (one instance while playing). Access it from any script:
//     if (auto* s = GetService<{{Name}}>()) s->AddScore(10);
#include <fledge/script/script.h>

using namespace fledge::script;

class {{Name}} : public Service {
public:
    FLEDGE_PROPERTY(int, score, 0, Tooltip("Current score"));

    void OnStart() override { Log::Info("{{Name}} started"); }
    void OnUpdate(float dt) override {}
    void OnStop() override { Log::Info("{{Name}} stopped with score {}", score); }

    void AddScore(int points) { score += points; }
};
FLEDGE_SERVICE({{Name}})
)";
    } else if (kind == "character") {
        t = R"(// {{Name}} - third-person character controller: WASD/arrows move relative to the camera,
// Sprint (LeftShift) runs, Jump (Space) hops, the character turns towards its movement.
// Attach to the player root entity; set `camera` to the follow camera (or leave empty: "Follow Camera").
#include <fledge/script/script.h>

using namespace fledge::script;

class {{Name}} : public Behaviour {
public:
    FLEDGE_PROPERTY(float, walkSpeed, 4.0f, Range(0, 20), Tooltip("m/s"));
    FLEDGE_PROPERTY(float, sprintSpeed, 8.0f, Range(0, 30), Tooltip("m/s while Sprint is held"));
    FLEDGE_PROPERTY(float, turnSharpness, 12.0f, Range(0, 40));
    FLEDGE_PROPERTY(float, jumpSpeed, 5.0f, Range(0, 20));
    FLEDGE_PROPERTY(float, gravity, 15.0f, Range(0, 50));
    FLEDGE_PROPERTY(Entity, camera, Entity(), Tooltip("Camera used to orient the input; empty = 'Follow Camera'"));

    void OnStart() override {
        if (!camera) camera = Scene::Find("Follow Camera");
        m_groundY = GetEntity().Position().y;
    }

    void OnUpdate(float dt) override {
        Entity self = GetEntity();
        Vec3 forward = camera ? camera.Forward() : Vec3::Forward();
        forward.y = 0;
        forward = forward.Normalized();
        Vec3 right = Vec3::Cross(forward, Vec3::Up());
        Vec3 move = right * Input::Axis("MoveX") + forward * Input::Axis("MoveY");
        if (move.LengthSquared() > 1) move = move.Normalized();
        float speed = Input::Down("Sprint") ? sprintSpeed : walkSpeed;
        Vec3 pos = self.Position() + move * speed * dt;

        if (Input::Pressed("Jump") && pos.y <= m_groundY + 1e-3f) m_verticalSpeed = jumpSpeed;
        m_verticalSpeed -= gravity * dt;
        pos.y += m_verticalSpeed * dt;
        if (pos.y < m_groundY) {
            pos.y = m_groundY;
            m_verticalSpeed = 0;
        }
        self.SetPosition(pos);
        if (move.LengthSquared() > 1e-4f)
            self.SetRotation(Quat::Slerp(self.Rotation(), Quat::LookRotation(move), Damp(turnSharpness, dt)));
    }

private:
    float m_groundY = 0, m_verticalSpeed = 0;
};
FLEDGE_BEHAVIOUR({{Name}})
)";
    } else if (kind == "empty") {
        t = R"(// {{Name}}
#include <fledge/script/script.h>

using namespace fledge::script;
)";
    } else {
        t = R"(// {{Name}} - a Fledge Script behaviour. Attach it in the Inspector (Add Component > Script > {{Name}})
// or with the MCP tool attach_script. Properties appear in the Inspector and survive hot reload.
#include <fledge/script/script.h>

using namespace fledge::script;

class {{Name}} : public Behaviour {
public:
    FLEDGE_PROPERTY(float, speed, 1.0f, Range(0, 10), Tooltip("Example property"));

    void OnStart() override { Log::Info("{{Name}} started on '{}'", GetEntity().Name()); }

    void OnUpdate(float dt) override {
        // GetEntity().Translate(Vec3::Up() * speed * dt);
    }
};
FLEDGE_BEHAVIOUR({{Name}})
)";
    }
    size_t pos;
    while ((pos = t.find("{{Name}}")) != std::string::npos) t.replace(pos, 8, name);
    return t;
}

Result<fs::path> createScript(const Project& project, const std::string& className, const std::string& kind) {
    if (!isValidScriptClassName(className))
        return Result<fs::path>::failure("invalid class name '" + className + "' (C++ identifier: letters, digits, '_'; not a keyword)");
    auto kinds = scriptTemplateKinds();
    if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end())
        return Result<fs::path>::failure("unknown script template '" + kind + "' (behaviour, service, character, empty)");
    auto path = resolveScriptPath(project, className + ".cpp");
    if (!path) return Result<fs::path>::failure(path.error());
    std::error_code ec;
    if (fs::exists(*path, ec)) return Result<fs::path>::failure("Scripts/" + className + ".cpp already exists");
    fs::create_directories(path->parent_path(), ec);
    if (!writeFileText(*path, scriptTemplateSource(kind, className))) return Result<fs::path>::failure("cannot write " + narrow(path->wstring()));
    return *path;
}

Status writeScriptIdeFiles(const Project& project, const ScriptBuildOptions& optIn) {
    ScriptBuildOptions opt = optIn;
    if (opt.sdkInclude.empty()) opt.sdkInclude = scriptSdkIncludeDir();
    std::string cxx = opt.toolchain.cxx.empty() ? "g++" : narrow(opt.toolchain.cxx.wstring());
    std::vector<std::string> flags = compileFlags(opt, project);
    Json db = Json::array();
    std::error_code ec;
    fs::path scripts = project.scriptsDir();
    for (auto it = fs::recursive_directory_iterator(scripts, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        auto ext = it->path().extension();
        if (ext != ".cpp" && ext != ".h" && ext != ".hpp") continue;
        Json args = Json::array({cxx});
        for (auto& f : flags) args.push_back(f);
        if (ext != ".cpp") args.push_back("-xc++-header");
        args.push_back("-c");
        args.push_back(narrow(it->path().generic_wstring()));
        db.push_back({{"directory", narrow(scripts.generic_wstring())}, {"file", narrow(it->path().generic_wstring())}, {"arguments", args}});
    }
    bool ok = writeFileText(scripts / "compile_commands.json", db.dump(2));
    std::string clangd = "# Generated by Fledge: clangd reads compile_commands.json in this folder.\nCompileFlags:\n  Add: [-std=c++20]\n";
    ok &= writeFileText(scripts / ".clangd", clangd);
    fs::path cm = scripts / "CMakeLists.txt";
    if (!fs::exists(cm, ec)) {
        std::string sdk = narrow(opt.sdkInclude.generic_wstring());
        ok &= writeFileText(cm, std::format(
                                    "# IDE hint generated by Fledge (CLion / Visual Studio / Qt Creator can open this folder).\n"
                                    "# The engine does NOT use this file: the editor / fledgepack build the scripts themselves\n"
                                    "# (Intermediate/Scripts) and hot reload them. Safe to delete; it is not regenerated if edited.\n"
                                    "cmake_minimum_required(VERSION 3.20)\nproject({0} CXX)\nset(CMAKE_CXX_STANDARD 20)\n"
                                    "file(GLOB_RECURSE FLEDGE_SCRIPT_SOURCES CONFIGURE_DEPENDS ${{CMAKE_CURRENT_SOURCE_DIR}}/*.cpp)\n"
                                    "file(WRITE ${{CMAKE_BINARY_DIR}}/script_module.cpp \"#define FLEDGE_SCRIPT_IMPLEMENTATION\\n#include <fledge/script/script.h>\\n\")\n"
                                    "add_library({0} SHARED ${{FLEDGE_SCRIPT_SOURCES}} ${{CMAKE_BINARY_DIR}}/script_module.cpp)\n"
                                    "set_target_properties({0} PROPERTIES PREFIX \"\")\n"
                                    "target_include_directories({0} PRIVATE \"{1}\" ${{CMAKE_CURRENT_SOURCE_DIR}})\n",
                                    project.scriptModuleName(), sdk));
    }
    return ok ? Status::success() : Status::failure("cannot write IDE files in Scripts/");
}

} // namespace fledge
