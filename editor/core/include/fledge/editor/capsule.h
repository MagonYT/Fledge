#pragma once
// Capsule: a whole project in one hash-verified file (<Name>.fledgecapsule). It holds the project
// files (no Intermediate/ or Build/) plus a manifest with the engine version, the project name and a
// SHA-256 for every file and for the manifest itself. Import verifies everything before writing a
// single file, and never overwrites an existing folder.
//
// Layout: "FLDGCAP1" | u64 manifest size | manifest JSON | file blobs (in manifest order) | "FLDGEND1"
#include <string>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/utility.h"

namespace fledge {

struct CapsuleInfo {
    std::string name, engineVersion, createdBy;
    size_t files = 0;
    uint64_t bytes = 0;
    Json manifest;
};

// projectRoot: the folder that holds <Name>.fledge.
Result<CapsuleInfo> exportCapsule(const fs::path& projectRoot, const fs::path& outFile);
// Reads and verifies a capsule (every hash) without extracting.
Result<CapsuleInfo> inspectCapsule(const fs::path& capsule);
// Verifies, then extracts into destDir/<name> (or <name>-2, ...). Returns the .fledge project file.
Result<fs::path> importCapsule(const fs::path& capsule, const fs::path& destDir);

} // namespace fledge
