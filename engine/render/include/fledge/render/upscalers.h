#pragma once
// Upscaler versions: catalog, local version cache, hardware checks and runtime lookup.
//
// The catalog lists vendor upscaler runtimes that are really published (researched from the
// vendors' public repositories; nothing is invented). Entries that cannot be downloaded are kept
// visible but greyed out with a reason. Downloads happen only in the editor/tools
// (fledge::editor::UpscalerManager), only after the licence text has been shown and accepted by the
// user, and every file is SHA-256 verified. Versions live side by side in the cache:
//
//   <cache>/<version id>/<files...> + fledge_upscaler.json (manifest: hashes, licence acceptance)
//   cache = <install>/Upscalers in portable mode, else <user data>/Upscalers (%APPDATA%\Fledge).
//
// Packaging copies the project's selected version next to the game executable (plus its licence),
// so a shipped game never downloads anything. The engine builds and runs without any vendor SDK.
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "fledge/render/graphics_settings.h"

namespace fledge::upscalers {

struct FileEntry {
    std::string name;    // file name inside the version folder
    std::string url;     // https:// (catalog) or file:// (tests)
    uint64_t size = 0;
    std::string sha256;  // lowercase hex
};

struct VersionEntry {
    std::string id;       // "dlss-310.9.16"
    UpscalerKind kind = UpscalerKind::Off;
    std::string product;  // "NVIDIA DLSS Super Resolution"
    std::string family;   // "DLSS 3.7", "DLSS 310 (transformer)"
    std::string version;  // "310.9.16"
    std::string date;     // release date (YYYY-MM-DD)
    std::string notes;    // vendor release notes, shortened
    bool downloadable = true;
    std::string unavailableReason;  // when !downloadable
    std::string licenseName;
    FileEntry license;              // shown to the user before the download
    std::vector<FileEntry> files;   // runtime files
    std::string sourceUrl;          // release page
    uint64_t downloadSize() const;
};

const std::vector<VersionEntry>& catalog();
const VersionEntry* findVersion(std::string_view id);
std::vector<const VersionEntry*> versionsFor(UpscalerKind kind);

// ---- local cache -----------------------------------------------------------------------------
std::filesystem::path cacheRoot();                         // portable-aware (see header comment)
void setCacheRootOverride(const std::filesystem::path& p);  // tests; empty = default
std::filesystem::path versionDir(const VersionEntry& v, const std::filesystem::path& root = {});
// Installed = manifest present, licence accepted, every file present with the catalog size.
// (Hashes are verified at install time; packaging re-verifies them.)
bool isInstalled(const VersionEntry& v, std::string* why = nullptr, const std::filesystem::path& root = {});
std::vector<const VersionEntry*> installedVersions(UpscalerKind kind, const std::filesystem::path& root = {});
// Version to use: `requested` if installed (or present next to the exe), else the newest installed.
const VersionEntry* selectVersion(UpscalerKind kind, std::string_view requested, const std::filesystem::path& root = {});

// ---- hardware / integration --------------------------------------------------------------------
struct Check {
    bool ok = false;
    std::string reason;  // one line when !ok
};
// adapterDescription: RenderDevice::description() of the running device ("" = probe the primary GPU).
Check hardwareCheck(UpscalerKind kind, const std::string& adapterDescription);
// Whether this build can evaluate the vendor runtime on `backend` (XeSS: D3D12; DLSS: not integrated).
Check runtimeIntegration(UpscalerKind kind, rhi::Backend backend);
// Runtime DLL for a version: next to the executable (packaged games) first, then the cache.
std::filesystem::path runtimeLibrary(const VersionEntry& v, const std::filesystem::path& exeDir, const std::filesystem::path& root = {});

// Effective upscaler after availability checks + the fallback chain (DLSS -> XeSS -> FSR -> TAA,
// XeSS -> FSR -> TAA, FSR -> TAA). `note` explains every fallback in one line.
struct Resolution {
    UpscalerKind kind = UpscalerKind::Off;
    const VersionEntry* version = nullptr;  // vendor runtime to load (DLSS / XeSS)
    std::string note;
};
Resolution resolve(const GraphicsSettings& g, rhi::Backend backend, const std::string& adapterDescription, bool fsr1Available,
                   const std::filesystem::path& exeDir, const std::filesystem::path& root = {});

} // namespace fledge::upscalers
