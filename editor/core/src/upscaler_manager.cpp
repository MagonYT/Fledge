#include "fledge/editor/upscaler_manager.h"

#include <chrono>
#include <format>

#include "fledge/core/json.h"
#include "fledge/core/log.h"
#include "fledge/core/sha256.h"

namespace fledge::editor {

namespace {
fs::path fromFileUrl(const std::string& url) {
    std::string p = url.substr(7);  // file://
    if (p.size() > 2 && p[0] == '/' && p[2] == ':') p.erase(0, 1);  // file:///C:/x
    return pathFromUtf8(p);
}
std::string nowIso() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}
} // namespace

UpscalerManager::UpscalerManager(std::shared_ptr<ai::HttpTransport> transport, fs::path root)
    : m_http(std::move(transport)), m_root(root.empty() ? upscalers::cacheRoot() : std::move(root)) {}

Result<std::string> UpscalerManager::fetch(const std::string& url) {
    if (url.rfind("file://", 0) == 0) {
        auto bytes = readFileBytes(fromFileUrl(url));
        if (!bytes) return Result<std::string>::failure("cannot read " + url);
        return std::string(bytes->begin(), bytes->end());
    }
    if (url.rfind("https://", 0) != 0) return Result<std::string>::failure("only https:// (or file:// in tests) downloads are allowed: " + url);
    if (!m_http) return Result<std::string>::failure("no HTTP transport");
    ai::HttpRequest req;
    req.method = "GET";
    req.url = url;
    req.timeoutMs = 600000;
    req.headers["User-Agent"] = "Fledge-upscaler-manager";
    ai::HttpResponse r = m_http->send(req);
    if (r.status < 200 || r.status >= 300)
        return Result<std::string>::failure(r.status == 0 ? "download failed: " + r.error : std::format("download failed: HTTP {}", r.status));
    return std::move(r.body);
}

UpscalerManager::LicensePreview UpscalerManager::fetchLicense(const upscalers::VersionEntry& v) {
    LicensePreview p;
    if (v.license.url.empty()) {
        p.error = "no licence file is published for this version";
        return p;
    }
    auto text = fetch(v.license.url);
    if (!text) {
        p.error = text.error();
        return p;
    }
    p.ok = true;
    p.text = std::move(*text);
    p.hashMatches = v.license.sha256.empty() || Sha256::hex(p.text) == v.license.sha256;
    return p;
}

Status UpscalerManager::install(const upscalers::VersionEntry& v, bool licenseAccepted, const Progress& progress) {
    if (!v.downloadable) return Status::failure(v.id + " cannot be downloaded: " + v.unavailableReason);
    if (!licenseAccepted)
        return Status::failure("the licence (" + v.licenseName + ") must be shown and accepted by the user before " + v.id + " is downloaded");
    if (v.files.empty()) return Status::failure("nothing to download for " + v.id);
    fs::path dst = upscalers::versionDir(v, m_root);
    fs::path tmp = dst;
    tmp += ".partial";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    if (ec) return Status::failure("cannot create " + narrow(tmp.wstring()) + ": " + ec.message());
    auto cleanup = [&] {
        std::error_code e2;
        fs::remove_all(tmp, e2);
    };
    Json manifest = {{"id", v.id}, {"product", v.product}, {"version", v.version}, {"licenseAccepted", true}, {"licenseAcceptedAt", nowIso()},
                     {"license", v.licenseName}, {"files", Json::array()}};
    std::vector<upscalers::FileEntry> all = v.files;
    if (!v.license.url.empty()) all.push_back(v.license);
    for (const auto& f : all) {
        if (progress) progress(f.name, 0, f.size);
        auto data = fetch(f.url);
        if (!data) {
            cleanup();
            return Status::failure(f.name + ": " + data.error());
        }
        if (f.size && data->size() != f.size) {
            cleanup();
            return Status::failure(std::format("{}: size mismatch ({} bytes, expected {})", f.name, data->size(), f.size));
        }
        std::string hash = Sha256::hex(*data);
        if (!f.sha256.empty() && hash != f.sha256) {
            cleanup();
            return Status::failure(std::format("{}: SHA-256 mismatch (got {}, expected {}) - download rejected", f.name, hash, f.sha256));
        }
        if (!writeFileBytes(tmp / pathFromUtf8(f.name), Bytes(data->begin(), data->end()))) {
            cleanup();
            return Status::failure("cannot write " + f.name);
        }
        manifest["files"].push_back({{"name", f.name}, {"size", data->size()}, {"sha256", hash}});
        if (progress) progress(f.name, data->size(), f.size);
    }
    std::string m = manifest.dump(2);
    if (!writeFileBytes(tmp / "fledge_upscaler.json", Bytes(m.begin(), m.end()))) {
        cleanup();
        return Status::failure("cannot write the version manifest");
    }
    fs::remove_all(dst, ec);
    fs::rename(tmp, dst, ec);
    if (ec) {
        cleanup();
        return Status::failure("cannot move the download into place: " + ec.message());
    }
    FLEDGE_LOG_INFO("upscalers", "Installed {} ({} files) into {}", v.id, all.size(), narrow(dst.wstring()));
    return Status::success();
}

Status UpscalerManager::remove(const upscalers::VersionEntry& v) {
    fs::path dir = upscalers::versionDir(v, m_root);
    std::error_code ec;
    if (!fs::exists(dir, ec)) return Status::failure(v.id + " is not installed");
    // only ever delete inside the version cache
    fs::path canonRoot = fs::weakly_canonical(m_root, ec), canonDir = fs::weakly_canonical(dir, ec);
    if (canonDir.parent_path() != canonRoot) return Status::failure("refusing to delete outside the upscaler cache");
    fs::remove_all(dir, ec);
    if (ec) return Status::failure("cannot remove " + narrow(dir.wstring()) + ": " + ec.message());
    return Status::success();
}

} // namespace fledge::editor
