#pragma once
// HLSL versions of the full-pipeline shaders (Direct3D 10/11/12; SM 4.0 / 5.0 / 5.1).
//
// Authoring rule (see docs/GRAPHICS.md "Shaders"): every built-in shader exists as GLSL 330
// (shaders.h) and as HLSL here, written side by side from the same algorithm. A
// cross-compiler (DXC + SPIRV-Cross) was evaluated and rejected for now: it would add two large
// native tools to every build and still need per-API fix-ups for clip space, UV origin and
// cube-face orientation. Instead the conventions are fixed in one place:
//
//   * cbuffer Frame : b0 (same std140-compatible layout as FrameData), cbuffer Params : b1
//     (per-draw "uniforms", written by name through shader reflection)
//   * texture slot N = tN + sN (the sampler comes from the bound texture's SamplerDesc)
//   * matrices are GL column-major as uploaded; mul(M, v) == M * v
//   * the renderer gives D3D devices projection matrices with clip z in [0, 1]
//   * render-target UVs: D3D has row 0 at the top, so fullscreen passes use uv.y = 1 - p.y and
//     shadow-map / cube-face lookups flip y where they project into a texture
// Equivalence is checked by the gpu tests (same scene on GL and D3D11/12, coarse image compare).
#include "shaders_robust.h"

namespace fledge::shaders::hlsl {

inline const char* kFrame = R"(
struct Light { float4 posType; float4 dirRange; float4 colorIntensity; float4 cone; };
cbuffer Frame : register(b0) {
    float4x4 uView;
    float4x4 uProj;
    float4x4 uViewProj;
    float4x4 uInvViewProj;
    float4x4 uCascadeVP[4];
    float4x4 uSpotVP[14];  // spot lights (1 each), point lights (6 faces each)
    float4 uCascadeSplits;
    float4 uCascadeTexel;
    float4 uCameraPos;
    float4 uParams0;
    float4 uParams1;
    Light uLights[8];
    float4x4 uViewProjNoJitter;
    float4x4 uPrevViewProj;
    float4 uParams2;  // xy jitter (NDC), z frame index, w RT flags
    float4 uFog0;     // x density, y height falloff, z fog height, w max opacity
    float4 uFog1;     // rgb fog colour, w start distance
    float4 uFog2;     // xyz towards the sun, w 1 = fog on
    float4 uFog3;     // rgb sun in-scatter
    float4 uWet;      // x wetness, y puddles
};
static const float PI = 3.14159265359;
float fogAmount(float3 p) {
    float3 d = p - uCameraPos.xyz;
    float t = max(length(d) - uFog1.w, 0.0);
    if (t <= 0.0) return 0.0;
    float k = uFog0.y;
    float atCamera = exp(clamp(-k * (uCameraPos.y - uFog0.z), -80.0, 80.0));
    float kd = k * d.y;
    float f = abs(kd) > 1e-4 ? (1.0 - exp(clamp(-kd, -80.0, 80.0))) / kd : 1.0;
    return min(1.0 - exp(-uFog0.x * atCamera * f * t), uFog0.w);
}
float3 fogColorAlong(float3 p) {
    float c = max(dot(normalize(p - uCameraPos.xyz), uFog2.xyz), 0.0);
    float c2 = c * c, c4 = c2 * c2;
    return uFog1.rgb + uFog3.rgb * (c4 * c4);
}
float3 applyFog(float3 color, float3 p) {
    if (uFog2.w < 0.5) return color;
    return lerp(color, fogColorAlong(p), fogAmount(p));
}
)" FLEDGE_HLSL_ROBUST;

inline const char* kFullscreen = R"(
struct FsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 ndc : TEXCOORD1; };
FsOut VSMain(uint id : SV_VertexID) {
    float2 p = float2((id << 1) & 2, id & 2);
    FsOut o;
    o.ndc = p * 2.0 - 1.0;
    o.uv = float2(p.x, 1.0 - p.y);
    o.pos = float4(o.ndc, 0.0, 1.0);
    return o;
}
)";

// ------------------------------------------------------------------ PBR ---
inline const char* kPbr = R"(
cbuffer Params : register(b1) {
    float4x4 uModel;
    float4 uBaseColor;
    float3 uEmissive; float uRoughness;
    float uMetallic; float uNormalScale; float uOcclusionStrength; float uAlphaCutoff;
    float uSelected; int uAlphaMode; int uUnlit; int uRtFlags;  // RT: 1 shadow, 2 AO (RT or SSAO), 4 reflections, 8 GI; 16 SSR
    float4x4 uPrevModel;
    float4 uScreen;  // xy = 1 / render size
    float4 uGiMin;
    float4 uGiExt;
};
Texture2D uBaseColorTex : register(t0);    SamplerState s0 : register(s0);
Texture2D uNormalTex : register(t1);       SamplerState s1 : register(s1);
Texture2D uMRTex : register(t2);           SamplerState s2 : register(s2);
Texture2D uOcclusionTex : register(t3);    SamplerState s3 : register(s3);
Texture2D uEmissiveTex : register(t4);     SamplerState s4 : register(s4);
TextureCube uIrradiance : register(t5);    SamplerState s5 : register(s5);
TextureCube uPrefiltered : register(t6);   SamplerState s6 : register(s6);
Texture2D uBrdfLut : register(t7);         SamplerState s7 : register(s7);
Texture2DArray<float> uCascadeShadow : register(t8); SamplerComparisonState s8 : register(s8);
Texture2DArray<float> uSpotShadow : register(t9);    SamplerComparisonState s9 : register(s9);
// ray-traced inputs (D3D12 + DXR only; denoised, render resolution)
Texture2D<float> uRtShadow : register(t10);      SamplerState s10 : register(s10);
Texture2D<float> uRtAo : register(t11);          SamplerState s11 : register(s11);
Texture2D<float4> uRtReflection : register(t12); SamplerState s12 : register(s12);
Texture2D<float4> uRtGi : register(t13);         SamplerState s13 : register(s13);
Texture2D uProbeGi : register(t15);              SamplerState s15 : register(s15);

struct VSIn { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; float4 tangent : TANGENT; };
struct PSIn {
    float4 pos : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 tangent : TEXCOORD3;
    float viewDepth : TEXCOORD4;
    float4 curClip : TEXCOORD5;
    float4 prevClip : TEXCOORD6;
};
struct PSOut { float4 color : SV_Target0; float4 velocity : SV_Target1; };

PSIn VSMain(VSIn i) {
    PSIn o;
    float4 wp = mul(uModel, float4(i.pos, 1.0));
    float3x3 mt = transpose((float3x3)uModel);  // mt[k] = column k
    float3 c0 = mt[0], c1 = mt[1], c2 = mt[2];
    float det = dot(c0, cross(c1, c2));
    o.normal = (cross(c1, c2) * i.normal.x + cross(c2, c0) * i.normal.y + cross(c0, c1) * i.normal.z) * (det < 0.0 ? -1.0 : 1.0);
    // 1.3.3: the adjugate is finite for singular (scale-0) matrices - zero when every axis collapsed, then the pixel shader
    // takes the face normal; NaN / Inf (a NaN bone matrix) -> 0 as well
    if (!fl_usable(o.normal)) o.normal = (0.0).xxx;
    o.tangent = float4(c0 * i.tangent.x + c1 * i.tangent.y + c2 * i.tangent.z, i.tangent.w);
    o.uv = i.uv;
    o.worldPos = wp.xyz;
    o.viewDepth = -mul(uView, wp).z;
    o.pos = mul(uViewProj, wp);
    o.curClip = mul(uViewProjNoJitter, wp);
    o.prevClip = mul(uPrevViewProj, mul(uPrevModel, float4(i.pos, 1.0)));
    return o;
}

float D_GGX(float NdotH, float a) {
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d + 1e-7);
}
float G_Smith(float NdotV, float NdotL, float r) {
    float k = (r + 1.0) * (r + 1.0) / 8.0;
    return (NdotV / (NdotV * (1.0 - k) + k)) * (NdotL / (NdotL * (1.0 - k) + k));
}
float3 F_Schlick(float c, float3 F0) { return F0 + (1.0 - F0) * pow(1.0 - c, 5.0); }
float3 F_SchlickR(float c, float3 F0, float r) { return F0 + (max((1.0 - r).xxx, F0) - F0) * pow(1.0 - c, 5.0); }

float pcfCascade(float3 uvz, float layer, float texel) {
#ifdef FLEDGE_FL10_0
    return 1.0;  // feature level 10_0: no comparison sampling of texture arrays -> no shadows
#endif
    float s = 0.0;
    [unroll] for (int x = -1; x <= 1; ++x)
        [unroll] for (int y = -1; y <= 1; ++y)
            s += uCascadeShadow.SampleCmpLevelZero(s8, float3(uvz.xy + float2(x, y) * texel, layer), uvz.z);
    return s / 9.0;
}
float pcfSpot(float3 uvz, float layer, float texel) {
#ifdef FLEDGE_FL10_0
    return 1.0;
#endif
    float s = 0.0;
    [unroll] for (int x = -1; x <= 1; ++x)
        [unroll] for (int y = -1; y <= 1; ++y)
            s += uSpotShadow.SampleCmpLevelZero(s9, float3(uvz.xy + float2(x, y) * texel, layer), uvz.z);
    return s / 9.0;
}
float3 shadowUvz(float4 clip) {
    float3 n = clip.xyz / clip.w;
    return float3(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5, n.z);  // D3D: texture row 0 is the top, depth in [0,1]
}

int cascadeIndex(float viewDepth) {
    int count = int(uParams0.w);
    [unroll] for (int i = 0; i < 4; ++i)
        if (i < count && viewDepth < uCascadeSplits[i]) return i;
    return count;
}

float directionalShadow(PSIn i, float3 N, float bias, float normalBias) {
    int c = cascadeIndex(i.viewDepth);
    if (c >= int(uParams0.w)) return 1.0;
    float3 p = i.worldPos + N * normalBias * uCascadeTexel[c];
    float3 uvz = shadowUvz(mul(uCascadeVP[c], float4(p, 1.0)));
    if (uvz.z >= 1.0) return 1.0;
    uvz.z -= bias;
    return pcfCascade(uvz, float(c), uParams1.y);
}

float spotShadow(PSIn i, int idx, float3 N, float bias, float normalBias, float dist) {
    float3 p = i.worldPos + N * normalBias * dist * 2.0 * uParams1.w;
    float4 clip = mul(uSpotVP[idx], float4(p, 1.0));
    if (clip.w <= 0.0) return 1.0;
    float3 uvz = shadowUvz(clip);
    uvz.z -= bias;
    return pcfSpot(uvz, float(idx), uParams1.w);
}

float pointShadow(PSIn i, int base, float3 fromLight, float3 N, float bias, float dist) {
    float3 a = abs(fromLight);
    int face = (a.x >= a.y && a.x >= a.z) ? (fromLight.x > 0.0 ? 0 : 1) : (a.y >= a.z ? (fromLight.y > 0.0 ? 2 : 3) : (fromLight.z > 0.0 ? 4 : 5));
    return spotShadow(i, base + face, N, bias, 1.0, dist);
}

float hash12(float2 p) { float3 p3 = frac(float3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return frac((p3.x + p3.y) * p3.z); }
float vnoise(float2 p) {
    float2 c = floor(p), f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash12(c), hash12(c + float2(1.0, 0.0)), u.x), lerp(hash12(c + float2(0.0, 1.0)), hash12(c + float2(1.0, 1.0)), u.x), u.y);
}
void applyWetness(inout float3 albedo, inout float rough, inout float3 N, float3 Ng, float metal, float3 wp) {
    if (uWet.x <= 0.0) return;
    float expo = Ng.y < -0.3 ? 0.0 : 0.4 + 0.6 * saturate(Ng.y);
    float wet = saturate(uWet.x) * expo;
    albedo *= 1.0 - 0.4 * wet * (1.0 - metal);
    rough = rough + (0.1 - rough) * wet * 0.85;
    float flatness = saturate((Ng.y - 0.9) / 0.08);
    float threshold = 1.0 - saturate(uWet.y) * saturate(uWet.x);
    float t = saturate((vnoise(wp.xz * 0.35) * 0.65 + vnoise(wp.xz * 1.3) * 0.35 - threshold) / 0.08);
    float puddle = flatness * t * t * (3.0 - 2.0 * t);
    rough = max(rough + (0.03 - rough) * puddle, 0.03);
    albedo *= 1.0 - 0.15 * puddle;
    N = fl_normalize(lerp(N, Ng, puddle), Ng);
}

float3 cascadeColor(int c) {
    if (c == 0) return float3(1.0, 0.35, 0.35);
    if (c == 1) return float3(0.35, 1.0, 0.35);
    if (c == 2) return float3(0.35, 0.5, 1.0);
    if (c == 3) return float3(1.0, 1.0, 0.35);
    return (0.6).xxx;
}

float3 probeGi(float3 wp) {
    if (uGiExt.w < 0.5) return 1.0.xxx;
    float3 t = saturate((wp - uGiMin.xyz) / max(uGiExt.xyz, 1e-3.xxx));
    float gx = max(uGiMin.w, 1.0);
    float gy = max(fmod(uGiExt.w, 16.0), 1.0);
    float gz = max(floor(uGiExt.w / 16.0), 1.0);
    float u = (t.x * (gx - 1.0) + 0.5) / gx;
    float slice = floor(t.z * (gz - 1.0) + 0.5);
    float v = (t.y * (gy - 1.0) + slice * gy + 0.5) / (gy * gz);
    return uProbeGi.Sample(s15, float2(u, v)).rgb;
}

float4 shade(PSIn i, bool front) {
    // 1.3.3 fallback normal: the face normal from screen-space derivatives (taken before any discard / branch), turned
    // towards the camera; the view vector if even that is degenerate. Same rule as the GLSL twin.
    float3 toEye = uCameraPos.xyz - i.worldPos;
    float3 faceN = fl_normalize(cross(ddx(i.worldPos), ddy(i.worldPos)), fl_normalize(toEye, float3(0.0, 1.0, 0.0)));
    if (dot(faceN, toEye) < 0.0) faceN = -faceN;
    float4 baseTex = uBaseColorTex.Sample(s0, i.uv);
    float3 albedo = uBaseColor.rgb * baseTex.rgb;
    float alpha = uBaseColor.a * baseTex.a;
    if (uAlphaMode == 1 && alpha < uAlphaCutoff) discard;

    float3 Ng;
    if (fl_usable(i.normal)) {  // valid normals: the 1.3.2 code, unchanged
        Ng = normalize(i.normal);
        if (!front) Ng = -Ng;
    } else {  // zero-length / NaN interpolated normal (zero vertex normals, a bone keyed to scale 0)
        Ng = faceN;
    }
    float3 N = Ng;
    float3 T = i.tangent.xyz - Ng * dot(Ng, i.tangent.xyz);
    if (dot(T, T) > 1e-8 && fl_usable(T)) {
        T = normalize(T);
        float3 B = cross(Ng, T) * (i.tangent.w < 0.0 ? -1.0 : 1.0);
        float3 tn = uNormalTex.Sample(s1, i.uv).xyz * 2.0 - 1.0;
        tn.xy *= uNormalScale;
        N = fl_normalize(T * tn.x + B * tn.y + Ng * tn.z, Ng);
    }
    float4 mr = uMRTex.Sample(s2, i.uv);
    float rough = clamp(uRoughness * mr.g, 0.04, 1.0);
    float metal = clamp(uMetallic * mr.b, 0.0, 1.0);
    float ao = lerp(1.0, uOcclusionTex.Sample(s3, i.uv).r, uOcclusionStrength);
    float3 emissive = uEmissive * uEmissiveTex.Sample(s4, i.uv).rgb;
    if (uUnlit == 0) applyWetness(albedo, rough, N, Ng, metal, i.worldPos);

    int debugView = int(uParams0.y);
    if (debugView == 1) return float4(albedo, 1.0);
    if (debugView == 2) return float4(N * 0.5 + 0.5, 1.0);
    if (debugView == 3) return float4(rough.xxx, 1.0);
    if (debugView == 4) return float4(metal.xxx, 1.0);
    if (debugView == 5) return float4((1.0 - saturate(i.viewDepth / 60.0)).xxx, 1.0);
    if (debugView == 7) return float4(0.9, 0.9, 0.9, 1.0);

    float3 color;
    if (uUnlit == 1) {
        color = albedo + emissive;
    } else {
        float3 V = fl_normalize(toEye, Ng);
        float NdotV = max(dot(N, V), 1e-4);
        float3 F0 = lerp((0.04).xxx, albedo, metal);
        float3 Lo = (0.0).xxx;
        int count = int(uParams0.z);
        [loop] for (int li = 0; li < 8; ++li) {
            if (li >= count) break;
            Light Lt = uLights[li];
            int type = int(Lt.posType.w);
            float3 L;
            float att = 1.0;
            float dist = 1.0;
            if (type == 0) {
                L = normalize(-Lt.dirRange.xyz);
                if (li == 0 && (uRtFlags & 1)) att *= uRtShadow.SampleLevel(s10, i.pos.xy * uScreen.xy, 0.0);
                else if (Lt.cone.z >= 0.0 && uParams1.x > 0.5) att *= directionalShadow(i, Ng, Lt.cone.w, Lt.posType.x);
            } else {
                float3 d = Lt.posType.xyz - i.worldPos;
                dist = length(d);
                L = d / max(dist, 1e-4);
                float f = saturate(1.0 - pow(dist / Lt.dirRange.w, 4.0));
                att = f * f / (dist * dist + 1.0);
                if (type == 2) {
                    float cd = dot(-L, normalize(Lt.dirRange.xyz));
                    att *= smoothstep(Lt.cone.y, Lt.cone.x, cd);
                    if (Lt.cone.z >= 0.0 && uParams1.x > 0.5) att *= spotShadow(i, int(Lt.cone.z), Ng, Lt.cone.w, 1.0, dist);
                } else if (Lt.cone.z >= 0.0 && uParams1.x > 0.5) {
                    att *= pointShadow(i, int(Lt.cone.z), i.worldPos - Lt.posType.xyz, Ng, Lt.cone.w, dist);
                }
            }
            float NdotL = max(dot(N, L), 0.0);
            if (NdotL <= 0.0 || att <= 0.0) continue;
            float3 H = normalize(V + L);
            float NdotH = max(dot(N, H), 0.0);
            float VdotH = max(dot(V, H), 0.0);
            float a = rough * rough;
            float3 F = F_Schlick(VdotH, F0);
            float3 spec = D_GGX(NdotH, a) * G_Smith(NdotV, NdotL, rough) * F / (4.0 * NdotV * NdotL + 1e-4);
            float3 kD = (1.0 - F) * (1.0 - metal);
            Lo += (kD * albedo / PI + spec) * Lt.colorIntensity.rgb * Lt.colorIntensity.w * att * NdotL;
        }
        float3 F = F_SchlickR(NdotV, F0, rough);
        float3 kD = (1.0 - F) * (1.0 - metal);
        float2 suv = i.pos.xy * uScreen.xy;
        float3 irr = (uRtFlags & 8) ? uRtGi.SampleLevel(s13, suv, 0.0).rgb : uIrradiance.Sample(s5, N).rgb * probeGi(i.worldPos);
        float3 R = reflect(-V, N);
        float3 pre = uPrefiltered.SampleLevel(s6, R, rough * uParams1.z).rgb;
        if (uRtFlags & 4) pre = lerp(uRtReflection.SampleLevel(s12, suv, 0.0).rgb, pre, smoothstep(0.6, 0.9, rough));
        if (uRtFlags & 16) {  // SSR over the IBL on smooth surfaces
            float4 ssr = uRtReflection.SampleLevel(s12, suv, 0.0);
            pre = lerp(pre, ssr.rgb, ssr.a * (1.0 - smoothstep(0.35, 0.75, rough)));
        }
        if (uRtFlags & 2) ao *= uRtAo.SampleLevel(s11, suv, 0.0);
        float2 brdf = uBrdfLut.Sample(s7, float2(NdotV, rough)).rg;
        float3 ambient = (kD * irr * albedo + pre * (F * brdf.x + brdf.y)) * ao * uParams0.x;
        color = Lo + ambient + emissive;
    }
    color = applyFog(color, i.worldPos);
    if (debugView == 6) color = albedo * cascadeColor(cascadeIndex(i.viewDepth));
    color = lerp(color, float3(1.0, 0.6, 0.1), uSelected * 0.25);
    return float4(color, uAlphaMode == 2 ? alpha : 1.0);
}

PSOut PSMain(PSIn i, bool front : SV_IsFrontFace) {
    PSOut o;
    o.color = fl_clampHdr(shade(i, front));  // 1.3.3: never NaN / Inf into the HDR target (identity for finite colours up to 65504)
    o.velocity = float4(fl_finiteOr0((i.curClip.xy / i.curClip.w - i.prevClip.xy / i.prevClip.w) * float2(0.5, -0.5)), 0.0, 1.0);
    return o;
}
)";

// ------------------------------------------------------------- shadows ---
inline const char* kShadow = R"(
cbuffer Params : register(b1) { float4x4 uModel; float4x4 uLightVP; };
float4 VSMain(float3 pos : POSITION) : SV_Position { return mul(uLightVP, mul(uModel, float4(pos, 1.0))); }
void PSMain() {}
)";

// ------------------------------------------------------------ sky/lines ---
inline const char* kSky = R"(
TextureCube uEnv : register(t0); SamplerState s0 : register(s0);
struct SkyOut { float4 pos : SV_Position; float2 ndc : TEXCOORD0; };
SkyOut VSMain(uint id : SV_VertexID) {
    float2 p = float2((id << 1) & 2, id & 2);
    SkyOut o;
    o.ndc = p * 2.0 - 1.0;
    o.pos = float4(o.ndc, 1.0, 1.0);  // far plane
    return o;
}
struct SkyPs { float4 color : SV_Target0; float4 velocity : SV_Target1; };
SkyPs PSMain(SkyOut i) {
    float4 p = mul(uInvViewProj, float4(i.ndc, 1.0, 1.0));
    float3 dir = normalize(p.xyz / p.w - uCameraPos.xyz);
    SkyPs o;
    o.color = float4(fl_clampHdr(applyFog(uEnv.SampleLevel(s0, dir, 0.0).rgb * uParams0.x, uCameraPos.xyz + dir * 2000.0)), 1.0);  // sky ~2 km away
    float4 far = float4(uCameraPos.xyz + dir * 1000.0, 1.0);
    float4 c = mul(uViewProjNoJitter, far), pc = mul(uPrevViewProj, far);
    o.velocity = float4((c.xy / c.w - pc.xy / pc.w) * float2(0.5, -0.5), 0.0, 1.0);
    return o;
}
)";
inline const char* kLines = R"(
struct LIn { float3 pos : POSITION; float3 color : COLOR; };
struct LOut { float4 pos : SV_Position; float3 color : COLOR; };
LOut VSMain(LIn i) { LOut o; o.color = i.color; o.pos = mul(uViewProj, float4(i.pos, 1.0)); return o; }
struct LinePs { float4 color : SV_Target0; float4 velocity : SV_Target1; };
LinePs PSMain(LOut i) { LinePs o; o.color = float4(i.color, 1.0); o.velocity = float4(0, 0, 0, 1); return o; }
)";

// ----------------------------------------------------------- particles ---
inline const char* kParticle = R"(
struct PVIn { float3 pos : POSITION; float3 color : NORMAL; float2 corner : TEXCOORD0; float4 params : TANGENT; };
struct PVOut { float4 pos : SV_Position; float3 worldPos : TEXCOORD0; float3 color : TEXCOORD1; float2 corner : TEXCOORD2; float4 params : TEXCOORD3; };
PVOut VSMain(PVIn i) {
    PVOut o;
    o.worldPos = i.pos;
    o.color = i.color;
    o.corner = i.corner;
    o.params = i.params;
    o.pos = mul(uViewProj, float4(i.pos, 1.0));
    return o;
}
struct ParticlePs { float4 color : SV_Target0; float4 velocity : SV_Target1; };
ParticlePs PSMain(PVOut i) {
    float shape = i.params.y > 0.5 ? (1.0 - abs(i.corner.x)) * (1.0 - i.corner.y * i.corner.y) : max(1.0 - dot(i.corner, i.corner), 0.0);
    float a = i.params.x * shape;
    if (a <= 0.002) discard;
    float fog = uFog2.w > 0.5 ? fogAmount(i.worldPos) : 0.0;
    ParticlePs o;
    if (i.params.z > 0.5) o.color = float4(i.color * (a * (1.0 - fog)), 0.0);
    else o.color = float4(lerp(i.color, fogColorAlong(i.worldPos), fog), a);
    o.color = fl_clampHdr(o.color);
    o.velocity = (0.0).xxxx;
    return o;
}
)";

// ------------------------------------------------------- IBL precompute ---
inline const char* kCubeCommon = R"(
cbuffer Params : register(b1) { int uFace; float3 uSunDir; float uRoughness; float uEnvSize; float2 padC; };
static const float PI = 3.14159265359;
float3 cubeDir(int face, float2 ndc) {
    float2 p = float2(ndc.x, -ndc.y);  // D3D render targets: NDC +y is texel row 0 (GL face convention otherwise)
    if (face == 0) return normalize(float3(1.0, -p.y, -p.x));
    if (face == 1) return normalize(float3(-1.0, -p.y, p.x));
    if (face == 2) return normalize(float3(p.x, 1.0, p.y));
    if (face == 3) return normalize(float3(p.x, -1.0, -p.y));
    if (face == 4) return normalize(float3(p.x, -p.y, 1.0));
    return normalize(float3(-p.x, -p.y, -1.0));
}
)";
inline const char* kEquirectToCube = R"(
Texture2D uEquirect : register(t0); SamplerState s0 : register(s0);
float4 PSMain(FsOut i) : SV_Target {
    float3 d = cubeDir(uFace, i.ndc);
    float2 uv = float2(atan2(d.z, d.x) / (2.0 * PI) + 0.5, acos(clamp(d.y, -1.0, 1.0)) / PI);
    return float4(min(uEquirect.SampleLevel(s0, uv, 0.0).rgb, (500.0).xxx), 1.0);
}
)";
inline const char* kProceduralSky = R"(
float3 sky(float3 d) {
    float h = d.y;
    float3 zenith = float3(0.18, 0.36, 0.85);
    float3 horizon = float3(0.75, 0.82, 0.95);
    float3 ground = float3(0.22, 0.2, 0.18);
    float3 c = h > 0.0 ? lerp(horizon, zenith, pow(h, 0.45)) : lerp(horizon * 0.6, ground, pow(min(-h * 4.0, 1.0), 0.7));
    float sd = max(dot(d, normalize(uSunDir)), 0.0);
    c += float3(1.0, 0.9, 0.7) * pow(sd, 64.0) * 1.5;
    c += float3(40.0, 36.0, 30.0) * smoothstep(0.9995, 0.99975, sd);
    return c * 0.8;
}
float4 PSMain(FsOut i) : SV_Target { return float4(sky(cubeDir(uFace, i.ndc)), 1.0); }
)";
inline const char* kIrradiance = R"(
TextureCube uEnv : register(t0); SamplerState s0 : register(s0);
float4 PSMain(FsOut i) : SV_Target {
    float3 N = cubeDir(uFace, i.ndc);
    float3 up = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 right = normalize(cross(up, N));
    up = cross(N, right);
    float3 sum = (0.0).xxx;
    float n = 0.0;
    const float delta = 0.05;
    [loop] for (float phi = 0.0; phi < 2.0 * PI; phi += delta)
        [loop] for (float theta = 0.0; theta < 0.5 * PI; theta += delta) {
            float3 t = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            float3 s = t.x * right + t.y * up + t.z * N;
            sum += uEnv.SampleLevel(s0, s, 3.0).rgb * cos(theta) * sin(theta);
            n += 1.0;
        }
    return float4(PI * sum / n, 1.0);
}
)";
inline const char* kPrefilter = R"(
TextureCube uEnv : register(t0); SamplerState s0 : register(s0);
float radicalInverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
float3 importanceGGX(float2 xi, float3 N, float r) {
    float a = r * r;
    float phi = 2.0 * PI * xi.x;
    float cosT = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinT = sqrt(1.0 - cosT * cosT);
    float3 H = float3(cos(phi) * sinT, sin(phi) * sinT, cosT);
    float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tx = normalize(cross(up, N));
    float3 ty = cross(N, tx);
    return normalize(tx * H.x + ty * H.y + N * H.z);
}
float4 PSMain(FsOut i) : SV_Target {
    float3 N = cubeDir(uFace, i.ndc);
    float3 V = N;
    const uint COUNT = 256u;
    float3 sum = (0.0).xxx;
    float w = 0.0;
    [loop] for (uint k = 0u; k < COUNT; ++k) {
        float2 xi = float2(float(k) / float(COUNT), radicalInverse(k));
        float3 H = importanceGGX(xi, N, uRoughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = dot(N, L);
        if (NdotL > 0.0) {
            float NdotH = max(dot(N, H), 0.0);
            float a2 = pow(uRoughness, 4.0);
            float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
            float D = a2 / (PI * d * d);
            float pdf = D * NdotH / (4.0 * max(dot(H, V), 1e-4)) + 1e-4;
            float saTexel = 4.0 * PI / (6.0 * uEnvSize * uEnvSize);
            float saSample = 1.0 / (float(COUNT) * pdf + 1e-4);
            float mip = uRoughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);
            sum += uEnv.SampleLevel(s0, L, mip).rgb * NdotL;
            w += NdotL;
        }
    }
    return float4(sum / max(w, 1e-4), 1.0);
}
)";

// --------------------------------------------------------------- post ---
inline const char* kBloomDown = FLEDGE_HLSL_ROBUST R"(
cbuffer Params : register(b1) { float3 uTexelSize; int uFirst; float uThreshold; float3 padB; };
Texture2D uSrc : register(t0); SamplerState s0 : register(s0);
// 1.3.3: every tap NaN / Inf -> 0 (a bad pixel must not turn into a min(NaN, 200) = 200 halo); identity for finite taps
float3 smp(float2 uv, float2 o) { return fl_finiteOr0(uSrc.SampleLevel(s0, uv + o * uTexelSize.xy, 0.0).rgb); }
float4 PSMain(FsOut i) : SV_Target {
    float2 uv = i.uv;
    float3 a = smp(uv, float2(-2, 2)), b = smp(uv, float2(0, 2)), c = smp(uv, float2(2, 2));
    float3 d = smp(uv, float2(-2, 0)), e = smp(uv, float2(0, 0)), f = smp(uv, float2(2, 0));
    float3 g = smp(uv, float2(-2, -2)), h = smp(uv, float2(0, -2)), k = smp(uv, float2(2, -2));
    float3 j = smp(uv, float2(-1, 1)), kk = smp(uv, float2(1, 1)), l = smp(uv, float2(-1, -1)), m = smp(uv, float2(1, -1));
    float3 col = e * 0.125 + (a + c + g + k) * 0.03125 + (b + d + f + h) * 0.0625 + (j + kk + l + m) * 0.125;
    if (uFirst == 1) {
        float luma = dot(col, float3(0.2126, 0.7152, 0.0722));
        col *= max(luma - uThreshold, 0.0) / max(luma, 1e-4);
        col = min(col, (200.0).xxx);
    }
    return float4(col, 1.0);
}
)";
inline const char* kBloomUp = R"(
cbuffer Params : register(b1) { float3 uTexelSize; float padU; };
Texture2D uSrc : register(t0); SamplerState s0 : register(s0);
float3 smp(float2 uv, float2 o) { return uSrc.SampleLevel(s0, uv + o * uTexelSize.xy, 0.0).rgb; }
float4 PSMain(FsOut i) : SV_Target {
    float2 uv = i.uv;
    float3 col = smp(uv, float2(0, 0)) * 4.0 + (smp(uv, float2(-1, 0)) + smp(uv, float2(1, 0)) + smp(uv, float2(0, -1)) + smp(uv, float2(0, 1))) * 2.0 +
                 (smp(uv, float2(-1, -1)) + smp(uv, float2(1, -1)) + smp(uv, float2(-1, 1)) + smp(uv, float2(1, 1)));
    return float4(col / 16.0, 1.0);
}
)";
inline const char* kTonemap = FLEDGE_HLSL_ROBUST R"(
cbuffer Params : register(b1) {
    float uExposure; float uBloomIntensity; int uBloomOn; int uTonemapper;
    int uDebug; int uHdrOutput; float uPaperWhite; float uPeakNits;
    int uAutoExposure; float3 padT;
    float4 uAutoEv;  // x min EV, y max EV
};
Texture2D uHdr : register(t0); SamplerState s0 : register(s0);
Texture2D uBloom : register(t1); SamplerState s1 : register(s1);
Texture2D uExposureTex : register(t2); SamplerState s2 : register(s2);  // auto-exposure: adapted log2 luminance (1x1)
float3 aces(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }
float3 agx(float3 v) {
    const float3x3 inMat = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                    0.0784335999999992, 0.878468636469772, 0.0784336,
                                    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 outMat = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                                     -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                                     -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    v = mul(max(v, (1e-10).xxx), inMat);  // == GLSL inMat * v (GLSL fills columns, HLSL rows)
    v = clamp(log2(v), minEv, maxEv);
    v = (v - minEv) / (maxEv - minEv);
    float3 v2 = v * v, v4 = v2 * v2;
    v = 15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4 - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232;
    v = mul(v, outMat);
    return pow(saturate(v), (2.2).xxx);
}
// HDR10 output: Rec.709 -> Rec.2020, scene-referred nits -> ST.2084 (PQ)
float3 pq(float3 nits) {
    float3 y = saturate(nits / 10000.0);
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(y, m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
float4 PSMain(FsOut i) : SV_Target {
    float3 c = fl_finiteOr0(uHdr.Sample(s0, i.uv).rgb);  // 1.3.3: NaN / Inf -> 0 (identity for finite colours)
    if (uDebug == 0) {
        if (uBloomOn == 1) c += fl_finiteOr0(uBloom.Sample(s1, i.uv).rgb) * uBloomIntensity;
        if (uAutoExposure == 1) c *= exp2(clamp(log2(0.18) - fl_finiteOr0(uExposureTex.Load(int3(0, 0, 0)).r), uAutoEv.x, uAutoEv.y));  // autoExposureEv()
        c *= uExposure;
        if (uHdrOutput == 1) {
            // highlights roll off towards the display peak; paper white sets the SDR reference level
            float3 n = c * uPaperWhite;
            n = n / (1.0 + n / max(uPeakNits, 100.0));
            const float3x3 toBt2020 = float3x3(0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956);
            return float4(pq(mul(toBt2020, n)), 1.0);
        }
        if (uTonemapper == 0) c = aces(c);
        else if (uTonemapper == 1) c = agx(c);
        else c = saturate(c);
    }
    c = pow(saturate(c), (1.0 / 2.2).xxx);
    return float4(c, dot(c, float3(0.299, 0.587, 0.114)));
}
)";
inline const char* kFxaa = R"(
cbuffer Params : register(b1) { float3 uTexelSize; int uEnabled; };
Texture2D uTex : register(t0); SamplerState s0 : register(s0);
float4 PSMain(FsOut i) : SV_Target {
    float2 uv = i.uv;
    float2 t = uTexelSize.xy;
    float4 m = uTex.SampleLevel(s0, uv, 0.0);
    if (uEnabled == 0) return float4(m.rgb, 1.0);
    float lNW = uTex.SampleLevel(s0, uv + float2(-1.0, -1.0) * t, 0.0).a;
    float lNE = uTex.SampleLevel(s0, uv + float2(1.0, -1.0) * t, 0.0).a;
    float lSW = uTex.SampleLevel(s0, uv + float2(-1.0, 1.0) * t, 0.0).a;
    float lSE = uTex.SampleLevel(s0, uv + float2(1.0, 1.0) * t, 0.0).a;
    float lM = m.a;
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    if (lMax - lMin < max(0.0312, lMax * 0.125)) return float4(m.rgb, 1.0);
    float2 dir = float2(-((lNW + lNE) - (lSW + lSE)), ((lNW + lSW) - (lNE + lSE)));
    float reduce = max((lNW + lNE + lSW + lSE) * 0.25 * 0.125, 1.0 / 128.0);
    float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpMin, (-8.0).xx, (8.0).xx) * t;
    float3 a = 0.5 * (uTex.SampleLevel(s0, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb + uTex.SampleLevel(s0, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    float3 b = a * 0.5 + 0.25 * (uTex.SampleLevel(s0, uv + dir * -0.5, 0.0).rgb + uTex.SampleLevel(s0, uv + dir * 0.5, 0.0).rgb);
    float lB = dot(b, float3(0.299, 0.587, 0.114));
    return float4((lB < lMin || lB > lMax) ? a : b, 1.0);
}
)";

} // namespace fledge::shaders::hlsl
