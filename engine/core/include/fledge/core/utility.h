#pragma once
// Small utilities: non-copyable base, scope exit, file I/O, base64, CRC32,
// string helpers, Result<T>.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fledge {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

struct NonCopyable {
    NonCopyable() = default;
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
};

template <typename F>
class ScopeExit {
public:
    explicit ScopeExit(F f) : m_f(std::move(f)) {}
    ~ScopeExit() { if (m_active) m_f(); }
    void dismiss() { m_active = false; }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
private:
    F m_f;
    bool m_active = true;
};

// Very small expected-like result. error() is a human readable message.
template <typename T>
class Result {
public:
    Result(T value) : m_value(std::move(value)) {}
    static Result failure(std::string err) { Result r; r.m_error = std::move(err); return r; }
    explicit operator bool() const { return m_value.has_value(); }
    bool ok() const { return m_value.has_value(); }
    T& value() { return *m_value; }
    const T& value() const { return *m_value; }
    T* operator->() { return &*m_value; }
    const T* operator->() const { return &*m_value; }
    T& operator*() { return *m_value; }
    const T& operator*() const { return *m_value; }
    const std::string& error() const { return m_error; }
private:
    Result() = default;
    std::optional<T> m_value;
    std::string m_error;
};

struct Status {
    bool ok = true;
    std::string error;
    static Status success() { return {}; }
    static Status failure(std::string e) { return {false, std::move(e)}; }
    explicit operator bool() const { return ok; }
};

// Windows: the extended-length form of a long path (\\?\C:\..., \\?\UNC\server\share\...), absolute and normalised, so the
// file APIs work past MAX_PATH (260 chars) whatever the system's LongPathsEnabled setting. Paths under 240 chars and other
// platforms are returned unchanged. Every engine file helper below goes through it.
fs::path osPath(const fs::path& path);
// Why a file cannot be read, naming the path: "... does not exist", "... is a directory", "... access denied" ...
std::string fileErrorReason(const fs::path& path);

std::optional<Bytes> readFileBytes(const fs::path& path);
std::optional<Bytes> readFileBytes(const fs::path& path, std::string* error);  // error = fileErrorReason on failure
std::optional<std::string> readFileText(const fs::path& path);
bool writeFileBytes(const fs::path& path, std::span<const uint8_t> data);
bool writeFileText(const fs::path& path, std::string_view text);

std::string base64Encode(std::span<const uint8_t> data);
Bytes base64Decode(std::string_view text);

uint32_t crc32(std::span<const uint8_t> data, uint32_t seed = 0);

std::string toLower(std::string_view s);
bool endsWith(std::string_view s, std::string_view suffix);
bool startsWith(std::string_view s, std::string_view prefix);

// UTF-8 <-> wide helpers (Windows APIs). On non-Windows they are byte copies.
std::wstring widen(std::string_view s);
std::string narrow(std::wstring_view s);

// UTF-8 string -> filesystem path (avoids deprecated u8path).
inline fs::path pathFromUtf8(std::string_view s) { return fs::path(widen(s)); }

// Path of the running executable.
fs::path executablePath();

} // namespace fledge
