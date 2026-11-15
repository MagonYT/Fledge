#pragma once
// Built-in shaders. GLSL 330 core for the full pipeline; the basic forward
// shader is dual-source (GLSL + HLSL SM5) so the partial D3D12 backend can use it.
// 1.3.3: the robustness helpers (fl_*) come with the Frame block; rules in shaders_robust.h and docs/GRAPHICS.md.
#include "shaders_robust.h"
namespace fledge::shaders {

// ------------------------------------------------------------- common ---
inline const char* kFrameBlock = R"(
struct Light { vec4 posType; vec4 dirRange; vec4 colorIntensity; vec4 cone; };
layout(std140) uniform Frame {
    mat4 uView;
    mat4 uProj;
    mat4 uViewProj;
    mat4 uInvViewProj;
    mat4 uCascadeVP[4];
    mat4 uSpotVP[14];      // shadow views: spot lights (1 each), point lights (6 faces each, pointShadowViewProj())
    vec4 uCascadeSplits;   // view-space far distance per cascade
    vec4 uCascadeTexel;    // world-space texel size per cascade
    vec4 uCameraPos;       // xyz
    vec4 uParams0;         // x env intensity, y debug view, z light count, w cascade count
    vec4 uParams1;         // x shadows on, y 1/shadow res, z prefilter max lod, w 1/spot shadow res
    Light uLights[8];
    mat4 uViewProjNoJitter;  // current view-projection without TAA jitter (motion vectors)
    mat4 uPrevViewProj;      // previous frame, without jitter
    vec4 uParams2;           // xy jitter (NDC), z frame index, w RT flags
    vec4 uFog0;              // x density, y height falloff, z fog height, w max opacity
    vec4 uFog1;              // rgb fog colour, w start distance
    vec4 uFog2;              // xyz direction towards the sun, w 1 = fog on
    vec4 uFog3;              // rgb sun in-scatter (sun colour * intensity * sunScatter * 0.1)
    vec4 uWet;               // x wetness 0..1, y puddles 0..1 (wet surfaces, CPU: wetSurface())
};
// Exponential height fog (CPU reference: fogAmount() / fogInscatter() in render_math.cpp).
float fogAmount(vec3 p) {
    vec3 d = p - uCameraPos.xyz;
    float t = max(length(d) - uFog1.w, 0.0);
    if (t <= 0.0) return 0.0;
    float k = uFog0.y;
    float atCamera = exp(clamp(-k * (uCameraPos.y - uFog0.z), -80.0, 80.0));
    float kd = k * d.y;
    float f = abs(kd) > 1e-4 ? (1.0 - exp(clamp(-kd, -80.0, 80.0))) / kd : 1.0;
    return min(1.0 - exp(-uFog0.x * atCamera * f * t), uFog0.w);
}
vec3 fogColorAlong(vec3 p) {
    float c = max(dot(normalize(p - uCameraPos.xyz), uFog2.xyz), 0.0);
    float c2 = c * c, c4 = c2 * c2;
    return uFog1.rgb + uFog3.rgb * (c4 * c4);
}
vec3 applyFog(vec3 color, vec3 p) {
    if (uFog2.w < 0.5) return color;
    return mix(color, fogColorAlong(p), fogAmount(p));
}
)" FLEDGE_GLSL_ROBUST;

inline const char* kFullscreenVs = R"(#version 330 core
out vec2 vUv;
out vec2 vNdc;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = p;
    vNdc = p * 2.0 - 1.0;
    gl_Position = vec4(vNdc, 0.0, 1.0);
}
)";

// ------------------------------------------------------------------ PBR ---
inline const char* kPbrVsBody = R"(
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUv;
layout(location=3) in vec4 aTangent;
uniform mat4 uModel;
uniform mat4 uPrevModel;
out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUv;
out vec4 vTangent;
out float vViewDepth;
out vec4 vCurClip;
out vec4 vPrevClip;
// 1.3.3: world normal through a model / skin matrix. Invertible: the inverse transpose (exactly as before). Singular (a
// bone keyed to scale 0, a zero-scale transform): inverse() is undefined there (Inf / NaN on real drivers), so the adjugate
// transpose is used instead - finite, the same direction where one exists, exactly zero when every axis collapsed (the
// fragment shader then uses the face normal). Never NaN / Inf.
vec3 normalToWorld(mat4 m, vec3 n) {
    mat3 a = mat3(m);
    if (abs(determinant(a)) > 1e-24) return mat3(transpose(inverse(m))) * n;
    vec3 r = cross(a[1], a[2]) * n.x + cross(a[2], a[0]) * n.y + cross(a[0], a[1]) * n.z;
    return fl_usable(r) ? r : vec3(0.0);
}
void main() {
    vec4 wp = uModel * vec4(aPos, 1.0);
    vCurClip = uViewProjNoJitter * wp;
    vPrevClip = uPrevViewProj * (uPrevModel * vec4(aPos, 1.0));
    vNormal = normalToWorld(uModel, aNormal);
    vTangent = vec4(mat3(uModel) * aTangent.xyz, aTangent.w);
    vUv = aUv;
    vWorldPos = wp.xyz;
    vViewDepth = -(uView * wp).z;
    gl_Position = uViewProj * wp;
}
)";

inline const char* kPbrFsBody = R"(
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUv;
in vec4 vTangent;
in float vViewDepth;
in vec4 vCurClip;
in vec4 vPrevClip;
layout(location=0) out vec4 oColor;
layout(location=1) out vec4 oVelocity;  // RG16F motion vector in UV units (current - previous)
uniform sampler2D uBaseColorTex;
uniform sampler2D uNormalTex;
uniform sampler2D uMRTex;
uniform sampler2D uOcclusionTex;
uniform sampler2D uEmissiveTex;
uniform samplerCube uIrradiance;
uniform samplerCube uPrefiltered;
uniform sampler2D uBrdfLut;
uniform sampler2DArrayShadow uCascadeShadow;
uniform sampler2DArrayShadow uSpotShadow;
uniform sampler2D uRtAo;          // unit 11: SSAO (uRtFlags & 2)
uniform sampler2D uRtReflection;  // unit 12: SSR, rgb + confidence (uRtFlags & 16)
uniform int uRtFlags;
uniform vec4 uScreen;             // xy = 1 / render size
uniform sampler2D uProbeGi;       // unit 15: baked irradiance atlas
uniform vec4 uGiMin;              // xyz world min, w = gx
uniform vec4 uGiExt;              // xyz extent; w = gy + gz*16 (0 = off)
uniform vec4 uBaseColor;
uniform vec3 uEmissive;
uniform float uRoughness;
uniform float uMetallic;
uniform float uNormalScale;
uniform float uOcclusionStrength;
uniform float uAlphaCutoff;
uniform float uSelected;
uniform int uAlphaMode;
uniform int uUnlit;
const float PI = 3.14159265359;

float D_GGX(float NdotH, float a) {
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d + 1e-7);
}
float G_Smith(float NdotV, float NdotL, float r) {
    float k = (r + 1.0) * (r + 1.0) / 8.0;
    return (NdotV / (NdotV * (1.0 - k) + k)) * (NdotL / (NdotL * (1.0 - k) + k));
}
vec3 F_Schlick(float c, vec3 F0) { return F0 + (1.0 - F0) * pow(1.0 - c, 5.0); }
vec3 F_SchlickR(float c, vec3 F0, float r) { return F0 + (max(vec3(1.0 - r), F0) - F0) * pow(1.0 - c, 5.0); }

float pcf(sampler2DArrayShadow tex, vec3 uvz, float layer, float texel) {
    float s = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            s += texture(tex, vec4(uvz.xy + vec2(x, y) * texel, layer, uvz.z));
    return s / 9.0;
}

int cascadeIndex() {
    int count = int(uParams0.w);
    for (int i = 0; i < 4; ++i)
        if (i < count && vViewDepth < uCascadeSplits[i]) return i;
    return count;  // beyond the shadow distance
}

float directionalShadow(vec3 N, float bias, float normalBias) {
    int c = cascadeIndex();
    if (c >= int(uParams0.w)) return 1.0;
    vec3 p = vWorldPos + N * normalBias * uCascadeTexel[c];
    vec4 clip = uCascadeVP[c] * vec4(p, 1.0);
    vec3 uvz = clip.xyz / clip.w * 0.5 + 0.5;
    if (uvz.z >= 1.0) return 1.0;
    uvz.z -= bias;
    return pcf(uCascadeShadow, uvz, float(c), uParams1.y);
}

float spotShadow(int idx, vec3 N, float bias, float normalBias, float dist) {
    vec3 p = vWorldPos + N * normalBias * dist * 2.0 * uParams1.w;  // ~one shadow texel at this distance
    vec4 clip = uSpotVP[idx] * vec4(p, 1.0);
    if (clip.w <= 0.0) return 1.0;
    vec3 uvz = clip.xyz / clip.w * 0.5 + 0.5;
    uvz.z -= bias;
    return pcf(uSpotShadow, uvz, float(idx), uParams1.w);
}

// point light: the face of its 6-face map that the direction from the light falls on (CPU: pointShadowFace())
float pointShadow(int base, vec3 fromLight, vec3 N, float bias, float dist) {
    vec3 a = abs(fromLight);
    int face = (a.x >= a.y && a.x >= a.z) ? (fromLight.x > 0.0 ? 0 : 1) : (a.y >= a.z ? (fromLight.y > 0.0 ? 2 : 3) : (fromLight.z > 0.0 ? 4 : 5));
    return spotShadow(base + face, N, bias, 1.0, dist);
}

// wet surfaces (CPU reference: wetSurface() in render_math.cpp)
float hash12(vec2 p) { vec3 p3 = fract(vec3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), u.x), mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x), u.y);
}
void applyWetness(inout vec3 albedo, inout float rough, inout vec3 N, vec3 Ng, float metal, vec3 wp) {
    if (uWet.x <= 0.0) return;
    float expo = Ng.y < -0.3 ? 0.0 : 0.4 + 0.6 * clamp(Ng.y, 0.0, 1.0);
    float wet = clamp(uWet.x, 0.0, 1.0) * expo;
    albedo *= 1.0 - 0.4 * wet * (1.0 - metal);
    rough = rough + (0.1 - rough) * wet * 0.85;
    float flatness = clamp((Ng.y - 0.9) / 0.08, 0.0, 1.0);
    float threshold = 1.0 - clamp(uWet.y, 0.0, 1.0) * clamp(uWet.x, 0.0, 1.0);
    float t = clamp((vnoise(wp.xz * 0.35) * 0.65 + vnoise(wp.xz * 1.3) * 0.35 - threshold) / 0.08, 0.0, 1.0);
    float puddle = flatness * t * t * (3.0 - 2.0 * t);
    rough = max(rough + (0.03 - rough) * puddle, 0.03);
    albedo *= 1.0 - 0.15 * puddle;
    N = fl_normalize(mix(N, Ng, puddle), Ng);
}

vec3 cascadeColor(int c) {
    if (c == 0) return vec3(1.0, 0.35, 0.35);
    if (c == 1) return vec3(0.35, 1.0, 0.35);
    if (c == 2) return vec3(0.35, 0.5, 1.0);
    if (c == 3) return vec3(1.0, 1.0, 0.35);
    return vec3(0.6);
}

vec3 probeGi(vec3 wp) {
    if (uGiExt.w < 0.5) return vec3(1.0);
    vec3 t = clamp((wp - uGiMin.xyz) / max(uGiExt.xyz, vec3(1e-3)), 0.0, 1.0);
    float gx = max(uGiMin.w, 1.0);
    float gy = max(mod(uGiExt.w, 16.0), 1.0);
    float gz = max(floor(uGiExt.w / 16.0), 1.0);
    float u = (t.x * (gx - 1.0) + 0.5) / gx;
    float slice = floor(t.z * (gz - 1.0) + 0.5);
    float v = (t.y * (gy - 1.0) + slice * gy + 0.5) / (gy * gz);
    return texture(uProbeGi, vec2(u, v)).rgb;
}

vec4 shade() {
    // 1.3.3 fallback normal: the face normal from screen-space derivatives (taken before any discard / branch, where the
    // derivatives are defined), turned towards the camera; the view vector if even that is degenerate.
    vec3 toEye = uCameraPos.xyz - vWorldPos;
    vec3 faceN = fl_normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)), fl_normalize(toEye, vec3(0.0, 1.0, 0.0)));
    if (dot(faceN, toEye) < 0.0) faceN = -faceN;
    vec4 baseTex = texture(uBaseColorTex, vUv);
    vec3 albedo = uBaseColor.rgb * baseTex.rgb;
    float alpha = uBaseColor.a * baseTex.a;
    if (uAlphaMode == 1 && alpha < uAlphaCutoff) discard;

    vec3 Ng;
    if (fl_usable(vNormal)) {  // valid normals: the 1.3.2 code, unchanged
        Ng = normalize(vNormal);
        if (!gl_FrontFacing) Ng = -Ng;
    } else {  // zero-length / NaN interpolated normal (zero vertex normals, a bone keyed to scale 0)
        Ng = faceN;
    }
    vec3 N = Ng;
    vec3 T = vTangent.xyz - Ng * dot(Ng, vTangent.xyz);
    if (dot(T, T) > 1e-8 && fl_usable(T)) {
        T = normalize(T);
        vec3 B = cross(Ng, T) * (vTangent.w < 0.0 ? -1.0 : 1.0);
        vec3 tn = texture(uNormalTex, vUv).xyz * 2.0 - 1.0;
        tn.xy *= uNormalScale;
        N = fl_normalize(mat3(T, B, Ng) * tn, Ng);
    }
    vec4 mr = texture(uMRTex, vUv);
    float rough = clamp(uRoughness * mr.g, 0.04, 1.0);
    float metal = clamp(uMetallic * mr.b, 0.0, 1.0);
    float ao = mix(1.0, texture(uOcclusionTex, vUv).r, uOcclusionStrength);
    vec3 emissive = uEmissive * texture(uEmissiveTex, vUv).rgb;
    if (uUnlit == 0) applyWetness(albedo, rough, N, Ng, metal, vWorldPos);

    int debugView = int(uParams0.y);
    if (debugView == 1) return vec4(albedo, 1.0);
    if (debugView == 2) return vec4(N * 0.5 + 0.5, 1.0);
    if (debugView == 3) return vec4(vec3(rough), 1.0);
    if (debugView == 4) return vec4(vec3(metal), 1.0);
    if (debugView == 5) return vec4(vec3(1.0 - clamp(vViewDepth / 60.0, 0.0, 1.0)), 1.0);
    if (debugView == 7) return vec4(0.9, 0.9, 0.9, 1.0);

    vec3 color;
    if (uUnlit == 1) {
        color = albedo + emissive;
    } else {
        vec3 V = fl_normalize(toEye, Ng);
        float NdotV = max(dot(N, V), 1e-4);
        vec3 F0 = mix(vec3(0.04), albedo, metal);
        vec3 Lo = vec3(0.0);
        int count = int(uParams0.z);
        for (int i = 0; i < 8; ++i) {
            if (i >= count) break;
            Light Lt = uLights[i];
            int type = int(Lt.posType.w);
            vec3 L;
            float att = 1.0;
            float dist = 1.0;
            if (type == 0) {
                L = normalize(-Lt.dirRange.xyz);
                if (Lt.cone.z >= 0.0 && uParams1.x > 0.5) att *= directionalShadow(Ng, Lt.cone.w, Lt.posType.x);
            } else {
                vec3 d = Lt.posType.xyz - vWorldPos;
                dist = length(d);
                L = d / max(dist, 1e-4);
                float f = clamp(1.0 - pow(dist / Lt.dirRange.w, 4.0), 0.0, 1.0);
                att = f * f / (dist * dist + 1.0);
                if (type == 2) {
                    float cd = dot(-L, normalize(Lt.dirRange.xyz));
                    att *= smoothstep(Lt.cone.y, Lt.cone.x, cd);
                    if (Lt.cone.z >= 0.0 && uParams1.x > 0.5) att *= spotShadow(int(Lt.cone.z), Ng, Lt.cone.w, 1.0, dist);
                } else if (Lt.cone.z >= 0.0 && uParams1.x > 0.5) {
                    att *= pointShadow(int(Lt.cone.z), vWorldPos - Lt.posType.xyz, Ng, Lt.cone.w, dist);
                }
            }
            float NdotL = max(dot(N, L), 0.0);
            if (NdotL <= 0.0 || att <= 0.0) continue;
            vec3 H = normalize(V + L);
            float NdotH = max(dot(N, H), 0.0);
            float VdotH = max(dot(V, H), 0.0);
            float a = rough * rough;
            vec3 F = F_Schlick(VdotH, F0);
            vec3 spec = D_GGX(NdotH, a) * G_Smith(NdotV, NdotL, rough) * F / (4.0 * NdotV * NdotL + 1e-4);
            vec3 kD = (1.0 - F) * (1.0 - metal);
            Lo += (kD * albedo / PI + spec) * Lt.colorIntensity.rgb * Lt.colorIntensity.w * att * NdotL;
        }
        vec3 F = F_SchlickR(NdotV, F0, rough);
        vec3 kD = (1.0 - F) * (1.0 - metal);
        vec3 irr = texture(uIrradiance, N).rgb;
        if ((uRtFlags & 8) == 0) irr *= probeGi(vWorldPos);
        vec3 R = reflect(-V, N);
        vec3 pre = textureLod(uPrefiltered, R, rough * uParams1.z).rgb;
        vec2 suv = gl_FragCoord.xy * uScreen.xy;
        if ((uRtFlags & 16) != 0) {  // SSR over the IBL on smooth surfaces
            vec4 ssr = texture(uRtReflection, suv);
            pre = mix(pre, ssr.rgb, ssr.a * (1.0 - smoothstep(0.35, 0.75, rough)));
        }
        if ((uRtFlags & 2) != 0) ao *= texture(uRtAo, suv).r;
        vec2 brdf = texture(uBrdfLut, vec2(NdotV, rough)).rg;
        vec3 ambient = (kD * irr * albedo + pre * (F * brdf.x + brdf.y)) * ao * uParams0.x;
        color = Lo + ambient + emissive;
    }
    color = applyFog(color, vWorldPos);
    if (debugView == 6) color = albedo * cascadeColor(cascadeIndex());
    color = mix(color, vec3(1.0, 0.6, 0.1), uSelected * 0.25);
    return vec4(color, uAlphaMode == 2 ? alpha : 1.0);
}

void main() {
    oColor = fl_clampHdr(shade());  // 1.3.3: never NaN / Inf into the HDR target (identity for finite colours up to 65504)
    oVelocity = vec4(fl_finiteOr0((vCurClip.xy / vCurClip.w - vPrevClip.xy / vPrevClip.w) * 0.5), 0.0, 1.0);
}
)";

// ------------------------------------------------------------- shadows ---
inline const char* kShadowVs = R"(#version 330 core
layout(location=0) in vec3 aPos;
uniform mat4 uModel;
uniform mat4 uLightVP;
void main() { gl_Position = uLightVP * uModel * vec4(aPos, 1.0); }
)";
inline const char* kShadowFs = R"(#version 330 core
void main() {}
)";

// ------------------------------------------------------------ sky/lines ---
inline const char* kSkyVsBody = R"(
out vec2 vNdc;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vNdc = p * 2.0 - 1.0;
    gl_Position = vec4(vNdc, 1.0, 1.0);  // far plane: only fills the background
}
)";
inline const char* kSkyFsBody = R"(
in vec2 vNdc;
layout(location=0) out vec4 oColor;
layout(location=1) out vec4 oVelocity;
uniform samplerCube uEnv;
void main() {
    vec4 p = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(p.xyz / p.w - uCameraPos.xyz);
    oColor = vec4(fl_clampHdr(applyFog(textureLod(uEnv, dir, 0.0).rgb * uParams0.x, uCameraPos.xyz + dir * 2000.0)), 1.0);  // sky ~2 km away
    vec4 far = vec4(uCameraPos.xyz + dir * 1000.0, 1.0);
    vec4 c = uViewProjNoJitter * far, pc = uPrevViewProj * far;
    oVelocity = vec4((c.xy / c.w - pc.xy / pc.w) * 0.5, 0.0, 1.0);
}
)";
inline const char* kLinesVsBody = R"(
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
out vec3 vColor;
void main() { vColor = aColor; gl_Position = uViewProj * vec4(aPos, 1.0); }
)";
inline const char* kLinesFs = R"(#version 330 core
in vec3 vColor;
layout(location=0) out vec4 oColor;
layout(location=1) out vec4 oVelocity;
void main() { oColor = vec4(vColor, 1.0); oVelocity = vec4(0.0, 0.0, 0.0, 1.0); }
)";

// ----------------------------------------------------------- particles ---
// Quads expanded on the CPU (particleQuad); vertex slots reused: normal = colour, uv = corner (-1..1),
// tangent = (opacity, shape 0 soft disc / 1 streak, additive, 0). Outputs alpha 0 to the velocity target so
// blending leaves the motion vectors of what is behind untouched.
inline const char* kParticleVsBody = R"(
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
layout(location=2) in vec2 aCorner;
layout(location=3) in vec4 aParams;
out vec3 vWorldPos;
out vec3 vColor;
out vec2 vCorner;
out vec4 vParams;
void main() {
    vWorldPos = aPos;
    vColor = aColor;
    vCorner = aCorner;
    vParams = aParams;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";
inline const char* kParticleFsBody = R"(
in vec3 vWorldPos;
in vec3 vColor;
in vec2 vCorner;
in vec4 vParams;
layout(location=0) out vec4 oColor;
layout(location=1) out vec4 oVelocity;
void main() {
    float shape = vParams.y > 0.5 ? (1.0 - abs(vCorner.x)) * (1.0 - vCorner.y * vCorner.y) : max(1.0 - dot(vCorner, vCorner), 0.0);
    float a = vParams.x * shape;
    if (a <= 0.002) discard;
    float fog = uFog2.w > 0.5 ? fogAmount(vWorldPos) : 0.0;
    if (vParams.z > 0.5) oColor = vec4(vColor * (a * (1.0 - fog)), 0.0);  // additive: fades out in fog
    else oColor = vec4(mix(vColor, fogColorAlong(vWorldPos), fog), a);
    oColor = fl_clampHdr(oColor);
    oVelocity = vec4(0.0);
}
)";

// ------------------------------------------------------- IBL precompute ---
inline const char* kCubeCommon = R"(#version 330 core
in vec2 vUv;
in vec2 vNdc;
out vec4 oColor;
uniform int uFace;
const float PI = 3.14159265359;
vec3 cubeDir(int face, vec2 p) {
    if (face == 0) return normalize(vec3(1.0, -p.y, -p.x));
    if (face == 1) return normalize(vec3(-1.0, -p.y, p.x));
    if (face == 2) return normalize(vec3(p.x, 1.0, p.y));
    if (face == 3) return normalize(vec3(p.x, -1.0, -p.y));
    if (face == 4) return normalize(vec3(p.x, -p.y, 1.0));
    return normalize(vec3(-p.x, -p.y, -1.0));
}
)";
inline const char* kEquirectToCubeFs = R"(
uniform sampler2D uEquirect;
void main() {
    vec3 d = cubeDir(uFace, vNdc);
    vec2 uv = vec2(atan(d.z, d.x) / (2.0 * PI) + 0.5, acos(clamp(d.y, -1.0, 1.0)) / PI);
    oColor = vec4(min(textureLod(uEquirect, uv, 0.0).rgb, vec3(500.0)), 1.0);
}
)";
inline const char* kProceduralSkyFs = R"(
uniform vec3 uSunDir;  // towards the sun
vec3 sky(vec3 d) {
    float h = d.y;
    vec3 zenith = vec3(0.18, 0.36, 0.85);
    vec3 horizon = vec3(0.75, 0.82, 0.95);
    vec3 ground = vec3(0.22, 0.2, 0.18);
    vec3 c = h > 0.0 ? mix(horizon, zenith, pow(h, 0.45)) : mix(horizon * 0.6, ground, pow(min(-h * 4.0, 1.0), 0.7));
    float sd = max(dot(d, normalize(uSunDir)), 0.0);
    c += vec3(1.0, 0.9, 0.7) * pow(sd, 64.0) * 1.5;   // glow
    c += vec3(40.0, 36.0, 30.0) * smoothstep(0.9995, 0.99975, sd);  // disk
    return c * 0.8;
}
void main() { oColor = vec4(sky(cubeDir(uFace, vNdc)), 1.0); }
)";
inline const char* kIrradianceFs = R"(
uniform samplerCube uEnv;
void main() {
    vec3 N = cubeDir(uFace, vNdc);
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up = cross(N, right);
    vec3 sum = vec3(0.0);
    float n = 0.0;
    const float delta = 0.05;
    for (float phi = 0.0; phi < 2.0 * PI; phi += delta)
        for (float theta = 0.0; theta < 0.5 * PI; theta += delta) {
            vec3 t = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 s = t.x * right + t.y * up + t.z * N;
            sum += textureLod(uEnv, s, 3.0).rgb * cos(theta) * sin(theta);
            n += 1.0;
        }
    oColor = vec4(PI * sum / n, 1.0);
}
)";
inline const char* kPrefilterFs = R"(
uniform samplerCube uEnv;
uniform float uRoughness;
uniform float uEnvSize;
float radicalInverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
vec3 importanceGGX(vec2 xi, vec3 N, float r) {
    float a = r * r;
    float phi = 2.0 * PI * xi.x;
    float cosT = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinT = sqrt(1.0 - cosT * cosT);
    vec3 H = vec3(cos(phi) * sinT, sin(phi) * sinT, cosT);
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tx = normalize(cross(up, N));
    vec3 ty = cross(N, tx);
    return normalize(tx * H.x + ty * H.y + N * H.z);
}
void main() {
    vec3 N = cubeDir(uFace, vNdc);
    vec3 V = N;
    const uint COUNT = 256u;
    vec3 sum = vec3(0.0);
    float w = 0.0;
    for (uint i = 0u; i < COUNT; ++i) {
        vec2 xi = vec2(float(i) / float(COUNT), radicalInverse(i));
        vec3 H = importanceGGX(xi, N, uRoughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
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
            sum += textureLod(uEnv, L, mip).rgb * NdotL;
            w += NdotL;
        }
    }
    oColor = vec4(sum / max(w, 1e-4), 1.0);
}
)";

// --------------------------------------------------------------- post ---
inline const char* kBloomDownFs = R"(#version 330 core
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uSrc;
uniform vec3 uTexelSize;  // source texel size (xy)
#define uTexel (uTexelSize.xy)
uniform int uFirst;
uniform float uThreshold;
)" FLEDGE_GLSL_ROBUST R"(
// 1.3.3: every tap NaN / Inf -> 0 (a bad pixel must not turn into a min(NaN, 200) = 200 halo); identity for finite taps
vec3 s(vec2 o) { return fl_finiteOr0(textureLod(uSrc, vUv + o * uTexel, 0.0).rgb); }
void main() {
    vec3 a = s(vec2(-2, 2)), b = s(vec2(0, 2)), c = s(vec2(2, 2));
    vec3 d = s(vec2(-2, 0)), e = s(vec2(0, 0)), f = s(vec2(2, 0));
    vec3 g = s(vec2(-2, -2)), h = s(vec2(0, -2)), i = s(vec2(2, -2));
    vec3 j = s(vec2(-1, 1)), k = s(vec2(1, 1)), l = s(vec2(-1, -1)), m = s(vec2(1, -1));
    vec3 col = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    if (uFirst == 1) {
        float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));
        col *= max(luma - uThreshold, 0.0) / max(luma, 1e-4);
        col = min(col, vec3(200.0));
    }
    oColor = vec4(col, 1.0);
}
)";
inline const char* kBloomUpFs = R"(#version 330 core
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uSrc;
uniform vec3 uTexelSize;
#define uTexel (uTexelSize.xy)
vec3 s(vec2 o) { return textureLod(uSrc, vUv + o * uTexel, 0.0).rgb; }
void main() {
    vec3 col = s(vec2(0, 0)) * 4.0 + (s(vec2(-1, 0)) + s(vec2(1, 0)) + s(vec2(0, -1)) + s(vec2(0, 1))) * 2.0 +
               (s(vec2(-1, -1)) + s(vec2(1, -1)) + s(vec2(-1, 1)) + s(vec2(1, 1)));
    oColor = vec4(col / 16.0, 1.0);
}
)";
inline const char* kTonemapFs = R"(#version 330 core
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uHdr;
uniform sampler2D uBloom;
uniform sampler2D uExposureTex;  // auto-exposure: adapted log2 luminance (1x1)
uniform int uAutoExposure;
uniform vec4 uAutoEv;            // x min EV, y max EV
uniform float uExposure;
uniform float uBloomIntensity;
uniform int uBloomOn;
uniform int uTonemapper;
uniform int uDebug;
)" FLEDGE_GLSL_ROBUST R"(
vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
vec3 agx(vec3 v) {
    const mat3 inMat = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                            0.0784335999999992, 0.878468636469772, 0.0784336,
                            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outMat = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                             -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                             -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    v = inMat * max(v, vec3(1e-10));
    v = clamp(log2(v), minEv, maxEv);
    v = (v - minEv) / (maxEv - minEv);
    vec3 v2 = v * v, v4 = v2 * v2;
    v = 15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4 - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232;
    v = outMat * v;
    return pow(clamp(v, 0.0, 1.0), vec3(2.2));
}
void main() {
    vec3 c = fl_finiteOr0(texture(uHdr, vUv).rgb);  // 1.3.3: NaN / Inf -> 0 (identity for finite colours)
    if (uDebug == 0) {
        if (uBloomOn == 1) c += fl_finiteOr0(texture(uBloom, vUv).rgb) * uBloomIntensity;
        if (uAutoExposure == 1) c *= exp2(clamp(log2(0.18) - fl_finiteOr0(texelFetch(uExposureTex, ivec2(0), 0).r), uAutoEv.x, uAutoEv.y));  // autoExposureEv()
        c *= uExposure;
        if (uTonemapper == 0) c = aces(c);
        else if (uTonemapper == 1) c = agx(c);
        else c = clamp(c, 0.0, 1.0);
    }
    c = pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
    oColor = vec4(c, dot(c, vec3(0.299, 0.587, 0.114)));  // luma in alpha for FXAA
}
)";
inline const char* kFxaaFs = R"(#version 330 core
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uTex;
uniform vec3 uTexelSize;
#define uTexel (uTexelSize.xy)
uniform int uEnabled;
void main() {
    vec4 m = textureLod(uTex, vUv, 0.0);
    if (uEnabled == 0) { oColor = vec4(m.rgb, 1.0); return; }
    float lNW = textureLod(uTex, vUv + vec2(-1.0, -1.0) * uTexel, 0.0).a;
    float lNE = textureLod(uTex, vUv + vec2(1.0, -1.0) * uTexel, 0.0).a;
    float lSW = textureLod(uTex, vUv + vec2(-1.0, 1.0) * uTexel, 0.0).a;
    float lSE = textureLod(uTex, vUv + vec2(1.0, 1.0) * uTexel, 0.0).a;
    float lM = m.a;
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    if (lMax - lMin < max(0.0312, lMax * 0.125)) { oColor = vec4(m.rgb, 1.0); return; }
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), ((lNW + lSW) - (lNE + lSE)));
    float reduce = max((lNW + lNE + lSW + lSE) * 0.25 * 0.125, 1.0 / 128.0);
    float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpMin, vec2(-8.0), vec2(8.0)) * uTexel;
    vec3 a = 0.5 * (textureLod(uTex, vUv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb + textureLod(uTex, vUv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    vec3 b = a * 0.5 + 0.25 * (textureLod(uTex, vUv + dir * -0.5, 0.0).rgb + textureLod(uTex, vUv + dir * 0.5, 0.0).rgb);
    float lB = dot(b, vec3(0.299, 0.587, 0.114));
    oColor = vec4((lB < lMin || lB > lMax) ? a : b, 1.0);
}
)";

// ------------------------------------------------- basic (dual source) ---
inline const char* kBasicVs = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUv;
uniform mat4 uModel;
uniform mat4 uViewProj;
out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUv;
void main() {
    vec4 wp = uModel * vec4(aPos, 1.0);
    vWorldPos = wp.xyz;
    vNormal = mat3(uModel) * aNormal;
    vUv = aUv;
    gl_Position = uViewProj * wp;
}
)";
inline const char* kBasicFs = R"(#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uAlbedo;
uniform vec3 uColor;
uniform vec3 uAmbient;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
void main() {
    vec3 albedo = uColor * texture(uAlbedo, vUv).rgb;
    float l2 = dot(vNormal, vNormal);  // 1.3.3: zero-length / NaN / Inf normal -> straight up (comparison test: GLSL + SM 3 alike)
    vec3 n = l2 > 1e-30 && l2 < 1e30 ? normalize(vNormal) : vec3(0.0, 1.0, 0.0);
    float ndl = max(dot(n, -normalize(uLightDir)), 0.0);
    vec3 c = albedo * (uAmbient + uLightColor * ndl);
    oColor = vec4(pow(c, vec3(1.0 / 2.2)), 1.0);
}
)";
inline const char* kBasicHlsl = R"(
cbuffer Params : register(b1) {
    float4x4 uModel;
    float4x4 uViewProj;
    float3 uColor; float pad0;
    float3 uAmbient; float pad1;
    float3 uLightDir; float pad2;
    float3 uLightColor; float pad3;
};
Texture2D uAlbedo : register(t0);
SamplerState uSampler : register(s0);
struct VSIn { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct PSIn { float4 pos : SV_Position; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
PSIn VSMain(VSIn i) {
    PSIn o;
    // Matrices are uploaded in GL column-major layout, which is HLSL's default cbuffer
    // packing (column_major), so the column-vector convention mul(M, v) applies.
    float4 wp = mul(uModel, float4(i.pos, 1.0));
    o.pos = mul(uViewProj, wp);
    o.normal = mul(uModel, float4(i.normal, 0.0)).xyz;
    o.uv = i.uv;
    return o;
}
float4 PSMain(PSIn i) : SV_Target {
    float3 albedo = uColor * uAlbedo.Sample(uSampler, i.uv).rgb;
    float l2 = dot(i.normal, i.normal);  // 1.3.3: zero-length / NaN / Inf normal -> straight up
    float3 n = l2 > 1e-30 && l2 < 1e30 ? normalize(i.normal) : float3(0.0, 1.0, 0.0);
    float ndl = max(dot(n, -normalize(uLightDir)), 0.0);
    float3 c = albedo * (uAmbient + uLightColor * ndl);
    return float4(pow(c, 1.0 / 2.2), 1.0);
}
)";

} // namespace fledge::shaders
