#pragma once
// Temporal anti-aliasing / temporal upscaling, the scaled blit, and the ray-tracing helper passes
// (G-buffer prepass and denoiser). GLSL 330 + HLSL twins where both APIs run the pass; the
// ray-tracing passes are HLSL only (DXR exists on Direct3D 12 only).
#include "shaders_robust.h"

namespace fledge::shaders {

// ----------------------------------------------------------------- TAA ---
// Temporal anti-aliasing and the built-in temporal upscaler (render size <= output size):
//   * current frame reconstructed at the output pixel from a 3x3 Gaussian-weighted neighborhood
//     of the jittered render (un-jittered by uJitterUv)
//   * motion vector taken from the closest-depth texel of the 3x3 (dilation)
//   * history reprojected with the motion vector, clipped to the YCoCg neighborhood
//     (mean +- 1.25 sigma), blended with a confidence-scaled weight in a tone-mapped space
inline const char* kTaaFs = R"(#version 330 core
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uCurrent;
uniform sampler2D uHistory;
uniform sampler2D uVelocity;
uniform sampler2D uDepth;
uniform vec4 uRenderSize;  // xy size, zw 1/size
uniform vec4 uOutSize;     // xy size, zw 1/size
uniform vec4 uJitterUv;    // xy: jitter of the current frame in render UV
uniform float uAlpha;      // weight of the current frame (native resolution)
uniform int uHistoryValid;
uniform int uUseDepth;
)" FLEDGE_GLSL_ROBUST R"(
vec3 toY(vec3 c) { return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25))); }
vec3 fromY(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
vec3 tm(vec3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 itm(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }
void main() {
    vec2 src = (vUv + uJitterUv.xy) * uRenderSize.xy - 0.5;
    vec2 nearest = floor(src + 0.5);
    vec3 sum = vec3(0.0), m1 = vec3(0.0), m2 = vec3(0.0);
    float wsum = 0.0, closest = 1e9;
    vec2 closestUv = vUv + uJitterUv.xy;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 t = nearest + vec2(dx, dy);
            vec2 tuv = (t + 0.5) * uRenderSize.zw;
            vec3 c = tm(max(fl_finiteOr0(textureLod(uCurrent, tuv, 0.0).rgb), vec3(0.0)));  // 1.3.3: NaN / Inf -> 0
            vec2 d = t - src;
            float w = exp(-2.29 * dot(d, d));
            sum += c * w;
            wsum += w;
            vec3 y = toY(c);
            m1 += y;
            m2 += y * y;
            if (uUseDepth == 1) {
                float z = textureLod(uDepth, tuv, 0.0).r;
                if (z < closest) { closest = z; closestUv = tuv; }
            }
        }
    vec3 cur = sum / max(wsum, 1e-4);
    vec2 vel = fl_finiteOr0(textureLod(uVelocity, closestUv, 0.0).xy);
    vec2 huv = vUv - vel;
    if (uHistoryValid == 0 || huv.x < 0.0 || huv.y < 0.0 || huv.x > 1.0 || huv.y > 1.0) {
        oColor = vec4(fl_clampHdr(itm(cur)), 1.0);
        return;
    }
    vec3 mean = m1 / 9.0, sigma = sqrt(abs(m2 / 9.0 - mean * mean));
    vec3 h = tm(max(fl_finiteOr0(textureLod(uHistory, huv, 0.0).rgb), vec3(0.0)));  // a poisoned history heals in one frame
    h = fromY(clamp(toY(h), mean - 1.25 * sigma, mean + 1.25 * sigma));
    float coverage = clamp(length(src - nearest) * 1.4, 0.0, 1.0);  // upscaling: far from a render texel -> trust history
    float a = uAlpha * mix(1.0, 0.4, coverage * step(uRenderSize.x + 0.5, uOutSize.x));
    a = mix(a, 0.3, clamp(length(vel * uOutSize.xy) / 40.0, 0.0, 1.0));  // fast motion: favour the new frame
    oColor = vec4(fl_clampHdr(itm(mix(h, cur, a))), 1.0);
}
)";

// --------------------------------------------------------- scaled blit ---
inline const char* kBlitFs = R"(#version 330 core
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uSrc;
uniform vec4 uSrcSize;    // xy size, zw 1/size
uniform float uSharpness; // 0..1 contrast-adaptive sharpening after the bilinear upscale
void main() {
    vec3 c = texture(uSrc, vUv).rgb;
    if (uSharpness > 0.0) {
        vec3 n = texture(uSrc, vUv + vec2(0.0, uSrcSize.w)).rgb, s = texture(uSrc, vUv - vec2(0.0, uSrcSize.w)).rgb;
        vec3 e = texture(uSrc, vUv + vec2(uSrcSize.z, 0.0)).rgb, w = texture(uSrc, vUv - vec2(uSrcSize.z, 0.0)).rgb;
        vec3 mn = min(c, min(min(n, s), min(e, w))), mx = max(c, max(max(n, s), max(e, w)));
        vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, 1e-4), 0.0, 1.0));
        vec3 k = -amp / mix(8.0, 5.0, uSharpness);
        c = clamp((c + (n + s + e + w) * k) / (1.0 + 4.0 * k), 0.0, 1.0);
    }
    oColor = vec4(c, 1.0);
}
)";

// ------------------------------------------------------------- overlay ---
// In-game menus: 2D triangles in pixels; mesh layout reused (pos.xy, normal = rgb, uv.x = alpha).
inline const char* kOverlayVs = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
layout(location=2) in vec2 aAlpha;
uniform vec4 uScreen;
out vec4 vColor;
out vec2 vUv;
out float vBrowser;
void main() { vColor = vec4(aColor, aAlpha.x); vUv = vec2(aPos.z, aAlpha.y); vBrowser = uScreen.z; gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0); }
)";
inline const char* kOverlayFs = R"(#version 330 core
in vec4 vColor;
in vec2 vUv;
in float vBrowser;
out vec4 oColor;
uniform sampler2D uOverlayTex;  // UI font atlas (alpha = coverage), or 1x1 white
void main() { vec4 t = texture(uOverlayTex, vUv); oColor = vec4(mix(vColor.rgb, t.rgb, vBrowser), vColor.a * t.a); }
)";

namespace hlsl {

inline const char* kOverlay = R"(
cbuffer Params : register(b1) { float4 uScreen; };
struct VSIn { float3 pos : POSITION; float3 color : NORMAL; float2 alpha : TEXCOORD0; };
Texture2D uOverlayTex : register(t0); SamplerState s0 : register(s0);
struct PSIn { float4 pos : SV_Position; float4 color : COLOR; float2 uv : TEXCOORD0; float browser : TEXCOORD1; };
PSIn VSMain(VSIn i) { PSIn o; o.color = float4(i.color, i.alpha.x); o.uv = float2(i.pos.z, i.alpha.y); o.browser = uScreen.z; o.pos = float4(i.pos.x / uScreen.x * 2.0 - 1.0, 1.0 - i.pos.y / uScreen.y * 2.0, 0.0, 1.0); return o; }
float4 PSMain(PSIn i) : SV_Target { float4 t = uOverlayTex.Sample(s0, i.uv); return float4(lerp(i.color.rgb, t.rgb, i.browser), i.color.a * t.a); }
)";

inline const char* kTaa = FLEDGE_HLSL_ROBUST R"(
cbuffer Params : register(b1) {
    float4 uRenderSize; float4 uOutSize; float4 uJitterUv;
    float uAlpha; int uHistoryValid; int uUseDepth; float padT;
};
Texture2D uCurrent : register(t0);   SamplerState s0 : register(s0);
Texture2D uHistory : register(t1);   SamplerState s1 : register(s1);
Texture2D uVelocity : register(t2);  SamplerState s2 : register(s2);
Texture2D<float> uDepth : register(t3); SamplerState s3 : register(s3);
float3 toY(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 fromY(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
float3 tm(float3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }
float3 itm(float3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }
float4 PSMain(FsOut i) : SV_Target {
    float2 src = (i.uv + uJitterUv.xy) * uRenderSize.xy - 0.5;
    float2 nearest = floor(src + 0.5);
    float3 sum = 0, m1 = 0, m2 = 0;
    float wsum = 0.0, closest = 1e9;
    float2 closestUv = i.uv + uJitterUv.xy;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
            float2 t = nearest + float2(dx, dy);
            float2 tuv = (t + 0.5) * uRenderSize.zw;
            float3 c = tm(max(fl_finiteOr0(uCurrent.SampleLevel(s0, tuv, 0.0).rgb), 0.0));  // 1.3.3: NaN / Inf -> 0
            float2 d = t - src;
            float w = exp(-2.29 * dot(d, d));
            sum += c * w;
            wsum += w;
            float3 y = toY(c);
            m1 += y;
            m2 += y * y;
            if (uUseDepth == 1) {
                float z = uDepth.SampleLevel(s3, tuv, 0.0);
                if (z < closest) { closest = z; closestUv = tuv; }
            }
        }
    float3 cur = sum / max(wsum, 1e-4);
    float2 vel = fl_finiteOr0(uVelocity.SampleLevel(s2, closestUv, 0.0).xy);
    float2 huv = i.uv - vel;
    if (uHistoryValid == 0 || any(huv < 0.0) || any(huv > 1.0)) return float4(fl_clampHdr(itm(cur)), 1.0);
    float3 mean = m1 / 9.0, sigma = sqrt(abs(m2 / 9.0 - mean * mean));
    float3 h = tm(max(fl_finiteOr0(uHistory.SampleLevel(s1, huv, 0.0).rgb), 0.0));  // a poisoned history heals in one frame
    h = fromY(clamp(toY(h), mean - 1.25 * sigma, mean + 1.25 * sigma));
    float coverage = saturate(length(src - nearest) * 1.4);
    float a = uAlpha * lerp(1.0, 0.4, coverage * step(uRenderSize.x + 0.5, uOutSize.x));
    a = lerp(a, 0.3, saturate(length(vel * uOutSize.xy) / 40.0));
    return float4(fl_clampHdr(itm(lerp(h, cur, a))), 1.0);
}
)";

inline const char* kBlit = R"(
cbuffer Params : register(b1) { float4 uSrcSize; float uSharpness; float3 padB; };
Texture2D uSrc : register(t0); SamplerState s0 : register(s0);
float4 PSMain(FsOut i) : SV_Target {
    float3 c = uSrc.Sample(s0, i.uv).rgb;
    if (uSharpness > 0.0) {
        float3 n = uSrc.Sample(s0, i.uv - float2(0.0, uSrcSize.w)).rgb, s = uSrc.Sample(s0, i.uv + float2(0.0, uSrcSize.w)).rgb;
        float3 e = uSrc.Sample(s0, i.uv + float2(uSrcSize.z, 0.0)).rgb, w = uSrc.Sample(s0, i.uv - float2(uSrcSize.z, 0.0)).rgb;
        float3 mn = min(c, min(min(n, s), min(e, w))), mx = max(c, max(max(n, s), max(e, w)));
        float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
        float3 k = -amp / lerp(8.0, 5.0, uSharpness);
        c = saturate((c + (n + s + e + w) * k) / (1.0 + 4.0 * k));
    }
    return float4(c, 1.0);
}
)";

// ---- ray tracing prepass: world normal + roughness and single-sample depth (Frame + body)
inline const char* kRtGbuffer = R"(
cbuffer Params : register(b1) {
    float4x4 uModel;
    float4 uBaseColor;
    float uNormalScale; float uRoughness; float uAlphaCutoff; int uAlphaMode;
};
Texture2D uBaseColorTex : register(t0); SamplerState s0 : register(s0);
Texture2D uNormalTex : register(t1);    SamplerState s1 : register(s1);
Texture2D uMRTex : register(t2);        SamplerState s2 : register(s2);
struct VSIn { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; float4 tangent : TANGENT; };
struct PSIn { float4 pos : SV_Position; float3 normal : TEXCOORD0; float2 uv : TEXCOORD1; float4 tangent : TEXCOORD2; float3 worldPos : TEXCOORD3; };
PSIn VSMain(VSIn i) {
    PSIn o;
    float4 wp = mul(uModel, float4(i.pos, 1.0));
    o.worldPos = wp.xyz;
    float3x3 mt = transpose((float3x3)uModel);
    float3 c0 = mt[0], c1 = mt[1], c2 = mt[2];
    float det = dot(c0, cross(c1, c2));
    o.normal = (cross(c1, c2) * i.normal.x + cross(c2, c0) * i.normal.y + cross(c0, c1) * i.normal.z) * (det < 0.0 ? -1.0 : 1.0);
    o.tangent = float4(c0 * i.tangent.x + c1 * i.tangent.y + c2 * i.tangent.z, i.tangent.w);
    o.uv = i.uv;
    o.pos = mul(uViewProj, wp);
    return o;
}
float4 PSMain(PSIn i, bool front : SV_IsFrontFace) : SV_Target {
    // 1.3.3: zero-length / NaN normal -> the face normal from derivatives (taken before the discard), towards the camera
    float3 faceN = fl_normalize(cross(ddx(i.worldPos), ddy(i.worldPos)), float3(0.0, 1.0, 0.0));
    if (dot(faceN, uCameraPos.xyz - i.worldPos) < 0.0) faceN = -faceN;
    if (uAlphaMode == 1 && uBaseColor.a * uBaseColorTex.Sample(s0, i.uv).a < uAlphaCutoff) discard;
    float3 Ng = faceN;
    if (fl_usable(i.normal)) {
        Ng = normalize(i.normal);
        if (!front) Ng = -Ng;
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
    float rough = clamp(uRoughness * uMRTex.Sample(s2, i.uv).g, 0.04, 1.0);
    return float4(N, rough);
}
)";

// ---- ray tracing denoiser: 5x5 cross-bilateral (depth plane + normal) + temporal accumulation with
// depth reprojection and neighborhood clamping. One pass per effect (Frame + Fullscreen + body).
inline const char* kRtDenoise = R"(
cbuffer Params : register(b1) { float4 uSize; float uAlpha; int uHistoryValid; float uRadius; float padD; };
Texture2D uNoisy : register(t0);      SamplerState s0 : register(s0);
Texture2D uHistory : register(t1);    SamplerState s1 : register(s1);
Texture2D<float> uDepth : register(t2); SamplerState s2 : register(s2);
Texture2D uNormals : register(t3);    SamplerState s3 : register(s3);
float3 worldAt(int2 p, float d) {
    float2 uv = (float2(p) + 0.5) * uSize.zw;
    float4 w = mul(uInvViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0));
    return w.xyz / w.w;
}
float4 PSMain(FsOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float d0 = uDepth.Load(int3(p, 0));
    float4 c0 = fl_finiteOr0(uNoisy.Load(int3(p, 0)));  // 1.3.3: NaN / Inf -> 0 before the accumulation
    if (d0 >= 1.0) return c0;
    float3 n0 = fl_normalize(uNormals.Load(int3(p, 0)).xyz, float3(0.0, 1.0, 0.0));
    float3 p0 = worldAt(p, d0);
    float scale = max(length(p0 - uCameraPos.xyz) * 0.02, 0.02);
    float4 sum = 0, mn = 1e9, mx = -1e9;
    float wsum = 0;
    [unroll] for (int dy = -2; dy <= 2; ++dy)
        [unroll] for (int dx = -2; dx <= 2; ++dx) {
            int2 q = clamp(p + int2(dx, dy) * (int)uRadius, int2(0, 0), int2(uSize.xy) - 1);
            float d = uDepth.Load(int3(q, 0));
            if (d >= 1.0) continue;
            float4 c = fl_finiteOr0(uNoisy.Load(int3(q, 0)));
            float3 n = uNormals.Load(int3(q, 0)).xyz;
            float plane = abs(dot(worldAt(q, d) - p0, n0)) / scale;
            float w = exp(-0.5 * float(dx * dx + dy * dy) / 2.25) * pow(saturate(dot(n, n0)), 16.0) * exp(-plane * 4.0);
            sum += c * w;
            wsum += w;
            if (abs(dx) <= 1 && abs(dy) <= 1) { mn = min(mn, c); mx = max(mx, c); }
        }
    float4 cur = wsum > 1e-5 ? sum / wsum : c0;
    float4 pc = mul(uPrevViewProj, float4(p0, 1.0));
    float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
    if (uHistoryValid == 0 || any(puv < 0.0) || any(puv > 1.0)) return cur;
    float4 h = fl_finiteOr0(uHistory.SampleLevel(s1, puv, 0.0));
    float4 lo = min(mn, cur), hi = max(mx, cur);
    float4 pad = (hi - lo) * 0.25 + 0.02;
    h = clamp(h, lo - pad, hi + pad);
    return lerp(h, cur, uAlpha);
}
)";

} // namespace hlsl
} // namespace fledge::shaders
