#pragma once
// fledgepack library: turns a project into a standalone game folder.
// Used by the fledgepack CLI, the editor's Package button and the MCP
// build_package tool.
#include <functional>
#include <string>
#include <vector>

#include "fledge/assets/fpack.h"
#include "fledge/core/utility.h"

namespace fledge {

struct PackageOptions {
    fs::path projectFile;           // .fledge
    fs::path runtimeExe;            // default: fledge_runtime.exe next to the calling executable
    fs::path moduleDir;             // where gameplay module DLLs are found (default: runtime dir)
    fs::path outputDir;             // default: <project>/Build/<GameName>
    bool compress = true;           // legacy v1 only; v2 decides per chunk
    bool legacyPak = false;         // true = emit the v1 "CAGEPAK1" monolithic layout
    bool incremental = true;        // v2: reuse unchanged chunks from the previous package
    bool embedPak = true;           // false = write <GameName>.fpak next to the exe (sidecar)
    bool applyIcon = true;          // Windows only: embed project icon into the exe
    int subsystem = -1;             // -1 = project setting ("subsystem"), 2 = GUI, 3 = console
    bool buildScripts = true;       // build Scripts/ (Fledge Script, -O2) and ship <GameName>Scripts.dll
    fs::path compiler;              // g++ for scripts (default: Settings / FLEDGE_CXX / PATH)
    fs::path upscalerCache;         // upscaler version cache (default: upscalers::cacheRoot())
};

struct PackageResult {
    fs::path outputDir;
    fs::path executable;
    size_t fileCount = 0;
    uint64_t contentBytes = 0;
    uint64_t pakBytes = 0;
    std::vector<std::string> copiedModules;
    bool iconApplied = false;
    int subsystem = 0;              // PE subsystem written to the game exe (2 = GUI, 3 = console)
    std::string scriptModule;       // shipped Fledge Script module file name, empty if the project has no scripts
    std::string upscalerVersion;    // vendor upscaler runtime shipped next to the exe (graphics.upscaler = dlss/xess)
    int pakFormat = 0;              // container version written (1 = legacy, 2 = chunked)
    size_t chunksReused = 0;        // v2 incremental: chunks carried over from the previous package
    size_t chunksWritten = 0;       // v2: distinct chunks stored in this package
    std::vector<std::string> warnings;
};

// fraction is 0..1, or negative when the step has no known share of the work.
using ProgressFn = std::function<void(const std::string& message, float fraction)>;

Result<PackageResult> packageProject(const PackageOptions& options, const ProgressFn& progress = {});

// Builds only the .fpak image for a project (content + Game.fledge).
// legacyPak=false emits the v2 chunked container ("CAGEPAK2").
Result<Bytes> buildProjectPak(const fs::path& projectFile, bool compress, size_t* fileCount = nullptr,
                              uint64_t* contentBytes = nullptr,
                              const std::function<void(size_t index, size_t total, const std::string& relativePath)>& onFile = {},
                              const std::function<void(size_t done, size_t total, const std::string& path)>& onCompress = {},
                              bool legacyPak = false);

// Streams a v2 package straight to 'file' (chunk payloads are never all in
// memory) and finalizes it atomically (tmp + rename). 'previousFile', when it
// holds a readable v2 archive, seeds incremental reuse: chunks whose raw
// content reappears keep their stored payload instead of being re-compressed.
Result<FpackIndex> buildProjectFpackFile(const fs::path& projectFile, const fs::path& file, const fs::path& previousFile,
                                         size_t* fileCount = nullptr, uint64_t* contentBytes = nullptr,
                                         const std::function<void(size_t index, size_t total, const std::string& relativePath)>& onFile = {},
                                         const std::function<void(size_t done, size_t total, const std::string& path)>& onChunk = {});

// PE optional-header Subsystem field (2 = Windows GUI, 3 = console). Works on any host OS.
Result<int> readPeSubsystem(const fs::path& exe);
Status setPeSubsystem(const fs::path& exe, int subsystem);

// Windows: replace the exe's icon with an .ico file (RT_GROUP_ICON/RT_ICON).
Status setExecutableIcon(const fs::path& exe, const fs::path& icoFile);

} // namespace fledge
