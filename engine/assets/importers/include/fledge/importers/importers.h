#pragma once
// Source asset importers (editor/tools only; never linked into the runtime).
//   OBJ               -> MeshData (and copied into Content/ as-is; cooked by fledgepack)
//   PNG/JPG/TGA/BMP   -> ImageData (stb_image); .hdr -> HdrImage (linear float)
//   glTF 2.0 (.gltf/.glb, cgltf) -> meshes (.cmesh), textures, materials (.fmat)
//                        and a .fmodel describing the node hierarchy.
//   FBX (.fbx, ufbx)  -> the same layout as glTF, plus a .fchar (skeleton, skinned mesh, baked clips) when skinned.
// Every produced asset is registered (GUID) and gets a .meta sidecar.
// Store imports (.unitypackage, Fab .zip, links): fledge/importers/store_import.h.
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "fledge/anim/anim.h"
#include "fledge/assets/asset_registry.h"
#include "fledge/assets/cooked.h"
#include "fledge/assets/image.h"
#include "fledge/assets/material.h"
#include "fledge/core/features.h"
#include "fledge/core/mesh_data.h"
#include "fledge/core/vfs.h"
#include "fledge/importers/convert.h"

namespace fledge {

// Where and how a model (glTF / FBX / OBJ) is imported. The defaults give the classic layout content:/Models/<name>/.
struct ModelImportOptions {
    std::string destDir;  // content-relative parent folder ("" = "Models"); the model goes to <destDir>/<name>/
    std::string name;     // "" = the sanitised file stem
    convert::SourceSpace space = convert::SourceSpace::Fledge;  // OBJ only: glTF is always Y-up, FBX declares its axes
    float scale = 1.0f;                                        // extra uniform scale (after unit conversion)
    // External files referenced by the model may only be read from below this folder. Store imports always set it;
    // "" = FBX: the source's folder, glTF: no restriction (the behaviour of plain imports before 1.1.0).
    fs::path externalRoot;
    // texture file name as written in the model -> texture GUID already imported ("" = load it from externalRoot)
    std::function<std::string(const std::string& textureFile)> textureResolver;
    // material name -> material GUID to use instead of converting the file's own material ("" = convert it)
    std::function<std::string(const std::string& materialName)> materialResolver;
    Json provenance;  // stored as import.provenance in every created .meta (store, listing URL, licence ...)
    // 1.3.2 skinned characters (docs/ANIMATION.md "Skin weights"): vertices whose skin weights are all zero (UE5 glTF exports
    // have thousands; they skin to the model origin) are always reported as a warning. true = also give each of them the
    // joints / weights of its nearest weighted vertex (off by default: the file's weights are kept as they are).
    bool fixZeroWeights = false;
    // 1.3.3 (docs/ANIMATION.md "Zero-length normals"): vertices with zero-length / NaN normals (a Kinemation attachment mesh
    // had 421) and clips that key a bone's scale to exactly 0 are always reported (one warning per mesh set / character,
    // counts and examples). true = also regenerate those normals from the geometry (area-weighted face normals; valid normals
    // are kept). Off by default; recorded in the .meta and reused by re-imports. glTF / FBX (OBJ: reported only).
    bool fixZeroNormals = false;
};

// Why Unreal assets are refused, and what to do instead (shown by every import path).
inline constexpr const char* kUnrealAssetHelp =
    "Unreal Engine assets (.uasset / .umap) cannot be imported: the format is proprietary, changes with every engine version "
    "and is often cooked or encrypted. Open the asset in Unreal Engine and export it (Content Browser: right-click > Asset "
    "Actions > Export) as FBX or glTF, or download the FBX / glTF / OBJ format from Fab if the listing offers one, then import that.";

std::optional<MeshData> parseObj(std::string_view text, std::string* error = nullptr);
std::optional<ImageData> decodeImage(std::span<const uint8_t> bytes, std::string* error = nullptr);
// Radiance .hdr (and any stb_image format) as linear float RGBA.
std::optional<HdrImage> decodeHdr(std::span<const uint8_t> bytes, std::string* error = nullptr);

struct ImportResult {
    AssetRecord primary;               // the asset to use (mesh, texture, or the .fmodel)
    std::vector<AssetRecord> created;  // everything written/registered
    std::vector<std::string> warnings;
};

// Imports a file from anywhere on disk into the content mounted at "content:/".
// OBJ/images are validated and copied to Meshes/ or Textures/; glTF goes to Models/<name>/.
Result<ImportResult> importAsset(const fs::path& source, Vfs& vfs, AssetRegistry& registry, const ModelImportOptions& options = {});

// glTF 2.0 import. Output folder: content:/Models/<name>/ (source file and external
// buffers are copied to Models/<name>/source/ for re-import; they are editor-only
// and excluded from paks). Re-importing keeps GUIDs stable (same paths).
Result<ImportResult> importGltf(const fs::path& source, Vfs& vfs, AssetRegistry& registry, const ModelImportOptions& options = {});
// FBX 6100-7700, binary and ASCII (ufbx v0.23.1, MIT / public domain): converted to Fledge space (right-handed, Y-up,
// metres) from the axes and units the file declares. Meshes (per material, triangulated), node hierarchy, PBR / legacy
// Phong materials (separate roughness / metalness / AO maps are packed into one ORM texture), embedded and external
// textures (only from options.externalRoot), and - when skinned - the first skin with every animation stack baked as
// clips into <name>.fchar. External files (caches, references) are never followed.
Result<ImportResult> importFbx(const fs::path& source, Vfs& vfs, AssetRegistry& registry, const ModelImportOptions& options = {});
Result<anim::Character> importFbxCharacter(std::span<const uint8_t> fbxBytes, std::vector<std::string>* warnings = nullptr,
                                           bool fixZeroWeights = false, bool fixZeroNormals = false);
// The first skin of a glTF file with its skinned mesh and every animation (joint channels) as a character.
// importGltf also writes it as Models/<name>/<name>.fchar. baseDir resolves external buffers.
// 1.3.2: JOINTS_0/WEIGHTS_0 + JOINTS_1/WEIGHTS_1 (up to 8 influences; 8- / 16-bit joints, float / normalized weights),
// weights renormalised over all influences; warnings (zero-weight vertices with examples, joints outside the skin, broken
// primitives) go to *warnings; fixZeroWeights: see ModelImportOptions.
Result<anim::Character> importGltfCharacter(std::span<const uint8_t> gltfBytes, const fs::path& baseDir, std::vector<std::string>* warnings = nullptr,
                                            bool fixZeroWeights = false, bool fixZeroNormals = false);
// 1.3.3: the one warning for the static meshes of a model with zero-length normals (zero of vertices in meshes, examples
// "'mesh' (n of m: 1, 2, ...)"); fixed >= 0: fixZeroNormals ran and fixed that many.
std::string zeroNormalSetWarning(const std::string& model, size_t zero, size_t vertices, size_t meshes, const std::string& examples, int fixed);
// The zero-weight warning (count, vertex examples, the fix) for a character, applying fixZeroWeights when asked. {} = none.
std::vector<std::string> skinWeightWarnings(anim::Character& ch, const std::string& name, bool fixZeroWeights);
// 1.3.1: import warning for a character whose skeleton exceeds the GPU skinning limit of the edition (Fledge 256, Fledge
// Advanced 1024 = kMaxGpuSkinBones / kMaxGpuSkinBonesAdvanced in renderer.h; it still imports and is skinned on the CPU),
// "" when it fits. Joint indices are uint16 everywhere (import, .fchar, CPU skinning, the float GPU skin texture), so
// 1024+ bone rigs import unchanged.
std::string characterBoneWarning(const anim::Character& ch, const std::string& name, Edition edition);

// Re-runs the importer for an asset (glTF models re-import from their stored source copy;
// plain files only re-validate). Returns the affected records.
// fixZeroWeights / fixZeroNormals: nullopt = what the first import used (recorded in the .meta), otherwise overrides it (1.3.2 / 1.3.3).
Result<ImportResult> reimportAsset(const AssetRecord& asset, Vfs& vfs, AssetRegistry& registry, std::optional<bool> fixZeroWeights = std::nullopt,
                                   std::optional<bool> fixZeroNormals = std::nullopt);

} // namespace fledge
