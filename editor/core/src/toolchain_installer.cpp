#include "fledge/editor/toolchain_installer.h"

#include <stb_image.h>

#include <cstring>
#include <format>

#include "fledge/core/log.h"
#include "fledge/core/sha256.h"

namespace fledge::editor {

const ToolchainEntry& pinnedToolchain() {
    // WinLibs GCC 16.2.0 (POSIX threads, SEH) + MinGW-w64 14.0.0, MSVCRT runtime (matches the editor's CRT family).
    // SHA-256 and size as published for the GitHub release asset.
    static const ToolchainEntry e{
        "winlibs-gcc-16.2.0-msvcrt-r3",
        "MinGW-w64 GCC 16.2.0 (WinLibs, 64-bit, MSVCRT)",
        "https://github.com/brechtsanders/winlibs_mingw/releases/download/16.2.0posix-23.1.3-14.0.0-msvcrt-r3/"
        "winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64msvcrt-14.0.0-r3.zip",
        "62af32ea58acdea1318ea5dd8eb7017eaee694dd40b269d52a59c769dcfefb82",
        277203392,
        "mingw64/bin/g++.exe",
        "This downloads a third-party compiler toolchain (about 277 MB) from the WinLibs project (brechtsanders/winlibs_mingw "
        "on GitHub) and unpacks it into Fledge's data folder only. It is not part of Fledge and is not redistributed by Fledge.\n\n"
        "Main licences of the parts:\n"
        "- GCC compilers: GNU GPL v3 or later; the GCC runtime libraries (libgcc, libstdc++) carry the GCC Runtime Library "
        "Exception, so programs you build (your game scripts) are not covered by the GPL.\n"
        "- GNU Binutils (as, ld): GNU GPL v3 or later.\n"
        "- MinGW-w64 runtime and headers: ZPL 2.1 / MIT-style licences (see the COPYING files in the toolchain).\n"
        "- winpthreads: MIT / BSD-style. Other bundled tools keep their own licences (see the licenses folder inside it).\n\n"
        "By choosing Accept and install you accept these licences. The archive is verified against a pinned SHA-256 before "
        "anything is unpacked.",
        "https://winlibs.com"};
    return e;
}

// ---------------------------------------------------------------- zip ----
namespace {
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t rd64(const uint8_t* p) { return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32); }
} // namespace

Status extractZip(const std::string& archive, const fs::path& dest, const std::function<void(size_t, size_t)>& progress) {
    const auto* d = reinterpret_cast<const uint8_t*>(archive.data());
    size_t n = archive.size();
    if (n < 22) return Status::failure("not a zip archive");
    // end of central directory (search backwards, comment up to 64 KB)
    size_t eocd = std::string::npos;
    for (size_t i = n - 22 + 1; i-- > 0 && n - i <= 22 + 65535;)
        if (rd32(d + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) return Status::failure("zip: end of central directory not found");
    uint64_t count = rd16(d + eocd + 10), cdSize = rd32(d + eocd + 12), cdOff = rd32(d + eocd + 16);
    if ((count == 0xFFFF || cdOff == 0xFFFFFFFF) && eocd >= 20 && rd32(d + eocd - 20) == 0x07064b50) {  // ZIP64
        uint64_t z64 = rd64(d + eocd - 20 + 8);
        if (z64 + 56 > n || rd32(d + z64) != 0x06064b50) return Status::failure("zip: bad ZIP64 record");
        count = rd64(d + z64 + 32);
        cdSize = rd64(d + z64 + 40);
        cdOff = rd64(d + z64 + 48);
    }
    if (cdOff + cdSize > n) return Status::failure("zip: central directory out of range");
    std::error_code ec;
    fs::create_directories(dest, ec);
    size_t p = static_cast<size_t>(cdOff);
    for (uint64_t i = 0; i < count; ++i) {
        if (p + 46 > n || rd32(d + p) != 0x02014b50) return Status::failure("zip: bad central directory entry");
        uint16_t method = rd16(d + p + 10);
        uint64_t csize = rd32(d + p + 20), usize = rd32(d + p + 24);
        uint16_t nameLen = rd16(d + p + 28), extraLen = rd16(d + p + 30), commentLen = rd16(d + p + 32);
        uint64_t local = rd32(d + p + 42);
        std::string name(reinterpret_cast<const char*>(d + p + 46), nameLen);
        // ZIP64 extra field: the 0xFFFFFFFF values follow in order
        const uint8_t* ex = d + p + 46 + nameLen;
        for (size_t e = 0; e + 4 <= extraLen;) {
            uint16_t id = rd16(ex + e), sz = rd16(ex + e + 2);
            if (id == 0x0001) {
                size_t q = e + 4;
                if (usize == 0xFFFFFFFF && q + 8 <= e + 4 + sz) usize = rd64(ex + q), q += 8;
                if (csize == 0xFFFFFFFF && q + 8 <= e + 4 + sz) csize = rd64(ex + q), q += 8;
                if (local == 0xFFFFFFFF && q + 8 <= e + 4 + sz) local = rd64(ex + q), q += 8;
            }
            e += 4 + sz;
        }
        p += 46 + nameLen + extraLen + commentLen;
        // sanitise the path
        for (auto& c : name)
            if (c == '\\') c = '/';
        if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos || ("/" + name + "/").find("/../") != std::string::npos)
            return Status::failure("zip: unsafe path " + name);
        fs::path out = dest / pathFromUtf8(name);
        if (name.back() == '/') {
            fs::create_directories(out, ec);
            continue;
        }
        if (local + 30 > n || rd32(d + local) != 0x04034b50) return Status::failure("zip: bad local header for " + name);
        size_t dataOff = static_cast<size_t>(local) + 30 + rd16(d + local + 26) + rd16(d + local + 28);
        if (dataOff + csize > n) return Status::failure("zip: data out of range for " + name);
        std::string bytes;
        if (method == 0) {
            bytes.assign(archive.data() + dataOff, static_cast<size_t>(csize));
        } else if (method == 8) {
            if (usize > 0x7FFFFFFF || csize > 0x7FFFFFFF) return Status::failure("zip: entry too large: " + name);
            bytes.resize(static_cast<size_t>(usize));
            int got = usize == 0 ? 0
                                 : stbi_zlib_decode_noheader_buffer(bytes.data(), static_cast<int>(usize), archive.data() + dataOff, static_cast<int>(csize));
            if (got != static_cast<int>(usize)) return Status::failure("zip: cannot inflate " + name);
        } else {
            return Status::failure(std::format("zip: unsupported compression {} for {}", method, name));
        }
        fs::create_directories(out.parent_path(), ec);
        if (!writeFileBytes(out, Bytes(bytes.begin(), bytes.end()))) return Status::failure("cannot write " + narrow(out.wstring()));
        if (progress && (i % 64 == 0 || i + 1 == count)) progress(static_cast<size_t>(i + 1), static_cast<size_t>(count));
    }
    return {};
}

// ---------------------------------------------------------- installer ----
ToolchainInstaller::ToolchainInstaller(std::shared_ptr<ai::HttpTransport> http, fs::path root) : m_http(std::move(http)), m_root(std::move(root)) {}

fs::path ToolchainInstaller::compilerPath(const ToolchainEntry& e) const { return m_root / pathFromUtf8(e.id) / pathFromUtf8(e.gxx); }

bool ToolchainInstaller::isInstalled(const ToolchainEntry& e) const {
    std::error_code ec;
    return fs::is_regular_file(compilerPath(e), ec);
}

Status ToolchainInstaller::remove(const ToolchainEntry& e) {
    std::error_code ec;
    fs::remove_all(m_root / pathFromUtf8(e.id), ec);
    return ec ? Status::failure(ec.message()) : Status::success();
}

Result<std::string> ToolchainInstaller::fetch(const std::string& url) {
    if (url.rfind("file://", 0) == 0) {
        std::string p = url.substr(7);
        if (p.size() > 2 && p[0] == '/' && p[2] == ':') p = p.substr(1);  // file:///C:/...
        auto bytes = readFileBytes(pathFromUtf8(p));
        if (!bytes) return Result<std::string>::failure("cannot read " + url);
        return std::string(bytes->begin(), bytes->end());
    }
    if (url.rfind("https://", 0) != 0) return Result<std::string>::failure("only https:// downloads are allowed: " + url);
    if (!m_http) return Result<std::string>::failure("no HTTP transport");
    ai::HttpRequest req;
    req.method = "GET";
    req.url = url;
    req.timeoutMs = 3600000;
    req.headers["User-Agent"] = "Fledge-toolchain-installer";
    ai::HttpResponse r = m_http->send(req);
    if (r.status < 200 || r.status >= 300)
        return Result<std::string>::failure(r.status == 0 ? "download failed: " + r.error : std::format("download failed: HTTP {}", r.status));
    return std::move(r.body);
}

Result<fs::path> ToolchainInstaller::install(const ToolchainEntry& e, bool licenseAccepted, const Progress& progress) {
    if (!licenseAccepted) return Result<fs::path>::failure("the licences were not accepted");
    if (isInstalled(e)) return compilerPath(e);
    if (progress) progress("Downloading " + e.title, 0);
    auto archive = fetch(e.url);
    if (!archive) return Result<fs::path>::failure(archive.error());
    if (e.size && archive->size() != e.size)
        return Result<fs::path>::failure(std::format("download has {} bytes, expected {} (incomplete or changed): nothing was installed", archive->size(), e.size));
    if (progress) progress("Verifying SHA-256", 0.5);
    std::string sha = Sha256::hex(*archive);
    if (sha != e.sha256) return Result<fs::path>::failure("SHA-256 mismatch (" + sha + "): the download was rejected, nothing was installed");
    fs::path final = m_root / pathFromUtf8(e.id), partial = m_root / pathFromUtf8(e.id + ".partial");
    std::error_code ec;
    fs::remove_all(partial, ec);
    auto st = extractZip(*archive, partial, [&](size_t done, size_t total) {
        if (progress) progress("Unpacking", 0.55 + 0.45 * static_cast<double>(done) / static_cast<double>(std::max<size_t>(total, 1)));
    });
    if (!st) {
        fs::remove_all(partial, ec);
        return Result<fs::path>::failure(st.error);
    }
    if (!fs::is_regular_file(partial / pathFromUtf8(e.gxx), ec)) {
        fs::remove_all(partial, ec);
        return Result<fs::path>::failure("the archive does not contain " + e.gxx);
    }
    fs::remove_all(final, ec);
    fs::rename(partial, final, ec);
    if (ec) return Result<fs::path>::failure("cannot move the toolchain into place: " + ec.message());
    FLEDGE_LOG_INFO("scripts", "Installed {} into {}", e.title, narrow(final.wstring()));
    return compilerPath(e);
}

} // namespace fledge::editor
