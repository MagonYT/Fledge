#pragma once
// Logging. Every message passes through fledge::Redactor before reaching any
// sink, so secrets registered with the redactor never hit disk or the console.
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <format>

namespace fledge {

enum class LogLevel : uint8_t { Trace, Debug, Info, Warn, Error };

const char* toString(LogLevel level);

struct LogEntry {
    LogLevel level;
    double timeSeconds;   // since logger start
    std::string category;
    std::string message;  // already redacted
};

class Logger {
public:
    using Sink = std::function<void(const LogEntry&)>;

    static Logger& get();

    void log(LogLevel level, std::string_view category, std::string_view message);
    int addSink(Sink sink);
    void removeSink(int id);
    void setMinLevel(LogLevel l) { m_minLevel = l; }
    LogLevel minLevel() const { return m_minLevel; }
    // stderr is used by default so that stdout stays clean for MCP stdio.
    void setConsoleEnabled(bool enabled) { m_console = enabled; }

    // Ring buffer of recent entries (for editor console and MCP read_log).
    std::vector<LogEntry> recent(size_t maxCount = 200) const;
    // As recent(), but gives up instead of waiting when the logger is busy (crash handler: the crashing thread may hold it).
    bool tryRecent(size_t maxCount, std::vector<LogEntry>& out) const;
    void clearRecent();

private:
    Logger();
    mutable std::mutex m_mutex;
    std::vector<std::pair<int, Sink>> m_sinks;
    std::deque<LogEntry> m_ring;
    size_t m_ringCapacity = 2000;
    int m_nextSink = 1;
    LogLevel m_minLevel = LogLevel::Info;
    bool m_console = true;
};

template <typename... Args>
void logf(LogLevel level, std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
    if (level < Logger::get().minLevel()) return;
    Logger::get().log(level, category, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace fledge

#define FLEDGE_LOG_TRACE(cat, ...) ::fledge::logf(::fledge::LogLevel::Trace, cat, __VA_ARGS__)
#define FLEDGE_LOG_DEBUG(cat, ...) ::fledge::logf(::fledge::LogLevel::Debug, cat, __VA_ARGS__)
#define FLEDGE_LOG_INFO(cat, ...)  ::fledge::logf(::fledge::LogLevel::Info, cat, __VA_ARGS__)
#define FLEDGE_LOG_WARN(cat, ...)  ::fledge::logf(::fledge::LogLevel::Warn, cat, __VA_ARGS__)
#define FLEDGE_LOG_ERROR(cat, ...) ::fledge::logf(::fledge::LogLevel::Error, cat, __VA_ARGS__)
