#pragma once
// Cooker: source assets in <project>/Content -> cooked runtime formats in
// <project>/Intermediate/Cooked (the cook cache).
//   OBJ  -> .cmesh (indexed, interleaved, bounds, tangents optional)
//   PNG/JPG/TGA/BMP -> .ctex (mip chain, BC1/BC3 via stb_dxt or RGBA8)
// Incremental: each output records the FNV-1a hash of (source bytes + cook
// settings) in CookCache.json; unchanged sources are skipped.
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "fledge/framework/project.h"

namespace fledge {

struct CookOptions {
    bool force = false;  // ignore the cache
    // Package: cook only textures referenced by scenes, materials, models, characters, or the project environment.
    // Editor Cook still cooks the whole library so unused maps stay previewable.
    bool usedTexturesOnly = false;
};

struct CookReport {
    size_t cooked = 0;
    size_t skipped = 0;      // up to date
    size_t removed = 0;      // stale outputs deleted
    size_t passthrough = 0;  // files shipped as-is (scenes, materials, already-cooked meshes...)
    size_t unusedTextures = 0;  // library textures left uncooked (usedTexturesOnly)
    uint64_t sourceBytes = 0, cookedBytes = 0;
    std::vector<std::string> cookedFiles;
};

bool isContentTexture(const std::string& relPath);
// PNG/JPG/HDR/… paths referenced from scenes, assigned materials/models/characters, UI, or RenderSettings.environment.
std::unordered_set<std::string> collectUsedTextures(const Project& project);

Result<CookReport> cookProject(const Project& project, const CookOptions& options = {},
                               const std::function<void(const std::string&)>& progress = {});

} // namespace fledge
