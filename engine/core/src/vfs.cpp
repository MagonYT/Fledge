#include "fledge/core/vfs.h"

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fledge {

static std::string normalizeRel(std::string_view p) {
    std::string s(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    return s;
}

fs::path DirectoryProvider::resolve(std::string_view relPath) const {
    std::string rel = normalizeRel(relPath);
    fs::path p = (m_root / fs::path(widen(rel))).lexically_normal();
    fs::path root = m_root.lexically_normal();
    auto rs = root.native(), ps = p.native();
    if (ps.compare(0, rs.size(), rs) != 0) return {};  // escaped the mount root
    for (auto& part : fs::path(widen(rel)))
        if (part == "..") return {};
    return p;
}

std::optional<Bytes> DirectoryProvider::read(std::string_view relPath) const {
    auto p = resolve(relPath);
    if (p.empty()) return std::nullopt;
    return readFileBytes(p);
}

bool DirectoryProvider::exists(std::string_view relPath) const {
    auto p = resolve(relPath);
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(osPath(p), ec);
}

#ifdef _WIN32
// Win32 walk for "\\?\" roots: libstdc++'s directory_entry status queries do not work on extended-length paths.
static void listLong(const std::wstring& dir, const std::wstring& rel, std::vector<std::string>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring r = rel.empty() ? n : rel + L"/" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) listLong(dir + L"\\" + n, r, out);  // no junction loops
        } else {
            out.push_back(narrow(r));
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
#endif

std::vector<std::string> DirectoryProvider::list() const {
    std::vector<std::string> out;
    std::error_code ec;
#ifdef _WIN32
    fs::path root = osPath(m_root);
    if (root.wstring().rfind(L"\\\\?\\", 0) == 0) {  // project folder past MAX_PATH
        listLong(root.wstring(), L"", out);
        std::sort(out.begin(), out.end());
        return out;
    }
#endif
    if (!fs::is_directory(m_root, ec)) return out;
    const auto options = fs::directory_options::skip_permission_denied;
    for (auto it = fs::recursive_directory_iterator(m_root, options, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code fileError;
        if (!it->is_regular_file(fileError)) continue;
        const auto rel = it->path().lexically_relative(m_root);
        out.push_back(narrow(rel.generic_wstring()));
    }
    std::sort(out.begin(), out.end());
    return out;
}
bool DirectoryProvider::write(std::string_view relPath, std::span<const uint8_t> data) {
    auto p = resolve(relPath);
    if (p.empty()) return false;
    return writeFileBytes(p, data);
}

std::optional<Bytes> MemoryProvider::read(std::string_view relPath) const {
    auto it = m_files.find(normalizeRel(relPath));
    if (it == m_files.end()) return std::nullopt;
    return it->second;
}

bool MemoryProvider::exists(std::string_view relPath) const { return m_files.contains(normalizeRel(relPath)); }

std::vector<std::string> MemoryProvider::list() const {
    std::vector<std::string> out;
    for (auto& [k, v] : m_files) out.push_back(k);
    return out;
}

bool MemoryProvider::write(std::string_view relPath, std::span<const uint8_t> data) {
    std::string rel = normalizeRel(relPath);
    if (rel.empty() || rel.find("..") != std::string::npos) return false;
    m_files[rel] = Bytes(data.begin(), data.end());
    return true;
}

std::optional<Bytes> OverlayProvider::read(std::string_view relPath) const {
    for (auto& l : m_layers)
        if (auto b = l->read(relPath)) return b;
    return std::nullopt;
}
bool OverlayProvider::exists(std::string_view relPath) const {
    for (auto& l : m_layers)
        if (l->exists(relPath)) return true;
    return false;
}
std::vector<std::string> OverlayProvider::list() const {
    std::vector<std::string> out;
    for (auto& l : m_layers) {
        auto v = l->list();
        out.insert(out.end(), v.begin(), v.end());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool Vfs::split(std::string_view vpath, std::string& scheme, std::string& rel) {
    auto pos = vpath.find(":/");
    if (pos == std::string_view::npos || pos == 0) return false;
    scheme = std::string(vpath.substr(0, pos));
    rel = normalizeRel(vpath.substr(pos + 2));
    return true;
}

void Vfs::mount(std::string scheme, std::shared_ptr<FileProvider> provider) { m_mounts[std::move(scheme)] = std::move(provider); }
void Vfs::unmount(const std::string& scheme) { m_mounts.erase(scheme); }

FileProvider* Vfs::provider(const std::string& scheme) const {
    auto it = m_mounts.find(scheme);
    return it == m_mounts.end() ? nullptr : it->second.get();
}

std::optional<Bytes> Vfs::read(std::string_view vpath) const {
    std::string scheme, rel;
    if (!split(vpath, scheme, rel)) return std::nullopt;
    auto* p = provider(scheme);
    return p ? p->read(rel) : std::nullopt;
}

std::optional<std::string> Vfs::readText(std::string_view vpath) const {
    auto b = read(vpath);
    if (!b) return std::nullopt;
    return std::string(b->begin(), b->end());
}

bool Vfs::exists(std::string_view vpath) const {
    std::string scheme, rel;
    if (!split(vpath, scheme, rel)) return false;
    auto* p = provider(scheme);
    return p && p->exists(rel);
}

bool Vfs::write(std::string_view vpath, std::span<const uint8_t> data) {
    std::string scheme, rel;
    if (!split(vpath, scheme, rel)) return false;
    auto* p = provider(scheme);
    return p && p->writable() && p->write(rel, data);
}

bool Vfs::writeText(std::string_view vpath, std::string_view text) {
    return write(vpath, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

std::vector<std::string> Vfs::list(const std::string& scheme) const {
    auto* p = provider(scheme);
    return p ? p->list() : std::vector<std::string>{};
}

} // namespace fledge
