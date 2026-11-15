#include "fledge/assets/material.h"
#include "fledge/core/legacy.h"

#include <format>

namespace fledge {

const TypeInfo& Material::typeInfo() {
    static const TypeInfo info = TypeBuilder<Material>("Material", "Surface appearance")
                                     .enumeration("shader", &Material::shader, {"Lit", "Unlit"})
                                     .color("baseColor", &Material::baseColor, "albedo tint (linear 0..1)")
                                     .field("opacity", &Material::opacity, "used with alphaMode Blend/Mask", 0, 1)
                                     .assetRef("baseColorTexture", &Material::baseColorTexture, "texture GUID or content path")
                                     .assetRef("normalTexture", &Material::normalTexture, "tangent-space normal map (linear)")
                                     .field("normalScale", &Material::normalScale, "normal map strength", 0, 2)
                                     .assetRef("metallicRoughnessTexture", &Material::metallicRoughnessTexture, "G = roughness, B = metallic (linear)")
                                     .field("roughness", &Material::roughness, "GGX roughness (perceptual)", 0, 1)
                                     .field("metallic", &Material::metallic, "", 0, 1)
                                     .assetRef("occlusionTexture", &Material::occlusionTexture, "ambient occlusion in R (linear)")
                                     .field("occlusionStrength", &Material::occlusionStrength, "", 0, 1)
                                     .color("emissive", &Material::emissive, "emissive color (linear)")
                                     .field("emissiveIntensity", &Material::emissiveIntensity, "emissive multiplier (HDR)", 0, 100)
                                     .assetRef("emissiveTexture", &Material::emissiveTexture, "emissive map (sRGB)")
                                     .field("twoSided", &Material::twoSided, "disable back-face culling")
                                     .enumeration("alphaMode", &Material::alphaMode, {"Opaque", "Mask", "Blend"})
                                     .field("alphaCutoff", &Material::alphaCutoff, "Mask threshold", 0, 1)
                                     .build();
    return info;
}

Json Material::toJson() const {
    Json j = typeInfo().toJson(this);
    j["format"] = "fmat";
    j["version"] = 1;
    return j;
}

Result<Material> Material::fromJson(const Json& j) {
    if (!j.is_object() || !legacy::formatIs(j, "fmat")) return Result<Material>::failure("not a .fmat document");
    if (j.value("version", 0) != 1) return Result<Material>::failure("unsupported .fmat version");
    Json props = j;
    props.erase("format");
    props.erase("version");
    props.erase("name");  // tolerated (informational)
    Material m;
    if (auto st = typeInfo().fromJson(&m, props); !st) return Result<Material>::failure(st.error);
    return m;
}

Json ModelAsset::toJson() const {
    Json nodesJ = Json::array();
    for (const auto& n : nodes) {
        Json meshes = Json::array();
        for (auto& m : n.meshes) meshes.push_back({{"mesh", m.mesh}, {"material", m.material}});
        nodesJ.push_back({{"name", n.name}, {"parent", n.parent}, {"position", n.position}, {"rotation", n.rotation},
                          {"scale", n.scale}, {"meshes", meshes}});
    }
    return {{"format", "fmodel"}, {"version", 1}, {"name", name}, {"nodes", nodesJ}};
}

Result<ModelAsset> ModelAsset::fromJson(const Json& j) {
    if (!j.is_object() || !legacy::formatIs(j, "fmodel")) return Result<ModelAsset>::failure("not a .fmodel document");
    ModelAsset m;
    m.name = j.value("name", "Model");
    try {
        for (const auto& nj : j.value("nodes", Json::array())) {
            ModelNode n;
            n.name = nj.value("name", "Node");
            n.parent = nj.value("parent", -1);
            if (n.parent >= static_cast<int>(m.nodes.size())) return Result<ModelAsset>::failure("model node parent must precede child");
            n.position = nj.value("position", Vec3{0, 0, 0});
            n.rotation = nj.value("rotation", Quat{});
            n.scale = nj.value("scale", Vec3{1, 1, 1});
            for (auto& mj : nj.value("meshes", Json::array())) n.meshes.push_back({mj.value("mesh", ""), mj.value("material", "")});
            m.nodes.push_back(std::move(n));
        }
    } catch (const std::exception& e) {
        return Result<ModelAsset>::failure(std::string("invalid .fmodel: ") + e.what());
    }
    return m;
}

} // namespace fledge
