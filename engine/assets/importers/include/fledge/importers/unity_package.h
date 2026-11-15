#pragma once
// Unity .unitypackage reading and Unity material mapping (editor/tools only).
//
// A .unitypackage is a gzip-compressed tar archive with one folder per asset, named by the asset's Unity GUID:
//   <guid>/pathname    the asset's project path ("Assets/Props/Crate.fbx"), first line only
//   <guid>/asset       the file's bytes (absent for folders)
//   <guid>/asset.meta  Unity's importer settings (YAML)
//   <guid>/preview.png an optional thumbnail
// readPackage() streams the archive through archive::gunzip + TarParser (all limits apply) and writes asset bytes into
// a staging folder as <staging>/<guid>/asset. Nothing in a package is ever executed or loaded as code: C# scripts,
// DLLs, shaders and editor extensions are classified and skipped by the importer (store_import.h).
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "fledge/assets/image.h"
#include "fledge/assets/material.h"
#include "fledge/importers/archive.h"

namespace fledge::unity {

struct PackageEntry {
    std::string guid;          // 32 lowercase hex digits
    std::string pathname;      // sanitised project path ("Assets/Props/Crate.fbx"); empty = unusable
    std::string rawPathname;   // as stored
    fs::path assetFile;        // extracted bytes in the staging folder (empty = folder / no asset)
    uint64_t assetSize = 0;
    std::string meta;          // asset.meta text (YAML), at most 1 MiB
    bool hasPreview = false;
    bool folder = false;       // folderAsset: yes, or a pathname without an asset
};

struct Package {
    std::vector<PackageEntry> entries;  // sorted by pathname
    std::vector<std::string> warnings;  // skipped tar entries, missing pathnames ...
    uint64_t totalBytes = 0;
};

// Is this a GUID folder name Unity writes (32 hex digits)?
bool isUnityGuid(std::string_view s);
// Reads and validates a package; rejects files that are not gzip+tar or have no GUID entries at all.
Result<Package> readPackage(const fs::path& file, const fs::path& staging, const archive::Limits& limits = {});

enum class EntryKind { Folder, Model, Texture, Audio, Material, Font, Document, Script, Shader, UnityData, Unsupported };
const char* toString(EntryKind k);
// Classification by extension; `reason` explains skipped kinds ("C# script: code is never imported or run" ...).
EntryKind classify(std::string_view pathname, std::string* reason = nullptr);

// ---- materials (.mat YAML)
struct MaterialTexture {
    std::string guid;  // referenced texture's Unity GUID ("" = none)
    Vec2 scale{1, 1}, offset{0, 0};
};
struct UnityMaterial {
    std::string name;
    std::string shaderGuid;
    int64_t shaderFileId = 0;
    std::map<std::string, MaterialTexture> textures;  // "_MainTex" -> ...
    std::map<std::string, float> floats;              // m_Floats and m_Ints
    std::map<std::string, Vec4> colors;
    std::vector<std::string> keywords;
};
// Parses the subset of Unity's YAML serialisation used by materials (serializedVersion 2 and 3 layouts).
std::optional<UnityMaterial> parseMaterial(std::string_view yaml, std::string* error = nullptr);

enum class ShaderFamily { Standard, StandardSpecular, UrpLit, UrpSimpleLit, UrpUnlit, HdrpLit, Unlit, Unknown };
const char* toString(ShaderFamily f);
// By the shader reference (built-in Standard, Unity's URP / HDRP shader GUIDs) and, for custom shaders, by the
// property names it uses (_BaseMap -> URP-like, _MainTex -> Standard-like, _MaskMap -> HDRP-like).
ShaderFamily shaderFamily(const UnityMaterial& m, bool* guessed = nullptr);

// Texture access for the mapper (the importer provides package-backed implementations; tests provide fakes).
struct TextureProvider {
    std::function<std::string(const std::string& unityGuid)> fledgeGuid;     // "" = not imported
    std::function<const ImageData*(const std::string& unityGuid)> image;      // decoded pixels, nullptr = unavailable
    std::function<std::string(const std::string& name, const ImageData& image)> writePacked;  // new linear texture -> GUID
};
struct MaterialReport {
    std::string shader;                  // e.g. "URP Lit"
    std::vector<std::string> mapped;     // "_BaseMap -> baseColorTexture"
    std::vector<std::string> notMapped;  // "_ParallaxMap (height map): not supported" ...
};
// Best-effort mapping of Standard / URP Lit / HDRP Lit (and look-alikes) onto Fledge's metallic-roughness Material.
// Metallic-smoothness and mask maps are converted into an ORM texture via provider.writePacked.
Material mapMaterial(const UnityMaterial& m, const TextureProvider& provider, MaterialReport& report);
// Texture GUIDs a material uses as data (normal, metallic, mask, occlusion): they are imported as linear.
std::vector<std::string> linearTextureGuids(const UnityMaterial& m);
// Unity texture .meta: sRGBTexture: 0 -> linear; textureType: 1 -> normal map.
bool metaSaysLinear(std::string_view meta);

// Model importer settings from a model's .meta: "externalObjects" material remaps (name -> material GUID) and
// globalScale (1 = none).
struct ModelMeta {
    std::map<std::string, std::string> materialRemap;
    float globalScale = 1.0f;
};
ModelMeta parseModelMeta(std::string_view meta);

} // namespace fledge::unity
