#pragma once
// Persistent log file (1.2.1). The editor (Fledge.exe), the MCP server and the runtime write every log line to
//   <user data>/Logs/<prefix>-<yyyy-mm-dd>.log            (prefix "fledge"; packaged games: <user data>/Games/<game>/Logs)
// flushed after every line, so the file is complete up to the last line even when the process dies. A file that reaches
// maxBytes continues in <prefix>-<date>-2.log, -3.log ...; only the newest keepFiles logs (and crash dumps) are kept.
// --log-file <path> writes to exactly that file instead (appended; at maxBytes it is moved to <path>.1 and restarted).
//
// installCrashHandler(): on Windows an unhandled-exception filter writes the exception code, the faulting address as
// module + RVA, the last log lines and a minidump (<log>.dmp-style name next to the log, via dbghelp.dll loaded at run
// time, so no extra DLL import) before the process ends.
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "fledge/core/utility.h"

namespace fledge {

struct LogFileOptions {
    fs::path directory;        // folder for dated logs (created if missing)
    std::string prefix = "fledge";
    fs::path overridePath;     // --log-file: this exact file instead of dated logs in `directory`
    uint64_t maxBytes = 16ull << 20;
    int keepFiles = 10;        // dated logs kept in `directory` (oldest deleted first); crash dumps likewise
    std::string appName;       // written in the session header line
    bool backfill = true;      // first open: also write the lines logged so far (Logger ring), so startup is not lost
};

class LogFile {
public:
    static LogFile& get();
    // Opens (or switches to) the file and registers a Logger sink. Never throws; failure = Status error, nothing written.
    Status open(const LogFileOptions& options);
    void close();
    bool isOpen() const;
    fs::path path() const;  // current file
    fs::path pathNoLock() const { return m_path; }  // crash handler only
    const LogFileOptions& options() const { return m_opt; }
    // Writes one already formatted line (no lock when `noLock`, for the crash handler) and flushes.
    void writeLine(const std::string& line, bool noLock = false);
    // Dated logs of `prefix` in `dir`, oldest first.
    static std::vector<fs::path> listLogs(const fs::path& dir, const std::string& prefix);
    // "2026-11-03" (local time).
    static std::string today();

private:
    LogFile() = default;
    void openCurrent();   // m_mutex held
    void rotateIfNeeded();  // m_mutex held
    void prune();         // m_mutex held
    fs::path datedPath(const std::string& date, int index) const;
    mutable std::mutex m_mutex;
    LogFileOptions m_opt;
    std::FILE* m_file = nullptr;
    fs::path m_path;
    std::string m_date;
    int m_index = 1;
    uint64_t m_bytes = 0;
    int m_sink = 0;
};

// Value of "--log-file <path>" (or "--log-file=<path>") in argv, empty when absent.
fs::path logFileArgument(int argc, char** argv);
// The standard start for an app: LogFile in <logsDir> (or the --log-file path from argv) + the crash handler. Logs where the
// file is ("Log file: ...") and never fails the app (a read-only folder just means no file log).
void startPersistentLog(const std::string& appName, const fs::path& logsDir, int argc, char** argv);
// Windows: SetUnhandledExceptionFilter (code, address, module + RVA, last log lines, minidump). Elsewhere: nothing.
void installCrashHandler();
// Test hook for the crash handler (fledge_runtime --crash-test): raises an access violation.
[[noreturn]] void crashForTest();

} // namespace fledge
