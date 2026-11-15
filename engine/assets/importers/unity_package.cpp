// .unitypackage reading, entry classification and Unity material mapping (see unity_package.h).
#include "fledge/importers/unity_package.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <memory>
#include <set>

#include "fledge/importers/convert.h"

namespace fledge::unity {

bool isUnityGuid(std::string_view s) {
    return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

Result<Package> readPackage(const fs::path& file, const fs::path& staging, const archive::Limits& limits) {
    using R = Result<Package>;
    archive::FileSource src(file);
    if (!src.ok()) return R::failure("cannot open " + narrow(file.wstring()));
    if (src.size() > limits.maxArchiveBytes) return R::failure("the package is larger than the size limit");
    std::map<std::string, PackageEntry> byGuid;
    std::unique_ptr<archive::FileSink> out;
    std::string* collecting = nullptr;
    size_t collectCap = 0;
    std::vector<std::string> warnings;
    std::string sinkError;
    archive::TarParser tar(
        limits,
        [&](const archive::TarEntry& e) -> archive::Sink {
            out.reset();
            collecting = nullptr;
            size_t slash = e.path.find('/');
            std::string guid = e.path.substr(0, slash);
            if (!isUnityGuid(guid)) {
                if (e.type != '5') warnings.push_back("ignored non-package entry '" + e.path + "'");
                return {};
            }
            PackageEntry& pe = byGuid[guid];
            pe.guid = guid;
            if (slash == std::string::npos || e.type == '5') return {};
            std::string name = e.path.substr(slash + 1);
            if (name == "asset") {
                pe.assetFile = staging / pathFromUtf8(guid) / "asset";
                pe.assetSize = e.size;
                out = std::make_unique<archive::FileSink>(pe.assetFile);
                if (!out->ok()) {
                    sinkError = "cannot write into the staging folder";
                    return {};
                }
                return [&](const uint8_t* d, size_t n) { return out->write(d, n); };
            }
            if (name == "pathname" || name == "asset.meta") {
                collecting = name == "pathname" ? &pe.rawPathname : &pe.meta;
                collectCap = name == "pathname" ? 4096 : (1u << 20);
                collecting->clear();
                return [&](const uint8_t* d, size_t n) {
                    if (collecting->size() + n > collectCap) {
                        sinkError = "oversized pathname or .meta entry";
                        return false;
                    }
                    collecting->append(reinterpret_cast<const char*>(d), n);
                    return true;
                };
            }
            if (name == "preview.png") {
                pe.hasPreview = true;
                return [](const uint8_t*, size_t) { return true; };  // thumbnails are not kept
            }
            warnings.push_back("ignored unknown package entry '" + e.path + "'");
            return {};
        },
        [&](const archive::TarEntry&) {
            if (out) out->close();
            out.reset();
            collecting = nullptr;
        });
    Status st = archive::gunzip(src, [&](const uint8_t* d, size_t n) { return tar.feed(d, n); }, limits);
    out.reset();
    if (!st) {
        if (!sinkError.empty()) return R::failure(sinkError);
        if (!tar.error().empty()) return R::failure(tar.error());
        return R::failure(st.error);
    }
    if (Status f = tar.finish(); !f) return R::failure(f.error);
    if (byGuid.empty()) return R::failure("not a Unity package: no asset folders found");
    Package pkg;
    pkg.warnings = std::move(warnings);
    for (auto& w : tar.warnings()) pkg.warnings.push_back(w);
    for (auto& [g, pe] : byGuid) {
        std::string first = pe.rawPathname.substr(0, pe.rawPathname.find('\n'));
        while (!first.empty() && (first.back() == '\r' || first.back() == ' ')) first.pop_back();
        pe.rawPathname = first;
        if (first.empty()) {
            pkg.warnings.push_back("asset " + g + " has no pathname; skipped");
            continue;
        }
        if (auto p = archive::sanitizeEntryPath(first, limits)) pe.pathname = *p;
        else {
            pkg.warnings.push_back("asset '" + first + "' has an unsafe path; skipped");
            continue;
        }
        pe.folder = pe.assetFile.empty() || pe.meta.find("folderAsset: yes") != std::string::npos;
        pkg.totalBytes += pe.assetSize;
        pkg.entries.push_back(std::move(pe));
    }
    std::sort(pkg.entries.begin(), pkg.entries.end(), [](auto& a, auto& b) { return a.pathname < b.pathname; });
    return pkg;
}

const char* toString(EntryKind k) {
    switch (k) {
        case EntryKind::Folder: return "folder";
        case EntryKind::Model: return "model";
        case EntryKind::Texture: return "texture";
        case EntryKind::Audio: return "audio";
        case EntryKind::Material: return "material";
        case EntryKind::Font: return "font";
        case EntryKind::Document: return "document";
        case EntryKind::Script: return "script";
        case EntryKind::Shader: return "shader";
        case EntryKind::UnityData: return "unity-data";
        default: return "unsupported";
    }
}

EntryKind classify(std::string_view pathname, std::string* reason) {
    std::string p = toLower(pathname);
    size_t dot = p.rfind('.');
    std::string ext = dot == std::string::npos || p.find('/', dot) != std::string::npos ? "" : p.substr(dot);
    auto why = [&](const char* r, EntryKind k) {
        if (reason) *reason = r;
        return k;
    };
    static const std::set<std::string> models = {".fbx", ".obj", ".gltf", ".glb"};
    static const std::set<std::string> textures = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif", ".hdr"};
    static const std::set<std::string> audio = {".wav", ".mp3", ".ogg", ".flac"};
    static const std::set<std::string> scripts = {".cs", ".js", ".boo", ".dll", ".so", ".dylib", ".bundle", ".jslib", ".jspre", ".a", ".aar",
                                                  ".jar", ".exe", ".bat", ".cmd", ".ps1", ".sh", ".py", ".pdb", ".mdb", ".rsp", ".winmd"};
    static const std::set<std::string> shaders = {".shader", ".cginc", ".hlsl", ".hlslinc", ".glsl", ".glslinc", ".compute", ".shadergraph",
                                                  ".shadersubgraph", ".raytrace", ".shadervariants", ".uss", ".vfx", ".vfxoperator"};
    static const std::set<std::string> unityData = {".prefab", ".unity", ".asset", ".asmdef", ".asmref", ".controller", ".anim", ".overridecontroller",
                                                    ".mask", ".physicmaterial", ".physicsmaterial2d", ".rendertexture", ".lighting", ".preset", ".guiskin",
                                                    ".fontsettings", ".spriteatlas", ".spriteatlasv2", ".playable", ".signal", ".mixer", ".terrainlayer",
                                                    ".brush", ".cubemap", ".flare", ".giparams", ".inputactions", ".uxml", ".tss", ".index", ".lighting"};
    if (models.contains(ext)) return EntryKind::Model;
    if (textures.contains(ext)) return EntryKind::Texture;
    if (audio.contains(ext)) return EntryKind::Audio;
    if (ext == ".mat") return EntryKind::Material;
    if (ext == ".ttf" || ext == ".otf") return EntryKind::Font;
    if (ext == ".txt" || ext == ".md" || ext == ".pdf" || ext == ".rtf") return EntryKind::Document;
    if (scripts.contains(ext)) return why("code (C# scripts, plug-ins and binaries are never imported or run)", EntryKind::Script);
    if (shaders.contains(ext)) return why("Unity shader / shader graph (not convertible; materials are mapped onto Fledge's PBR shader)", EntryKind::Shader);
    if (unityData.contains(ext))
        return why("Unity-specific data (prefabs, scenes, animator controllers, ScriptableObjects ...) is not converted", EntryKind::UnityData);
    if (ext == ".dae" || ext == ".3ds" || ext == ".blend" || ext == ".max" || ext == ".ma" || ext == ".mb" || ext == ".c4d" || ext == ".skp")
        return why("model format not supported: export it to FBX or glTF", EntryKind::Unsupported);
    if (ext == ".tif" || ext == ".tiff" || ext == ".exr" || ext == ".dds" || ext == ".ktx" || ext == ".iff" || ext == ".pict")
        return why("texture format not supported (PNG, JPG, TGA, BMP, PSD, GIF and HDR are)", EntryKind::Unsupported);
    if (ext == ".aif" || ext == ".aiff" || ext == ".mod" || ext == ".it" || ext == ".s3m" || ext == ".xm")
        return why("audio format not supported (WAV, MP3, OGG Vorbis and FLAC are)", EntryKind::Unsupported);
    return why("file type not supported", EntryKind::Unsupported);
}

// ---------------------------------------------------------------- material YAML ----
namespace {
std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return std::string(s.substr(a, b - a));
}
// "{fileID: 2800000, guid: abc, type: 3}" -> {fileID: 2800000, guid: abc, type: 3}
std::map<std::string, std::string> flowMap(std::string_view v) {
    std::map<std::string, std::string> out;
    std::string s = trim(v);
    if (s.size() < 2 || s.front() != '{' || s.back() != '}') return out;
    s = s.substr(1, s.size() - 2);
    size_t p = 0;
    while (p <= s.size()) {
        size_t c = s.find(',', p);
        if (c == std::string::npos) c = s.size();
        std::string item = s.substr(p, c - p);
        if (auto colon = item.find(':'); colon != std::string::npos) out[trim(item.substr(0, colon))] = trim(item.substr(colon + 1));
        p = c + 1;
    }
    return out;
}
float num(const std::string& s, float def = 0) {
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    return end == s.c_str() ? def : v;
}
} // namespace

std::optional<UnityMaterial> parseMaterial(std::string_view yaml, std::string* error) {
    if (yaml.find("Material:") == std::string_view::npos) {
        if (error) *error = "not a Unity material (no 'Material:' document)";
        return std::nullopt;
    }
    UnityMaterial m;
    enum class Sec { None, Tex, Floats, Colors, Keywords } sec = Sec::None;
    int secIndent = -1;
    std::string item;        // current property name
    bool inSecond = false;   // v2 layout: inside "second:"
    MaterialTexture* tex = nullptr;
    size_t pos = 0;
    int lines = 0;
    while (pos < yaml.size()) {
        size_t e = yaml.find('\n', pos);
        if (e == std::string_view::npos) e = yaml.size();
        std::string_view raw = yaml.substr(pos, e - pos);
        pos = e + 1;
        if (++lines > 200000) break;
        int indent = 0;
        while (indent < static_cast<int>(raw.size()) && raw[static_cast<size_t>(indent)] == ' ') ++indent;
        std::string line = trim(raw);
        if (line.empty() || line[0] == '%' || line.rfind("---", 0) == 0) continue;
        bool dash = line.rfind("- ", 0) == 0;
        if (sec != Sec::None && (indent < secIndent || (indent == secIndent && !dash))) sec = Sec::None;
        std::string body = dash ? trim(std::string_view(line).substr(2)) : line;
        size_t colon = body.find(':');
        std::string key = colon == std::string::npos ? body : trim(std::string_view(body).substr(0, colon));
        std::string val = colon == std::string::npos ? "" : trim(std::string_view(body).substr(colon + 1));
        if (sec == Sec::None) {
            if (key == "m_Name" && m.name.empty()) m.name = val;
            else if (key == "m_Shader") {
                auto f = flowMap(val);
                m.shaderGuid = f.contains("guid") ? f["guid"] : "";
                m.shaderFileId = f.contains("fileID") ? std::strtoll(f["fileID"].c_str(), nullptr, 10) : 0;
            } else if (key == "m_ShaderKeywords") {
                for (size_t p = 0; p < val.size();) {
                    size_t sp = val.find(' ', p);
                    if (sp == std::string::npos) sp = val.size();
                    if (sp > p) m.keywords.push_back(val.substr(p, sp - p));
                    p = sp + 1;
                }
            } else if ((key == "m_TexEnvs" || key == "m_Floats" || key == "m_Ints" || key == "m_Colors" || key == "m_ValidKeywords") && val.empty()) {
                sec = key == "m_TexEnvs" ? Sec::Tex : key == "m_Colors" ? Sec::Colors : key == "m_ValidKeywords" ? Sec::Keywords : Sec::Floats;
                secIndent = indent;
                item.clear();
                tex = nullptr;
                inSecond = false;
            }
            continue;
        }
        if (sec == Sec::Keywords) {
            if (dash) m.keywords.push_back(body);
            continue;
        }
        if (dash) {  // new property: "- _Name: value" (v3) or "- first:" (v2)
            item.clear();
            tex = nullptr;
            inSecond = false;
            if (key == "first") continue;
            item = key;
            if (sec == Sec::Tex) tex = &m.textures[item];
            else if (!val.empty()) {
                if (sec == Sec::Floats) m.floats[item] = num(val);
                else {
                    auto f = flowMap(val);
                    m.colors[item] = {num(f["r"], 1), num(f["g"], 1), num(f["b"], 1), num(f["a"], 1)};
                }
            }
            continue;
        }
        if (key == "name" && !inSecond) {  // v2: "name: _MainTex" under "first:"
            item = val;
            if (sec == Sec::Tex) tex = &m.textures[item];
            continue;
        }
        if (key == "second") {
            inSecond = true;
            if (!val.empty() && !item.empty()) {
                if (sec == Sec::Floats) m.floats[item] = num(val);
                else if (sec == Sec::Colors) {
                    auto f = flowMap(val);
                    m.colors[item] = {num(f["r"], 1), num(f["g"], 1), num(f["b"], 1), num(f["a"], 1)};
                }
            }
            continue;
        }
        if (sec == Sec::Tex && tex) {
            auto f = flowMap(val);
            if (key == "m_Texture") tex->guid = (f.contains("guid") && isUnityGuid(f["guid"])) ? f["guid"] : "";
            else if (key == "m_Scale") tex->scale = {num(f["x"], 1), num(f["y"], 1)};
            else if (key == "m_Offset") tex->offset = {num(f["x"], 0), num(f["y"], 0)};
        }
    }
    return m;
}

const char* toString(ShaderFamily f) {
    switch (f) {
        case ShaderFamily::Standard: return "Standard";
        case ShaderFamily::StandardSpecular: return "Standard (Specular setup)";
        case ShaderFamily::UrpLit: return "URP Lit";
        case ShaderFamily::UrpSimpleLit: return "URP Simple Lit";
        case ShaderFamily::UrpUnlit: return "URP Unlit";
        case ShaderFamily::HdrpLit: return "HDRP Lit";
        case ShaderFamily::Unlit: return "Unlit";
        default: return "custom";
    }
}

ShaderFamily shaderFamily(const UnityMaterial& m, bool* guessed) {
    if (guessed) *guessed = false;
    if (m.shaderGuid == "0000000000000000f000000000000000") {  // built-in shaders, by file ID
        if (m.shaderFileId == 46) return ShaderFamily::Standard;
        if (m.shaderFileId == 45) return ShaderFamily::StandardSpecular;
        if (m.shaderFileId >= 10750 && m.shaderFileId <= 10755) return ShaderFamily::Unlit;
    }
    // Unity's render pipeline shaders (GUIDs from the URP / HDRP packages; unconfirmed for every version)
    if (m.shaderGuid == "933532a4fcc9baf4fa0491de14d08ed7") return ShaderFamily::UrpLit;
    if (m.shaderGuid == "8d2bb70cbf9db8d4da26e15b26e74248") return ShaderFamily::UrpSimpleLit;
    if (m.shaderGuid == "650dd9526735d5b46b79224bc6e94025") return ShaderFamily::UrpUnlit;
    if (m.shaderGuid == "6e4ae4064600d784cac1e41a9e6f2e59") return ShaderFamily::HdrpLit;
    if (guessed) *guessed = true;
    auto has = [&](const char* n) { return m.textures.contains(n) || m.colors.contains(n) || m.floats.contains(n); };
    if (has("_MaskMap") || has("_BaseColorMap")) return ShaderFamily::HdrpLit;
    if (has("_BaseMap") || has("_BaseColor")) return has("_Metallic") || has("_Smoothness") ? ShaderFamily::UrpLit : ShaderFamily::UrpUnlit;
    if (has("_MainTex") || has("_Color")) return has("_SpecGlossMap") ? ShaderFamily::StandardSpecular : ShaderFamily::Standard;
    return ShaderFamily::Unknown;
}

std::vector<std::string> linearTextureGuids(const UnityMaterial& m) {
    std::vector<std::string> out;
    for (const char* n : {"_BumpMap", "_NormalMap", "_MetallicGlossMap", "_MaskMap", "_OcclusionMap", "_ParallaxMap", "_DetailNormalMap", "_SpecGlossMap"})
        if (auto it = m.textures.find(n); it != m.textures.end() && !it->second.guid.empty()) out.push_back(it->second.guid);
    return out;
}

bool metaSaysLinear(std::string_view meta) {
    return meta.find("sRGBTexture: 0") != std::string_view::npos || meta.find("textureType: 1\n") != std::string_view::npos ||
           meta.find("textureType: 1\r") != std::string_view::npos;
}

Material mapMaterial(const UnityMaterial& m, const TextureProvider& provider, MaterialReport& report) {
    bool guessed = false;
    ShaderFamily fam = shaderFamily(m, &guessed);
    report.shader = std::string(toString(fam)) + (guessed ? " (identified from its property names)" : "");
    bool hdrp = fam == ShaderFamily::HdrpLit;
    bool urp = fam == ShaderFamily::UrpLit || fam == ShaderFamily::UrpSimpleLit || fam == ShaderFamily::UrpUnlit;
    std::set<std::string> used;
    auto tex = [&](const char* name) -> std::string {
        auto it = m.textures.find(name);
        if (it == m.textures.end() || it->second.guid.empty()) return {};
        used.insert(name);
        if (it->second.scale.x != 1 || it->second.scale.y != 1 || it->second.offset.x != 0 || it->second.offset.y != 0)
            report.notMapped.push_back(std::format("{}: tiling {}x{} / offset ({}, {}) is not supported (ignored)", name, it->second.scale.x,
                                                   it->second.scale.y, it->second.offset.x, it->second.offset.y));
        return it->second.guid;
    };
    auto fledgeTex = [&](const std::string& unityGuid, const char* prop, const char* field) -> std::string {
        if (unityGuid.empty()) return {};
        std::string g = provider.fledgeGuid ? provider.fledgeGuid(unityGuid) : "";
        if (g.empty()) report.notMapped.push_back(std::format("{}: texture {} is not in the package or could not be imported", prop, unityGuid));
        else report.mapped.push_back(std::format("{} -> {}", prop, field));
        return g;
    };
    auto fl = [&](const char* n, float def) {
        auto it = m.floats.find(n);
        return it == m.floats.end() ? def : it->second;
    };
    auto col = [&](const char* n, Vec4 def) {
        auto it = m.colors.find(n);
        return it == m.colors.end() ? def : it->second;
    };
    bool keywordsKnown = !m.keywords.empty();
    auto keyword = [&](const char* k) { return std::find(m.keywords.begin(), m.keywords.end(), k) != m.keywords.end(); };

    Material out;
    const char* albedoName = hdrp ? "_BaseColorMap" : (urp ? "_BaseMap" : "_MainTex");
    if (urp && !m.textures.contains("_BaseMap")) albedoName = "_MainTex";  // URP materials keep _MainTex in sync, older ones only it
    Vec4 base = col(hdrp || urp ? "_BaseColor" : "_Color", col("_Color", {1, 1, 1, 1}));
    out.baseColor = {base.x, base.y, base.z};
    out.opacity = base.w;
    std::string albedoGuid = tex(albedoName);
    out.baseColorTexture = fledgeTex(albedoGuid, albedoName, "baseColorTexture");
    if (!m.textures.empty() && m.textures.contains("_MainTex") && albedoName != std::string("_MainTex")) used.insert("_MainTex");

    if (fam == ShaderFamily::Unlit || fam == ShaderFamily::UrpUnlit) {
        out.shader = MaterialShader::Unlit;
        report.mapped.push_back("shader -> Unlit");
    }
    // normal map
    const char* normalName = hdrp ? "_NormalMap" : "_BumpMap";
    out.normalTexture = fledgeTex(tex(normalName), normalName, "normalTexture");
    out.normalScale = fl(hdrp ? "_NormalScale" : "_BumpScale", 1.0f);

    // metallic / smoothness / occlusion -> ORM
    float metallic = fl("_Metallic", 0.0f);
    float smoothness = fam == ShaderFamily::Standard || fam == ShaderFamily::StandardSpecular || fam == ShaderFamily::Unknown ? fl("_Glossiness", fl("_Smoothness", 0.5f))
                                                                                                                              : fl("_Smoothness", 0.5f);
    out.metallic = metallic;
    out.roughness = 1.0f - smoothness;
    if (fam == ShaderFamily::StandardSpecular || fl("_WorkflowMode", 1.0f) == 0.0f) {
        report.notMapped.push_back("specular workflow (_SpecColor / _SpecGlossMap): approximated as non-metallic with the same smoothness");
        out.metallic = 0;
        if (m.textures.contains("_SpecGlossMap")) used.insert("_SpecGlossMap");
    }
    if (hdrp) {
        std::string maskGuid = tex("_MaskMap");
        if (!maskGuid.empty()) {
            const ImageData* mask = provider.image ? provider.image(maskGuid) : nullptr;
            if (mask && provider.writePacked) {
                out.metallicRoughnessTexture = out.occlusionTexture = provider.writePacked(convert::sanitizeName(m.name, "material") + "_ORM", convert::hdrpMaskToOrm(*mask));
                out.metallic = 1.0f;
                out.roughness = 1.0f;
                report.mapped.push_back("_MaskMap (R metallic, G occlusion, A smoothness) -> ORM texture");
                report.notMapped.push_back("_MaskMap remap ranges (_MetallicRemap*, _SmoothnessRemap*, _AORemap*) and detail mask: ignored");
            } else {
                report.notMapped.push_back("_MaskMap: texture could not be decoded");
            }
        }
    } else {
        std::string metalGuid = tex("_MetallicGlossMap");
        std::string aoGuid = tex("_OcclusionMap");
        bool fromAlbedo = fl("_SmoothnessTextureChannel", 0.0f) == 1.0f;
        const ImageData* metalImg = metalGuid.empty() || !provider.image ? nullptr : provider.image(metalGuid);
        const ImageData* aoImg = aoGuid.empty() || !provider.image ? nullptr : provider.image(aoGuid);
        const ImageData* smoothImg = fromAlbedo ? (albedoGuid.empty() || !provider.image ? nullptr : provider.image(albedoGuid)) : metalImg;
        if (!metalGuid.empty() && !metalImg) report.notMapped.push_back("_MetallicGlossMap: texture could not be decoded");
        if (!aoGuid.empty() && !aoImg) report.notMapped.push_back("_OcclusionMap: texture could not be decoded");
        if ((metalImg || aoImg || (fromAlbedo && smoothImg)) && provider.writePacked) {
            float glossScale = (fam == ShaderFamily::Standard || fam == ShaderFamily::Unknown) ? fl("_GlossMapScale", 1.0f) : smoothness;
            if (!smoothImg) glossScale = 1.0f;
            convert::Channel r{aoImg, 1, false, 1.0f, 1.0f};  // Unity reads occlusion from green
            convert::Channel g = smoothImg ? convert::Channel{smoothImg, 3, true, glossScale, 1.0f} : convert::Channel{nullptr, 0, false, 1.0f, 1.0f};
            convert::Channel b = metalImg ? convert::Channel{metalImg, 0} : convert::Channel{nullptr, 0, false, 1.0f, 1.0f};
            std::string orm = provider.writePacked(convert::sanitizeName(m.name, "material") + "_ORM", convert::packChannels(r, g, b));
            out.metallicRoughnessTexture = orm;
            if (smoothImg) out.roughness = 1.0f;  // the texture carries the value (factors multiply)
            if (metalImg) out.metallic = 1.0f;
            if (aoImg) {
                out.occlusionTexture = orm;
                out.occlusionStrength = fl("_OcclusionStrength", 1.0f);
            }
            report.mapped.push_back(std::string("metallic / smoothness") + (fromAlbedo ? " (smoothness from albedo alpha)" : "") +
                                    (aoImg ? " / occlusion" : "") + " -> ORM texture (roughness = 1 - smoothness)");
        }
    }

    // emission
    const char* emitMap = hdrp ? "_EmissiveColorMap" : "_EmissionMap";
    Vec4 emit = col(hdrp ? "_EmissiveColor" : "_EmissionColor", {0, 0, 0, 1});
    bool emissionOn = !keywordsKnown || keyword("_EMISSION") || hdrp;
    std::string emitGuid = tex(emitMap);
    if (emissionOn) {
        out.emissive = {emit.x, emit.y, emit.z};
        out.emissiveTexture = fledgeTex(emitGuid, emitMap, "emissiveTexture");
        if (!out.emissiveTexture.empty() && out.emissive == Vec3{0, 0, 0}) out.emissive = {1, 1, 1};
        float peak = std::max({out.emissive.x, out.emissive.y, out.emissive.z});
        if (peak > 1.0f) {  // HDR emission colours: Fledge keeps the colour in 0..1 and the strength in emissiveIntensity
            out.emissive = out.emissive * (1.0f / peak);
            out.emissiveIntensity = peak;
        }
    }

    // transparency, cut-out, culling
    if (hdrp) {
        if (fl("_SurfaceType", 0) == 1) out.alphaMode = AlphaMode::Blend;
        if (fl("_AlphaCutoffEnable", 0) == 1) out.alphaMode = AlphaMode::Mask, out.alphaCutoff = fl("_AlphaCutoff", 0.5f);
        out.twoSided = fl("_DoubleSidedEnable", 0) == 1;
    } else if (urp) {
        if (fl("_Surface", 0) == 1) out.alphaMode = AlphaMode::Blend;
        if (fl("_AlphaClip", 0) == 1) out.alphaMode = AlphaMode::Mask, out.alphaCutoff = fl("_Cutoff", 0.5f);
        out.twoSided = fl("_Cull", 2) == 0;
    } else {
        int mode = static_cast<int>(fl("_Mode", 0));
        if (mode == 1) out.alphaMode = AlphaMode::Mask, out.alphaCutoff = fl("_Cutoff", 0.5f);
        else if (mode >= 2) out.alphaMode = AlphaMode::Blend;
        if (fl("_Cull", 2) == 0) out.twoSided = true;
        if (mode == 2) report.notMapped.push_back("_Mode Fade (fades specular too): mapped to Blend");
    }
    if (out.alphaMode != AlphaMode::Opaque) report.mapped.push_back(std::string("alpha -> ") + (out.alphaMode == AlphaMode::Mask ? "Mask" : "Blend"));

    for (auto& [name, t] : m.textures)
        if (!t.guid.empty() && !used.contains(name))
            report.notMapped.push_back(name + (name == "_ParallaxMap" ? " (height / parallax map): not supported"
                                               : name.find("Detail") != std::string::npos ? " (detail map): not supported"
                                                                                           : ": texture property not mapped"));
    if (fam == ShaderFamily::Unknown) report.notMapped.push_back("custom shader: only common property names were mapped");
    return out;
}

ModelMeta parseModelMeta(std::string_view meta) {
    ModelMeta mm;
    size_t pos = 0;
    bool inExternal = false;
    int extIndent = -1;
    std::string type, name;
    while (pos < meta.size()) {
        size_t e = meta.find('\n', pos);
        if (e == std::string_view::npos) e = meta.size();
        std::string_view raw = meta.substr(pos, e - pos);
        pos = e + 1;
        int indent = 0;
        while (indent < static_cast<int>(raw.size()) && raw[static_cast<size_t>(indent)] == ' ') ++indent;
        std::string line = trim(raw);
        if (line.empty()) continue;
        bool dash = line.rfind("- ", 0) == 0;
        if (inExternal && (indent < extIndent || (indent == extIndent && !dash))) inExternal = false;
        std::string body = dash ? trim(std::string_view(line).substr(2)) : line;
        size_t colon = body.find(':');
        std::string key = colon == std::string::npos ? body : trim(std::string_view(body).substr(0, colon));
        std::string val = colon == std::string::npos ? "" : trim(std::string_view(body).substr(colon + 1));
        if (!inExternal) {
            if (key == "externalObjects" && val.empty()) {
                inExternal = true;
                extIndent = indent;
            } else if (key == "globalScale") {
                mm.globalScale = num(val, 1.0f);
                if (!(mm.globalScale > 0.0f) || mm.globalScale > 10000.0f) mm.globalScale = 1.0f;
            }
            continue;
        }
        if (dash) type.clear(), name.clear();
        if (key == "type") type = val;
        else if (key == "name") name = val;
        else if (key == "second" && type.find("Material") != std::string::npos && !name.empty()) {
            auto f = flowMap(val);
            if (f.contains("guid") && isUnityGuid(f["guid"])) mm.materialRemap[name] = f["guid"];
        }
    }
    return mm;
}

} // namespace fledge::unity
