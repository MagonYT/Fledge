#pragma once
// Conversions for assets made for other engines (editor/tools only):
//   * texture channel packing: Unity metallic-smoothness, HDRP mask maps, separate roughness / metalness / AO maps and
//     Unreal ORM maps -> the glTF metallic-roughness layout Fledge renders (R = occlusion, G = roughness, B = metallic);
//   * coordinate spaces: Unity (left-handed, Y-up, +Z forward, metres) and Unreal (left-handed, Z-up, +X forward,
//     centimetres) -> Fledge (right-handed, Y-up, -Z forward, metres, the glTF convention);
//   * file / asset name sanitising;
//   * guessing a loose texture's role from its file name (BaseColor, Normal, ORM ...).
#include <optional>
#include <string>
#include <string_view>

#include "fledge/assets/image.h"
#include "fledge/core/math.h"
#include "fledge/core/mesh_data.h"

namespace fledge::convert {

// One output channel: a channel of an image (resampled to the output size) or `constant` when image is null,
// times `scale`, then optionally inverted (1 - v).
struct Channel {
    const ImageData* image = nullptr;
    int channel = 0;  // 0 R, 1 G, 2 B, 3 A
    bool invert = false;
    float scale = 1.0f;
    float constant = 1.0f;  // 0..1
};
// Size = the largest input (nearest-neighbour resampling for smaller ones); 1x1 when no input has an image.
ImageData packChannels(const Channel& r, const Channel& g, const Channel& b, const Channel& a = {});

// Unity Standard / URP Lit metallic map (R = metallic, A = smoothness) -> ORM. smoothnessSource = the image holding
// smoothness in alpha (the metallic map, or the albedo with _SmoothnessTextureChannel = 1); occlusion = Unity's AO map
// (green channel), may be null.
ImageData unityMetallicSmoothnessToOrm(const ImageData* metallic, const ImageData* smoothnessSource, float glossScale,
                                        const ImageData* occlusion);
// HDRP mask map (R metallic, G occlusion, B detail mask, A smoothness) -> ORM.
ImageData hdrpMaskToOrm(const ImageData& mask);

enum class SourceSpace { Fledge, Unity, Unreal };  // Fledge == glTF
const char* toString(SourceSpace s);
// Point / direction / rotation conversion into Fledge space (Unreal also scales cm -> m for points).
Vec3 toFledgePoint(Vec3 p, SourceSpace s);
Vec3 toFledgeDirection(Vec3 d, SourceSpace s);
Quat toFledgeRotation(Quat q, SourceSpace s);
// Converts a mesh in place: positions, normals, tangents, and the triangle winding when the change of basis mirrors.
void convertMesh(MeshData& mesh, SourceSpace s, float extraScale = 1.0f);

// A safe file / folder name component: [A-Za-z0-9_-.] kept, anything else '_', no leading dots, at most maxLen.
std::string sanitizeName(std::string_view s, const std::string& fallback, size_t maxLen = 64);

enum class TextureRole { Unknown, BaseColor, Normal, Orm, Roughness, Metallic, Occlusion, Emissive, Opacity, Height, Smoothness, MetallicSmoothness };
const char* toString(TextureRole r);
// From the file name ("Chair_BaseColor.png" -> BaseColor; "T_Rock_N.tga" -> Normal; "Crate_ORM.png" -> Orm ...).
// setName receives the part before the role suffix ("Chair", "Rock", "Crate").
TextureRole guessTextureRole(std::string_view fileName, std::string* setName = nullptr);

} // namespace fledge::convert
