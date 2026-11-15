#include "fledge/render/renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string>

#include "fledge/core/log.h"
#include "shaders.h"
#include "shaders_hlsl.h"
#include "shaders_post.h"
#include "shaders_effects.h"
#include "shaders_robust.h"
#include "fsr1.h"
#include "fledge/render/graphics_api.h"
#include "fledge/render/raytracing.h"
#include "fledge/render/upscalers.h"
#include "fledge/render/vendor_upscaler.h"
#include "fledge/core/paths.h"

namespace fledge {

namespace {

struct GpuLight {
    Vec4 posType;         // xyz position, w type (0 dir, 1 point, 2 spot)
    Vec4 dirRange;        // xyz direction (travel), w range
    Vec4 colorIntensity;  // rgb color, w intensity
    Vec4 cone;            // x cos(inner), y cos(outer), z shadow index (-1 none), w depth bias
};
struct FrameData {
    Mat4 view, proj, viewProj, invViewProj;
    Mat4 cascadeVP[4];
    Mat4 spotVP[14];  // shadow views: spot lights (1 each) then point lights (6 faces each)
    Vec4 cascadeSplits, cascadeTexel, cameraPos, params0, params1;
    GpuLight lights[8];
    Mat4 viewProjNoJitter, prevViewProj;  // motion vectors
    Vec4 params2;                         // xy jitter (NDC), z frame index
    Vec4 fog0, fog1, fog2, fog3;          // height fog (see kFrameBlock)
    Vec4 wet;                             // x wetness, y puddles (wet surfaces)
};
static_assert(sizeof(FrameData) == 64 * 22 + 16 * 5 + 64 * 8 + 64 * 2 + 16 * 6, "FrameData must match the std140 Frame block");

constexpr int kEnvSize = 256, kIrradianceSize = 32, kPrefilterSize = 128, kPrefilterMips = 5, kBrdfSize = 128;
constexpr int kSpotShadowRes = 1024, kMaxSpotShadows = 2, kMaxPointShadows = 2, kMaxShadowViews = 14;

std::string withFrame(const char* body, bool vertex) {
    std::string s = "#version 330 core\n";
    s += shaders::kFrameBlock;
    s += body;
    (void)vertex;
    return s;
}

std::string cubeShader(const char* body) { return std::string(shaders::kCubeCommon) + body; }

// 1.3.2: turns a 4-influence skinning shader (GLSL or HLSL, 1-block or 4-block palette) into its 8-influence variant: 4 skin
// texels per vertex instead of 2, and the second joint / weight texel pair added to the skin matrix. "" when the source does
// not have the expected skinning code (the variant is then not created and 8-influence characters stay on the CPU).
std::string skinEightInfluences(std::string s) {
    auto swap = [&](const std::string& a, const std::string& b) {
        size_t p = s.find(a);
        if (p == std::string::npos) return false;
        s.replace(p, a.size(), b);
        return true;
    };
    const bool glsl = s.find("int i = gl_VertexID * 2;") != std::string::npos;
    if (glsl) {
        if (!swap("int i = gl_VertexID * 2;", "int i = gl_VertexID * 4;")) return {};
        if (!swap("vec4 w = texelFetch(uSkinTex, ivec2((i + 1) % W, (i + 1) / W), 0);\n",
                  "vec4 w = texelFetch(uSkinTex, ivec2((i + 1) % W, (i + 1) / W), 0);\n"
                  "    vec4 j1 = texelFetch(uSkinTex, ivec2((i + 2) % W, (i + 2) / W), 0);\n"
                  "    vec4 w1 = texelFetch(uSkinTex, ivec2((i + 3) % W, (i + 3) / W), 0);\n"))
            return {};
        if (swap("uBones[int(j.w)] * w.w;\n}",
                 "uBones[int(j.w)] * w.w\n         + uBones[int(j1.x)] * w1.x + uBones[int(j1.y)] * w1.y + uBones[int(j1.z)] * w1.z + uBones[int(j1.w)] * w1.w;\n}"))
            return s;
        if (swap("boneMatrix(j.w) * w.w;\n}",
                 "boneMatrix(j.w) * w.w\n         + boneMatrix(j1.x) * w1.x + boneMatrix(j1.y) * w1.y + boneMatrix(j1.z) * w1.z + boneMatrix(j1.w) * w1.w;\n}"))
            return s;
        return {};
    }
    if (!swap("uint i = vid * 2;", "uint i = vid * 4;")) return {};
    if (!swap("float4 w = uSkinTex.Load(int3((i + 1) % W, (i + 1) / W, 0));\n",
              "float4 w = uSkinTex.Load(int3((i + 1) % W, (i + 1) / W, 0));\n"
              "    float4 j1 = uSkinTex.Load(int3((i + 2) % W, (i + 2) / W, 0));\n"
              "    float4 w1 = uSkinTex.Load(int3((i + 3) % W, (i + 3) / W, 0));\n"))
        return {};
    if (swap("uBones[(int)j.w] * w.w;\n}",
             "uBones[(int)j.w] * w.w\n         + uBones[(int)j1.x] * w1.x + uBones[(int)j1.y] * w1.y + uBones[(int)j1.z] * w1.z + uBones[(int)j1.w] * w1.w;\n}"))
        return s;
    if (swap("boneMatrix(j.w) * w.w;\n}",
             "boneMatrix(j.w) * w.w\n         + boneMatrix(j1.x) * w1.x + boneMatrix(j1.y) * w1.y + boneMatrix(j1.z) * w1.z + boneMatrix(j1.w) * w1.w;\n}"))
        return s;
    return {};
}

// HLSL composition (see shaders_hlsl.h): Frame + body, Fullscreen [+ CubeCommon] + body.
std::string hlslFrame(const char* body) { return std::string(shaders::hlsl::kFrame) + body; }
std::string hlslPost(const char* body) { return std::string(shaders::hlsl::kFullscreen) + body; }
std::string hlslCube(const char* body) { return std::string(shaders::hlsl::kFullscreen) + shaders::hlsl::kCubeCommon + body; }

// GL clip z [-1,1] -> D3D [0,1]
Mat4 clipFix(bool zeroToOne) {
    Mat4 fix;
    if (zeroToOne) {
        fix.c[2] = {0, 0, 0.5f, 0};
        fix.c[3] = {0, 0, 0.5f, 1};
    }
    return fix;
}

rhi::TextureDesc depthArrayDesc(int res, int layers) {
    rhi::TextureDesc d;
    d.type = rhi::TextureType::Tex2DArray;
    d.format = rhi::TextureFormat::Depth32F;
    d.width = d.height = res;
    d.layers = layers;
    d.sampler = {rhi::Filter::Linear, false, rhi::Wrap::Clamp, true};
    return d;
}

rhi::TextureDesc colorDesc(rhi::TextureFormat f, int w, int h, int samples = 1) {
    rhi::TextureDesc d;
    d.format = f;
    d.width = std::max(1, w);
    d.height = std::max(1, h);
    d.samples = samples;
    d.sampler = {rhi::Filter::Linear, false, rhi::Wrap::Clamp, false};
    return d;
}

} // namespace

ForwardRenderer::ForwardRenderer(rhi::RenderDevice& device) : m_device(device) {
    m_full = device.caps().fullPipeline;
    // 1.3.3: FLEDGE_COUNT_NONFINITE=1 turns the NaN / Inf pixel counter on in any host (editor viewport stats, games)
    if (const char* e = std::getenv("FLEDGE_COUNT_NONFINITE"); e && e[0] == '1') m_nonFiniteCheck = true;
    using shaders::kFullscreenVs;
    auto make = [&](const char* name, std::string vs, std::string fs, std::string hlsl, std::vector<std::pair<std::string, int>> samplers) {
        rhi::ShaderDesc d;
        d.name = name;
        d.glslVertex = std::move(vs);
        d.glslFragment = std::move(fs);
        d.hlsl = std::move(hlsl);
        d.uniformBlocks = {{"Frame", 0}};
        d.samplers = std::move(samplers);
        auto h = m_device.createShader(d);
        if (!h) FLEDGE_LOG_ERROR("render", "failed to create shader '{}'", name);
        return h;
    };
    {
        rhi::ShaderDesc d{"basic", shaders::kBasicVs, shaders::kBasicFs, shaders::kBasicHlsl, {}, {{"uAlbedo", 0}}};
        m_basic = m_device.createShader(d);
        rhi::ShaderDesc o{"overlay", shaders::kOverlayVs, shaders::kOverlayFs, shaders::hlsl::kOverlay, {}, {{"uOverlayTex", 0}}};
        m_overlayShader = m_device.createShader(o);
    }
    if (m_full) {
        namespace h = shaders::hlsl;
        m_pbr = make("pbr", withFrame(shaders::kPbrVsBody, true), withFrame(shaders::kPbrFsBody, false), hlslFrame(h::kPbr),
                     {{"uBaseColorTex", 0}, {"uNormalTex", 1}, {"uMRTex", 2}, {"uOcclusionTex", 3}, {"uEmissiveTex", 4},
                      {"uIrradiance", 5}, {"uPrefiltered", 6}, {"uBrdfLut", 7}, {"uCascadeShadow", 8}, {"uSpotShadow", 9}, {"uRtAo", 11}, {"uRtReflection", 12}});
        m_shadow = make("shadow", shaders::kShadowVs, shaders::kShadowFs, h::kShadow, {});
#if FLEDGE_ADVANCED_RENDER
        // 1.3.2 projected decals (Fledge Advanced at run time): the PBR shader with the coverage multiplied by the per-vertex
        // angle fade (tangent.w; tangent.xyz = 0 so no normal mapping) and a small depth bias towards the camera
        if (advancedRenderFeatures()) {
            auto swap = [](std::string s, const std::string& a, const std::string& b) {
                size_t p = s.find(a);
                if (p == std::string::npos) return std::string();
                return s.replace(p, a.size(), b);
            };
            std::string vs = swap(withFrame(shaders::kPbrVsBody, true), "    gl_Position = uViewProj * wp;\n",
                                  "    gl_Position = uViewProj * wp;\n    gl_Position.z -= 4.0e-6 * gl_Position.w;  // decal depth bias\n");
            std::string fs = swap(withFrame(shaders::kPbrFsBody, false), "    float alpha = uBaseColor.a * baseTex.a;\n",
                                  "    float alpha = uBaseColor.a * baseTex.a * clamp(vTangent.w, 0.0, 1.0);  // decal angle fade\n");
            std::string hl = swap(hlslFrame(h::kPbr), "    o.pos = mul(uViewProj, wp);\n", "    o.pos = mul(uViewProj, wp);\n    o.pos.z -= 4.0e-6 * o.pos.w;\n");
            hl = hl.empty() ? hl : swap(hl, "    float alpha = uBaseColor.a * baseTex.a;\n", "    float alpha = uBaseColor.a * baseTex.a * saturate(i.tangent.w);\n");
            if (!vs.empty() && !fs.empty() && !hl.empty())
                m_pbrDecal = make("pbr_decal", vs, fs, hl,
                                  {{"uBaseColorTex", 0}, {"uNormalTex", 1}, {"uMRTex", 2}, {"uOcclusionTex", 3}, {"uEmissiveTex", 4}, {"uIrradiance", 5},
                                   {"uPrefiltered", 6}, {"uBrdfLut", 7}, {"uCascadeShadow", 8}, {"uSpotShadow", 9}, {"uRtAo", 11}, {"uRtReflection", 12}});
            if (!m_pbrDecal) FLEDGE_LOG_WARN("render", "decal shader unavailable on {}: projected decals are not drawn", rhi::toString(device.backend()));
        }
#endif
        if (device.caps().instancing) {  // instanced variants: per-instance model matrices in uniform block "Instances" (binding 1)
            auto glslInst = [](std::string vs, const std::string& decl) {
                std::string block = "layout(std140) uniform Instances { mat4 uInst[128]; };\n#define uModel uInst[gl_InstanceID]\n#define uPrevModel uInst[gl_InstanceID]\n";
                size_t p = vs.find(decl);
                if (p != std::string::npos) vs.replace(p, decl.size(), block);
                return vs;
            };
            auto hlslInst = [](std::string src, const std::string& sig, const std::string& newSig) {
                size_t p = src.find(sig);
                if (p == std::string::npos) return std::string();
                std::string pre = "cbuffer Instances : register(b2) { float4x4 uInst[128]; };\n#define uModel uInst[iid]\n#define uPrevModel uInst[iid]\n";
                src.replace(p, sig.size(), pre + newSig);
                size_t end = src.find("\n}\n", p);
                if (end != std::string::npos) src.insert(end + 3, "#undef uModel\n#undef uPrevModel\n");
                return src;
            };
            auto mk = [&](const char* name, std::string vs, std::string fs, std::string hl, std::vector<std::pair<std::string, int>> samplers) {
                rhi::ShaderDesc d;
                d.name = name;
                d.glslVertex = std::move(vs);
                d.glslFragment = std::move(fs);
                d.hlsl = std::move(hl);
                d.uniformBlocks = {{"Frame", 0}, {"Instances", 1}};
                d.samplers = std::move(samplers);
                return m_device.createShader(d);
            };
            m_pbrInst = mk("pbr_instanced", glslInst(withFrame(shaders::kPbrVsBody, true), "uniform mat4 uModel;\nuniform mat4 uPrevModel;\n"),
                           withFrame(shaders::kPbrFsBody, false), hlslInst(hlslFrame(h::kPbr), "PSIn VSMain(VSIn i) {", "PSIn VSMain(VSIn i, uint iid : SV_InstanceID) {"),
                           {{"uBaseColorTex", 0}, {"uNormalTex", 1}, {"uMRTex", 2}, {"uOcclusionTex", 3}, {"uEmissiveTex", 4},
                            {"uIrradiance", 5}, {"uPrefiltered", 6}, {"uBrdfLut", 7}, {"uCascadeShadow", 8}, {"uSpotShadow", 9}, {"uRtAo", 11}, {"uRtReflection", 12}});
            m_shadowInst = mk("shadow_instanced", glslInst(shaders::kShadowVs, "uniform mat4 uModel;\n"), shaders::kShadowFs,
                              hlslInst(h::kShadow, "float4 VSMain(float3 pos : POSITION) : SV_Position {", "float4 VSMain(float3 pos : POSITION, uint iid : SV_InstanceID) : SV_Position {"), {});
            m_instUbo = m_device.createUniformBuffer(sizeof(Mat4) * kMaxInstances);
            // GPU skinning variants: joints/weights fetched by vertex id from a float texture (t14 / unit 14), bones in block "Bones" (binding 2)
            {
                // kMaxBones (256) float4x4 = 16 KB: the minimum uniform block size every GL 3.3 / Vulkan device supports
                const std::string nBones = std::to_string(kMaxBones);
                const std::string glslSkin =
                    "layout(std140) uniform Bones { mat4 uBones[" + nBones + "]; };\nuniform sampler2D uSkinTex;\n"
                    "mat4 skinMatrix() {\n    int i = gl_VertexID * 2;\n    int W = textureSize(uSkinTex, 0).x;\n"
                    "    vec4 j = texelFetch(uSkinTex, ivec2(i % W, i / W), 0);\n    vec4 w = texelFetch(uSkinTex, ivec2((i + 1) % W, (i + 1) / W), 0);\n"
                    "    return uBones[int(j.x)] * w.x + uBones[int(j.y)] * w.y + uBones[int(j.z)] * w.z + uBones[int(j.w)] * w.w;\n}\n";
                const std::string hlslSkin =
                    "cbuffer Bones : register(b3) { float4x4 uBones[" + nBones + "]; };\nTexture2D<float4> uSkinTex : register(t14);\n"
                    "float4x4 skinMatrix(uint vid) {\n    uint W, H; uSkinTex.GetDimensions(W, H);\n    uint i = vid * 2;\n"
                    "    float4 j = uSkinTex.Load(int3(i % W, i / W, 0));\n    float4 w = uSkinTex.Load(int3((i + 1) % W, (i + 1) / W, 0));\n"
                    "    return uBones[(int)j.x] * w.x + uBones[(int)j.y] * w.y + uBones[(int)j.z] * w.z + uBones[(int)j.w] * w.w;\n}\n";
                auto replaceAll = [](std::string s, const std::string& a, const std::string& b) {
                    for (size_t p = s.find(a); p != std::string::npos; p = s.find(a, p + b.size())) s.replace(p, a.size(), b);
                    return s;
                };
                // GLSL PBR: model = uModel * skin (computed once at the top of main)
                std::string vs = withFrame(shaders::kPbrVsBody, true);
                size_t mainAt = vs.find("void main() {");
                std::string head = vs.substr(0, mainAt), body = vs.substr(mainAt);
                body = replaceAll(body, "uPrevModel * vec4", "(uPrevModel * skinM) * vec4");
                body = replaceAll(body, "uModel", "sModel");
                body = replaceAll(body, "void main() {", "void main() {\n    mat4 skinM = skinMatrix();\n    mat4 sModel = uModel * skinM;");
                std::string vsSkin = head + glslSkin + body;
                std::string hp = hlslFrame(h::kPbr);
                size_t vsAt = hp.find("PSIn VSMain(VSIn i) {");
                size_t vsEnd = hp.find("\n}\n", vsAt);
                std::string hlslBody;
                if (vsAt != std::string::npos && vsEnd != std::string::npos) {
                    std::string fn = hp.substr(vsAt, vsEnd + 3 - vsAt);
                    fn = replaceAll(fn, "mul(uPrevModel, float4", "mul(mul(uPrevModel, skinM), float4");
                    fn = replaceAll(fn, "uModel", "sModel");
                    fn = replaceAll(fn, "PSIn VSMain(VSIn i) {", "PSIn VSMain(VSIn i, uint vid : SV_VertexID) {\n    float4x4 skinM = skinMatrix(vid);\n    float4x4 sModel = mul(uModel, skinM);");
                    hlslBody = hp.substr(0, vsAt) + hlslSkin + fn + hp.substr(vsEnd + 3);
                }
                rhi::ShaderDesc d;
                d.name = "pbr_skinned";
                d.glslVertex = vsSkin;
                d.glslFragment = withFrame(shaders::kPbrFsBody, false);
                d.hlsl = hlslBody;
                d.uniformBlocks = {{"Frame", 0}, {"Bones", 2}};
                d.samplers = {{"uBaseColorTex", 0}, {"uNormalTex", 1}, {"uMRTex", 2}, {"uOcclusionTex", 3}, {"uEmissiveTex", 4}, {"uIrradiance", 5},
                              {"uPrefiltered", 6}, {"uBrdfLut", 7}, {"uCascadeShadow", 8}, {"uSpotShadow", 9}, {"uRtAo", 11}, {"uRtReflection", 12}, {"uSkinTex", 14}};
                m_pbrSkin = hlslBody.empty() && device.backend() != rhi::Backend::OpenGL ? rhi::ShaderHandle{} : m_device.createShader(d);
                std::string svs = shaders::kShadowVs;
                svs = replaceAll(svs, "void main() { gl_Position = uLightVP * uModel *", glslSkin + "void main() { gl_Position = uLightVP * uModel * skinMatrix() *");
                std::string shl = h::kShadow;
                shl = replaceAll(shl, "float4 VSMain(float3 pos : POSITION) : SV_Position { return mul(uLightVP, mul(uModel, float4(pos, 1.0))); }",
                                 hlslSkin + "float4 VSMain(float3 pos : POSITION, uint vid : SV_VertexID) : SV_Position { return mul(uLightVP, mul(mul(uModel, skinMatrix(vid)), float4(pos, 1.0))); }");
                rhi::ShaderDesc sd;
                sd.name = "shadow_skinned";
                sd.glslVertex = svs;
                sd.glslFragment = shaders::kShadowFs;
                sd.hlsl = shl;
                sd.uniformBlocks = {{"Frame", 0}, {"Bones", 2}};
                sd.samplers = {{"uSkinTex", 14}};
                m_shadowSkin = m_device.createShader(sd);
                m_boneUbo = m_device.createUniformBuffer(sizeof(Mat4) * kMaxBones);
                if (!m_pbrSkin || !m_shadowSkin) FLEDGE_LOG_WARN("render", "GPU skinning shaders unavailable: characters are skinned on the CPU");
                // 1.3.2: 8 skin influences (glTF JOINTS_1 / WEIGHTS_1). Separate variants of the shaders above, made by text
                // substitution, so 4-influence characters keep the exact 1.3.1 shaders (same pixels): the skin texture holds 4
                // texels per vertex (joints 1-4, weights 1-4, joints 5-8, weights 5-8) instead of 2.
                if (m_pbrSkin && m_shadowSkin) {
                    rhi::ShaderDesc d8 = d;
                    d8.name = "pbr_skinned8";
                    d8.glslVertex = skinEightInfluences(vsSkin);
                    d8.hlsl = hlslBody.empty() ? std::string() : skinEightInfluences(hlslBody);
                    rhi::ShaderDesc s8 = sd;
                    s8.name = "shadow_skinned8";
                    s8.glslVertex = skinEightInfluences(svs);
                    s8.hlsl = skinEightInfluences(shl);
                    if (!d8.glslVertex.empty() && !s8.glslVertex.empty()) {
                        m_pbrSkin8 = m_device.createShader(d8);
                        m_shadowSkin8 = m_device.createShader(s8);
                    }
                    if (!m_pbrSkin8 || !m_shadowSkin8)
                        FLEDGE_LOG_WARN("render", "8-influence GPU skinning shaders unavailable on {}: 8-influence characters are skinned on the CPU",
                                        rhi::toString(device.backend()));
                }
#if FLEDGE_GPU_SKIN_EXTENDED
                // Fledge Advanced (1.3.1): rigs with 257..1024 bones. Same shaders, but the palette is split over 4 uniform
                // blocks of 256 bones (16 KB each, the guaranteed block size on GL 3.3 / Vulkan): Bones (binding 2, b3) plus
                // Bones1..3 (bindings 3..5, b4..b6). Rigs with <= 256 bones keep the 1-block shaders above (same pixels as
                // Fledge). Only when the 1-block path exists (same backends: OpenGL, D3D11, D3D10, Vulkan, Null).
                // Created only when this executable runs as Fledge Advanced (maxGpuSkinBones() > 256): fledge_runtime of an
                // Advanced build (Standard edition, the core runtime) then behaves exactly like Fledge's.
                if (m_pbrSkin && m_shadowSkin && maxGpuSkinBones() > kMaxBones) {
                    const std::string nb = std::to_string(kGpuSkinBonesPerBlock);
                    const std::string glslXL =
                        "layout(std140) uniform Bones { mat4 uBones[" + nb + "]; };\nlayout(std140) uniform Bones1 { mat4 uBones1[" + nb +
                        "]; };\nlayout(std140) uniform Bones2 { mat4 uBones2[" + nb + "]; };\nlayout(std140) uniform Bones3 { mat4 uBones3[" + nb +
                        "]; };\nuniform sampler2D uSkinTex;\n"
                        "mat4 boneMatrix(float f) {\n    int b = clamp(int(f), 0, 1023);\n    if (b < 256) return uBones[b];\n"
                        "    if (b < 512) return uBones1[b - 256];\n    if (b < 768) return uBones2[b - 512];\n    return uBones3[b - 768];\n}\n"
                        "mat4 skinMatrix() {\n    int i = gl_VertexID * 2;\n    int W = textureSize(uSkinTex, 0).x;\n"
                        "    vec4 j = texelFetch(uSkinTex, ivec2(i % W, i / W), 0);\n    vec4 w = texelFetch(uSkinTex, ivec2((i + 1) % W, (i + 1) / W), 0);\n"
                        "    return boneMatrix(j.x) * w.x + boneMatrix(j.y) * w.y + boneMatrix(j.z) * w.z + boneMatrix(j.w) * w.w;\n}\n";
                    const std::string hlslXL =
                        "cbuffer Bones : register(b3) { float4x4 uBones[" + nb + "]; };\ncbuffer Bones1 : register(b4) { float4x4 uBones1[" + nb +
                        "]; };\ncbuffer Bones2 : register(b5) { float4x4 uBones2[" + nb + "]; };\ncbuffer Bones3 : register(b6) { float4x4 uBones3[" + nb +
                        "]; };\nTexture2D<float4> uSkinTex : register(t14);\n"
                        "float4x4 boneMatrix(float f) {\n    int b = clamp((int)f, 0, 1023);\n    if (b < 256) return uBones[b];\n"
                        "    if (b < 512) return uBones1[b - 256];\n    if (b < 768) return uBones2[b - 512];\n    return uBones3[b - 768];\n}\n"
                        "float4x4 skinMatrix(uint vid) {\n    uint W, H; uSkinTex.GetDimensions(W, H);\n    uint i = vid * 2;\n"
                        "    float4 j = uSkinTex.Load(int3(i % W, i / W, 0));\n    float4 w = uSkinTex.Load(int3((i + 1) % W, (i + 1) / W, 0));\n"
                        "    return boneMatrix(j.x) * w.x + boneMatrix(j.y) * w.y + boneMatrix(j.z) * w.z + boneMatrix(j.w) * w.w;\n}\n";
                    auto swap = [](std::string s, const std::string& a, const std::string& b) {
                        size_t p = s.find(a);
                        if (p != std::string::npos) s.replace(p, a.size(), b);
                        return s;
                    };
                    const std::vector<std::pair<std::string, int>> blocks = {{"Frame", 0}, {"Bones", 2}, {"Bones1", 3}, {"Bones2", 4}, {"Bones3", 5}};
                    rhi::ShaderDesc dx = d;
                    dx.name = "pbr_skinned_1024";
                    dx.glslVertex = swap(vsSkin, glslSkin, glslXL);
                    dx.hlsl = hlslBody.empty() ? std::string() : swap(hlslBody, hlslSkin, hlslXL);
                    dx.uniformBlocks = blocks;
                    rhi::ShaderDesc sx = sd;
                    sx.name = "shadow_skinned_1024";
                    sx.glslVertex = swap(svs, glslSkin, glslXL);
                    sx.hlsl = swap(shl, hlslSkin, hlslXL);
                    sx.uniformBlocks = blocks;
                    m_pbrSkinXL = m_device.createShader(dx);
                    m_shadowSkinXL = m_device.createShader(sx);
                    for (auto& b : m_boneUboXL) b = m_device.createUniformBuffer(sizeof(Mat4) * kGpuSkinBonesPerBlock);
                    if (!m_pbrSkinXL || !m_shadowSkinXL)
                        FLEDGE_LOG_WARN("render", "1024-bone GPU skinning shaders unavailable on {}: rigs with more than {} bones are skinned on the CPU",
                                        rhi::toString(device.backend()), kMaxBones);
                    if (m_pbrSkinXL && m_shadowSkinXL && m_pbrSkin8 && m_shadowSkin8) {  // 1.3.2: 8 influences on 257..1024-bone rigs
                        rhi::ShaderDesc dx8 = dx, sx8 = sx;
                        dx8.name = "pbr_skinned8_1024";
                        dx8.glslVertex = skinEightInfluences(dx.glslVertex);
                        dx8.hlsl = dx.hlsl.empty() ? std::string() : skinEightInfluences(dx.hlsl);
                        sx8.name = "shadow_skinned8_1024";
                        sx8.glslVertex = skinEightInfluences(sx.glslVertex);
                        sx8.hlsl = skinEightInfluences(sx.hlsl);
                        m_pbrSkinXL8 = m_device.createShader(dx8);
                        m_shadowSkinXL8 = m_device.createShader(sx8);
                    }
                }
#endif
            }
            if (!m_pbrInst || !m_shadowInst) FLEDGE_LOG_WARN("render", "instanced shaders unavailable: drawing items one by one");
        }
        m_sky = make("sky", withFrame(shaders::kSkyVsBody, true), withFrame(shaders::kSkyFsBody, false), hlslFrame(h::kSky), {{"uEnv", 0}});
        m_lines = make("lines", withFrame(shaders::kLinesVsBody, true), shaders::kLinesFs, hlslFrame(h::kLines), {});
        m_particleShader = make("particles", withFrame(shaders::kParticleVsBody, true), withFrame(shaders::kParticleFsBody, false), hlslFrame(h::kParticle), {});
        m_toCube = make("equirect_to_cube", kFullscreenVs, cubeShader(shaders::kEquirectToCubeFs), hlslCube(h::kEquirectToCube), {{"uEquirect", 0}});
        m_skyCube = make("procedural_sky", kFullscreenVs, cubeShader(shaders::kProceduralSkyFs), hlslCube(h::kProceduralSky), {});
        m_irradiance = make("irradiance", kFullscreenVs, cubeShader(shaders::kIrradianceFs), hlslCube(h::kIrradiance), {{"uEnv", 0}});
        m_prefilter = make("prefilter", kFullscreenVs, cubeShader(shaders::kPrefilterFs), hlslCube(h::kPrefilter), {{"uEnv", 0}});
        m_bloomDown = make("bloom_down", kFullscreenVs, shaders::kBloomDownFs, hlslPost(h::kBloomDown), {{"uSrc", 0}});
        m_bloomUp = make("bloom_up", kFullscreenVs, shaders::kBloomUpFs, hlslPost(h::kBloomUp), {{"uSrc", 0}});
        m_tonemap = make("tonemap", kFullscreenVs, shaders::kTonemapFs, hlslPost(h::kTonemap), {{"uHdr", 0}, {"uBloom", 1}, {"uExposureTex", 2}});
        m_fxaa = make("fxaa", kFullscreenVs, shaders::kFxaaFs, hlslPost(h::kFxaa), {{"uTex", 0}});
        m_taa = make("taa", kFullscreenVs, shaders::kTaaFs, hlslPost(h::kTaa), {{"uCurrent", 0}, {"uHistory", 1}, {"uVelocity", 2}, {"uDepth", 3}});
        m_blit = make("blit", kFullscreenVs, shaders::kBlitFs, hlslPost(h::kBlit), {{"uSrc", 0}});
        // 1.3.3 debug counter of NaN / Inf pixels (only used while setNonFiniteCheck(true))
        m_nonFinite = make("nonfinite_mask", kFullscreenVs, shaders::kNonFiniteFs, hlslPost(h::kNonFinite), {{"uHdr", 0}, {"uPost", 1}});
        m_nfInject = make("nonfinite_inject", kFullscreenVs, shaders::kInjectFs, hlslPost(h::kInject), {});
        {  // 1.2.0 effects (shaders_effects.h): Frame + common helpers + body on both languages
            namespace fx = shaders::effects;
            auto mk = [&](const char* name, const char* glsl, const char* hlsl, std::vector<std::pair<std::string, int>> samplers) {
                return make(name, kFullscreenVs, std::string("#version 330 core\n") + shaders::kFrameBlock + fx::kCommonGlsl + glsl,
                            std::string(h::kFrame) + h::kFullscreen + fx::kCommonHlsl + hlsl, std::move(samplers));
            };
            m_ssao = mk("ssao", fx::kSsaoGlsl, fx::kSsaoHlsl, {{"uDepth", 0}});
            m_ssaoBlur = mk("ssao_blur", fx::kSsaoBlurGlsl, fx::kSsaoBlurHlsl, {{"uAoTex", 0}, {"uDepth", 1}});
            m_ssr = mk("ssr", fx::kSsrGlsl, fx::kSsrHlsl, {{"uDepth", 0}, {"uPrevColor", 1}});
            m_volumetric = mk("volumetric", fx::kVolumetricGlsl, fx::kVolumetricHlsl, {{"uDepth", 0}, {"uCascade", 1}});
            m_lumaDown = mk("luma_down", fx::kLumaDownGlsl, fx::kLumaDownHlsl, {{"uSrc", 0}});
            m_lumaReduce = mk("luma_reduce", fx::kLumaReduceGlsl, fx::kLumaReduceHlsl, {{"uSrc", 0}});
            m_lumaAdapt = mk("luma_adapt", fx::kLumaAdaptGlsl, fx::kLumaAdaptHlsl, {{"uCurrent", 0}, {"uPrevious", 1}});
            m_dof = mk("dof", fx::kDofGlsl, fx::kDofHlsl, {{"uSrc", 0}, {"uDepth", 1}});
            m_motionBlur = mk("motion_blur", fx::kMotionBlurGlsl, fx::kMotionBlurHlsl, {{"uSrc", 0}, {"uVelocity", 1}});
            m_apply = mk("effect_apply", fx::kApplyGlsl, fx::kApplyHlsl, {{"uSrc", 0}});
        }
        rhi::Backend be = device.backend();
        if ((be == rhi::Backend::D3D11 || be == rhi::Backend::D3D12) && shaders::fsr1Available()) {  // FSR 1 needs SM 5 (Gather)
            m_fsrEasu = make("fsr_easu", {}, {}, shaders::fsr1EasuHlsl(), {});
            m_fsrRcas = make("fsr_rcas", {}, {}, shaders::fsr1RcasHlsl(), {});
        }
        if (device.rayTracing()) {  // DXR helper passes (HLSL only)
            m_rtGbuffer = make("rt_gbuffer", {}, {}, hlslFrame(h::kRtGbuffer), {});
            m_rtDenoise = make("rt_denoise", {}, {}, std::string(h::kFrame) + h::kFullscreen + h::kRtDenoise, {});
        }
        m_frameUbo = m_device.createUniformBuffer(sizeof(FrameData));
        m_vmFrameUbo = m_device.createUniformBuffer(sizeof(FrameData));  // 1.3.2 viewmodel pass
        m_pool = std::make_unique<TransientTexturePool>(m_device);

        // 1x1 defaults so every sampler is always bound to something valid.
        const uint8_t flat[4] = {128, 128, 255, 255};
        rhi::TextureDesc d1 = colorDesc(rhi::TextureFormat::RGBA8, 1, 1);
        d1.sampler.wrap = rhi::Wrap::Repeat;
        rhi::TextureSubresource s1{0, 0, 1, 1, flat, 4};
        m_flatNormal = m_device.createTexture(d1, std::span(&s1, 1));
        // BRDF LUT computed on the CPU (also unit-tested).
        auto lut = computeBrdfLut(kBrdfSize, 128);
        std::vector<float> rg;
        rg.reserve(lut.size() * 2);
        for (auto& v : lut) {
            rg.push_back(v.x);
            rg.push_back(v.y);
        }
        rhi::TextureDesc ld = colorDesc(rhi::TextureFormat::RG16F, kBrdfSize, kBrdfSize);
        rhi::TextureSubresource ls{0, 0, kBrdfSize, kBrdfSize, rg.data(), rg.size() * sizeof(float)};
        m_brdfLut = m_device.createTexture(ld, std::span(&ls, 1));
    }
    const uint8_t white[4] = {255, 255, 255, 255};
    m_white = m_device.createTexture2D(1, 1, white);

    std::vector<rhi::LineVertex> grid;
    const int n = 10;
    const float y = 0.002f;  // lift slightly above ground planes to avoid z-fighting
    for (int i = -n; i <= n; ++i) {
        float f = static_cast<float>(i);
        Vec3 c = i == 0 ? Vec3{0.45f, 0.45f, 0.5f} : Vec3{0.25f, 0.25f, 0.28f};
        grid.push_back({{f, y, static_cast<float>(-n)}, i == 0 ? Vec3{0.2f, 0.3f, 0.9f} : c});
        grid.push_back({{f, y, static_cast<float>(n)}, i == 0 ? Vec3{0.2f, 0.3f, 0.9f} : c});
        grid.push_back({{static_cast<float>(-n), y, f}, i == 0 ? Vec3{0.9f, 0.25f, 0.2f} : c});
        grid.push_back({{static_cast<float>(n), y, f}, i == 0 ? Vec3{0.9f, 0.25f, 0.2f} : c});
    }
    m_gridMesh = m_device.createLineMesh(grid);
    m_debugMesh = m_device.createLineMesh({});
}

ForwardRenderer::~ForwardRenderer() {
    m_pool.reset();
    for (auto& [n, m] : m_builtins) m_device.destroyMesh(m);
    m_device.destroyMesh(m_gridMesh);
    m_device.destroyMesh(m_debugMesh);
    if (m_overlayMesh) m_device.destroyMesh(m_overlayMesh);
    if (m_browserOverlayMesh) m_device.destroyMesh(m_browserOverlayMesh);
    for (auto m : m_particleMesh)
        if (m) m_device.destroyMesh(m);
    for (auto t : {m_white, m_flatNormal, m_brdfLut, m_envCube, m_irradianceCube, m_prefilteredCube, m_giTex})
        if (t) m_device.destroyTexture(t);
    if (m_frameUbo) m_device.destroyBuffer(m_frameUbo);
    if (m_vmFrameUbo) m_device.destroyBuffer(m_vmFrameUbo);
    if (m_instUbo) m_device.destroyBuffer(m_instUbo);
    if (m_boneUbo) m_device.destroyBuffer(m_boneUbo);
    for (auto s : {m_pbrInst, m_shadowInst, m_pbrSkin, m_shadowSkin, m_pbrSkin8, m_shadowSkin8, m_pbrDecal})
        if (s) m_device.destroyShader(s);
#if FLEDGE_GPU_SKIN_EXTENDED
    for (auto b : m_boneUboXL)
        if (b) m_device.destroyBuffer(b);
    for (auto s : {m_pbrSkinXL, m_shadowSkinXL, m_pbrSkinXL8, m_shadowSkinXL8})
        if (s) m_device.destroyShader(s);
#endif
    for (auto& [k, v] : m_views) releaseHistory(v);
    if (m_nfMask) m_device.destroyTexture(m_nfMask);
    for (auto s : {m_nonFinite, m_nfInject, m_pbr, m_shadow, m_sky, m_lines, m_toCube, m_skyCube, m_irradiance, m_prefilter, m_bloomDown, m_bloomUp, m_tonemap, m_fxaa,
                   m_basic, m_taa, m_blit, m_rtGbuffer, m_rtDenoise, m_fsrEasu, m_fsrRcas, m_overlayShader, m_particleShader, m_ssao, m_ssaoBlur,
                   m_ssr, m_volumetric, m_lumaDown, m_lumaReduce, m_lumaAdapt, m_dof, m_motionBlur, m_apply})
        if (s) m_device.destroyShader(s);
}

rhi::MeshHandle ForwardRenderer::builtinMesh(const std::string& name) {
    if (auto it = m_builtins.find(name); it != m_builtins.end()) return it->second;
    MeshData d = primitives::byName(name);
    if (d.empty()) return {};
    generateTangents(d);
    auto h = m_device.createMesh(d);
    m_builtins[name] = h;
    return h;
}

void ForwardRenderer::addLine(Vec3 a, Vec3 b, Vec3 color) {
    m_debugLines.push_back({a, color});
    m_debugLines.push_back({b, color});
}

void ForwardRenderer::addParticles(std::span<const ParticleItem> particles) { m_particles.insert(m_particles.end(), particles.begin(), particles.end()); }

int ForwardRenderer::drawParticles(const ViewItem& view) {
    if (m_particles.empty() || !m_particleShader) return 0;
    // camera basis from the view matrix rows (world -> view): right = row 0, up = row 1
    Vec3 right = normalize(Vec3{view.view.at(0, 0), view.view.at(0, 1), view.view.at(0, 2)});
    Vec3 up = normalize(Vec3{view.view.at(1, 0), view.view.at(1, 1), view.view.at(1, 2)});
    std::vector<std::pair<float, const ParticleItem*>> sorted[2];  // alpha (back-to-front), additive (any order)
    for (const auto& p : m_particles) {
        if (p.color.w <= 0.002f || p.size <= 0) continue;
        Vec3 d = p.position - view.position;
        sorted[p.additive ? 1 : 0].push_back({dot(d, d), &p});
    }
    std::sort(sorted[0].begin(), sorted[0].end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    size_t total = sorted[0].size() + sorted[1].size();
    if (total > kMaxParticles) {  // over budget: drop the farthest of each kind proportionally
        auto keep = [&](std::vector<std::pair<float, const ParticleItem*>>& v, bool farFirst) {
            size_t n = v.size() * kMaxParticles / total;
            if (!farFirst) std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() - n));
        };
        keep(sorted[0], true);
        keep(sorted[1], false);
    }
    m_device.setShader(m_particleShader);
    m_device.setDepthWrite(false);
    m_device.setCullMode(rhi::CullMode::None);
    static const Vec2 kCorner[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    int drawn = 0;
    for (int kind = 0; kind < 2; ++kind) {
        if (m_particleMesh[kind]) {
            m_device.destroyMesh(m_particleMesh[kind]);
            m_particleMesh[kind] = {};
        }
        if (sorted[kind].empty()) continue;
        MeshData md;
        md.vertices.reserve(sorted[kind].size() * 4);
        md.tangents.reserve(sorted[kind].size() * 4);
        md.indices.reserve(sorted[kind].size() * 6);
        for (const auto& [dist, p] : sorted[kind]) {
            auto q = particleQuad(*p, view.position, right, up);
            uint32_t base = static_cast<uint32_t>(md.vertices.size());
            Vec4 params{std::clamp(p->color.w, 0.0f, 1.0f), p->stretch > 0 ? 1.0f : 0.0f, p->additive ? 1.0f : 0.0f, 0};
            for (int c = 0; c < 4; ++c) {
                md.vertices.push_back({q[static_cast<size_t>(c)], p->color.xyz(), kCorner[c]});
                md.tangents.push_back(params);
            }
            for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) md.indices.push_back(base + i);
        }
        m_particleMesh[kind] = m_device.createMesh(md);
        m_device.setBlendMode(kind == 0 ? rhi::BlendMode::Alpha : rhi::BlendMode::Additive);
        m_device.draw(m_particleMesh[kind], rhi::Topology::Triangles);
        drawn += static_cast<int>(sorted[kind].size());
    }
    m_device.setBlendMode(rhi::BlendMode::None);
    m_device.setDepthWrite(true);
    m_device.setCullMode(rhi::CullMode::Back);
    return drawn;
}

void ForwardRenderer::setEnvironmentEquirect(int width, int height, std::vector<float> rgba) {
    m_equirect = std::move(rgba);
    m_equirectW = width;
    m_equirectH = height;
    m_procedural = m_equirect.empty();
    m_envDirty = true;
}

void ForwardRenderer::setProceduralSky() {
    if (m_procedural && m_envCube) return;
    m_equirect.clear();
    m_procedural = true;
    m_envDirty = true;
}

void ForwardRenderer::setGiBake(const GiBake& bake) {
    if (m_giTex) {
        m_device.destroyTexture(m_giTex);
        m_giTex = {};
    }
    m_giMin = {0, 0, 0, 1};
    m_giExt = {1, 1, 1, 0};
    if (bake.rgba.empty() || bake.gx <= 0 || bake.gy <= 0 || bake.gz <= 0) return;
    m_giTex = m_device.createTexture2D(bake.gx, bake.gy * bake.gz, bake.rgba.data());
    m_giMin = Vec4(bake.min, static_cast<float>(bake.gx));
    m_giExt = Vec4(bake.extent, static_cast<float>(bake.gy) + static_cast<float>(bake.gz) * 16.0f);
}

// ------------------------------------------------------------ environment ---
void ForwardRenderer::ensureEnvironment(Vec3 sunDir) {
    if (m_procedural && m_envCube && dot(normalize(sunDir), m_envSun) < 0.9995f) m_envDirty = true;  // sun moved
    if (!m_envDirty && m_envCube) return;
    m_envDirty = false;
    m_envSun = normalize(sunDir);
    for (auto* t : {&m_envCube, &m_irradianceCube, &m_prefilteredCube})
        if (*t) {
            m_device.destroyTexture(*t);
            *t = {};
        }
    auto cubeDesc = [](int size, int mips) {
        rhi::TextureDesc d;
        d.type = rhi::TextureType::Cube;
        d.format = rhi::TextureFormat::RGBA16F;
        d.width = d.height = size;
        d.mips = mips;
        d.sampler = {rhi::Filter::Linear, mips > 1, rhi::Wrap::Clamp, false};
        return d;
    };
    int envMips = 1;
    while ((kEnvSize >> envMips) > 0) ++envMips;
    m_envCube = m_device.createTexture(cubeDesc(kEnvSize, envMips));
    m_irradianceCube = m_device.createTexture(cubeDesc(kIrradianceSize, 1));
    m_prefilteredCube = m_device.createTexture(cubeDesc(kPrefilterSize, kPrefilterMips));

    auto renderFaces = [&](rhi::TextureHandle cube, int size, int mip, auto&& setup) {
        for (int face = 0; face < 6; ++face) {
            rhi::FramebufferDesc fd;
            fd.colorCount = 1;
            fd.colors[0] = {cube, mip, face};
            auto fb = m_device.createFramebuffer(fd);
            rhi::ClearDesc cd;
            cd.depth = false;
            m_device.beginPass(fb, 0, 0, std::max(1, size >> mip), std::max(1, size >> mip), cd);
            m_device.setDepthTest(false);
            m_device.setCullMode(rhi::CullMode::None);
            setup();
            m_device.setUniform("uFace", face);
            m_device.drawFullscreenTriangle();
            m_device.endPass();
            m_device.destroyFramebuffer(fb);
        }
    };
    rhi::TextureHandle equirect;
    if (!m_procedural && !m_equirect.empty()) {
        rhi::TextureDesc ed = colorDesc(rhi::TextureFormat::RGBA32F, m_equirectW, m_equirectH);
        ed.sampler.wrap = rhi::Wrap::Repeat;
        rhi::TextureSubresource s{0, 0, m_equirectW, m_equirectH, m_equirect.data(), m_equirect.size() * sizeof(float)};
        equirect = m_device.createTexture(ed, std::span(&s, 1));
    }
    if (equirect) {
        renderFaces(m_envCube, kEnvSize, 0, [&] {
            m_device.setShader(m_toCube);
            m_device.bindTexture(0, equirect);
        });
        m_device.destroyTexture(equirect);
    } else {
        renderFaces(m_envCube, kEnvSize, 0, [&] {
            m_device.setShader(m_skyCube);
            m_device.setUniform("uSunDir", m_envSun);
        });
    }
    m_device.generateMips(m_envCube);
    renderFaces(m_irradianceCube, kIrradianceSize, 0, [&] {
        m_device.setShader(m_irradiance);
        m_device.bindTexture(0, m_envCube);
    });
    for (int mip = 0; mip < kPrefilterMips; ++mip) {
        float rough = static_cast<float>(mip) / static_cast<float>(kPrefilterMips - 1);
        renderFaces(m_prefilteredCube, kPrefilterSize, mip, [&] {
            m_device.setShader(m_prefilter);
            m_device.bindTexture(0, m_envCube);
            m_device.setUniform("uRoughness", rough);
            m_device.setUniform("uEnvSize", static_cast<float>(kEnvSize));
        });
    }
    FLEDGE_LOG_INFO("render", "Environment built ({}): env {}^2, irradiance {}^2, prefiltered {}^2 x{} mips",
                  m_procedural || !equirect ? "procedural sky" : "HDR", kEnvSize, kIrradianceSize, kPrefilterSize, kPrefilterMips);
}

// ---------------------------------------------------------------- render ---
void ForwardRenderer::render(rhi::RenderTargetHandle target, int width, int height, const ViewItem& view,
                             const std::vector<DrawItem>& items, const std::vector<LightItem>& lights, const RenderOptions& options) {
    m_device.stats() = {};
    width = std::max(1, width);
    height = std::max(1, height);
    if (m_full) renderFull(target ? m_device.framebufferOf(target) : rhi::FramebufferHandle{}, width, height, view, items, lights, options);
    else renderBasic(target, width, height, view, items, lights, options);
    m_debugLines.clear();
    m_overlay.clear();
    m_particles.clear();
}

void ForwardRenderer::drawOverlay(int width, int height) {
    if ((!m_browserOverlay && m_overlay.empty()) || !m_overlayShader) return;
    m_device.setDepthTest(false);
    m_device.setDepthWrite(false);
    m_device.setCullMode(rhi::CullMode::None);
    m_device.setBlendMode(rhi::BlendMode::Alpha);
    m_device.setShader(m_overlayShader);
    auto draw = [&](std::span<const OverlayVertex> vertices, rhi::TextureHandle texture, rhi::MeshHandle& mesh, bool browser) {
        if (vertices.empty()) return;
        if (mesh) m_device.destroyMesh(mesh);
        MeshData md;
        md.vertices.reserve(vertices.size());
        // UV rides in the unused position z and texcoord y.
        for (const auto& v : vertices) md.vertices.push_back({{v.pos.x, v.pos.y, v.uv.x}, {v.color.x, v.color.y, v.color.z}, {v.color.w, v.uv.y}});
        for (uint32_t i = 0; i < md.vertices.size(); ++i) md.indices.push_back(i);
        mesh = m_device.createMesh(md);
        if (!mesh) return;
        m_device.setUniform("uScreen", Vec4(static_cast<float>(width), static_cast<float>(height), browser ? 1.0f : 0.0f, 0));
        m_device.bindTexture(0, texture ? texture : m_white);
        m_device.draw(mesh, rhi::Topology::Triangles);
    };
    draw(m_overlay, m_overlayTexture, m_overlayMesh, false);
    if (m_browserOverlay) {
        const float w = static_cast<float>(width), h = static_cast<float>(height);
        constexpr float inset = 0.0001f; // UV (0,0) is reserved for untextured geometry.
        const Vec4 white{1, 1, 1, 1};
        const OverlayVertex a{{0, 0}, white, {inset, inset}}, b{{w, 0}, white, {1, inset}},
                            c{{w, h}, white, {1, 1}}, d{{0, h}, white, {inset, 1}};
        const OverlayVertex quad[] = {a, b, c, a, c, d};
        draw(quad, m_browserOverlay, m_browserOverlayMesh, true);
    }
    m_device.setBlendMode(rhi::BlendMode::None);
    m_device.setDepthWrite(true);
    m_device.setDepthTest(true);
}

void ForwardRenderer::renderBasic(rhi::RenderTargetHandle target, int width, int height, const ViewItem& view,
                                  const std::vector<DrawItem>& items, const std::vector<LightItem>& lights, const RenderOptions& options) {
    m_frame = {};
    m_frame.passes = {"basic.forward"};
    m_frame.renderWidth = width;
    m_frame.renderHeight = height;
    Mat4 proj = clipFix(m_device.caps().depthZeroToOne) * view.projection;
    Mat4 viewProj = proj * view.view;
    Vec3 lightDir{0.3f, -1.0f, 0.2f}, lightColor{1, 1, 1};
    for (auto& l : lights)
        if (l.kind == LightKind::Directional) {
            lightDir = l.direction;
            lightColor = l.color * l.intensity;
            break;
        }
    Vec4 clear = options.clearColor;
    for (int i = 0; i < 3; ++i) clear[i] = std::pow(clear[i], 1.0f / 2.2f);
    m_device.beginPass(target, width, height, clear);
    m_device.setShader(m_basic);
    m_device.setUniform("uViewProj", viewProj);
    m_device.setUniform("uAmbient", options.ambient);
    m_device.setUniform("uLightDir", lightDir);
    m_device.setUniform("uLightColor", lightColor);
    // 1.3.2: viewmodel items last, with their own projection in the front depth slice (same rule as the full pipeline)
    const bool vmPass = view.viewmodelPass;
    for (int pass = 0; pass < (vmPass ? 2 : 1); ++pass) {
        if (pass == 1) m_device.setUniform("uViewProj", clipFix(m_device.caps().depthZeroToOne) * viewmodelDepthRemap() * view.viewmodelProjection * view.view);
        for (const auto& item : items) {
            if (item.shadowOnly || item.offCamera || item.decal) continue;  // the basic path has no shadow maps (and no decal shader)
            if (vmPass && item.viewmodel != (pass == 1)) continue;
            m_device.setUniform("uModel", item.model);
            m_device.setUniform("uColor", item.color);
            m_device.setCullMode(item.twoSided ? rhi::CullMode::None : rhi::CullMode::Back);
            m_device.bindTexture(0, item.texture ? item.texture : m_white);
            m_device.draw(item.mesh, rhi::Topology::Triangles);
            if (pass == 1) ++m_frame.viewmodelItems;
        }
    }
    drawOverlay(width, height);
    m_device.endPass();
}

void ForwardRenderer::bindMaterial(const DrawItem& item) {
    m_device.setUniform("uModel", item.model);
    m_device.setUniform("uBaseColor", Vec4(item.color, item.opacity));
    m_device.setUniform("uEmissive", item.emissive);
    m_device.setUniform("uRoughness", item.roughness);
    m_device.setUniform("uMetallic", item.metallic);
    m_device.setUniform("uNormalScale", item.normalScale);
    m_device.setUniform("uOcclusionStrength", item.occlusionStrength);
    m_device.setUniform("uAlphaCutoff", item.alphaCutoff);
    m_device.setUniform("uAlphaMode", item.alphaMode);
    m_device.setUniform("uUnlit", item.unlit ? 1 : 0);
    m_device.setUniform("uSelected", item.selected ? 1.0f : 0.0f);
    m_device.setCullMode(item.twoSided ? rhi::CullMode::None : rhi::CullMode::Back);
    m_device.bindTexture(0, item.texture ? item.texture : m_white);
    m_device.bindTexture(1, item.normalTexture ? item.normalTexture : m_flatNormal);
    m_device.bindTexture(2, item.metallicRoughnessTexture ? item.metallicRoughnessTexture : m_white);
    m_device.bindTexture(3, item.occlusionTexture ? item.occlusionTexture : m_white);
    m_device.bindTexture(4, item.emissiveTexture ? item.emissiveTexture : m_white);
    m_device.bindTexture(15, m_giTex ? m_giTex : m_white);
    m_device.setUniform("uGiMin", m_giMin);
    m_device.setUniform("uGiExt", m_giExt);
}

namespace {
bool sameMaterial(const DrawItem& a, const DrawItem& b) {
    return a.mesh.id == b.mesh.id && a.texture.id == b.texture.id && a.normalTexture.id == b.normalTexture.id &&
           a.metallicRoughnessTexture.id == b.metallicRoughnessTexture.id && a.occlusionTexture.id == b.occlusionTexture.id &&
           a.emissiveTexture.id == b.emissiveTexture.id && a.color == b.color && a.emissive == b.emissive && a.roughness == b.roughness &&
           a.metallic == b.metallic && a.opacity == b.opacity && a.alphaMode == b.alphaMode && a.alphaCutoff == b.alphaCutoff &&
           a.twoSided == b.twoSided && a.unlit == b.unlit && a.selected == b.selected && a.normalScale == b.normalScale &&
           a.occlusionStrength == b.occlusionStrength;
}
} // namespace

#if FLEDGE_GPU_SKIN_EXTENDED
// Uploads the bone palette of a GPU-skinned item. xl = the 4-block variant (257..1024 bones): block k holds bones
// [256k, 256k + 256); only the blocks the rig uses are written (bounded: at most 64 KB per draw), the rest stay bound.
void ForwardRenderer::bindBones(const DrawItem& item, bool xl) {
    const int n = std::min(item.boneCount, xl ? kMaxGpuSkinBonesAdvanced : kMaxBones);
    m_device.updateBuffer(m_boneUbo, item.bones, sizeof(Mat4) * static_cast<size_t>(std::min(n, kGpuSkinBonesPerBlock)));
    m_device.bindUniformBuffer(2, m_boneUbo);
    if (!xl) return;
    for (int k = 1; k < 4; ++k) {
        int count = std::clamp(n - k * kGpuSkinBonesPerBlock, 0, kGpuSkinBonesPerBlock);
        rhi::BufferHandle b = m_boneUboXL[k - 1];
        if (count > 0) m_device.updateBuffer(b, item.bones + k * kGpuSkinBonesPerBlock, sizeof(Mat4) * static_cast<size_t>(count));
        m_device.bindUniformBuffer(2 + k, b);
    }
}
#endif

rhi::ShaderHandle ForwardRenderer::skinShader(const DrawItem& item, bool shadow, bool& xl) const {
    const bool eight = item.skinInfluences > 4;
    xl = false;
#if FLEDGE_GPU_SKIN_EXTENDED
    if (item.boneCount > kMaxBones && (shadow ? m_shadowSkinXL : m_pbrSkinXL)) {
        xl = true;
        if (eight) return shadow ? m_shadowSkinXL8 : m_pbrSkinXL8;
        return shadow ? m_shadowSkinXL : m_pbrSkinXL;
    }
#endif
    if (eight) return shadow ? m_shadowSkin8 : m_pbrSkin8;
    return shadow ? m_shadowSkin : m_pbrSkin;
}

int ForwardRenderer::gpuSkinInfluences() const {
    if (!gpuSkinning()) return 0;
    bool eight = m_pbrSkin8 && m_shadowSkin8;
#if FLEDGE_GPU_SKIN_EXTENDED
    if (m_pbrSkinXL && m_shadowSkinXL && !(m_pbrSkinXL8 && m_shadowSkinXL8)) eight = false;  // one rule for every bone count
#endif
    return eight ? 8 : 4;
}

int ForwardRenderer::gpuSkinBoneLimit() const {
    if (!gpuSkinning()) return 0;
#if FLEDGE_GPU_SKIN_EXTENDED
    if (m_pbrSkinXL && m_shadowSkinXL && m_boneUboXL[0] && m_boneUboXL[1] && m_boneUboXL[2]) return maxGpuSkinBones();
#endif
    return kMaxBones;
}

std::string ForwardRenderer::gpuSkinNote() const {
    const char* api = rhi::toString(m_device.backend());
    if (!gpuSkinningEnabled) return "GPU skinning is turned off (--cpu-skinning or a test): characters are skinned on the CPU";
    if (!m_full) return std::string(api) + " runs the basic forward path: no GPU skinning, characters are skinned on the CPU";
    if (!gpuSkinning()) return std::string(api) + " has no GPU skinning path: characters are skinned on the CPU";
    const int edCap = maxGpuSkinBones();
    if (gpuSkinBoneLimit() < edCap)
        return std::format("{}: the {}-bone shader variant is unavailable, rigs above {} bones are skinned on the CPU", api, edCap, gpuSkinBoneLimit());
    if (edCap < kMaxGpuSkinBonesAdvanced)
        return std::format("{} skins up to {} bones on the GPU (Fledge Advanced: up to {})", editionName(currentEdition()), edCap, kMaxGpuSkinBonesAdvanced);
    return "";
}

std::string advancedRenderNote(const char* feature) {
#if FLEDGE_ADVANCED_RENDER
    if (!advancedRenderFeatures())
        return std::format("{} need Fledge Advanced at run time: this executable runs as {} (packaged games that use them need the Advanced runtime)",
                           feature, editionName(currentEdition()));
    return {};
#else
    return std::format("{} are a Fledge Advanced feature: {} loads the data and ignores it", feature, editionName(currentEdition()));
#endif
}

Mat4 viewmodelDepthRemap(float slice) {
    // GL clip space: z' = slice * z + (slice - 1) * w, so NDC z in [-1, 1] lands in [-1, -1 + 2 * slice]
    Mat4 r;
    r.c[2] = {0, 0, slice, 0};
    r.c[3] = {0, 0, slice - 1.0f, 1};
    return r;
}

SkinningDecision skinningDecision(int bones, int gpuLimit, bool crowd) {
    SkinningDecision d;
    d.bones = bones;
    d.limit = gpuLimit;
    if (crowd) {
        d.reason = "crowd agent: shares a CPU-skinned mesh with the other agents";
    } else if (gpuLimit <= 0) {
        d.reason = "no GPU skinning on this renderer / backend";
    } else if (bones > gpuLimit) {
        d.reason = std::format("{} bones is more than the GPU skinning limit of {} here ({}: up to {}; Fledge {}, Fledge Advanced {})", bones, gpuLimit,
                               editionName(currentEdition()), maxGpuSkinBones(), kMaxGpuSkinBones, kMaxGpuSkinBonesAdvanced);
    } else {
        d.gpu = true;
        d.reason = bones > kMaxGpuSkinBones ? std::format("{} bones: vertex shader, 4-block bone palette (Fledge Advanced)", bones)
                                            : std::format("{} bones: vertex shader, 1-block bone palette", bones);
    }
    return d;
}

namespace {
uint64_t shadowMix(uint64_t h, uint64_t v) {
    h ^= v;
    return h * 1099511628211ull;
}
// True when a moving caster overlaps this cascade, so the cached depth cannot be reused.
bool shadowCascadeDynamic(const std::vector<DrawItem>& items, const Mat4& vp, uint64_t& key) {
    key = 14695981039346656037ull;
    for (const auto& item : items) {
        if (!item.castShadows || item.alphaMode == 2 || item.viewmodel || item.decal) continue;
        if (!sphereInClip(vp, item.boundsCenter, item.boundsRadius)) continue;
        if (item.bones || !item.staticCaster) return true;
        key = shadowMix(key, item.mesh.id);
        const float* f = item.model.data();
        for (int i = 0; i < 16; ++i) {
            uint32_t bits = 0;
            std::memcpy(&bits, f + i, sizeof(bits));
            key = shadowMix(key, bits);
        }
    }
    return false;
}
bool sameMatrix(const Mat4& a, const Mat4& b) { return std::memcmp(a.data(), b.data(), sizeof(float) * 16) == 0; }
} // namespace

void ForwardRenderer::drawShadowCasters(const std::vector<DrawItem>& items, const Mat4& lightVP, bool depthPrepass) {
    m_device.setCullMode(rhi::CullMode::None);  // robust for open meshes; bias handles acne
    std::vector<const DrawItem*> casters;
    for (const auto& item : items) {
        if ((!item.castShadows && !depthPrepass) || item.alphaMode == 2) continue;
        if (depthPrepass && (item.shadowOnly || item.offCamera)) continue;  // not in the camera view: shadow maps only
        if (item.viewmodel || item.decal) continue;  // 1.3.2: viewmodel items / decals cast no shadow, write no prepass depth
        if (!sphereInClip(lightVP, item.boundsCenter, item.boundsRadius)) {  // per-cascade caster culling
            if (!depthPrepass) ++m_frame.shadowCulled;
            continue;
        }
        if (item.bones && gpuSkinning()) {  // skinned caster
            bool xl = false;
            const rhi::ShaderHandle sh = skinShader(item, true, xl);
            if (!sh) continue;  // no variant for it (AnimationSystem CPU-skins such characters; never drawn half-skinned)
#if FLEDGE_GPU_SKIN_EXTENDED
            m_device.setShader(sh);
            m_device.setUniform("uLightVP", lightVP);
            m_device.setUniform("uModel", item.model);
            bindBones(item, xl);
#else
            m_device.setShader(sh);
            m_device.setUniform("uLightVP", lightVP);
            m_device.setUniform("uModel", item.model);
            m_device.updateBuffer(m_boneUbo, item.bones, sizeof(Mat4) * static_cast<size_t>(std::min(item.boneCount, kMaxBones)));
            m_device.bindUniformBuffer(2, m_boneUbo);
#endif
            m_device.bindTexture(14, item.skinTexture);
            m_device.draw(item.mesh, rhi::Topology::Triangles);
            continue;
        }
        casters.push_back(&item);
    }
    bool inst = instancingEnabled && m_shadowInst && m_instUbo;
    if (inst) {  // group by mesh
        std::stable_sort(casters.begin(), casters.end(), [](const DrawItem* a, const DrawItem* b) { return a->mesh.id < b->mesh.id; });
        m_device.setShader(m_shadowInst);
        m_device.setUniform("uLightVP", lightVP);
        size_t i = 0;
        std::vector<Mat4> mats;
        while (i < casters.size()) {
            size_t j = i;
            while (j < casters.size() && casters[j]->mesh.id == casters[i]->mesh.id && j - i < static_cast<size_t>(kMaxInstances)) ++j;
            if (j - i >= 2) {
                mats.clear();
                for (size_t k = i; k < j; ++k) mats.push_back(casters[k]->model);
                m_device.updateBuffer(m_instUbo, mats.data(), mats.size() * sizeof(Mat4));
                m_device.bindUniformBuffer(1, m_instUbo);
                m_device.drawInstanced(casters[i]->mesh, static_cast<int>(j - i));
                ++m_frame.instancedDraws;
                m_frame.instancedItems += static_cast<int>(j - i);
                i = j;
                continue;
            }
            m_device.setShader(m_shadow);
            m_device.setUniform("uLightVP", lightVP);
            m_device.setUniform("uModel", casters[i]->model);
            m_device.draw(casters[i]->mesh, rhi::Topology::Triangles);
            m_device.setShader(m_shadowInst);
            m_device.setUniform("uLightVP", lightVP);
            ++i;
        }
        return;
    }
    m_device.setShader(m_shadow);
    m_device.setUniform("uLightVP", lightVP);
    for (const DrawItem* item : casters) {
        m_device.setUniform("uModel", item->model);
        m_device.draw(item->mesh, rhi::Topology::Triangles);
    }
}

void ForwardRenderer::drawOpaque(const std::vector<const DrawItem*>& items, const std::function<const Mat4&(const DrawItem&)>& prevModel, int rtFlags,
                                 Vec4 screen) {
    bool inst = instancingEnabled && m_pbrInst && m_instUbo;
    std::vector<bool> done(items.size(), false);
    std::vector<Mat4> mats;
    for (size_t i = 0; i < items.size(); ++i) {
        if (done[i]) continue;
        const DrawItem& a = *items[i];
        mats.clear();
        if (a.bones && gpuSkinning()) {  // GPU-skinned character: never instanced (bones per draw)
            done[i] = true;
            bool xl = false;
            const rhi::ShaderHandle sh = skinShader(a, false, xl);
            if (!sh) continue;
            m_device.setShader(sh);
            bindMaterial(a);
            m_device.setUniform("uPrevModel", prevModel(a));
            m_device.setUniform("uRtFlags", rtFlags);
            m_device.setUniform("uScreen", screen);
#if FLEDGE_GPU_SKIN_EXTENDED
            bindBones(a, xl);
#else
            m_device.updateBuffer(m_boneUbo, a.bones, sizeof(Mat4) * static_cast<size_t>(std::min(a.boneCount, kMaxBones)));
            m_device.bindUniformBuffer(2, m_boneUbo);
#endif
            m_device.bindTexture(14, a.skinTexture);
            m_device.draw(a.mesh, rhi::Topology::Triangles);
            ++m_frame.gpuSkinned;
            continue;
        }
        std::vector<size_t> group{i};
        // static this frame (prev == current, the whole matrix: a spinning object keeps its motion vectors) and identical
        // material: instance it
        auto still = [&](const DrawItem& d) {
            const Mat4& p = prevModel(d);
            return p.c[0] == d.model.c[0] && p.c[1] == d.model.c[1] && p.c[2] == d.model.c[2] && p.c[3] == d.model.c[3];
        };
        if (inst && a.instanceable && still(a)) {
            for (size_t j = i + 1; j < items.size() && group.size() < static_cast<size_t>(kMaxInstances); ++j)
                if (!done[j] && items[j]->instanceable && sameMaterial(a, *items[j]) && still(*items[j])) group.push_back(j);
        }
        if (group.size() >= 2) {
            for (size_t k : group) {
                mats.push_back(items[k]->model);
                done[k] = true;
            }
            m_device.setShader(m_pbrInst);
            bindMaterial(a);
            m_device.setUniform("uRtFlags", rtFlags);
            m_device.setUniform("uScreen", screen);
            m_device.updateBuffer(m_instUbo, mats.data(), mats.size() * sizeof(Mat4));
            m_device.bindUniformBuffer(1, m_instUbo);
            m_device.drawInstanced(a.mesh, static_cast<int>(mats.size()));
            ++m_frame.instancedDraws;
            m_frame.instancedItems += static_cast<int>(mats.size());
            continue;
        }
        done[i] = true;
        m_device.setShader(m_pbr);
        bindMaterial(a);
        m_device.setUniform("uPrevModel", prevModel(a));
        m_device.setUniform("uRtFlags", rtFlags);
        m_device.setUniform("uScreen", screen);
        m_device.draw(a.mesh, rhi::Topology::Triangles);
    }
}

void ForwardRenderer::releaseHistory(ViewHistory& h) {
    auto drop = [&](rhi::TextureHandle& t) {
        if (t) {
            m_historyDescs.erase(t.id);
            m_device.destroyTexture(t);
        }
        t = {};
    };
    for (auto& t : h.taa) drop(t);
    for (auto& t : h.ssrColor) drop(t);
    for (auto& t : h.exposure) drop(t);
    h.ssrValid = h.exposureValid = false;
    for (auto& e : h.rt)
        for (auto& t : e) drop(t);
    drop(h.pathTrace);
    h.taaValid = false;
    for (bool& v : h.rtValid) v = false;
    h.ptFrames = 0;
}

rhi::TextureHandle ForwardRenderer::historyTexture(rhi::TextureHandle& slot, const rhi::TextureDesc& d) {
    if (slot) {
        auto it = m_historyDescs.find(slot.id);
        if (it != m_historyDescs.end() && it->second == d) return slot;
        m_historyDescs.erase(slot.id);
        m_device.destroyTexture(slot);
    }
    slot = m_device.createTexture(d);
    if (slot) m_historyDescs[slot.id] = d;
    return slot;
}

namespace {
int rtRays(RtQuality q) { return q == RtQuality::Ultra ? 4 : q == RtQuality::High ? 2 : 1; }
} // namespace

void ForwardRenderer::renderFull(rhi::FramebufferHandle out, int width, int height, const ViewItem& view, const std::vector<DrawItem>& items,
                                 const std::vector<LightItem>& lights, const RenderOptions& options) {
    RenderSettings rs = options.settings;
    rs.sanitize();
    GraphicsSettings gfx = options.graphics;
    gfx.sanitize();
    m_frame = {};
    m_frame.fullPipeline = true;
    ++m_frameCounter;
    const rhi::DeviceCaps caps = m_device.caps();
    const bool d3d = caps.depthZeroToOne;
    const bool debug = rs.debugView != DebugView::None;

    // --- upscaler / render resolution -----------------------------------------------------------
    UpscalerKind upscaler = gfx.upscaler;
    rhi::VendorUpscaler* vendor = nullptr;
    if (upscaler == UpscalerKind::Dlss || upscaler == UpscalerKind::XeSS || upscaler == UpscalerKind::Fsr) {
        // availability + fallback chain (DLSS -> XeSS -> FSR -> TAA), re-evaluated when settings change or every ~2 s
        std::string key = std::string(toString(upscaler)) + "|" + gfx.upscalerVersion;
        if (key != m_upscalerKey || m_frameCounter >= m_upscalerCheckFrame) {
            m_upscalerKey = key;
            m_upscalerCheckFrame = m_frameCounter + 120;
            auto res = upscalers::resolve(gfx, m_device.backend(), m_device.description(), m_fsrEasu && m_fsrRcas, enginePaths().exeDir);
            m_upscalerResolved = res.kind;
            m_upscalerNoteCache = res.note;
            m_vendorLibrary = res.version ? narrow(upscalers::runtimeLibrary(*res.version, enginePaths().exeDir).wstring()) : std::string();
        }
        upscaler = m_upscalerResolved;
        m_frame.upscalerNote = m_upscalerNoteCache;
        if (upscaler == UpscalerKind::XeSS) {
            vendor = m_vendorLibrary.empty() || m_vendorFailed.count(m_vendorLibrary) ? nullptr : m_device.vendorUpscaler("xess", m_vendorLibrary);
            if (!vendor) {
                m_frame.upscalerNote += (m_frame.upscalerNote.empty() ? "" : "; ") + std::string("XeSS runtime could not be started");
                upscaler = m_fsrEasu && m_fsrRcas ? UpscalerKind::Fsr : UpscalerKind::Taa;
            }
        }
    }
    if (upscaler == UpscalerKind::Fsr && !(m_fsrEasu && m_fsrRcas)) {
        m_frame.upscalerNote = "FSR 1 needs the HLSL FidelityFX shaders (Direct3D 11/12); using the built-in temporal upscaler";
        upscaler = UpscalerKind::Taa;
    }
    if (upscaler == UpscalerKind::Taa && !m_taa) upscaler = UpscalerKind::Off;
    if (debug || !options.temporal || (options.pathTraceReference && m_device.rayTracing())) upscaler = UpscalerKind::Off;
    float scale = upscaler == UpscalerKind::Off ? gfx.renderScale : upscalerScale(gfx.upscalerQuality);
    if (debug || (options.pathTraceReference && m_device.rayTracing())) scale = 1.0f;  // debug views / reference at native res
    const int rw = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * scale)));
    const int rh = std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * scale)));
    const bool temporal = upscaler == UpscalerKind::Taa || upscaler == UpscalerKind::Fsr || upscaler == UpscalerKind::XeSS;
    m_frame.upscaler = upscaler;
    m_frame.renderWidth = rw;
    m_frame.renderHeight = rh;

    ViewHistory& hist = m_views[(static_cast<uint64_t>(out.id) << 1) | (options.temporal ? 1u : 0u)];
    if (hist.outW != width || hist.outH != height || hist.renderW != rw || hist.renderH != rh) {
        releaseHistory(hist);
        hist.prevValid = false;
        hist.outW = width;
        hist.outH = height;
        hist.renderW = rw;
        hist.renderH = rh;
    }
    hist.lastUsed = m_frameCounter;
    if (!temporal) hist.taaValid = false;

    // jitter (Halton 2,3 over 8 frames) in NDC; jitterUv = the same shift in texture UV of this API
    Vec2 jitterNdc{0, 0}, jitterUv{0, 0};
    if (temporal) {
        Vec2 j = taaJitter(static_cast<uint32_t>(hist.frame));
        jitterNdc = {j.x * 2.0f / static_cast<float>(rw), j.y * 2.0f / static_cast<float>(rh)};
        jitterUv = {jitterNdc.x * 0.5f, (d3d ? -0.5f : 0.5f) * jitterNdc.y};
    }
    Mat4 jitter;
    jitter.c[3] = {jitterNdc.x, jitterNdc.y, 0, 1};

    // --- ray tracing / path tracing availability ----------------------------------------------------
    rhi::RayTracingDevice* rtDev = m_device.rayTracing();
    bool pathTrace = options.pathTraceReference && rtDev && !debug;
    if (options.pathTraceReference && !rtDev) m_frame.rayTracingNote = "path-traced reference needs Direct3D 12 with DXR";
    bool rtWanted = gfx.rt.enabled && !debug && !pathTrace;
    if (rtWanted && !rtDev) {
        rtWanted = false;
        auto s = rhi::featureSupport(m_device.backend(), rhi::GfxFeature::RayTracing, &caps);
        m_frame.rayTracingNote = s.supported ? "ray tracing is unavailable on this device" : s.reason;
    }
    const bool rtShadowFx = rtWanted && gfx.rt.shadows, rtAoFx = rtWanted && gfx.rt.ambientOcclusion, rtReflFx = rtWanted && gfx.rt.reflections,
               rtGiFx = rtWanted && gfx.rt.globalIllumination;
    const bool rtActive = rtShadowFx || rtAoFx || rtReflFx || rtGiFx;
    m_frame.rayTracing = rtActive || pathTrace;

    // --- lights, shadows, environment --------------------------------------------------------
    const LightItem* sun = nullptr;
    for (auto& l : lights)
        if (l.kind == LightKind::Directional) {
            sun = &l;
            break;
        }
    ensureEnvironment(sun ? -sun->direction : Vec3{0.4f, 0.8f, 0.3f});

    const Mat4& P = view.projection;
    float m22 = P.at(2, 2), m23 = P.at(2, 3);
    float nearPlane = m23 / (m22 - 1.0f), farPlane = m23 / (m22 + 1.0f);
    float fovY = 2.0f * std::atan(1.0f / P.at(1, 1));
    float aspect = P.at(1, 1) / P.at(0, 0);

    const Mat4 fix = clipFix(d3d);
    FrameData fd{};
    fd.view = view.view;
    fd.proj = fix * jitter * view.projection;
    fd.viewProj = fd.proj * view.view;
    fd.invViewProj = fd.viewProj.inverse();
    fd.cameraPos = Vec4(view.position, 0);
    fd.viewProjNoJitter = fix * view.projection * view.view;
    fd.prevViewProj = hist.prevValid ? hist.prevViewProj : fd.viewProjNoJitter;
    fd.params2 = {jitterNdc.x, jitterNdc.y, static_cast<float>(hist.frame % 1024), 0.0f};

    const LightItem* dirShadow = nullptr;
    std::vector<Cascade> cascades;
    int shadowViews = 0, spotShadowCount = 0, pointShadowCount = 0;  // layers of the spot/point shadow array
    int count = 0;
    std::vector<LightItem> clustered = clusterLights(view, lights, 8);
    m_frame.clusteredLights = static_cast<int>(clustered.size());
    m_frame.giProbes = m_giExt.w >= 0.5f;
    std::vector<const LightItem*> ordered;  // directionals first
    for (auto& l : clustered)
        if (l.kind == LightKind::Directional) ordered.push_back(&l);
    for (auto& l : clustered)
        if (l.kind != LightKind::Directional) ordered.push_back(&l);
    for (const LightItem* l : ordered) {
        if (count == 8) break;
        GpuLight& g = fd.lights[count++];
        g.posType = Vec4(l->position, static_cast<float>(static_cast<int>(l->kind)));
        g.dirRange = Vec4(normalize(l->direction), std::max(l->range, 0.01f));
        g.colorIntensity = Vec4(l->color, l->intensity);
        g.cone = {std::cos(l->innerCone), std::cos(l->outerCone), -1.0f, l->shadowBias};
        if (l->kind == LightKind::Directional) g.posType = Vec4(l->normalBias, 0, 0, 0);  // position unused: carries the normal bias
        if (!rs.shadows || !l->castShadows || rs.debugView == DebugView::Wireframe) continue;
        if (l->kind == LightKind::Directional && !dirShadow) {
            dirShadow = l;
            g.cone.z = 0;
            if (!rtShadowFx || l != sun)  // the sun's shadow comes from rays when RT shadows are on
                cascades = computeCascades(view.view, fovY, aspect, nearPlane, std::min(rs.shadowDistance, farPlane), l->direction, rs.cascadeCount,
                                           rs.shadowResolution);
        } else if (l->kind == LightKind::Spot && spotShadowCount < kMaxSpotShadows && shadowViews + 1 <= kMaxShadowViews) {
            g.cone.z = static_cast<float>(shadowViews);
            Vec3 d = normalize(l->direction);
            Vec3 up = std::fabs(d.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
            fd.spotVP[shadowViews] = fix * Mat4::perspective(l->outerCone * 2.0f + radians(2), 1.0f, 0.05f, std::max(l->range, 0.1f)) *
                                     Mat4::lookAt(l->position, l->position + d, up);
            ++shadowViews;
            ++spotShadowCount;
        } else if (l->kind == LightKind::Point && rs.pointShadows && pointShadowCount < kMaxPointShadows && shadowViews + 6 <= kMaxShadowViews) {
            // 6 faces in consecutive layers; the shader picks the face from the direction (pointShadowFace())
            g.cone.z = static_cast<float>(shadowViews);
            for (int f = 0; f < 6; ++f) fd.spotVP[shadowViews + f] = fix * pointShadowViewProj(l->position, f, l->range);
            shadowViews += 6;
            ++pointShadowCount;
        }
    }
    m_frame.pointShadowLights = pointShadowCount;
    for (size_t i = 0; i < cascades.size(); ++i) {
        cascades[i].viewProj = fix * cascades[i].viewProj;
        fd.cascadeVP[i] = cascades[i].viewProj;
        fd.cascadeSplits[static_cast<int>(i)] = cascades[i].splitFar;
        fd.cascadeTexel[static_cast<int>(i)] = cascades[i].texelWorld;
    }
    m_frame.cascades = static_cast<int>(cascades.size());
    fd.params0 = {rs.environmentIntensity, static_cast<float>(static_cast<int>(rs.debugView)), static_cast<float>(count),
                  static_cast<float>(cascades.size())};
    fd.params1 = {dirShadow || shadowViews > 0 ? 1.0f : 0.0f, 1.0f / static_cast<float>(rs.shadowResolution),
                  static_cast<float>(kPrefilterMips - 1), 1.0f / static_cast<float>(kSpotShadowRes)};
    m_frame.fog = rs.fog && rs.fogDensity > 0 && !debug;
    if (m_frame.fog) {
        Vec3 toSun = sun ? normalize(-sun->direction) : Vec3{0, 1, 0};
        Vec3 sunLight = sun ? sun->color * sun->intensity : Vec3{0, 0, 0};
        fd.fog0 = {rs.fogDensity, std::max(rs.fogHeightFalloff, 0.0f), rs.fogHeight, std::clamp(rs.fogMaxOpacity, 0.0f, 1.0f)};
        fd.fog1 = Vec4(rs.fogColor, std::max(rs.fogStart, 0.0f));
        fd.fog2 = Vec4(toSun, 1.0f);
        fd.fog3 = Vec4(sunLight * (rs.fogSunScatter * 0.1f), 0.0f);  // same scale as fogInscatter()
    }
    fd.wet = {rs.wetness, rs.puddles, 0.0f, 0.0f};
    m_frame.wetness = rs.wetness;
    m_device.updateBuffer(m_frameUbo, &fd, sizeof(fd));
    // 1.3.2 viewmodel pass: the same Frame block (lights, shadows, fog, camera position and view: lighting stays in world
    // space) with the viewmodel projection squeezed into the front depth slice, in its own uniform buffer
    bool vmPass = view.viewmodelPass && m_vmFrameUbo && !pathTrace;
    if (vmPass) {
        vmPass = false;
        for (const auto& it : items) vmPass = vmPass || (it.viewmodel && !it.shadowOnly && !it.offCamera);
    }
    FrameData vfd = fd;
    if (vmPass) {
        const Mat4 R = viewmodelDepthRemap();
        vfd.proj = fix * jitter * R * view.viewmodelProjection;
        vfd.viewProj = vfd.proj * view.view;
        vfd.invViewProj = vfd.viewProj.inverse();
        vfd.viewProjNoJitter = fix * R * view.viewmodelProjection * view.view;
        vfd.prevViewProj = hist.prevVmValid ? hist.prevVmViewProj : vfd.viewProjNoJitter;
        m_device.updateBuffer(m_vmFrameUbo, &vfd, sizeof(vfd));
    }
    m_device.bindUniformBuffer(0, m_frameUbo);

    // per-object previous model matrices (motion vectors)
    hist.models.clear();
    auto prevModel = [&](const DrawItem& it) -> const Mat4& {
        if (it.objectId == 0) return it.model;
        hist.models[it.objectId] = it.model;
        auto p = hist.prevModels.find(it.objectId);
        return p != hist.prevModels.end() ? p->second : it.model;
    };

    // --- render graph --------------------------------------------------------------------------
    RenderGraph g;
    RGResource output = g.importFramebuffer("output", out, width, height);
    g.markOutput(output);
    int samples = std::min(rs.msaa, caps.maxSamples);
    if (pathTrace || vendor) samples = 1;  // vendor upscalers take single-sampled color + depth

    // Shadow maps (always produced so the samplers are valid; empty when unused).
    int cascadeLayers = std::max<int>(1, static_cast<int>(cascades.size()));
    if (m_shadowCacheRes != rs.shadowResolution || m_shadowCacheLayers != cascadeLayers) {
        m_shadowCacheRes = rs.shadowResolution;
        m_shadowCacheLayers = cascadeLayers;
        for (bool& v : m_cascadeCacheValid) v = false;
    }
    bool reuseCascade[4] = {};
    for (int c = 0; c < cascadeLayers && c < 4; ++c) {
        if (static_cast<size_t>(c) >= cascades.size()) break;
        const Mat4& vp = cascades[static_cast<size_t>(c)].viewProj;
        uint64_t key = 0;
        bool moving = shadowCascadeDynamic(items, vp, key);
        reuseCascade[c] = !moving && m_cascadeCacheValid[c] && m_cascadeCacheKey[c] == key && sameMatrix(m_cascadeCacheVP[c], vp);
        if (reuseCascade[c]) ++m_frame.shadowCacheHits;
        else {
            m_cascadeCacheValid[c] = !moving;
            m_cascadeCacheVP[c] = vp;
            m_cascadeCacheKey[c] = key;
        }
    }
    RGResource cascadeMap = kRGInvalid, spotMap = kRGInvalid;
    for (int c = 0; c < cascadeLayers; ++c) {
        g.addPass(
            std::format("shadow.cascade{}", c),
            [&](RGBuilder& b) {
                if (c == 0) cascadeMap = b.create("shadow.cascades", depthArrayDesc(cascades.empty() ? 1 : rs.shadowResolution, cascadeLayers));
                b.writeDepth(cascadeMap, 0, c);
                rhi::ClearDesc cd;
                cd.color = false;
                cd.depth = !(c < 4 && reuseCascade[c]);
                b.clear(cd);
            },
            [&, c](RGContext&) {
                if (c < 4 && reuseCascade[c]) return;
                if (static_cast<size_t>(c) < cascades.size()) drawShadowCasters(items, cascades[static_cast<size_t>(c)].viewProj);
            });
    }
    int spotLayers = std::max<int>(1, shadowViews);
    for (int s = 0; s < spotLayers; ++s) {
        g.addPass(
            std::format("shadow.spot{}", s),
            [&](RGBuilder& b) {
                if (s == 0) spotMap = b.create("shadow.spots", depthArrayDesc(shadowViews == 0 ? 1 : kSpotShadowRes, spotLayers));
                b.writeDepth(spotMap, 0, s);
                rhi::ClearDesc cd;
                cd.color = false;
                b.clear(cd);
            },
            [&, s](RGContext&) {
                if (s < shadowViews) drawShadowCasters(items, fd.spotVP[s]);
            });
    }

    // --- ray tracing: scene, G-buffer prepass, trace, denoise ----------------------------------
    std::vector<rhi::RtInstance> rtInstances;
    std::vector<rhi::RtMaterial> rtMaterials;
    std::vector<rhi::RtLight> rtLights;
    if (rtActive || pathTrace) {
        for (const auto& it : items) {
            if (it.alphaMode == 2 || !it.mesh || it.shadowOnly || it.viewmodel || it.decal) continue;  // shadow-only: raster maps only; offCamera still hits rays
            rhi::RtMaterial m;
            m.baseColor = Vec4(it.color, it.opacity);
            m.emissiveRoughness = Vec4(it.emissive, it.roughness);
            m.params = {it.metallic, 0, 0, 0};
            rtInstances.push_back({it.mesh, it.model, static_cast<uint32_t>(rtMaterials.size())});
            rtMaterials.push_back(m);
        }
        for (int i = 0; i < count; ++i)
            rtLights.push_back({fd.lights[i].posType, fd.lights[i].dirRange, fd.lights[i].colorIntensity, fd.lights[i].cone});
    }
    rhi::RtTraceDesc trace;
    trace.width = rw;
    trace.height = rh;
    trace.environment = m_envCube;
    trace.invViewProj = fd.invViewProj;
    trace.cameraPos = view.position;
    if (sun) {
        trace.sunDirection = -normalize(sun->direction);
        trace.sunColor = sun->color * sun->intensity;
    } else {
        trace.sunColor = {0, 0, 0};
    }
    trace.environmentIntensity = rs.environmentIntensity;
    trace.raysPerPixel = rtRays(gfx.rt.quality);
    trace.quality = static_cast<int>(gfx.rt.quality);
    trace.aoRadius = gfx.rt.quality == RtQuality::Low ? 0.75f : gfx.rt.quality == RtQuality::Medium ? 1.0f : 1.5f;
    trace.frameIndex = static_cast<uint32_t>(m_frameCounter);
    trace.lights = rtLights;

    RGResource rtNormals = kRGInvalid, rtDepth = kRGInvalid;
    RGResource rtRaw[4] = {kRGInvalid, kRGInvalid, kRGInvalid, kRGInvalid}, rtOut[4] = {kRGInvalid, kRGInvalid, kRGInvalid, kRGInvalid};
    RGResource rtPrev[4] = {kRGInvalid, kRGInvalid, kRGInvalid, kRGInvalid};
    const bool rtFx[4] = {rtShadowFx, rtAoFx, rtReflFx, rtGiFx};
    static const char* kFxName[4] = {"shadow", "ao", "reflection", "gi"};
    if (rtActive && m_rtGbuffer) {
        g.addPass(
            "rt.gbuffer",
            [&](RGBuilder& b) {
                rtNormals = b.create("rt.normals", colorDesc(rhi::TextureFormat::RGBA16F, rw, rh));
                rtDepth = b.create("rt.depth", colorDesc(rhi::TextureFormat::Depth32F, rw, rh));
                b.write(rtNormals);
                b.writeDepth(rtDepth);
                rhi::ClearDesc cd;
                cd.colorValue = {0, 1, 0, 1};
                b.clear(cd);
            },
            [&](RGContext&) {
                m_device.bindUniformBuffer(0, m_frameUbo);
                m_device.setShader(m_rtGbuffer);
                for (const auto& it : items) {
                    if (it.alphaMode == 2 || it.shadowOnly || it.offCamera || it.viewmodel || it.decal) continue;
                    m_device.setUniform("uModel", it.model);
                    m_device.setUniform("uBaseColor", Vec4(it.color, it.opacity));
                    m_device.setUniform("uNormalScale", it.normalScale);
                    m_device.setUniform("uRoughness", it.roughness);
                    m_device.setUniform("uAlphaCutoff", it.alphaCutoff);
                    m_device.setUniform("uAlphaMode", it.alphaMode);
                    m_device.setCullMode(it.twoSided ? rhi::CullMode::None : rhi::CullMode::Back);
                    m_device.bindTexture(0, it.texture ? it.texture : m_white);
                    m_device.bindTexture(1, it.normalTexture ? it.normalTexture : m_flatNormal);
                    m_device.bindTexture(2, it.metallicRoughnessTexture ? it.metallicRoughnessTexture : m_white);
                    m_device.draw(it.mesh, rhi::Topology::Triangles);
                }
                m_device.setCullMode(rhi::CullMode::Back);
            });
        g.addPass(
            "rt.trace",
            [&](RGBuilder& b) {
                b.read(rtNormals);
                b.read(rtDepth);
                for (int e = 0; e < 4; ++e) {
                    if (!rtFx[e]) continue;
                    rhi::TextureDesc d = colorDesc(e < 2 ? rhi::TextureFormat::R16F : rhi::TextureFormat::RGBA16F, rw, rh);
                    d.storage = true;
                    rtRaw[e] = b.create(std::string("rt.raw.") + kFxName[e], d);
                    b.write(rtRaw[e]);
                }
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&](RGContext& ctx) {
                trace.depth = ctx.texture(rtDepth);
                trace.normals = ctx.texture(rtNormals);
                trace.shadow = rtShadowFx ? ctx.texture(rtRaw[0]) : rhi::TextureHandle{};
                trace.ao = rtAoFx ? ctx.texture(rtRaw[1]) : rhi::TextureHandle{};
                trace.reflection = rtReflFx ? ctx.texture(rtRaw[2]) : rhi::TextureHandle{};
                trace.gi = rtGiFx ? ctx.texture(rtRaw[3]) : rhi::TextureHandle{};
                if (!rtDev->setScene(rtInstances, rtMaterials) || !rtDev->trace(trace)) m_frame.rayTracingNote = "DXR dispatch failed";
            });
        for (int e = 0; e < 4; ++e) {
            if (!rtFx[e]) continue;
            rhi::TextureDesc hd = colorDesc(e < 2 ? rhi::TextureFormat::R16F : rhi::TextureFormat::RGBA16F, rw, rh);
            int cur = hist.rtIndex, nxt = 1 - hist.rtIndex;
            rtPrev[e] = g.importTexture(std::string("rt.history.") + kFxName[e], historyTexture(hist.rt[e][cur], hd), hd);
            rtOut[e] = g.importTexture(std::string("rt.denoised.") + kFxName[e], historyTexture(hist.rt[e][nxt], hd), hd);
            bool denoise = gfx.rt.denoise && m_rtDenoise;
            g.addPass(
                std::string("rt.denoise.") + kFxName[e],
                [&, e](RGBuilder& b) {
                    b.read(rtRaw[e]);
                    b.read(rtPrev[e]);
                    b.read(rtNormals);
                    b.read(rtDepth);
                    b.write(rtOut[e]);
                    rhi::ClearDesc cd;
                    cd.color = cd.depth = false;
                    b.clear(cd);
                },
                [&, e, denoise](RGContext& ctx) {
                    m_device.setDepthTest(false);
                    m_device.setCullMode(rhi::CullMode::None);
                    m_device.bindUniformBuffer(0, m_frameUbo);
                    m_device.setShader(m_rtDenoise);
                    m_device.bindTexture(0, ctx.texture(rtRaw[e]));
                    m_device.bindTexture(1, ctx.texture(rtPrev[e]));
                    m_device.bindTexture(2, ctx.texture(rtDepth));
                    m_device.bindTexture(3, ctx.texture(rtNormals));
                    m_device.setUniform("uSize", Vec4(static_cast<float>(rw), static_cast<float>(rh), 1.0f / static_cast<float>(rw),
                                                      1.0f / static_cast<float>(rh)));
                    m_device.setUniform("uAlpha", denoise ? (e >= 2 ? 0.12f : 0.2f) : 1.0f);
                    m_device.setUniform("uHistoryValid", denoise && hist.rtValid[e] ? 1 : 0);
                    m_device.setUniform("uRadius", denoise ? (gfx.rt.quality >= RtQuality::High ? 1.0f : 2.0f) : 0.0f);
                    m_device.drawFullscreenTriangle();
                    m_device.setDepthTest(true);
                });
        }
    } else if (gfx.rt.enabled && rtDev && !m_rtGbuffer) {
        m_frame.rayTracingNote = "ray-tracing helper shaders failed to compile";
    }

    // --- 1.2.0 screen-space effects: depth prepass -> SSAO (+ blur) -> SSR, all before the main pass ----------------------
    const bool fxOk = !debug && !pathTrace;
    const bool ssaoOn = fxOk && rs.ssao && !rtAoFx && m_ssao && m_ssaoBlur;
    const bool ssrOn = fxOk && rs.ssr && !rtReflFx && m_ssr && m_blit;
    const bool volOn = fxOk && rs.volumetric && m_volumetric && m_apply && sun && rs.volumetricDensity > 0;
    const bool aeOn = fxOk && rs.autoExposure && m_lumaDown && m_lumaReduce && m_lumaAdapt;
    const bool dofOn = fxOk && rs.dof && m_dof && rs.dofMaxRadius > 0.25f;
    const bool mbOn = fxOk && rs.motionBlur && m_motionBlur && rs.motionBlurStrength > 0;
    // SSAO / SSR need depth before the main pass; volumetric light and DoF read the main depth unless it is multisampled
    const bool prepass = ssaoOn || ssrOn || ((volOn || dofOn) && samples > 1);
    m_frame.ssao = ssaoOn;
    m_frame.ssr = ssrOn;
    m_frame.volumetric = volOn;
    m_frame.autoExposure = aeOn;
    m_frame.dof = dofOn;
    m_frame.motionBlur = mbOn;
    m_frame.depthPrepass = prepass;
    const int hw = std::max(1, (rw + 1) / 2), hh = std::max(1, (rh + 1) / 2);  // half resolution of SSAO / SSR / volumetric
    const Vec4 depthSize{static_cast<float>(rw), static_cast<float>(rh), 1.0f / static_cast<float>(rw), 1.0f / static_cast<float>(rh)};
    const float frameNoise = static_cast<float>(hist.frame % 64);
    auto fsState = [&] {
        m_device.setDepthTest(false);
        m_device.setCullMode(rhi::CullMode::None);
        m_device.bindUniformBuffer(0, m_frameUbo);
    };
    RGResource preDepth = kRGInvalid, aoRaw = kRGInvalid, aoOut = kRGInvalid, ssrOut = kRGInvalid, ssrPrev = kRGInvalid, ssrNext = kRGInvalid;
    if (prepass) {
        g.addPass(
            "depth.prepass",
            [&](RGBuilder& b) {
                preDepth = b.create("depth.prepass", colorDesc(rhi::TextureFormat::Depth32F, rw, rh));
                b.writeDepth(preDepth);
                rhi::ClearDesc cd;
                cd.color = false;
                b.clear(cd);
            },
            [&](RGContext&) {
                m_device.bindUniformBuffer(0, m_frameUbo);
                drawShadowCasters(items, fd.viewProj, true);  // camera view-projection (with the TAA jitter of the main pass)
                m_device.setCullMode(rhi::CullMode::Back);
            });
    }
    if (ssaoOn) {
        g.addPass(
            "ssao",
            [&](RGBuilder& b) {
                aoRaw = b.create("ssao.raw", colorDesc(rhi::TextureFormat::R16F, hw, hh));
                b.read(preDepth);
                b.write(aoRaw);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&](RGContext& ctx) {
                fsState();
                m_device.setShader(m_ssao);
                m_device.bindTexture(0, ctx.texture(preDepth));
                m_device.setUniform("uDepthSize", depthSize);
                m_device.setUniform("uAo", Vec4(rs.ssaoRadius, rs.ssaoIntensity, static_cast<float>(rs.ssaoSamples), frameNoise));
                m_device.drawFullscreenTriangle();
                m_device.setDepthTest(true);
            });
        g.addPass(
            "ssao.blur",
            [&](RGBuilder& b) {
                aoOut = b.create("ssao", colorDesc(rhi::TextureFormat::R16F, hw, hh));
                b.read(aoRaw);
                b.read(preDepth);
                b.write(aoOut);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&](RGContext& ctx) {
                fsState();
                m_device.setShader(m_ssaoBlur);
                m_device.bindTexture(0, ctx.texture(aoRaw));
                m_device.bindTexture(1, ctx.texture(preDepth));
                m_device.setUniform("uDepthSize", depthSize);
                m_device.setUniform("uAoSize", Vec4(static_cast<float>(hw), static_cast<float>(hh), 1.0f / static_cast<float>(hw), 1.0f / static_cast<float>(hh)));
                m_device.drawFullscreenTriangle();
                m_device.setDepthTest(true);
            });
    }
    if (ssrOn) {
        rhi::TextureDesc sd = colorDesc(rhi::TextureFormat::RGBA16F, rw, rh);
        ssrPrev = g.importTexture("ssr.prev", historyTexture(hist.ssrColor[hist.ssrIndex], sd), sd);
        ssrNext = g.importTexture("ssr.next", historyTexture(hist.ssrColor[1 - hist.ssrIndex], sd), sd);
        g.markOutput(ssrNext);  // read by the next frame only
        g.addPass(
            "ssr",
            [&](RGBuilder& b) {
                ssrOut = b.create("ssr", colorDesc(rhi::TextureFormat::RGBA16F, hw, hh));
                b.read(preDepth);
                b.read(ssrPrev);
                b.write(ssrOut);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&](RGContext& ctx) {
                fsState();
                m_device.setShader(m_ssr);
                m_device.bindTexture(0, ctx.texture(preDepth));
                m_device.bindTexture(1, ctx.texture(ssrPrev));
                m_device.setUniform("uDepthSize", depthSize);
                m_device.setUniform("uSsr", Vec4(rs.ssrMaxDistance, static_cast<float>(rs.ssrSteps), rs.ssrThickness, rs.ssrIntensity));
                m_device.setUniform("uSsr2", Vec4(hist.ssrValid && hist.prevValid ? 1.0f : 0.0f, frameNoise, 0, 0));
                m_device.drawFullscreenTriangle();
                m_device.setDepthTest(true);
            });
    }

    // Main HDR pass (or the path-traced reference)
    RGResource hdr = kRGInvalid, depth = kRGInvalid, velocity = kRGInvalid;
    if (pathTrace) {
        hist.taaValid = false;
        // restart accumulation whenever the camera or the drawn set changes
        // FNV-1a over the camera, every transform and material factor: any change restarts accumulation
        uint64_t key = 1469598103934665603ull;
        auto mix = [&](const void* p, size_t n) {
            for (size_t b = 0; b < n; ++b) key = (key ^ static_cast<const uint8_t*>(p)[b]) * 1099511628211ull;
        };
        mix(&fd.viewProjNoJitter, sizeof(Mat4));
        for (const auto& it : items) {
            mix(&it.model, sizeof(Mat4));
            mix(&it.color, sizeof(Vec3));
            mix(&it.emissive, sizeof(Vec3));
            mix(&it.roughness, sizeof(float));
            mix(&it.metallic, sizeof(float));
        }
        mix(&rs.environmentIntensity, sizeof(float));
        if (hist.ptFrames == 0 || hist.ptKey != key) hist.ptFrames = 0;
        hist.ptKey = key;
        rhi::TextureDesc pd = colorDesc(rhi::TextureFormat::RGBA16F, rw, rh);
        pd.storage = true;
        hdr = g.importTexture("pt.accum", historyTexture(hist.pathTrace, pd), pd);
        g.addPass(
            "pt.trace",
            [&](RGBuilder& b) {
                b.write(hdr);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&](RGContext& ctx) {
                trace.pathTrace = ctx.texture(hdr);
                trace.accumulatedFrames = hist.ptFrames;
                trace.maxBounces = 6;
                trace.invViewProj = fd.viewProjNoJitter.inverse();
                if (rtDev->setScene(rtInstances, rtMaterials) && rtDev->trace(trace)) ++hist.ptFrames;
            });
    } else {
        g.addPass(
            "main",
            [&](RGBuilder& b) {
                hdr = b.create("hdr", colorDesc(rhi::TextureFormat::RGBA16F, rw, rh, samples));
                velocity = b.create("velocity", colorDesc(rhi::TextureFormat::RG16F, rw, rh, samples));
                rhi::TextureDesc dd = colorDesc(rhi::TextureFormat::Depth32F, rw, rh, samples);
                depth = b.create("depth", dd);
                b.read(cascadeMap);
                b.read(spotMap);
                for (int e = 0; e < 4; ++e)
                    if (rtOut[e] != kRGInvalid) b.read(rtOut[e]);
                if (aoOut != kRGInvalid) b.read(aoOut);
                if (ssrOut != kRGInvalid) b.read(ssrOut);
                b.write(hdr);
                b.write(velocity);
                b.writeDepth(depth);
                rhi::ClearDesc cd;
                cd.colorValue = options.clearColor;
                b.clear(cd);
            },
            [&](RGContext& ctx) {
                m_device.bindUniformBuffer(0, m_frameUbo);
                m_device.setShader(m_pbr);
                m_device.bindTexture(5, m_irradianceCube);
                m_device.bindTexture(6, m_prefilteredCube);
                m_device.bindTexture(7, m_brdfLut);
                m_device.bindTexture(8, ctx.texture(cascadeMap));
                m_device.bindTexture(9, ctx.texture(spotMap));
                int rtFlags = 0;
                for (int e = 0; e < 4; ++e) {
                    m_device.bindTexture(10 + e, rtOut[e] != kRGInvalid ? ctx.texture(rtOut[e]) : m_white);
                    if (rtOut[e] != kRGInvalid) rtFlags |= 1 << e;
                }
                if (aoOut != kRGInvalid) {  // SSAO takes the AO slot (RT AO wins when both are on: ssaoOn is false then)
                    m_device.bindTexture(11, ctx.texture(aoOut));
                    rtFlags |= 2;
                }
                if (ssrOut != kRGInvalid) {
                    m_device.bindTexture(12, ctx.texture(ssrOut));
                    rtFlags |= 16;
                }
                const Vec4 screen{1.0f / static_cast<float>(rw), 1.0f / static_cast<float>(rh), 0, 0};
                bool wire = rs.debugView == DebugView::Wireframe;
                m_device.setWireframe(wire);
                std::vector<const DrawItem*> blended, opaque, vmBlended, vmOpaque, decals;
                for (const auto& item : items)
                    if (item.decal) {
                        if (m_pbrDecal && !debug) decals.push_back(&item);
                    } else if (!item.shadowOnly && !item.offCamera) {
                        const bool vm = vmPass && item.viewmodel;
                        (item.alphaMode == 2 ? (vm ? vmBlended : blended) : (vm ? vmOpaque : opaque)).push_back(&item);
                    }
                drawOpaque(opaque, prevModel, rtFlags, screen);
                if (!decals.empty()) {
                    // 1.3.2 projected decals (Fledge Advanced): on the opaque world, before the sky and transparent geometry;
                    // lit like the surface (its normal), SSAO / SSR apply. Order: decalOrder (sort order, then age).
                    std::stable_sort(decals.begin(), decals.end(), [](const DrawItem* a, const DrawItem* b) { return a->decalOrder < b->decalOrder; });
                    m_device.setShader(m_pbrDecal);
                    m_device.setBlendMode(rhi::BlendMode::Alpha);
                    m_device.setDepthWrite(false);
                    m_device.setDepthFunc(rhi::DepthFunc::LessEqual);
                    for (const DrawItem* d : decals) {
                        bindMaterial(*d);
                        m_device.setCullMode(rhi::CullMode::None);
                        m_device.setUniform("uPrevModel", prevModel(*d));
                        m_device.setUniform("uRtFlags", rtFlags);
                        m_device.setUniform("uScreen", screen);
                        m_device.draw(d->mesh, rhi::Topology::Triangles);
                    }
                    m_device.setDepthWrite(true);
                    m_device.setBlendMode(rhi::BlendMode::None);
                    m_device.setCullMode(rhi::CullMode::Back);
                    m_frame.decals = static_cast<int>(decals.size());
                }
                m_device.setWireframe(false);
                m_device.setCullMode(rhi::CullMode::Back);
                if (rs.showSky && !debug) {
                    m_device.setShader(m_sky);
                    m_device.bindTexture(0, m_envCube);
                    m_device.setDepthWrite(false);
                    m_device.setDepthFunc(rhi::DepthFunc::LessEqual);
                    m_device.drawFullscreenTriangle();
                    m_device.setDepthWrite(true);
                }
                m_device.setShader(m_lines);
                if (options.drawGrid) m_device.draw(m_gridMesh, rhi::Topology::Lines);
                if (!m_debugLines.empty()) {
                    m_device.updateLineMesh(m_debugMesh, m_debugLines);
                    m_device.draw(m_debugMesh, rhi::Topology::Lines);
                }
                if (!blended.empty()) {  // back-to-front, no depth writes
                    auto dist = [&](const DrawItem* d) { return length(d->model.c[3].xyz() - view.position); };
                    std::sort(blended.begin(), blended.end(), [&](auto* a, auto* b) { return dist(a) > dist(b); });
                    m_device.setShader(m_pbr);
                    m_device.setBlendMode(rhi::BlendMode::Alpha);
                    m_device.setDepthWrite(false);
                    m_device.setWireframe(wire);
                    for (auto* item : blended) {
                        bindMaterial(*item);
                        m_device.setUniform("uPrevModel", prevModel(*item));
                        m_device.setUniform("uRtFlags", 0);
                        m_device.setUniform("uScreen", screen);
                        m_device.draw(item->mesh, rhi::Topology::Triangles);
                    }
                    m_device.setWireframe(false);
                    m_device.setDepthWrite(true);
                    m_device.setBlendMode(rhi::BlendMode::None);
                }
                if (!debug) m_frame.particles = drawParticles(view);
                if (vmPass && (!vmOpaque.empty() || !vmBlended.empty())) {
                    // 1.3.2 viewmodel pass, after everything of the world: own projection, front depth slice (always in front of
                    // the world, sorted among itself). No SSAO / SSR / RT lookups: those buffers hold the world behind it.
                    m_device.bindUniformBuffer(0, m_vmFrameUbo);
                    m_device.setWireframe(wire);
                    drawOpaque(vmOpaque, prevModel, 0, screen);
                    if (!vmBlended.empty()) {
                        auto dist = [&](const DrawItem* d) { return length(d->model.c[3].xyz() - view.position); };
                        std::sort(vmBlended.begin(), vmBlended.end(), [&](auto* a, auto* b) { return dist(a) > dist(b); });
                        m_device.setShader(m_pbr);
                        m_device.setBlendMode(rhi::BlendMode::Alpha);
                        m_device.setDepthWrite(false);
                        for (auto* item : vmBlended) {
                            bindMaterial(*item);
                            m_device.setUniform("uPrevModel", prevModel(*item));
                            m_device.setUniform("uRtFlags", 0);
                            m_device.setUniform("uScreen", screen);
                            m_device.draw(item->mesh, rhi::Topology::Triangles);
                        }
                        m_device.setDepthWrite(true);
                        m_device.setBlendMode(rhi::BlendMode::None);
                    }
                    m_device.setWireframe(false);
                    m_device.bindUniformBuffer(0, m_frameUbo);
                    m_frame.viewmodelItems = static_cast<int>(vmOpaque.size() + vmBlended.size());
                }
                m_device.setCullMode(rhi::CullMode::Back);
            });
    }

    // NOTE: everything the pass lambdas capture by reference must outlive g.execute() below.
    RGResource scene = hdr;
    RGResource sceneVelocity = velocity;
    RGResource resolved = kRGInvalid, resolvedVelocity = kRGInvalid;
    std::vector<RGResource> chain;
    RGResource nfMask = kRGInvalid;
    if (samples > 1) {
        g.addPass(
            "resolve",
            [&](RGBuilder& b) {
                resolved = b.create("hdr.resolved", colorDesc(rhi::TextureFormat::RGBA16F, rw, rh));
                b.read(hdr);
                b.write(resolved);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&](RGContext& ctx) {
                rhi::FramebufferDesc src;
                src.colorCount = 1;
                src.colors[0] = {ctx.texture(hdr), 0, 0};
                m_device.resolve(m_pool->framebuffer(src), ctx.framebuffer, rw, rh);
            });
        scene = resolved;
        if (temporal || mbOn) {  // motion blur samples the resolved velocity too
            g.addPass(
                "resolve.velocity",
                [&](RGBuilder& b) {
                    resolvedVelocity = b.create("velocity.resolved", colorDesc(rhi::TextureFormat::RG16F, rw, rh));
                    b.read(velocity);
                    b.write(resolvedVelocity);
                    rhi::ClearDesc cd;
                    cd.color = cd.depth = false;
                    b.clear(cd);
                },
                [&](RGContext& ctx) {
                    rhi::FramebufferDesc src;
                    src.colorCount = 1;
                    src.colors[0] = {ctx.texture(velocity), 0, 0};
                    m_device.resolve(m_pool->framebuffer(src), ctx.framebuffer, rw, rh);
                });
            sceneVelocity = resolvedVelocity;
        }
    }

    const RGResource rawScene = scene;  // 1.3.3 debug counter: the HDR colour as the scene passes left it
    if (m_injectNonFinite && m_nfInject && !pathTrace) {  // 1.3.3 tests only: NaN / Inf pixels for the post chain
        g.addPass(
            "debug.inject",
            [&](RGBuilder& b) {
                b.write(scene);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&](RGContext&) {
                m_device.setDepthTest(false);
                m_device.setCullMode(rhi::CullMode::None);
                m_device.setBlendMode(rhi::BlendMode::None);
                m_device.setShader(m_nfInject);
                m_device.setUniform("uSize", Vec4(static_cast<float>(rw), static_cast<float>(rh), 0, 0));
                m_device.setUniform("uNanBits", 0x7fc00000);
                m_device.setUniform("uInfBits", 0x7f800000);
                m_device.drawFullscreenTriangle();
                m_device.setDepthTest(true);
            });
    }
    // Volumetric sun light (half resolution, added onto the HDR image before TAA) and the SSR colour history
    const RGResource effDepth = prepass ? preDepth : depth;  // single-sampled scene depth
    // (the pass lambdas run in g.execute(): resources they capture by reference live at function scope)
    RGResource volRaw = kRGInvalid, l64 = kRGInvalid, l8 = kRGInvalid, l1 = kRGInvalid, dofOut = kRGInvalid, mbOut = kRGInvalid;
    if (volOn) {
        const RGResource volTarget = scene;
        g.addPass(
            "volumetric",
            [&](RGBuilder& b) {
                volRaw = b.create("volumetric", colorDesc(rhi::TextureFormat::RGBA16F, hw, hh));
                b.read(effDepth);
                b.read(cascadeMap);
                b.write(volRaw);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&](RGContext& ctx) {
                fsState();
                m_device.setShader(m_volumetric);
                m_device.bindTexture(0, ctx.texture(effDepth));
                m_device.bindTexture(1, ctx.texture(cascadeMap));
                m_device.setUniform("uDepthSize", depthSize);
                m_device.setUniform("uVol", Vec4(rs.volumetricDensity, rs.volumetricIntensity, rs.volumetricAnisotropy, static_cast<float>(rs.volumetricSteps)));
                m_device.setUniform("uVol2", Vec4(rs.volumetricDistance, frameNoise, cascades.empty() ? 0.0f : 1.0f, rs.fog ? rs.fogHeightFalloff : 0.0f));
                m_device.setUniform("uVol3", Vec4(rs.fog ? rs.fogHeight : -1e6f, 0, 0, 0));
                m_device.drawFullscreenTriangle();
                m_device.setDepthTest(true);
            });
        g.addPass(
            "volumetric.apply",
            [&, volTarget](RGBuilder& b) {
                b.read(volRaw);
                b.write(volTarget);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&](RGContext& ctx) {
                fsState();
                m_device.setBlendMode(rhi::BlendMode::Alpha);  // scene * transmittance + in-scattered light
                m_device.setShader(m_apply);
                m_device.bindTexture(0, ctx.texture(volRaw));
                m_device.drawFullscreenTriangle();
                m_device.setBlendMode(rhi::BlendMode::None);
                m_device.setDepthTest(true);
            });
    }
    if (ssrOn) {
        const RGResource ssrSrc = scene;
        g.addPass(
            "ssr.history",
            [&, ssrSrc](RGBuilder& b) {
                b.read(ssrSrc);
                b.write(ssrNext);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&, ssrSrc](RGContext& ctx) {
                fsState();
                m_device.setShader(m_blit);
                m_device.bindTexture(0, ctx.texture(ssrSrc));
                m_device.setUniform("uSrcSize", depthSize);
                m_device.setUniform("uSharpness", 0.0f);
                m_device.drawFullscreenTriangle();
                m_device.setDepthTest(true);
            });
    }

    // Temporal AA / temporal upscale (TAA: to the output size; FSR: anti-aliasing at render size)
    int sceneW = rw, sceneH = rh;
    if (vendor && !pathTrace) {
        // Vendor temporal upscaler (XeSS): HDR + depth + motion vectors at render size -> output size
        RGResource upOut = kRGInvalid;
        const RGResource vendorInput = scene;
        g.addPass(
            "upscale.vendor",
            [&, vendorInput](RGBuilder& b) {
                rhi::TextureDesc od = colorDesc(rhi::TextureFormat::RGBA16F, width, height);
                od.storage = true;
                upOut = b.create("vendor.output", od);
                b.read(vendorInput);
                b.read(sceneVelocity);
                b.read(depth);
                b.write(upOut);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&, vendorInput](RGContext& ctx) {
                rhi::VendorUpscalerConfig cfg{rw, rh, width, height, static_cast<int>(gfx.upscalerQuality)};
                rhi::VendorUpscalerInputs in;
                in.color = ctx.texture(vendorInput);
                in.depth = ctx.texture(depth);
                in.velocity = ctx.texture(sceneVelocity);
                in.output = ctx.texture(upOut);
                in.jitterX = jitterNdc.x * static_cast<float>(rw) * 0.5f;
                in.jitterY = -jitterNdc.y * static_cast<float>(rh) * 0.5f;
                in.reset = !hist.taaValid;
                if (!vendor->configure(cfg) || !vendor->evaluate(in)) {
                    m_vendorFailed.insert(m_vendorLibrary);  // fall back from the next frame on
                    m_upscalerCheckFrame = 0;
                }
            });
        scene = upOut;
        sceneW = width;
        sceneH = height;
    } else if (temporal && !pathTrace) {
        int tw = upscaler == UpscalerKind::Taa ? width : rw, th = upscaler == UpscalerKind::Taa ? height : rh;
        rhi::TextureDesc td = colorDesc(rhi::TextureFormat::RGBA16F, tw, th);
        RGResource prevHist = g.importTexture("taa.history", historyTexture(hist.taa[hist.taaIndex], td), td);
        RGResource nextHist = g.importTexture("taa.output", historyTexture(hist.taa[1 - hist.taaIndex], td), td);
        bool useDepth = samples == 1;
        const RGResource taaInput = scene;  // `scene` is reassigned below: capture the input by value
        g.addPass(
            "taa",
            [&, taaInput](RGBuilder& b) {
                b.read(taaInput);
                b.read(sceneVelocity);
                b.read(prevHist);
                if (useDepth) b.read(depth);
                b.write(nextHist);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&, tw, th, useDepth, taaInput](RGContext& ctx) {
                m_device.setDepthTest(false);
                m_device.setCullMode(rhi::CullMode::None);
                m_device.setShader(m_taa);
                m_device.bindTexture(0, ctx.texture(taaInput));
                m_device.bindTexture(1, ctx.texture(prevHist));
                m_device.bindTexture(2, ctx.texture(sceneVelocity));
                m_device.bindTexture(3, useDepth ? ctx.texture(depth) : m_white);
                m_device.setUniform("uRenderSize", Vec4(static_cast<float>(rw), static_cast<float>(rh), 1.0f / static_cast<float>(rw),
                                                        1.0f / static_cast<float>(rh)));
                m_device.setUniform("uOutSize", Vec4(static_cast<float>(tw), static_cast<float>(th), 1.0f / static_cast<float>(tw),
                                                     1.0f / static_cast<float>(th)));
                m_device.setUniform("uJitterUv", Vec4(jitterUv.x, jitterUv.y, 0, 0));
                m_device.setUniform("uAlpha", 0.1f);
                m_device.setUniform("uHistoryValid", hist.taaValid ? 1 : 0);
                m_device.setUniform("uUseDepth", useDepth ? 1 : 0);
                m_device.drawFullscreenTriangle();
                m_device.setDepthTest(true);
            });
        scene = nextHist;
        sceneW = tw;
        sceneH = th;
    }

    // Auto-exposure: log2 luminance 64x64 -> 8x8 -> 1x1 -> adaptation (1x1 history), read by the tonemapper
    RGResource exposureTex = kRGInvalid;
    if (aeOn) {
        rhi::TextureDesc ed = colorDesc(rhi::TextureFormat::R16F, 1, 1);
        ed.sampler.filter = rhi::Filter::Nearest;
        RGResource expPrev = g.importTexture("exposure.prev", historyTexture(hist.exposure[hist.exposureIndex], ed), ed);
        exposureTex = g.importTexture("exposure.next", historyTexture(hist.exposure[1 - hist.exposureIndex], ed), ed);
        double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        float dt = hist.lastTime > 0 ? static_cast<float>(std::clamp(now - hist.lastTime, 0.0, 0.25)) : 0.0f;
        hist.lastTime = now;
        const RGResource lumSrc = scene;
        g.addPass(
            "exposure.luma",
            [&, lumSrc](RGBuilder& b) {
                l64 = b.create("exposure.64", colorDesc(rhi::TextureFormat::R16F, 64, 64));
                b.read(lumSrc);
                b.write(l64);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&, lumSrc](RGContext& ctx) {
                fsState();
                m_device.setShader(m_lumaDown);
                m_device.bindTexture(0, ctx.texture(lumSrc));
                m_device.setUniform("uDstTexel", Vec4(1.0f / 64.0f, 1.0f / 64.0f, 0, 0));
                m_device.drawFullscreenTriangle();
                m_device.setDepthTest(true);
            });
        for (int step = 0; step < 2; ++step) {
            g.addPass(
                step == 0 ? "exposure.reduce0" : "exposure.reduce1",
                [&, step](RGBuilder& b) {
                    rhi::TextureDesc rd = colorDesc(rhi::TextureFormat::R16F, step == 0 ? 8 : 1, step == 0 ? 8 : 1);
                    rd.sampler.filter = rhi::Filter::Nearest;
                    RGResource& dst = step == 0 ? l8 : l1;
                    dst = b.create(step == 0 ? "exposure.8" : "exposure.1", rd);
                    b.read(step == 0 ? l64 : l8);
                    b.write(dst);
                    rhi::ClearDesc cd;
                    cd.color = cd.depth = false;
                    b.clear(cd);
                },
                [&, step](RGContext& ctx) {
                    fsState();
                    m_device.setShader(m_lumaReduce);
                    m_device.bindTexture(0, ctx.texture(step == 0 ? l64 : l8));
                    m_device.drawFullscreenTriangle();
                    m_device.setDepthTest(true);
                });
        }
        g.addPass(
            "exposure.adapt",
            [&](RGBuilder& b) {
                b.read(l1);
                b.read(expPrev);
                b.write(exposureTex);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&, expPrev, dt](RGContext& ctx) {
                fsState();
                m_device.setShader(m_lumaAdapt);
                m_device.bindTexture(0, ctx.texture(l1));
                m_device.bindTexture(1, ctx.texture(expPrev));
                m_device.setUniform("uAdapt", Vec4(dt, rs.autoExposureSpeed, hist.exposureValid ? 1.0f : 0.0f, 0));
                m_device.drawFullscreenTriangle();
                m_device.setDepthTest(true);
            });
    }
    // Depth of field and motion blur at the output size (after TAA / upscaling), on the HDR image
    if (dofOn) {
        const RGResource dofSrc = scene;
        const int w = sceneW, h = sceneH;
        g.addPass(
            "dof",
            [&, dofSrc, w, h](RGBuilder& b) {
                dofOut = b.create("dof", colorDesc(rhi::TextureFormat::RGBA16F, w, h));
                b.read(dofSrc);
                b.read(effDepth);
                b.write(dofOut);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&, dofSrc, w, h](RGContext& ctx) {
                fsState();
                m_device.setShader(m_dof);
                m_device.bindTexture(0, ctx.texture(dofSrc));
                m_device.bindTexture(1, ctx.texture(effDepth));
                m_device.setUniform("uDepthSize", depthSize);
                m_device.setUniform("uSrcSize", Vec4(static_cast<float>(w), static_cast<float>(h), 1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h)));
                m_device.setUniform("uDof", Vec4(rs.dofFocusDistance, rs.dofFocusRange, rs.dofMaxRadius * static_cast<float>(h) / 1080.0f, 0));
                m_device.drawFullscreenTriangle();
                m_device.setDepthTest(true);
            });
        scene = dofOut;
    }
    if (mbOn && sceneVelocity != kRGInvalid) {
        const RGResource mbSrc = scene;
        const int w = sceneW, h = sceneH;
        g.addPass(
            "motion.blur",
            [&, mbSrc, w, h](RGBuilder& b) {
                mbOut = b.create("motion.blur", colorDesc(rhi::TextureFormat::RGBA16F, w, h));
                b.read(mbSrc);
                b.read(sceneVelocity);
                b.write(mbOut);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&, mbSrc, w, h](RGContext& ctx) {
                fsState();
                m_device.setShader(m_motionBlur);
                m_device.bindTexture(0, ctx.texture(mbSrc));
                m_device.bindTexture(1, ctx.texture(sceneVelocity));
                m_device.setUniform("uSrcSize", Vec4(static_cast<float>(w), static_cast<float>(h), 1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h)));
                m_device.setUniform("uMb", Vec4(rs.motionBlurStrength, static_cast<float>(rs.motionBlurSamples), 0.05f, 0));
                m_device.drawFullscreenTriangle();
                m_device.setDepthTest(true);
            });
        scene = mbOut;
    }

    // 1.3.3 debug: NaN / Inf pixel counter (setNonFiniteCheck). Mask at the size of the image bloom reads; read back after the frame.
    const bool nfOn = m_nonFiniteCheck && m_nonFinite;
    if (nfOn) {
        rhi::TextureDesc md = colorDesc(rhi::TextureFormat::RGBA8, sceneW, sceneH);
        md.sampler.filter = rhi::Filter::Nearest;
        nfMask = g.importTexture("debug.nonfinite", historyTexture(m_nfMask, md), md);
        const RGResource post = scene;
        g.addPass(
            "debug.nonfinite",
            [&, post](RGBuilder& b) {
                b.read(rawScene);
                b.read(post);
                b.write(nfMask);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&, post](RGContext& ctx) {
                m_device.setDepthTest(false);
                m_device.setCullMode(rhi::CullMode::None);
                m_device.setShader(m_nonFinite);
                m_device.bindTexture(0, ctx.texture(rawScene));
                m_device.bindTexture(1, ctx.texture(post));
                m_device.drawFullscreenTriangle();
                m_device.setDepthTest(true);
            });
        g.markOutput(nfMask);
    }

    // Bloom: 13-tap downsample chain + tent upsample (additive)
    RGResource bloom = kRGInvalid;
    if (rs.bloom && !debug) {
        int levels = 0;
        int w = sceneW, h = sceneH;
        while (levels < 5 && w > 8 && h > 8) {
            w = std::max(1, w / 2);
            h = std::max(1, h / 2);
            ++levels;
        }
        chain.resize(static_cast<size_t>(levels), kRGInvalid);
        for (int i = 0; i < levels; ++i) {
            g.addPass(
                std::format("bloom.down{}", i),
                [&, i](RGBuilder& b) {
                    chain[static_cast<size_t>(i)] =
                        b.create(std::format("bloom{}", i), colorDesc(rhi::TextureFormat::RGBA16F, sceneW >> (i + 1), sceneH >> (i + 1)));
                    b.read(i == 0 ? scene : chain[static_cast<size_t>(i - 1)]);
                    b.write(chain[static_cast<size_t>(i)]);
                    rhi::ClearDesc cd;
                    cd.depth = false;
                    b.clear(cd);
                },
                [&, i](RGContext& ctx) {
                    RGResource src = i == 0 ? scene : chain[static_cast<size_t>(i - 1)];
                    const auto& sd = g.desc(src);
                    m_device.setDepthTest(false);
                    m_device.setCullMode(rhi::CullMode::None);
                    m_device.setShader(m_bloomDown);
                    m_device.bindTexture(0, ctx.texture(src));
                    m_device.setUniform("uTexelSize", Vec3{1.0f / static_cast<float>(sd.width), 1.0f / static_cast<float>(sd.height), 0});
                    m_device.setUniform("uFirst", i == 0 ? 1 : 0);
                    m_device.setUniform("uThreshold", rs.bloomThreshold);
                    m_device.drawFullscreenTriangle();
                });
        }
        for (int i = levels - 1; i >= 1; --i) {
            g.addPass(
                std::format("bloom.up{}", i),
                [&, i](RGBuilder& b) {
                    b.read(chain[static_cast<size_t>(i)]);
                    b.write(chain[static_cast<size_t>(i - 1)]);
                    rhi::ClearDesc cd;
                    cd.color = cd.depth = false;
                    b.clear(cd);
                },
                [&, i](RGContext& ctx) {
                    const auto& sd = g.desc(chain[static_cast<size_t>(i)]);
                    m_device.setDepthTest(false);
                    m_device.setCullMode(rhi::CullMode::None);
                    m_device.setBlendMode(rhi::BlendMode::Additive);
                    m_device.setShader(m_bloomUp);
                    m_device.bindTexture(0, ctx.texture(chain[static_cast<size_t>(i)]));
                    m_device.setUniform("uTexelSize", Vec3{1.0f / static_cast<float>(sd.width), 1.0f / static_cast<float>(sd.height), 0});
                    m_device.drawFullscreenTriangle();
                    m_device.setBlendMode(rhi::BlendMode::None);
                });
        }
        if (levels > 0) bloom = chain[0];
    }

    // Tonemap -> LDR (or PQ for HDR10 output), then FXAA / FSR / scaled blit -> output
    const bool hdrOut = gfx.hdr && m_device.hdrActive();
    RGResource ldr = kRGInvalid;
    g.addPass(
        "tonemap",
        [&](RGBuilder& b) {
            ldr = b.create("ldr", colorDesc(hdrOut ? rhi::TextureFormat::RGBA16F : rhi::TextureFormat::RGBA8, sceneW, sceneH));
            b.read(scene);
            b.read(bloom);
            if (exposureTex != kRGInvalid) b.read(exposureTex);
            b.write(ldr);
            rhi::ClearDesc cd;
            cd.color = cd.depth = false;
            b.clear(cd);
        },
        [&](RGContext& ctx) {
            m_device.setDepthTest(false);
            m_device.setCullMode(rhi::CullMode::None);
            m_device.setShader(m_tonemap);
            m_device.bindTexture(0, ctx.texture(scene));
            m_device.bindTexture(1, bloom != kRGInvalid ? ctx.texture(bloom) : m_white);
            m_device.bindTexture(2, exposureTex != kRGInvalid ? ctx.texture(exposureTex) : m_white);
            m_device.setUniform("uExposure", std::exp2(rs.exposure));
            m_device.setUniform("uAutoExposure", exposureTex != kRGInvalid ? 1 : 0);
            m_device.setUniform("uAutoEv", Vec4(rs.autoExposureMinEv, rs.autoExposureMaxEv, 0, 0));
            m_device.setUniform("uBloomIntensity", rs.bloomIntensity);
            m_device.setUniform("uBloomOn", bloom != kRGInvalid ? 1 : 0);
            m_device.setUniform("uTonemapper", static_cast<int>(rs.tonemapper));
            m_device.setUniform("uDebug", debug ? 1 : 0);
            m_device.setUniform("uHdrOutput", hdrOut ? 1 : 0);
            m_device.setUniform("uPaperWhite", gfx.paperWhiteNits);
            m_device.setUniform("uPeakNits", gfx.peakNits);
            m_device.drawFullscreenTriangle();
        });

    const bool scaled = sceneW != width || sceneH != height;
    auto fullscreen = [&](const char* name, RGResource src, RGResource dst, auto&& setup) {
        g.addPass(
            name,
            [&, src, dst](RGBuilder& b) {
                b.read(src);
                b.write(dst);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&, src, setup](RGContext& ctx) {
                m_device.setDepthTest(false);
                m_device.setCullMode(rhi::CullMode::None);
                setup(ctx.texture(src));
                m_device.drawFullscreenTriangle();
                m_device.setDepthTest(true);
            });
    };
    if (upscaler == UpscalerKind::Fsr && scaled) {
        // AMD FidelityFX Super Resolution 1: EASU (edge-adaptive upscale) + RCAS (sharpening)
        RGResource up = kRGInvalid;
        g.addPass(
            "fsr.easu",
            [&](RGBuilder& b) {
                up = b.create("fsr.upscaled", colorDesc(hdrOut ? rhi::TextureFormat::RGBA16F : rhi::TextureFormat::RGBA8, width, height));
                b.read(ldr);
                b.write(up);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&](RGContext& ctx) {
                m_device.setDepthTest(false);
                m_device.setCullMode(rhi::CullMode::None);
                m_device.setShader(m_fsrEasu);
                m_device.bindTexture(0, ctx.texture(ldr));
                m_device.setUniform("uInputSize", Vec4(static_cast<float>(sceneW), static_cast<float>(sceneH), 0, 0));
                m_device.setUniform("uOutputSize", Vec4(static_cast<float>(width), static_cast<float>(height), 0, 0));
                m_device.drawFullscreenTriangle();
                m_device.setDepthTest(true);
            });
        fullscreen("fsr.rcas", up, output, [&](rhi::TextureHandle src) {
            m_device.setShader(m_fsrRcas);
            m_device.bindTexture(0, src);
            m_device.setUniform("uSharpness", gfx.sharpness);
        });
    } else if (scaled) {
        fullscreen("upscale", ldr, output, [&](rhi::TextureHandle src) {
            m_device.setShader(m_blit);
            m_device.bindTexture(0, src);
            m_device.setUniform("uSrcSize", Vec4(static_cast<float>(sceneW), static_cast<float>(sceneH), 1.0f / static_cast<float>(sceneW),
                                                 1.0f / static_cast<float>(sceneH)));
            m_device.setUniform("uSharpness", gfx.sharpness);
        });
    } else {
        fullscreen("fxaa", ldr, output, [&](rhi::TextureHandle src) {
            m_device.setShader(m_fxaa);
            m_device.bindTexture(0, src);
            m_device.setUniform("uTexelSize", Vec3{1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height), 0});
            m_device.setUniform("uEnabled", rs.fxaa && !debug && !temporal && !hdrOut ? 1 : 0);
        });
    }

    if (!m_overlay.empty() || m_browserOverlay) {
        g.addPass(
            "overlay",
            [&](RGBuilder& b) {
                b.write(output);
                rhi::ClearDesc cd;
                cd.color = cd.depth = false;
                b.clear(cd);
            },
            [&](RGContext&) { drawOverlay(width, height); });
    }
    if (auto st = g.compile(); !st) {
        FLEDGE_LOG_ERROR("render", "{}", st.error);
        return;
    }
    g.execute(m_device, *m_pool);
    m_pool->endFrame();
    m_frame.passes = g.executionOrder();
    m_frame.culled = g.culledPasses();
    m_frame.pathTraceSamples = pathTrace ? hist.ptFrames : 0;
    if (nfOn && m_nfMask) {  // 1.3.3 debug counter readback (GPU sync; only while the check is on)
        rhi::FramebufferDesc fd1;
        fd1.colorCount = 1;
        fd1.colors[0] = {m_nfMask, 0, 0};
        std::vector<uint8_t> px;
        if (m_device.readFramebuffer(m_pool->framebuffer(fd1), sceneW, sceneH, px) && px.size() >= static_cast<size_t>(sceneW) * sceneH * 4) {
            int a = 0, ai = 0, b = 0;
            for (size_t i = 0; i + 3 < px.size(); i += 4) {
                a += px[i] > 127;
                ai += px[i + 1] > 127;
                b += px[i + 2] > 127;
            }
            m_frame.nonFiniteHdr = a;
            m_frame.nonFiniteHdrInf = ai;
            m_frame.nonFinitePost = b;
        }
    }

    // advance temporal state
    if (temporal && !pathTrace) {
        hist.taaIndex = 1 - hist.taaIndex;
        hist.taaValid = true;
    }
    if (rtActive) {
        hist.rtIndex = 1 - hist.rtIndex;
        for (int e = 0; e < 4; ++e) hist.rtValid[e] = rtFx[e];
    } else {
        for (bool& v : hist.rtValid) v = false;
    }
    if (ssrOn) {
        hist.ssrIndex = 1 - hist.ssrIndex;
        hist.ssrValid = true;
    } else {
        hist.ssrValid = false;
    }
    if (aeOn) {
        hist.exposureIndex = 1 - hist.exposureIndex;
        hist.exposureValid = true;
    } else {
        hist.exposureValid = false;
        hist.lastTime = 0;
    }
    hist.prevViewProj = fd.viewProjNoJitter;
    hist.prevValid = true;
    hist.prevVmViewProj = vfd.viewProjNoJitter;
    hist.prevVmValid = vmPass;
    hist.prevModels.swap(hist.models);
    ++hist.frame;
    // drop histories of views that stopped rendering (closed editor viewports, thumbnails)
    if (m_frameCounter % 120 == 0)
        for (auto it = m_views.begin(); it != m_views.end();) {
            if (m_frameCounter - it->second.lastUsed > 600) {
                releaseHistory(it->second);
                it = m_views.erase(it);
            } else {
                ++it;
            }
        }
}

} // namespace fledge
