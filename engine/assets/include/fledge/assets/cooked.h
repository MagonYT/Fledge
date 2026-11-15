#pragma once
// Cooked (runtime) asset formats. All integers little-endian.
//
// .cmesh  "CMSH" | u32 version(1) | u32 vertexCount | u32 indexCount | u32 flags (bit0 = tangents)
//         | f32[3] boundsMin | f32[3] boundsMax | u32 vertexStride (bytes)
//         | vertices (interleaved: pos3 normal3 uv2 [tangent4]) | u32 indices[indexCount]
//
// .ctex   "CTEX" | u32 version(1) | u32 format (0 RGBA8, 1 BC1, 2 BC3) | u32 width | u32 height
//         | u32 mipCount | u32 flags (bit0 = sRGB) | per mip: u32 width | u32 height | u32 size | bytes
//         format 3 = RGBA16F (half floats, linear HDR - environment maps)
//
// Cooked file naming inside a pak / the cook cache: a source asset "X.obj"
// cooks to "X.obj.cmesh", "Y.png" to "Y.png.ctex". Assets already in a cooked
// format (".cmesh"/".ctex", e.g. glTF-imported meshes) keep their name.
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fledge/assets/image.h"
#include "fledge/core/mesh_data.h"
#include "fledge/core/utility.h"

namespace fledge {

// ------------------------------------------------------------------ mesh ---
Bytes writeCookedMesh(const MeshData& mesh);  // includes tangents if mesh.tangents is filled
std::optional<MeshData> readCookedMesh(std::span<const uint8_t> bytes, std::string* error = nullptr);
// generateTangents() lives in fledge/core/mesh_data.h

// --------------------------------------------------------------- texture ---
enum class TextureFormat : uint32_t { RGBA8 = 0, BC1 = 1, BC3 = 2, RGBA16F = 3 };
const char* toString(TextureFormat f);

struct TextureMip {
    int width = 0, height = 0;
    Bytes data;
};

struct TextureData {
    TextureFormat format = TextureFormat::RGBA8;
    bool srgb = true;
    std::vector<TextureMip> mips;  // mip 0 = full size
    int width() const { return mips.empty() ? 0 : mips[0].width; }
    int height() const { return mips.empty() ? 0 : mips[0].height; }
};

Bytes writeCookedTexture(const TextureData& tex);
std::optional<TextureData> readCookedTexture(std::span<const uint8_t> bytes, std::string* error = nullptr);
// Block-compressed -> RGBA8 (used when the GPU lacks S3TC support, and by tests).
TextureData decompressToRgba8(const TextureData& tex);

enum class TextureCompression { None, BC1, BC3, Auto };  // Auto: BC3 if the image has alpha, else BC1
TextureCompression textureCompressionFromString(std::string_view s);
const char* toString(TextureCompression c);

struct TextureCookOptions {
    bool mips = true;
    TextureCompression compression = TextureCompression::Auto;
    bool srgb = true;  // color textures: sRGB (gamma-correct mips); data maps (normal/MR/AO): linear
};
// Box-filtered mip chain down to 1x1. srgb=true averages in linear space (gamma-correct).
std::vector<ImageData> generateMipChain(const ImageData& image, bool srgb = true);
TextureData cookTexture(const ImageData& image, const TextureCookOptions& options);  // uses stb_dxt for BC1/BC3
TextureData uncompressedTexture(const ImageData& image, bool mips, bool srgb = true);  // editor: RGBA8 + mips

// Linear HDR image (RGBA float), e.g. an equirectangular environment.
struct HdrImage {
    int width = 0, height = 0;
    std::vector<float> rgba;
};
TextureData hdrToTexture(const HdrImage& img, int maxWidth = 2048);  // RGBA16F, 1 mip, box-downscaled if wider
HdrImage textureToHdr(const TextureData& tex);                          // RGBA16F/RGBA8(sRGB) mip 0 -> linear floats

// --------------------------------------------------------------- cooking ---
uint64_t contentHash(std::span<const uint8_t> data, uint64_t seed = 1469598103934665603ull);  // FNV-1a 64
// "Meshes/a.obj" -> "Meshes/a.obj.cmesh"; already-cooked names unchanged; "" if not cookable.
std::string cookedPathFor(const std::string& relPath);
// True for editor-only source files that must not be shipped (.gltf/.glb/.bin, .fbx, .meta, .fimport reports).
bool isEditorOnlyFile(const std::string& relPath);

} // namespace fledge
