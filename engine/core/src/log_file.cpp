#include "fledge/core/log_file.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <format>

#include "fledge/core/log.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>  // types only: MiniDumpWriteDump is loaded from dbghelp.dll at crash time (no import)
#include <share.h>
#else
#include <unistd.h>
#endif

namespace fledge {

namespace {
std::tm localNow(int* millis = nullptr) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    if (millis) *millis = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    return tm;
}

std::string timestamp() {
    int ms = 0;
    std::tm tm = localNow(&ms);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

std::FILE* openAppend(const fs::path& p) {
#ifdef _WIN32
    return _wfsopen(p.wstring().c_str(), L"ab", _SH_DENYNO);  // other Fledge processes may append to the same daily log
#else
    return std::fopen(p.string().c_str(), "ab");
#endif
}

unsigned long processId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}
} // namespace

LogFile& LogFile::get() {
    static LogFile instance;
    return instance;
}

std::string LogFile::today() {
    std::tm tm = localNow();
    return std::format("{:04}-{:02}-{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

fs::path LogFile::datedPath(const std::string& date, int index) const {
    std::string name = index <= 1 ? std::format("{}-{}.log", m_opt.prefix, date) : std::format("{}-{}-{}.log", m_opt.prefix, date, index);
    return m_opt.directory / name;
}

std::vector<fs::path> LogFile::listLogs(const fs::path& dir, const std::string& prefix) {
    std::vector<std::pair<fs::file_time_type, fs::path>> found;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string n = narrow(it->path().filename().wstring());
        if (n.size() > prefix.size() + 1 && n.compare(0, prefix.size() + 1, prefix + "-") == 0 && n.ends_with(".log"))
            found.emplace_back(fs::last_write_time(it->path(), ec), it->path());
    }
    std::sort(found.begin(), found.end());
    std::vector<fs::path> out;
    for (auto& f : found) out.push_back(f.second);
    return out;
}

Status LogFile::open(const LogFileOptions& options) {
    {
        std::lock_guard lock(m_mutex);
        if (m_file) std::fclose(m_file);
        m_file = nullptr;
        m_opt = options;
        if (m_opt.keepFiles < 1) m_opt.keepFiles = 1;
        if (m_opt.maxBytes < 4096) m_opt.maxBytes = 4096;
        m_date = today();
        m_index = 1;
        openCurrent();
        if (!m_file) return Status::failure("cannot open log file " + narrow(m_path.wstring()));
        if (m_opt.overridePath.empty()) prune();
    }
    writeLine(std::format("==== {} {} | {} | pid {} ====", timestamp(), m_opt.appName.empty() ? "Fledge" : m_opt.appName,
                          narrow(m_path.wstring()), processId()));
    if (m_opt.backfill && !m_sink)  // lines logged before the file was known (startup), with their time since start
        for (auto& e : Logger::get().recent(2000))
            writeLine(std::format("(+{:.3f} s)              [{:<5}] [{}] {}", e.timeSeconds, toString(e.level), e.category, e.message));
    if (!m_sink)
        m_sink = Logger::get().addSink([this](const LogEntry& e) {
            writeLine(std::format("{} [{:<5}] [{}] {}", timestamp(), toString(e.level), e.category, e.message));
        });
    return {};
}

void LogFile::close() {
    if (m_sink) Logger::get().removeSink(m_sink);
    m_sink = 0;
    std::lock_guard lock(m_mutex);
    if (m_file) std::fclose(m_file);
    m_file = nullptr;
}

bool LogFile::isOpen() const {
    std::lock_guard lock(m_mutex);
    return m_file != nullptr;
}

fs::path LogFile::path() const {
    std::lock_guard lock(m_mutex);
    return m_path;
}

void LogFile::openCurrent() {
    std::error_code ec;
    if (!m_opt.overridePath.empty()) {
        m_path = m_opt.overridePath;
        if (m_path.has_parent_path()) fs::create_directories(m_path.parent_path(), ec);
    } else {
        fs::create_directories(m_opt.directory, ec);
        // continue today's file unless it is full (then the next -N file)
        for (m_path = datedPath(m_date, m_index); fs::exists(m_path, ec) && fs::file_size(m_path, ec) >= m_opt.maxBytes; m_path = datedPath(m_date, ++m_index)) {}
    }
    m_file = openAppend(m_path);
    m_bytes = m_file ? static_cast<uint64_t>(fs::file_size(m_path, ec)) : 0;
}

void LogFile::rotateIfNeeded() {
    bool newDay = m_opt.overridePath.empty() && today() != m_date;
    if (!newDay && m_bytes < m_opt.maxBytes) return;
    if (m_file) std::fclose(m_file);
    m_file = nullptr;
    std::error_code ec;
    if (!m_opt.overridePath.empty()) {  // --log-file: keep one previous file
        fs::path old = m_path;
        old += ".1";
        fs::remove(old, ec);
        fs::rename(m_path, old, ec);
    } else if (newDay) {
        m_date = today();
        m_index = 1;
    } else {
        ++m_index;
    }
    openCurrent();
    if (m_opt.overridePath.empty()) prune();
}

void LogFile::prune() {
    std::error_code ec;
    auto logs = listLogs(m_opt.directory, m_opt.prefix);
    for (size_t i = 0; i + static_cast<size_t>(m_opt.keepFiles) < logs.size(); ++i)
        if (logs[i] != m_path) fs::remove(logs[i], ec);  // another process may still have it open: then it stays
    // crash dumps: the newest keepFiles
    std::vector<std::pair<fs::file_time_type, fs::path>> dumps;
    for (auto it = fs::directory_iterator(m_opt.directory, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".dmp") dumps.emplace_back(fs::last_write_time(it->path(), ec), it->path());
    std::sort(dumps.begin(), dumps.end());
    for (size_t i = 0; i + static_cast<size_t>(m_opt.keepFiles) < dumps.size(); ++i) fs::remove(dumps[i].second, ec);
}

void LogFile::writeLine(const std::string& line, bool noLock) {
    std::unique_lock lock(m_mutex, std::defer_lock);
    if (!noLock) lock.lock();
    if (!m_file) return;
    if (!noLock) rotateIfNeeded();
    if (!m_file) return;
    std::fwrite(line.data(), 1, line.size(), m_file);
    std::fputc('\n', m_file);
    std::fflush(m_file);  // every line: the file is complete up to here even if the process dies next
    m_bytes += line.size() + 1;
}

fs::path logFileArgument(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--log-file" && i + 1 < argc) return pathFromUtf8(argv[i + 1]);
        if (a.starts_with("--log-file=")) return pathFromUtf8(a.substr(11));
    }
    return {};
}

void startPersistentLog(const std::string& appName, const fs::path& logsDir, int argc, char** argv) {
    LogFileOptions o;
    o.appName = appName;
    o.directory = logsDir;
    o.overridePath = logFileArgument(argc, argv);
    if (auto st = LogFile::get().open(o); !st) FLEDGE_LOG_WARN("log", "No log file: {}", st.error);
    else FLEDGE_LOG_INFO("log", "Log file: {}", narrow(LogFile::get().path().wstring()));
    installCrashHandler();
}

#ifdef _WIN32
namespace {
LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_EXECUTE_HANDLER;
    LogFile& lf = LogFile::get();
    const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
    DWORD code = er ? er->ExceptionCode : 0;
    void* addr = er ? er->ExceptionAddress : nullptr;
    HMODULE mod = nullptr;
    wchar_t modName[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(addr), &mod))
        GetModuleFileNameW(mod, modName, MAX_PATH);
    uintptr_t rva = mod ? reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod) : 0;
    std::string modFile = narrow(fs::path(modName).filename().wstring());
    std::string what = code == EXCEPTION_ACCESS_VIOLATION ? "access violation" : code == EXCEPTION_STACK_OVERFLOW ? "stack overflow"
                     : code == EXCEPTION_INT_DIVIDE_BY_ZERO ? "integer divide by zero" : code == EXCEPTION_ILLEGAL_INSTRUCTION ? "illegal instruction"
                     : code == 0xE06D7363 ? "C++ exception" : "exception";
    std::string detail;
    if (code == EXCEPTION_ACCESS_VIOLATION && er && er->NumberParameters >= 2)
        detail = std::format(" ({} 0x{:x})", er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 1 ? "writing" : "executing",
                             static_cast<uint64_t>(er->ExceptionInformation[1]));
    std::string head = std::format("{} [fatal] [crash] Unhandled exception 0x{:08X} ({}){} at 0x{:x} = {}+0x{:x}", timestamp(), static_cast<unsigned>(code), what,
                                   detail, reinterpret_cast<uintptr_t>(addr), modFile, rva);
    std::fprintf(stderr, "%s\n", head.c_str());
    std::fflush(stderr);
    bool locked = false;
    {  // the crashing thread may hold the log mutex: never wait for it
        // (writeLine(noLock) only touches the FILE*, which has its own CRT lock)
        lf.writeLine(head, true);
        std::vector<LogEntry> last;
        if (Logger::get().tryRecent(30, last)) {
            locked = true;
            lf.writeLine("---- last log lines before the crash ----", true);
            for (auto& e : last) lf.writeLine(std::format("  [{:8.3f}][{}][{}] {}", e.timeSeconds, toString(e.level), e.category, e.message), true);
        }
    }
    if (!locked) lf.writeLine("(last log lines unavailable: the logger was busy)", true);
    // minidump next to the log (dbghelp.dll is part of Windows; loaded now so nothing imports it)
    fs::path logPath = lf.pathNoLock();
    fs::path dir = logPath.empty() ? fs::temp_directory_path() : logPath.parent_path();
    std::tm tm = localNow();
    fs::path dump = dir / std::format("{}-crash-{:04}{:02}{:02}-{:02}{:02}{:02}-{}.dmp", lf.options().prefix.empty() ? "fledge" : lf.options().prefix,
                                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, processId());
    using DumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                 PMINIDUMP_CALLBACK_INFORMATION);
    std::string dumpNote = "minidump: dbghelp.dll unavailable";
    if (HMODULE dbg = LoadLibraryW(L"dbghelp.dll"))
        if (auto fn = reinterpret_cast<DumpFn>(reinterpret_cast<void*>(GetProcAddress(dbg, "MiniDumpWriteDump")))) {
            HANDLE f = CreateFileW(dump.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
                BOOL ok = fn(GetCurrentProcess(), GetCurrentProcessId(), f, static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo),
                             ep ? &mei : nullptr, nullptr, nullptr);
                CloseHandle(f);
                dumpNote = ok ? "minidump: " + narrow(dump.wstring()) : "minidump: MiniDumpWriteDump failed";
            } else {
                dumpNote = "minidump: cannot create " + narrow(dump.wstring());
            }
        }
    lf.writeLine(dumpNote, true);
    std::fprintf(stderr, "%s\n", dumpNote.c_str());
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;  // end the process (no Windows error dialog)
}
} // namespace

void installCrashHandler() { SetUnhandledExceptionFilter(&crashFilter); }

void crashForTest() {
    FLEDGE_LOG_WARN("crash", "--crash-test: raising an access violation now");
    volatile uintptr_t address = 0x10;  // opaque to the optimiser: a real write fault inside this module
    *reinterpret_cast<volatile int*>(address) = 1;
    std::abort();
}
#else
void installCrashHandler() {}
void crashForTest() {
    FLEDGE_LOG_WARN("crash", "--crash-test");
    std::abort();
}
#endif

} // namespace fledge
