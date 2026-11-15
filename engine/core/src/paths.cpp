#include "fledge/core/paths.h"

#include <cstdio>
#include <cstdlib>
#include <format>
#include <mutex>

#include "fledge/core/json.h"

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace fledge {

namespace {
bool pathExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

fs::path normal(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p.lexically_normal() : c;
}
} // namespace

EnginePaths resolveEnginePaths(const fs::path& exeDirIn, bool forcePortable, const fs::path& appDataDir, const std::string& userDataFolder) {
    EnginePaths p;
    p.exeDir = normal(exeDirIn);
    fs::path manifest = p.exeDir / "fledge_paths.json";
    bool fromManifest = false;
    if (pathExists(manifest)) {
        if (auto text = readFileText(manifest)) {
            Json j = Json::parse(*text, nullptr, false);
            if (j.is_object()) {
                auto get = [&](const char* key, const fs::path& fallback) {
                    if (!j.contains(key) || !j[key].is_string()) return fallback;
                    fs::path v = pathFromUtf8(j[key].get<std::string>());
                    return normal(v.is_absolute() ? v : p.exeDir / v);
                };
                p.root = get("root", p.exeDir.parent_path());
                p.resources = get("resources", p.root / "resources");
                p.templates = get("templates", p.root / "templates");
                p.samples = get("samples", p.root / "samples");
                p.docs = get("docs", p.root / "docs");
                p.licenses = get("licenses", p.root / "licenses");
                p.sdk = get("sdk", p.root / "sdk" / "include");
                p.userData = get("userData", {});
                p.buildTree = true;
                fromManifest = true;
            }
        }
    }
    if (!fromManifest) {
        fs::path up = p.exeDir.parent_path();
        bool installLayout = pathExists(up / "resources") || pathExists(up / "templates") || p.exeDir.filename() == "bin";
        p.root = installLayout ? up : p.exeDir;
        p.resources = p.root / "resources";
        p.templates = p.root / "templates";
        p.samples = p.root / "samples";
        p.docs = p.root / "docs";
        p.licenses = p.root / "licenses";
        p.sdk = p.root / "sdk" / "include";
    }
    p.portable = forcePortable || pathExists(p.root / "portable.txt") || pathExists(p.exeDir / "portable.txt");
    if (!p.userData.empty()) {
        // build tree: the manifest's private UserData folder
    } else if (p.portable || appDataDir.empty()) p.userData = p.root / "UserData";
    else p.userData = appDataDir / pathFromUtf8(userDataFolder.empty() ? std::string("Fledge") : userDataFolder);
    return p;
}

std::string EnginePaths::describe() const {
    auto s = [](const fs::path& x) { return narrow(x.wstring()); };
    return std::format("exeDir: {}\nroot: {}\nresources: {}\ntemplates: {}\nsamples: {}\ndocs: {}\nlicenses: {}\nsdk: {}\nuserData: {}\n"
                       "portable: {}\nbuildTree: {}\n",
                       s(exeDir), s(root), s(resources), s(templates), s(samples), s(docs), s(licenses), s(sdk), s(userData),
                       portable ? 1 : 0, buildTree ? 1 : 0);
}

namespace {
std::once_flag g_once;
EnginePaths g_paths;
bool g_forcePortable = false;
std::string g_userDataFolder = "Fledge";
} // namespace

fs::path platformAppDataDir() {
#ifdef _WIN32
    if (const wchar_t* a = _wgetenv(L"APPDATA"); a && *a) return fs::path(a);
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return fs::path(x);
    if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h) / ".config";
#endif
    return {};
}

void initEnginePaths(bool forcePortable) {
    g_forcePortable = forcePortable;
    std::call_once(g_once, [] {
        g_paths = resolveEnginePaths(executablePath().parent_path(), g_forcePortable, platformAppDataDir(), g_userDataFolder);
        // FLEDGE_USER_DATA overrides the user data folder (tests, sandboxes, CI). Portable mode wins: a portable
        // install keeps everything next to itself.
        if (!g_paths.portable) {
#ifdef _WIN32
            if (const wchar_t* o = _wgetenv(L"FLEDGE_USER_DATA"); o && *o) g_paths.userData = fs::path(o);
#else
            if (const char* o = std::getenv("FLEDGE_USER_DATA"); o && *o) g_paths.userData = fs::path(o);
#endif
        }
    });
}

void setUserDataFolderName(const std::string& name) { g_userDataFolder = name.empty() ? std::string("Fledge") : name; }

const EnginePaths& enginePaths() {
    initEnginePaths(g_forcePortable);
    return g_paths;
}

void attachParentConsoleIfNeeded() {
#ifdef _WIN32
    auto valid = [](DWORD which) {
        HANDLE h = GetStdHandle(which);
        return h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    };
    if (valid(STD_OUTPUT_HANDLE)) {
        // Inherited pipe/file: make sure the CRT streams are bound to it (GUI subsystem CRTs may not be).
        auto bind = [](FILE* stream, DWORD which) {
            if (_fileno(stream) >= 0) return;
            FILE* f = nullptr;
            if (freopen_s(&f, "NUL", "w", stream) != 0 || _fileno(stream) < 0) return;
            int fd = _open_osfhandle(reinterpret_cast<intptr_t>(GetStdHandle(which)), _O_TEXT);
            if (fd >= 0) _dup2(fd, _fileno(stream));
        };
        bind(stdout, STD_OUTPUT_HANDLE);
        if (valid(STD_ERROR_HANDLE)) bind(stderr, STD_ERROR_HANDLE);
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        (void)freopen_s(&f, "CONOUT$", "w", stdout);
        (void)freopen_s(&f, "CONOUT$", "w", stderr);
    }
#endif
}

} // namespace fledge
