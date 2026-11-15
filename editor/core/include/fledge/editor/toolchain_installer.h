#pragma once
// In-app "Install C++ toolchain" (docs/FLEDGE_SCRIPT.md): downloads a pinned portable MinGW-w64 g++ (WinLibs), checks its
// size and SHA-256, shows the licences first, and unpacks it into the Fledge data folder
// (<user data>/Toolchains/<id>/). Nothing is installed system-wide; PATH is not changed. Fledge Script then finds it
// automatically (findToolchain looks there).
#include <functional>
#include <memory>
#include <string>

#include "fledge/ai/http.h"
#include "fledge/core/utility.h"

namespace fledge::editor {

struct ToolchainEntry {
    std::string id;           // folder name under Toolchains/
    std::string title;
    std::string url;          // https:// (file:// in tests)
    std::string sha256;       // lowercase hex of the archive
    uint64_t size = 0;        // archive bytes (0 = unchecked, tests only)
    std::string gxx;          // g++ path inside the archive
    std::string licenses;     // shown to the user before the download
    std::string homepage;
};
const ToolchainEntry& pinnedToolchain();

// Minimal ZIP reader (stored + deflate, ZIP64): extracts every entry below dest. Rejects absolute paths and "..".
Status extractZip(const std::string& archive, const fs::path& dest, const std::function<void(size_t done, size_t total)>& progress = {});

class ToolchainInstaller {
public:
    // root: <user data>/Toolchains
    ToolchainInstaller(std::shared_ptr<ai::HttpTransport> http, fs::path root);
    using Progress = std::function<void(const std::string& stage, double fraction)>;
    // licenseAccepted must come from the user's click after seeing entry.licenses. Returns the installed g++.exe.
    Result<fs::path> install(const ToolchainEntry& entry, bool licenseAccepted, const Progress& progress = {});
    bool isInstalled(const ToolchainEntry& entry) const;
    fs::path compilerPath(const ToolchainEntry& entry) const;
    Status remove(const ToolchainEntry& entry);
    const fs::path& root() const { return m_root; }

private:
    Result<std::string> fetch(const std::string& url);
    std::shared_ptr<ai::HttpTransport> m_http;
    fs::path m_root;
};

} // namespace fledge::editor
