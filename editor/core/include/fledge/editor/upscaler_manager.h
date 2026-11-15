#pragma once
// Upscaler version manager (editor / tools only): licence preview, download, SHA-256 verification,
// side-by-side installation and removal of vendor upscaler runtimes listed in
// fledge/render/upscalers.h. Nothing is downloaded unless the user has seen the licence text and
// accepted it in the UI (install() refuses otherwise). Network access goes through
// ai::HttpTransport (WinHTTP; tests use the mock transport or file:// URLs).
#include <functional>
#include <memory>
#include <string>

#include "fledge/ai/http.h"
#include "fledge/core/utility.h"
#include "fledge/render/upscalers.h"

namespace fledge::editor {

class UpscalerManager {
public:
    // root: version cache (default upscalers::cacheRoot()).
    explicit UpscalerManager(std::shared_ptr<ai::HttpTransport> transport, fs::path root = {});

    struct LicensePreview {
        bool ok = false;
        std::string text;      // licence text to show before the download
        bool hashMatches = false;  // text matches the catalog's pinned SHA-256
        std::string error;
    };
    LicensePreview fetchLicense(const upscalers::VersionEntry& v);

    using Progress = std::function<void(const std::string& file, uint64_t done, uint64_t total)>;
    // Downloads every file, verifies size + SHA-256, then atomically moves the folder into place.
    // licenseAccepted must come from the user's click on "Accept" after seeing fetchLicense().text.
    Status install(const upscalers::VersionEntry& v, bool licenseAccepted, const Progress& progress = {});
    Status remove(const upscalers::VersionEntry& v);
    bool isInstalled(const upscalers::VersionEntry& v) const { return upscalers::isInstalled(v, nullptr, m_root); }
    const fs::path& root() const { return m_root; }

    // Raw fetch (https:// via the transport, file:// from disk) - exposed for tests.
    Result<std::string> fetch(const std::string& url);

private:
    std::shared_ptr<ai::HttpTransport> m_http;
    fs::path m_root;
};

} // namespace fledge::editor
