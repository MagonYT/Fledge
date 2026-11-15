#pragma once
// Material assets (.fmat, JSON) and model assets (.fmodel, JSON).
//
// .fmat example:
// { "format": "fmat", "version": 1, "shader": "Lit", "baseColor": [1,1,1], "opacity": 1,
//   "baseColorTexture": "<guid or content path>", "normalTexture": "", "metallicRoughnessTexture": "",
//   "roughness": 0.5, "metallic": 0, "twoSided": false, "alphaMode": "Opaque", "alphaCutoff": 0.5 }
//
// Rendered by the PBR pipeline (metallic-roughness, Cook-Torrance GGX): every field below.
// Texture conventions (glTF): metallicRoughnessTexture G = roughness, B = metallic (linear);
// occlusionTexture R (linear); normalTexture tangent space (linear); base color + emissive sRGB.
#include <optional>
#include <string>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/math.h"
#include "fledge/core/reflection.h"
#include "fledge/core/utility.h"

namespace fledge {

enum class MaterialShader : int { Lit = 0, Unlit = 1 };
enum class AlphaMode : int { Opaque = 0, Mask = 1, Blend = 2 };

struct Material {
    MaterialShader shader = MaterialShader::Lit;
    Vec3 baseColor{1, 1, 1};
    float opacity = 1.0f;
    std::string baseColorTexture;
    std::string normalTexture;
    std::string metallicRoughnessTexture;
    float roughness = 0.5f;
    float metallic = 0.0f;
    float normalScale = 1.0f;
    std::string occlusionTexture;
    float occlusionStrength = 1.0f;
    Vec3 emissive{0, 0, 0};
    float emissiveIntensity = 1.0f;
    std::string emissiveTexture;
    bool twoSided = false;
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;

    static const TypeInfo& typeInfo();  // reflected properties (editor panel, MCP set_material_property)
    Json toJson() const;
    static Result<Material> fromJson(const Json& j);
    bool operator==(const Material&) const = default;
};

struct ModelMeshRef {
    std::string mesh;      // GUID
    std::string material;  // GUID (may be empty)
};

struct ModelNode {
    std::string name;
    int parent = -1;  // index into nodes, -1 = model root
    Vec3 position{0, 0, 0};
    Quat rotation{};
    Vec3 scale{1, 1, 1};
    std::vector<ModelMeshRef> meshes;  // one entity per primitive
};

struct ModelAsset {
    std::string name;
    std::vector<ModelNode> nodes;  // parents precede children
    Json toJson() const;
    static Result<ModelAsset> fromJson(const Json& j);
};

} // namespace fledge
