#include "fledge/core/utility.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fledge {

fs::path osPath(const fs::path& path) {
#ifdef _WIN32
    std::wstring raw = path.wstring();
    if (raw.size() < 240 || raw.rfind(L"\\\\?\\", 0) == 0) return path;
    auto sep = [](wchar_t ch) { return ch == L'\\' || ch == L'/'; };
    if (raw.size() > 2 && sep(raw[0]) && sep(raw[1])) {  // UNC \\server\share\...: libstdc++ would fold the leading separators
        std::wstring rest = fs::path(raw.substr(2)).lexically_normal().wstring();
        std::replace(rest.begin(), rest.end(), L'/', L'\\');
        return fs::path(L"\\\\?\\UNC\\" + rest);
    }
    std::error_code ec;
    fs::path abs = path.is_absolute() ? path : fs::absolute(path, ec);
    if (ec) return path;
    std::wstring w = abs.lexically_normal().wstring();
    std::replace(w.begin(), w.end(), L'/', L'\\');
    if (w.size() < 240) return fs::path(w);
    return fs::path(L"\\\\?\\" + w);
#else
    return path;
#endif
}

std::string fileErrorReason(const fs::path& path) {
    std::string p = narrow(path.wstring());
    std::error_code ec;
    fs::path os = osPath(path);
    auto st = fs::status(os, ec);
    if (ec || !fs::exists(st)) return p + ": file does not exist";
    if (fs::is_directory(st)) return p + ": is a directory, not a file";
    std::ifstream f(os, std::ios::binary);
    if (!f) return p + ": cannot be opened (access denied or in use by another program)";
    return p + ": read error";
}

std::optional<Bytes> readFileBytes(const fs::path& path, std::string* error) {
    auto b = readFileBytes(path);
    if (!b && error) *error = fileErrorReason(path);
    return b;
}

std::optional<Bytes> readFileBytes(const fs::path& path) {
    std::ifstream f(osPath(path), std::ios::binary);
    if (!f) return std::nullopt;
    f.seekg(0, std::ios::end);
    auto size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    Bytes data(size);
    if (size && !f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size))) return std::nullopt;
    return data;
}

std::optional<std::string> readFileText(const fs::path& path) {
    auto b = readFileBytes(path);
    if (!b) return std::nullopt;
    return std::string(b->begin(), b->end());
}

// fs::create_directories walks parent_path() up to the root, which does not terminate for "\\?\" paths in libstdc++:
// create the missing levels left to right instead.
static void createDirectoriesLong(const fs::path& dir, std::error_code& ec) {
#ifdef _WIN32
    std::wstring w = dir.wstring();
    if (w.rfind(L"\\\\?\\", 0) == 0) {
        size_t start = w.rfind(L"\\\\?\\UNC\\", 0) == 0 ? w.find(L'\\', w.find(L'\\', 8) + 1) : w.find(L'\\', 4);  // past the drive / share
        for (size_t pos = start; pos != std::wstring::npos && pos < w.size();) {
            pos = w.find(L'\\', pos + 1);
            std::wstring part = w.substr(0, pos);
            if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
                return;
            }
        }
        return;
    }
#endif
    fs::create_directories(dir, ec);
}
bool writeFileBytes(const fs::path& path, std::span<const uint8_t> data) {
    std::error_code ec;
    fs::path os = osPath(path);
    if (os.has_parent_path()) createDirectoriesLong(os.parent_path(), ec);
    std::ofstream f(os, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

bool writeFileText(const fs::path& path, std::string_view text) {
    return writeFileBytes(path, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(std::span<const uint8_t> data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63]; out += kB64[v & 63];
    }
    if (i < data.size()) {
        uint32_t v = data[i] << 16;
        if (i + 1 < data.size()) v |= data[i + 1] << 8;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += (i + 1 < data.size()) ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

Bytes base64Decode(std::string_view text) {
    std::array<int, 256> map;
    map.fill(-1);
    for (int i = 0; i < 64; ++i) map[static_cast<uint8_t>(kB64[i])] = i;
    Bytes out;
    uint32_t buf = 0;
    int bits = 0;
    for (char c : text) {
        int v = map[static_cast<uint8_t>(c)];
        if (v < 0) continue;
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

uint32_t crc32(std::span<const uint8_t> data, uint32_t seed) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (uint8_t b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::string toLower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}
bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
bool startsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

#ifdef _WIN32
std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
fs::path executablePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return fs::path(std::wstring(buf, n));
}
#else
std::wstring widen(std::string_view s) { return std::wstring(s.begin(), s.end()); }
std::string narrow(std::wstring_view s) { return std::string(s.begin(), s.end()); }
fs::path executablePath() { return fs::canonical("/proc/self/exe"); }
#endif

} // namespace fledge
