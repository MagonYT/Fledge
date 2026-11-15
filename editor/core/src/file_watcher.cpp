#include "fledge/editor/file_watcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "fledge/core/log.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fledge {

namespace {

// ----------------------------------------------------------------- polling --
class PollingWatcher final : public FileWatcher {
public:
    PollingWatcher(fs::path root, int intervalMs) : m_root(std::move(root)), m_interval(intervalMs) { m_snapshot = scan(); }
    const char* backend() const override { return "polling"; }
    std::vector<std::string> poll() override {
        auto now = std::chrono::steady_clock::now();
        if (now - m_last < std::chrono::milliseconds(m_interval)) return {};
        m_last = now;
        auto snap = scan();
        std::vector<std::string> out;
        for (auto& [p, st] : snap) {
            auto it = m_snapshot.find(p);
            if (it == m_snapshot.end() || it->second != st) out.push_back(p);
        }
        for (auto& [p, st] : m_snapshot)
            if (!snap.contains(p)) out.push_back(p);
        m_snapshot = std::move(snap);
        return out;
    }

private:
    std::map<std::string, std::pair<uintmax_t, int64_t>> scan() const {
        std::map<std::string, std::pair<uintmax_t, int64_t>> m;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(m_root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            auto rel = narrow(fs::relative(it->path(), m_root, ec).generic_wstring());
            m[rel] = {it->file_size(ec), static_cast<int64_t>(it->last_write_time(ec).time_since_epoch().count())};
        }
        return m;
    }
    fs::path m_root;
    int m_interval;
    std::chrono::steady_clock::time_point m_last = std::chrono::steady_clock::now();
    std::map<std::string, std::pair<uintmax_t, int64_t>> m_snapshot;
};

#ifdef _WIN32
// ------------------------------------------------------------------- win32 --
class Win32Watcher final : public FileWatcher {
public:
    explicit Win32Watcher(const fs::path& root) {
        m_dir = CreateFileW(root.wstring().c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (m_dir != INVALID_HANDLE_VALUE) m_thread = std::thread([this] { run(); });
    }
    ~Win32Watcher() override {
        SetEvent(m_stop);
        if (m_thread.joinable()) m_thread.join();
        if (m_dir != INVALID_HANDLE_VALUE) CloseHandle(m_dir);
        CloseHandle(m_stop);
    }
    bool ok() const { return m_dir != INVALID_HANDLE_VALUE; }
    const char* backend() const override { return "ReadDirectoryChangesW"; }
    std::vector<std::string> poll() override {
        std::lock_guard l(m_mutex);
        std::vector<std::string> out(m_pending.begin(), m_pending.end());
        m_pending.clear();
        return out;
    }

private:
    void run() {
        alignas(DWORD) char buffer[64 * 1024];
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
        for (;;) {
            ResetEvent(ov.hEvent);
            if (!ReadDirectoryChangesW(m_dir, buffer, sizeof(buffer), TRUE, filter, nullptr, &ov, nullptr)) break;
            HANDLE handles[2] = {ov.hEvent, m_stop};
            DWORD w = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0) {
                CancelIoEx(m_dir, &ov);
                DWORD dummy;
                GetOverlappedResult(m_dir, &ov, &dummy, TRUE);
                break;
            }
            DWORD bytes = 0;
            if (!GetOverlappedResult(m_dir, &ov, &bytes, FALSE)) break;
            if (bytes == 0) continue;  // overflow: caller will get nothing; a rescan on focus covers it (TODO)
            std::lock_guard l(m_mutex);
            for (auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);;) {
                std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
                std::string rel = narrow(name);
                std::replace(rel.begin(), rel.end(), '\\', '/');
                m_pending.insert(rel);
                if (!info->NextEntryOffset) break;
                info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<char*>(info) + info->NextEntryOffset);
            }
        }
        CloseHandle(ov.hEvent);
    }
    HANDLE m_dir = INVALID_HANDLE_VALUE;
    HANDLE m_stop = nullptr;
    std::thread m_thread;
    std::mutex m_mutex;
    std::set<std::string> m_pending;
};
#endif

} // namespace

std::unique_ptr<FileWatcher> FileWatcher::create(const fs::path& root, bool preferNative, int pollIntervalMs) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return nullptr;
#ifdef _WIN32
    if (preferNative) {
        auto w = std::make_unique<Win32Watcher>(root);
        if (w->ok()) return w;
        FLEDGE_LOG_WARN("watcher", "ReadDirectoryChangesW unavailable, falling back to polling");
    }
#else
    (void)preferNative;
#endif
    return std::make_unique<PollingWatcher>(root, pollIntervalMs);
}

} // namespace fledge
