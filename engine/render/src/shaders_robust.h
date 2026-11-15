#pragma once
// 1.3.3 rendering robustness (docs/GRAPHICS.md "Robustness rules"): helpers every built-in shader uses where it normalises an
// interpolated / skinned vector or reads HDR colour into an accumulation, plus the debug non-finite pixel counter.
//
// Why bit patterns: isnan() / isinf() and `x != x` may be folded away by shader compilers that assume finite math (fxc and
// some GL drivers do), and min / max are IEEE minNum / maxNum on D3D10+ / most GPUs (min(NaN, 200) = 200: the bloom
// prefilter turned a NaN pixel into a 200-bright halo). An exponent field of all ones (0x7f800000) is NaN or Inf on every
// IEEE-754 GPU and integer ops are never "fast-math"ed, so these tests survive every compiler.
//
// The helpers are selects: for finite input they return the input unchanged (same bits), so correct content renders
// pixel-identically to 1.3.2. They are appended to the Frame block (GLSL kFrameBlock / HLSL kFrame) and to the post shaders
// that have no Frame block (bloom, tonemap, TAA).

// ---------------------------------------------------------------- GLSL 330 ---
#define FLEDGE_GLSL_ROBUST R"(
bool fl_finite(float x) { return (floatBitsToUint(x) & 0x7f800000u) != 0x7f800000u; }
bool fl_isnan(float x) { return (floatBitsToUint(x) & 0x7fffffffu) > 0x7f800000u; }
// Shader outputs into HDR targets: NaN -> 0; +-Inf and anything an RGBA16F target cannot hold -> +-65504 (an overflowing
// highlight stays as bright as the format allows instead of being stored as Inf)
float fl_clampHdr(float x) { return fl_isnan(x) ? 0.0 : clamp(x, -65504.0, 65504.0); }
vec3 fl_clampHdr(vec3 c) { return vec3(fl_clampHdr(c.x), fl_clampHdr(c.y), fl_clampHdr(c.z)); }
vec4 fl_clampHdr(vec4 c) { return vec4(fl_clampHdr(c.x), fl_clampHdr(c.y), fl_clampHdr(c.z), fl_clampHdr(c.w)); }
// Reads into accumulations (bloom, TAA, exposure, DoF, motion blur, SSR, denoiser): NaN / +-Inf -> 0, i.e. "no information"
float fl_finiteOr0(float x) { return fl_finite(x) ? x : 0.0; }
vec2 fl_finiteOr0(vec2 c) { return vec2(fl_finiteOr0(c.x), fl_finiteOr0(c.y)); }
vec3 fl_finiteOr0(vec3 c) { return vec3(fl_finiteOr0(c.x), fl_finiteOr0(c.y), fl_finiteOr0(c.z)); }
vec4 fl_finiteOr0(vec4 c) { return vec4(fl_finiteOr0(c.x), fl_finiteOr0(c.y), fl_finiteOr0(c.z), fl_finiteOr0(c.w)); }
// true when v can be normalised: |v|^2 above 1e-30 and finite. 1e-30, not 1e-12: cofactor-transformed normals scale with
// the model scale squared (millimetre content at scale 0.001 has |n|^2 = 1e-12 and must keep its normals)
bool fl_usable(vec3 v) { float l2 = dot(v, v); return l2 > 1e-30 && fl_finite(l2); }
vec3 fl_normalize(vec3 v, vec3 fallback) { return fl_usable(v) ? normalize(v) : fallback; }
)"

// ------------------------------------------------------------ HLSL SM 4/5/6 ---
// (written with scalar ternaries: vector ?: is an error in HLSL 2021 / DXC, which compiles the Vulkan SPIR-V)
#define FLEDGE_HLSL_ROBUST R"(
bool fl_finite(float x) { return (asuint(x) & 0x7f800000u) != 0x7f800000u; }
bool fl_isnan(float x) { return (asuint(x) & 0x7fffffffu) > 0x7f800000u; }
float fl_clampHdr(float x) { return fl_isnan(x) ? 0.0 : clamp(x, -65504.0, 65504.0); }
float3 fl_clampHdr(float3 c) { return float3(fl_clampHdr(c.x), fl_clampHdr(c.y), fl_clampHdr(c.z)); }
float4 fl_clampHdr(float4 c) { return float4(fl_clampHdr(c.x), fl_clampHdr(c.y), fl_clampHdr(c.z), fl_clampHdr(c.w)); }
float fl_finiteOr0(float x) { return fl_finite(x) ? x : 0.0; }
float2 fl_finiteOr0(float2 c) { return float2(fl_finiteOr0(c.x), fl_finiteOr0(c.y)); }
float3 fl_finiteOr0(float3 c) { return float3(fl_finiteOr0(c.x), fl_finiteOr0(c.y), fl_finiteOr0(c.z)); }
float4 fl_finiteOr0(float4 c) { return float4(fl_finiteOr0(c.x), fl_finiteOr0(c.y), fl_finiteOr0(c.z), fl_finiteOr0(c.w)); }
bool fl_usable(float3 v) { float l2 = dot(v, v); return l2 > 1e-30 && fl_finite(l2); }
float3 fl_normalize(float3 v, float3 fallback) { return fl_usable(v) ? normalize(v) : fallback; }
)"

namespace fledge::shaders {

// Debug counter (ForwardRenderer::setNonFiniteCheck): per output pixel r = the raw HDR scene colour is NaN / Inf, g = it is
// Inf, b = the image bloom reads (after TAA / DoF / motion blur) is NaN / Inf. Nearest texel of each input (texelFetch).
inline const char* kNonFiniteFs = R"(#version 330 core
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uHdr;
uniform sampler2D uPost;
)" FLEDGE_GLSL_ROBUST R"(
bool bad(vec3 c) { return !(fl_finite(c.r) && fl_finite(c.g) && fl_finite(c.b)); }
bool inf(vec3 c) { return bad(c) && !(fl_isnan(c.r) || fl_isnan(c.g) || fl_isnan(c.b)); }
vec3 at(sampler2D t) { ivec2 s = textureSize(t, 0); return texelFetch(t, clamp(ivec2(vUv * vec2(s)), ivec2(0), s - 1), 0).rgb; }
void main() {
    vec3 a = at(uHdr), b = at(uPost);
    oColor = vec4(bad(a) ? 1.0 : 0.0, inf(a) ? 1.0 : 0.0, bad(b) ? 1.0 : 0.0, 1.0);
}
)";

// Tests only (ForwardRenderer::setNonFiniteInjection): NaN into the 2x2 block at the centre, +Inf into the pixel at 3/4 width,
// discard elsewhere. The bit patterns come from uniforms so no compiler can fold them.
inline const char* kInjectFs = R"(#version 330 core
out vec4 oColor;
uniform vec4 uSize;   // xy target size
uniform int uNanBits; // 0x7fc00000
uniform int uInfBits; // 0x7f800000
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), c = ivec2(uSize.xy) / 2;
    if (p.x >= c.x && p.x <= c.x + 1 && p.y >= c.y && p.y <= c.y + 1) { oColor = vec4(intBitsToFloat(uNanBits)); return; }
    if (p.x == int(uSize.x) * 3 / 4 && p.y == c.y) { oColor = vec4(vec3(intBitsToFloat(uInfBits)), 1.0); return; }
    discard;
}
)";

namespace hlsl {
inline const char* kInject = R"(
cbuffer Params : register(b1) { float4 uSize; int uNanBits; int uInfBits; float2 padI; };
float4 PSMain(FsOut i) : SV_Target {
    int2 p = int2(i.pos.xy), c = int2(uSize.xy) / 2;
    if (p.x >= c.x && p.x <= c.x + 1 && p.y >= c.y && p.y <= c.y + 1) return asfloat(uNanBits).xxxx;
    if (p.x == int(uSize.x) * 3 / 4 && p.y == c.y) return float4(asfloat(uInfBits).xxx, 1.0);
    discard;
    return (0.0).xxxx;
}
)";
inline const char* kNonFinite =FLEDGE_HLSL_ROBUST R"(
Texture2D uHdr : register(t0);  SamplerState s0 : register(s0);
Texture2D uPost : register(t1); SamplerState s1 : register(s1);
bool bad(float3 c) { return !(fl_finite(c.r) && fl_finite(c.g) && fl_finite(c.b)); }
bool isInf3(float3 c) { return bad(c) && !(fl_isnan(c.r) || fl_isnan(c.g) || fl_isnan(c.b)); }
float3 at(Texture2D t, float2 uv) {
    uint w, h;
    t.GetDimensions(w, h);
    int2 p = clamp(int2(uv * float2(w, h)), int2(0, 0), int2(w, h) - 1);
    return t.Load(int3(p, 0)).rgb;
}
float4 PSMain(FsOut i) : SV_Target {
    float3 a = at(uHdr, i.uv), b = at(uPost, i.uv);
    return float4(bad(a) ? 1.0 : 0.0, isInf3(a) ? 1.0 : 0.0, bad(b) ? 1.0 : 0.0, 1.0);
}
)";
} // namespace hlsl

} // namespace fledge::shaders
