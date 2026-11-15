#include "fledge/core/log.h"
#include "fledge/core/redact.h"

#include <chrono>
#include <cstdio>

namespace fledge {

namespace {
const auto g_start = std::chrono::steady_clock::now();
}

const char* toString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
    }
    return "?";
}

Logger::Logger() = default;

Logger& Logger::get() {
    static Logger instance;
    return instance;
}

void Logger::log(LogLevel level, std::string_view category, std::string_view message) {
    if (level < m_minLevel) return;
    LogEntry e;
    e.level = level;
    e.timeSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    e.category = std::string(category);
    e.message = Redactor::get().redact(message);

    std::vector<Sink> sinks;
    {
        std::lock_guard lock(m_mutex);
        m_ring.push_back(e);
        while (m_ring.size() > m_ringCapacity) m_ring.pop_front();
        for (auto& [id, s] : m_sinks) sinks.push_back(s);
        if (m_console) {
            std::fprintf(stderr, "[%8.3f][%-5s][%s] %s\n", e.timeSeconds, toString(level), e.category.c_str(),
                         e.message.c_str());
            std::fflush(stderr);
        }
    }
    for (auto& s : sinks) s(e);
}

int Logger::addSink(Sink sink) {
    std::lock_guard lock(m_mutex);
    int id = m_nextSink++;
    m_sinks.emplace_back(id, std::move(sink));
    return id;
}

void Logger::removeSink(int id) {
    std::lock_guard lock(m_mutex);
    std::erase_if(m_sinks, [id](auto& p) { return p.first == id; });
}

std::vector<LogEntry> Logger::recent(size_t maxCount) const {
    std::lock_guard lock(m_mutex);
    std::vector<LogEntry> out;
    size_t start = m_ring.size() > maxCount ? m_ring.size() - maxCount : 0;
    for (size_t i = start; i < m_ring.size(); ++i) out.push_back(m_ring[i]);
    return out;
}

bool Logger::tryRecent(size_t maxCount, std::vector<LogEntry>& out) const {
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    size_t start = m_ring.size() > maxCount ? m_ring.size() - maxCount : 0;
    for (size_t i = start; i < m_ring.size(); ++i) out.push_back(m_ring[i]);
    return true;
}

void Logger::clearRecent() {
    std::lock_guard lock(m_mutex);
    m_ring.clear();
}

} // namespace fledge
