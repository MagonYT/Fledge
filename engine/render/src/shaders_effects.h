#pragma once
// 1.2.0 screen-space and post effects (docs/GRAPHICS.md "Screen-space and post effects"): SSAO + bilateral blur, SSR,
// volumetric sun light, auto-exposure (log-luminance reduction + adaptation), depth of field and motion blur.
// GLSL 330 and HLSL twins written side by side from the same algorithm (the authoring rule of shaders_hlsl.h). Every
// effect is a fullscreen pass: GLSL = "#version 330 core" + kFrameBlock + kCommonGlsl + body (vertex: kFullscreenVs);
// HLSL = kFrame + kFullscreen + kCommonHlsl + body. Positions are reconstructed from a depth texture and compared as
// world-space distances to the camera, so the depth conventions of the APIs (GL [-1, 1], D3D / Vulkan [0, 1]) only
// appear in worldFromDepth() / toUv(). CPU references of the math: render_math.cpp (unit-tested).
namespace fledge::shaders::effects {

// ------------------------------------------------------------------ common ---
inline const char* kCommonGlsl = R"(
vec3 worldFromDepth(vec2 uv, float d) {
    vec4 w = uInvViewProj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return w.xyz / w.w;
}
vec2 toUv(vec4 clip) { return clip.xy / clip.w * 0.5 + 0.5; }
bool outside(vec2 uv) { return uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0; }
float depthAt(sampler2D t, vec2 uv, vec4 size) {
    ivec2 p = clamp(ivec2(uv * size.xy), ivec2(0), ivec2(size.xy) - 1);
    return texelFetch(t, p, 0).r;
}
// normal from depth: on each axis the neighbour with the smaller difference (no smearing across silhouettes)
vec3 normalAt(sampler2D t, vec2 uv, vec4 size, vec3 p) {
    vec2 dx = vec2(size.z, 0.0), dy = vec2(0.0, size.w);
    vec3 l = worldFromDepth(uv - dx, depthAt(t, uv - dx, size)), r = worldFromDepth(uv + dx, depthAt(t, uv + dx, size));
    vec3 d = worldFromDepth(uv - dy, depthAt(t, uv - dy, size)), u = worldFromDepth(uv + dy, depthAt(t, uv + dy, size));
    vec3 hx = length(r - p) < length(p - l) ? r - p : p - l;
    vec3 hy = length(u - p) < length(p - d) ? u - p : p - d;
    vec3 n = fl_normalize(cross(hx, hy), vec3(0.0));  // 1.3.3: degenerate (screen border, flat neighbours) -> 0, callers bail
    return dot(n, uCameraPos.xyz - p) < 0.0 ? -n : n;
}
float ign(vec2 px) { return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))); }  // interleaved gradient noise
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
)";

inline const char* kCommonHlsl = R"(
float3 worldFromDepth(float2 uv, float d) {
    float4 w = mul(uInvViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0));
    return w.xyz / w.w;
}
float2 toUv(float4 clip) { float2 n = clip.xy / clip.w; return float2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5); }
bool outside(float2 uv) { return uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0; }
float depthAt(Texture2D<float> t, float2 uv, float4 size) {
    int2 p = clamp(int2(uv * size.xy), int2(0, 0), int2(size.xy) - 1);
    return t.Load(int3(p, 0));
}
float3 normalAt(Texture2D<float> t, float2 uv, float4 size, float3 p) {
    float2 dx = float2(size.z, 0.0), dy = float2(0.0, size.w);
    float3 l = worldFromDepth(uv - dx, depthAt(t, uv - dx, size)), r = worldFromDepth(uv + dx, depthAt(t, uv + dx, size));
    float3 d = worldFromDepth(uv - dy, depthAt(t, uv - dy, size)), u = worldFromDepth(uv + dy, depthAt(t, uv + dy, size));
    float3 hx = length(r - p) < length(p - l) ? r - p : p - l;
    float3 hy = length(u - p) < length(p - d) ? u - p : p - d;
    float3 n = fl_normalize(cross(hx, hy), (0.0).xxx);  // 1.3.3: degenerate -> 0, callers bail
    return dot(n, uCameraPos.xyz - p) < 0.0 ? -n : n;
}
float ign(float2 px) { return frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715)))); }
float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
)";

// -------------------------------------------------------------------- SSAO ---
// World-space hemisphere around the reconstructed normal (cosine-weighted golden-angle spiral, rotated per pixel by
// interleaved gradient noise), each sample projected to the screen and compared with the depth there; a range check
// keeps far-away foreground from darkening the background. Half resolution, R16F; blurred by kSsaoBlur.
inline const char* kSsaoGlsl = R"(
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uDepth;
uniform vec4 uDepthSize;  // xy size, zw 1/size
uniform vec4 uAo;         // x radius (m), y intensity, z samples, w frame index
void main() {
    float d = depthAt(uDepth, vUv, uDepthSize);
    if (d >= 1.0) { oColor = vec4(1.0); return; }
    vec3 p = worldFromDepth(vUv, d);
    vec3 n = normalAt(uDepth, vUv, uDepthSize, p);
    if (dot(n, n) < 0.5) { oColor = vec4(1.0); return; }  // 1.3.3: no usable normal -> unoccluded
    vec3 t = normalize(cross(abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), n));
    vec3 b = cross(n, t);
    float noise = ign(gl_FragCoord.xy + uAo.w * 5.588238);
    int count = int(uAo.z);
    float camDist = length(p - uCameraPos.xyz);
    float occ = 0.0;
    for (int i = 0; i < 32; ++i) {
        if (i >= count) break;
        float u = (float(i) + 0.5) / float(count);
        float phi = float(i) * 2.39996323 + noise * 6.2831853;
        float sinT = sqrt(u), cosT = sqrt(1.0 - u);
        float s = fract(u + noise * 0.618034);
        float r = uAo.x * mix(0.1, 1.0, s * s);
        vec3 x = p + (t * (cos(phi) * sinT) + b * (sin(phi) * sinT) + n * cosT) * r + n * 0.02;
        vec4 c = uViewProj * vec4(x, 1.0);
        if (c.w <= 1e-4) continue;
        vec2 suv = toUv(c);
        if (outside(suv)) continue;
        vec3 q = worldFromDepth(suv, depthAt(uDepth, suv, uDepthSize));
        float sceneDist = length(q - uCameraPos.xyz), sampleDist = length(x - uCameraPos.xyz);
        float range = smoothstep(0.0, 1.0, uAo.x / max(abs(camDist - sceneDist), 1e-3));
        occ += (sceneDist < sampleDist - (0.02 + 0.004 * camDist) ? 1.0 : 0.0) * range;
    }
    oColor = vec4(clamp(1.0 - occ / float(max(count, 1)) * uAo.y, 0.0, 1.0), 0.0, 0.0, 1.0);
}
)";
inline const char* kSsaoHlsl = R"(
cbuffer Params : register(b1) { float4 uDepthSize; float4 uAo; };
Texture2D<float> uDepth : register(t0); SamplerState s0 : register(s0);
float4 PSMain(FsOut i) : SV_Target {
    float d = depthAt(uDepth, i.uv, uDepthSize);
    if (d >= 1.0) return (1.0).xxxx;
    float3 p = worldFromDepth(i.uv, d);
    float3 n = normalAt(uDepth, i.uv, uDepthSize, p);
    if (dot(n, n) < 0.5) return (1.0).xxxx;  // 1.3.3: no usable normal -> unoccluded
    float3 t = normalize(cross(abs(n.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0), n));
    float3 b = cross(n, t);
    float noise = ign(i.pos.xy + uAo.w * 5.588238);
    int count = int(uAo.z);
    float camDist = length(p - uCameraPos.xyz);
    float occ = 0.0;
    [loop] for (int k = 0; k < 32; ++k) {
        if (k >= count) break;
        float u = (float(k) + 0.5) / float(count);
        float phi = float(k) * 2.39996323 + noise * 6.2831853;
        float sinT = sqrt(u), cosT = sqrt(1.0 - u);
        float s = frac(u + noise * 0.618034);
        float r = uAo.x * lerp(0.1, 1.0, s * s);
        float3 x = p + (t * (cos(phi) * sinT) + b * (sin(phi) * sinT) + n * cosT) * r + n * 0.02;
        float4 c = mul(uViewProj, float4(x, 1.0));
        if (c.w <= 1e-4) continue;
        float2 suv = toUv(c);
        if (outside(suv)) continue;
        float3 q = worldFromDepth(suv, depthAt(uDepth, suv, uDepthSize));
        float sceneDist = length(q - uCameraPos.xyz), sampleDist = length(x - uCameraPos.xyz);
        float range = smoothstep(0.0, 1.0, uAo.x / max(abs(camDist - sceneDist), 1e-3));
        occ += (sceneDist < sampleDist - (0.02 + 0.004 * camDist) ? 1.0 : 0.0) * range;
    }
    return float4(saturate(1.0 - occ / float(max(count, 1)) * uAo.y), 0.0, 0.0, 1.0);
}
)";

// 5x5 depth-aware blur of the AO (weights fall off with the distance difference relative to the pixel's distance).
inline const char* kSsaoBlurGlsl = R"(
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uAoTex;
uniform sampler2D uDepth;
uniform vec4 uDepthSize;
uniform vec4 uAoSize;  // xy size, zw 1/size of the AO texture
void main() {
    float d0 = depthAt(uDepth, vUv, uDepthSize);
    if (d0 >= 1.0) { oColor = vec4(1.0); return; }
    float dist0 = length(worldFromDepth(vUv, d0) - uCameraPos.xyz);
    float sum = 0.0, wsum = 0.0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) {
            vec2 uv = vUv + vec2(float(x), float(y)) * uAoSize.zw;
            float dist = length(worldFromDepth(uv, depthAt(uDepth, uv, uDepthSize)) - uCameraPos.xyz);
            float w = exp(-abs(dist - dist0) / (0.04 * dist0 + 0.02));
            sum += textureLod(uAoTex, uv, 0.0).r * w;
            wsum += w;
        }
    oColor = vec4(sum / max(wsum, 1e-4), 0.0, 0.0, 1.0);
}
)";
inline const char* kSsaoBlurHlsl = R"(
cbuffer Params : register(b1) { float4 uDepthSize; float4 uAoSize; };
Texture2D<float> uAoTex : register(t0); SamplerState s0 : register(s0);
Texture2D<float> uDepth : register(t1); SamplerState s1 : register(s1);
float4 PSMain(FsOut i) : SV_Target {
    float d0 = depthAt(uDepth, i.uv, uDepthSize);
    if (d0 >= 1.0) return (1.0).xxxx;
    float dist0 = length(worldFromDepth(i.uv, d0) - uCameraPos.xyz);
    float sum = 0.0, wsum = 0.0;
    [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x) {
            float2 uv = i.uv + float2(float(x), float(y)) * uAoSize.zw;
            float dist = length(worldFromDepth(uv, depthAt(uDepth, uv, uDepthSize)) - uCameraPos.xyz);
            float w = exp(-abs(dist - dist0) / (0.04 * dist0 + 0.02));
            sum += uAoTex.SampleLevel(s0, uv, 0.0) * w;
            wsum += w;
        }
    return float4(sum / max(wsum, 1e-4), 0.0, 0.0, 1.0);
}
)";

// --------------------------------------------------------------------- SSR ---
// Reflection ray from the reconstructed position / normal, marched in world space with a jittered constant step,
// each step projected and compared with the depth there; a hit within the thickness is refined by bisection and
// shaded with the PREVIOUS frame's HDR colour (reprojected with the previous view-projection). Output: rgb colour,
// a = confidence (fades at the screen edges, at the end of the ray and for rays towards the camera). The PBR shader
// blends it over the prefiltered IBL on smooth surfaces. Half resolution, RGBA16F.
inline const char* kSsrGlsl = R"(
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uDepth;
uniform sampler2D uPrevColor;
uniform vec4 uDepthSize;
uniform vec4 uSsr;   // x max distance (m), y steps, z thickness (m), w intensity
uniform vec4 uSsr2;  // x previous colour valid, y frame index
float behindAt(vec3 x, out vec2 uv) {
    vec4 c = uViewProj * vec4(x, 1.0);
    uv = toUv(c);
    if (c.w <= 1e-4 || outside(uv)) return -1e9;
    float sd = depthAt(uDepth, uv, uDepthSize);
    if (sd >= 1.0) return -1e9;
    return length(x - uCameraPos.xyz) - length(worldFromDepth(uv, sd) - uCameraPos.xyz);
}
void main() {
    oColor = vec4(0.0);
    float d = depthAt(uDepth, vUv, uDepthSize);
    if (d >= 1.0 || uSsr2.x < 0.5) return;
    vec3 p = worldFromDepth(vUv, d);
    vec3 n = normalAt(uDepth, vUv, uDepthSize, p);
    if (dot(n, n) < 0.5) return;  // 1.3.3: no usable normal -> no reflection
    vec3 v = normalize(p - uCameraPos.xyz);
    vec3 r = reflect(v, n);
    float towards = dot(r, -v);
    if (towards > 0.6) return;
    int steps = int(uSsr.y);
    float stepLen = uSsr.x / float(steps);
    vec3 start = p + n * 0.03;
    float t = stepLen * (0.25 + ign(gl_FragCoord.xy + uSsr2.y * 5.588238));
    float prevT = 0.0;
    vec2 uv;
    bool hit = false;
    for (int i = 0; i < 96; ++i) {
        if (i >= steps) break;
        float b = behindAt(start + r * t, uv);
        if (b < -1e8 && outside(uv)) break;
        if (b > 0.0 && b < uSsr.z + stepLen) {
            float lo = prevT, hi = t;
            for (int k = 0; k < 5; ++k) {
                float m = (lo + hi) * 0.5;
                vec2 muv;
                if (behindAt(start + r * m, muv) > 0.0) hi = m; else lo = m;
            }
            t = hi;
            hit = true;
            break;
        }
        prevT = t;
        t += stepLen;
    }
    if (!hit) return;
    vec3 x = start + r * t;
    vec4 pc = uPrevViewProj * vec4(x, 1.0);
    vec2 puv = toUv(pc);
    if (pc.w <= 1e-4 || outside(puv)) return;
    vec3 col = min(fl_finiteOr0(textureLod(uPrevColor, puv, 0.0).rgb), vec3(64.0));  // 1.3.3: min(NaN, 64) would be 64
    float edge = smoothstep(0.0, 0.08, min(min(puv.x, 1.0 - puv.x), min(puv.y, 1.0 - puv.y)));
    float fade = edge * (1.0 - smoothstep(0.7, 1.0, t / uSsr.x)) * (1.0 - smoothstep(0.2, 0.6, towards));
    oColor = vec4(col, fade * uSsr.w);
}
)";
inline const char* kSsrHlsl = R"(
cbuffer Params : register(b1) { float4 uDepthSize; float4 uSsr; float4 uSsr2; };
Texture2D<float> uDepth : register(t0); SamplerState s0 : register(s0);
Texture2D uPrevColor : register(t1);    SamplerState s1 : register(s1);
float behindAt(float3 x, out float2 uv) {
    float4 c = mul(uViewProj, float4(x, 1.0));
    uv = toUv(c);
    if (c.w <= 1e-4 || outside(uv)) return -1e9;
    float sd = depthAt(uDepth, uv, uDepthSize);
    if (sd >= 1.0) return -1e9;
    return length(x - uCameraPos.xyz) - length(worldFromDepth(uv, sd) - uCameraPos.xyz);
}
float4 PSMain(FsOut i) : SV_Target {
    float d = depthAt(uDepth, i.uv, uDepthSize);
    if (d >= 1.0 || uSsr2.x < 0.5) return (0.0).xxxx;
    float3 p = worldFromDepth(i.uv, d);
    float3 n = normalAt(uDepth, i.uv, uDepthSize, p);
    if (dot(n, n) < 0.5) return (0.0).xxxx;  // 1.3.3: no usable normal -> no reflection
    float3 v = normalize(p - uCameraPos.xyz);
    float3 r = reflect(v, n);
    float towards = dot(r, -v);
    if (towards > 0.6) return (0.0).xxxx;
    int steps = int(uSsr.y);
    float stepLen = uSsr.x / float(steps);
    float3 start = p + n * 0.03;
    float t = stepLen * (0.25 + ign(i.pos.xy + uSsr2.y * 5.588238));
    float prevT = 0.0;
    float2 uv;
    bool hit = false;
    [loop] for (int k = 0; k < 96; ++k) {
        if (k >= steps) break;
        float b = behindAt(start + r * t, uv);
        if (b < -1e8 && outside(uv)) break;
        if (b > 0.0 && b < uSsr.z + stepLen) {
            float lo = prevT, hi = t;
            [unroll] for (int j = 0; j < 5; ++j) {
                float m = (lo + hi) * 0.5;
                float2 muv;
                if (behindAt(start + r * m, muv) > 0.0) hi = m; else lo = m;
            }
            t = hi;
            hit = true;
            break;
        }
        prevT = t;
        t += stepLen;
    }
    if (!hit) return (0.0).xxxx;
    float3 x = start + r * t;
    float4 pc = mul(uPrevViewProj, float4(x, 1.0));
    float2 puv = toUv(pc);
    if (pc.w <= 1e-4 || outside(puv)) return (0.0).xxxx;
    float3 col = min(fl_finiteOr0(uPrevColor.SampleLevel(s1, puv, 0.0).rgb), (64.0).xxx);  // 1.3.3: min(NaN, 64) would be 64
    float edge = smoothstep(0.0, 0.08, min(min(puv.x, 1.0 - puv.x), min(puv.y, 1.0 - puv.y)));
    float fade = edge * (1.0 - smoothstep(0.7, 1.0, t / uSsr.x)) * (1.0 - smoothstep(0.2, 0.6, towards));
    return float4(col, fade * uSsr.w);
}
)";

// -------------------------------------------------------------- volumetric ---
// Sun light scattered by a participating medium along the view ray: jittered ray march from the camera to the surface
// (or the max distance), sun visibility from the shadow cascades at every step, Beer-Lambert transmittance,
// Henyey-Greenstein phase (CPU: henyeyGreenstein()), density decaying above the fog height like the height fog. Half
// resolution, added onto the HDR image (before TAA, which smooths the noise).
inline const char* kVolumetricGlsl = R"(
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uDepth;
uniform sampler2DArrayShadow uCascade;
uniform vec4 uDepthSize;
uniform vec4 uVol;   // x density, y intensity, z anisotropy g, w steps
uniform vec4 uVol2;  // x max distance, y frame index, z 1 = cascades exist, w height falloff
uniform vec4 uVol3;  // x reference height
float hg(float c, float g) { float g2 = g * g; float d = 1.0 + g2 - 2.0 * g * c; return (1.0 - g2) / (12.5663706 * d * sqrt(max(d, 1e-6))); }
float sunVisibility(vec3 x) {
    if (uVol2.z < 0.5) return 1.0;
    float viewDepth = -(uView * vec4(x, 1.0)).z;
    int count = int(uParams0.w), c = count;
    for (int i = 0; i < 4; ++i)
        if (i < count && viewDepth < uCascadeSplits[i]) { c = i; break; }
    if (c >= count) return 1.0;
    vec4 clip = uCascadeVP[c] * vec4(x, 1.0);
    vec3 uvz = clip.xyz / clip.w * 0.5 + 0.5;
    if (uvz.x < 0.0 || uvz.y < 0.0 || uvz.x > 1.0 || uvz.y > 1.0 || uvz.z >= 1.0) return 1.0;
    return texture(uCascade, vec4(uvz.xy, float(c), uvz.z - 0.002));
}
void main() {
    if (uLights[0].posType.w != 0.0) { oColor = vec4(0.0); return; }  // directionals come first: light 0 is the sun
    float d = min(depthAt(uDepth, vUv, uDepthSize), 0.999999);
    vec3 p = worldFromDepth(vUv, d);
    vec3 dir = normalize(p - uCameraPos.xyz);
    float dist = min(length(p - uCameraPos.xyz), uVol2.x);
    int steps = int(uVol.w);
    float stepLen = dist / float(steps);
    float noise = ign(gl_FragCoord.xy + uVol2.y * 5.588238);
    vec3 L = normalize(-uLights[0].dirRange.xyz);
    float trans = 1.0, acc = 0.0;
    for (int i = 0; i < 64; ++i) {
        if (i >= steps) break;
        vec3 x = uCameraPos.xyz + dir * ((float(i) + noise) * stepLen);
        float dens = uVol.x * exp(-uVol2.w * max(x.y - uVol3.x, 0.0));
        acc += trans * dens * stepLen * sunVisibility(x);
        trans *= exp(-dens * stepLen);
    }
    vec3 sun = uLights[0].colorIntensity.rgb * uLights[0].colorIntensity.w;
    // alpha blend (src * a + dst * (1 - a)) gives dst * T + in-scatter: rgb is divided by a = 1 - T
    float a = 1.0 - trans;
    oColor = fl_clampHdr(vec4(sun * (acc * hg(dot(dir, L), uVol.z) * uVol.y) / max(a, 1e-4), a));
}
)";
inline const char* kVolumetricHlsl = R"(
cbuffer Params : register(b1) { float4 uDepthSize; float4 uVol; float4 uVol2; float4 uVol3; };
Texture2D<float> uDepth : register(t0);              SamplerState s0 : register(s0);
Texture2DArray<float> uCascade : register(t1);       SamplerComparisonState s1 : register(s1);
float hg(float c, float g) { float g2 = g * g; float d = 1.0 + g2 - 2.0 * g * c; return (1.0 - g2) / (12.5663706 * d * sqrt(max(d, 1e-6))); }
float sunVisibility(float3 x) {
#ifdef FLEDGE_FL10_0
    return 1.0;  // feature level 10_0: no comparison sampling of texture arrays
#endif
    if (uVol2.z < 0.5) return 1.0;
    float viewDepth = -mul(uView, float4(x, 1.0)).z;
    int count = int(uParams0.w), c = count;
    [unroll] for (int k = 0; k < 4; ++k)
        if (c == count && k < count && viewDepth < uCascadeSplits[k]) c = k;
    if (c >= count) return 1.0;
    float4 clip = mul(uCascadeVP[c], float4(x, 1.0));
    float3 nd = clip.xyz / clip.w;
    float3 uvz = float3(nd.x * 0.5 + 0.5, 0.5 - nd.y * 0.5, nd.z);
    if (uvz.x < 0.0 || uvz.y < 0.0 || uvz.x > 1.0 || uvz.y > 1.0 || uvz.z >= 1.0) return 1.0;
    return uCascade.SampleCmpLevelZero(s1, float3(uvz.xy, float(c)), uvz.z - 0.002);
}
float4 PSMain(FsOut i) : SV_Target {
    if (uLights[0].posType.w != 0.0) return (0.0).xxxx;
    float d = min(depthAt(uDepth, i.uv, uDepthSize), 0.999999);
    float3 p = worldFromDepth(i.uv, d);
    float3 dir = normalize(p - uCameraPos.xyz);
    float dist = min(length(p - uCameraPos.xyz), uVol2.x);
    int steps = int(uVol.w);
    float stepLen = dist / float(steps);
    float noise = ign(i.pos.xy + uVol2.y * 5.588238);
    float3 L = normalize(-uLights[0].dirRange.xyz);
    float trans = 1.0, acc = 0.0;
    [loop] for (int k = 0; k < 64; ++k) {
        if (k >= steps) break;
        float3 x = uCameraPos.xyz + dir * ((float(k) + noise) * stepLen);
        float dens = uVol.x * exp(-uVol2.w * max(x.y - uVol3.x, 0.0));
        acc += trans * dens * stepLen * sunVisibility(x);
        trans *= exp(-dens * stepLen);
    }
    float3 sun = uLights[0].colorIntensity.rgb * uLights[0].colorIntensity.w;
    float a = 1.0 - trans;
    return fl_clampHdr(float4(sun * (acc * hg(dot(dir, L), uVol.z) * uVol.y) / max(a, 1e-4), a));
}
)";

// Bilinear upsample of an effect texture with its alpha (volumetric.apply: alpha blending onto the HDR image).
inline const char* kApplyGlsl = R"(
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uSrc;
void main() { oColor = textureLod(uSrc, vUv, 0.0); }
)";
inline const char* kApplyHlsl = R"(
Texture2D uSrc : register(t0); SamplerState s0 : register(s0);
float4 PSMain(FsOut i) : SV_Target { return uSrc.SampleLevel(s0, i.uv, 0.0); }
)";

// ---------------------------------------------------------- auto-exposure ---
// 1) log2 luminance of the HDR image into 64x64 (4 bilinear taps per texel), 2) two 8x8 box reductions (64 -> 8 -> 1),
// 3) adaptation of the 1x1 average towards the new value (CPU: adaptLuminance()). The tonemapper applies
// autoExposureEv() of the adapted value.
inline const char* kLumaDownGlsl = R"(
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uSrc;
uniform vec4 uDstTexel;  // xy 1/destination size
void main() {
    float s = 0.0;
    for (int k = 0; k < 4; ++k) {
        vec2 o = vec2(k == 1 || k == 3 ? 0.25 : -0.25, k >= 2 ? 0.25 : -0.25) * uDstTexel.xy;
        s += log2(max(luma(fl_finiteOr0(texture(uSrc, vUv + o).rgb)), 1e-5));  // 1.3.3: one NaN pixel must not poison the average
    }
    oColor = vec4(s * 0.25, 0.0, 0.0, 1.0);
}
)";
inline const char* kLumaDownHlsl = R"(
cbuffer Params : register(b1) { float4 uDstTexel; };
Texture2D uSrc : register(t0); SamplerState s0 : register(s0);
float4 PSMain(FsOut i) : SV_Target {
    float s = 0.0;
    [unroll] for (int k = 0; k < 4; ++k) {
        float2 o = float2(k == 1 || k == 3 ? 0.25 : -0.25, k >= 2 ? 0.25 : -0.25) * uDstTexel.xy;
        s += log2(max(luma(fl_finiteOr0(uSrc.SampleLevel(s0, i.uv + o, 0.0).rgb)), 1e-5));  // 1.3.3: NaN / Inf -> 0
    }
    return float4(s * 0.25, 0.0, 0.0, 1.0);
}
)";
inline const char* kLumaReduceGlsl = R"(
out vec4 oColor;
uniform sampler2D uSrc;
void main() {
    ivec2 base = ivec2(gl_FragCoord.xy) * 8;
    ivec2 size = textureSize(uSrc, 0);
    float s = 0.0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) s += fl_finiteOr0(texelFetch(uSrc, min(base + ivec2(x, y), size - 1), 0).r);
    oColor = vec4(s / 64.0, 0.0, 0.0, 1.0);
}
)";
inline const char* kLumaReduceHlsl = R"(
Texture2D<float> uSrc : register(t0); SamplerState s0 : register(s0);
float4 PSMain(FsOut i) : SV_Target {
    int2 base = int2(i.pos.xy) * 8;
    uint w, h;
    uSrc.GetDimensions(w, h);
    float s = 0.0;
    [unroll] for (int y = 0; y < 8; ++y)
        [unroll] for (int x = 0; x < 8; ++x) s += fl_finiteOr0(uSrc.Load(int3(min(base + int2(x, y), int2(w, h) - 1), 0)));
    return float4(s / 64.0, 0.0, 0.0, 1.0);
}
)";
inline const char* kLumaAdaptGlsl = R"(
out vec4 oColor;
uniform sampler2D uCurrent;
uniform sampler2D uPrevious;
uniform vec4 uAdapt;  // x dt (s), y speed (1/s), z previous valid
void main() {
    float cur = texelFetch(uCurrent, ivec2(0), 0).r;
    float prev = texelFetch(uPrevious, ivec2(0), 0).r;
    // 1.3.3: a non-finite value never enters the adapted exposure (it is a 1-texel history: it would stick)
    if (!fl_finite(prev)) prev = fl_finite(cur) ? cur : 0.0;
    if (!fl_finite(cur)) cur = prev;
    float k = 1.0 - exp(-max(uAdapt.x, 0.0) * uAdapt.y);
    oColor = vec4(uAdapt.z > 0.5 ? prev + (cur - prev) * k : cur, 0.0, 0.0, 1.0);
}
)";
inline const char* kLumaAdaptHlsl = R"(
cbuffer Params : register(b1) { float4 uAdapt; };
Texture2D<float> uCurrent : register(t0);  SamplerState s0 : register(s0);
Texture2D<float> uPrevious : register(t1); SamplerState s1 : register(s1);
float4 PSMain(FsOut i) : SV_Target {
    float cur = uCurrent.Load(int3(0, 0, 0));
    float prev = uPrevious.Load(int3(0, 0, 0));
    if (!fl_finite(prev)) prev = fl_finite(cur) ? cur : 0.0;  // 1.3.3: never a non-finite adapted exposure
    if (!fl_finite(cur)) cur = prev;
    float k = 1.0 - exp(-max(uAdapt.x, 0.0) * uAdapt.y);
    return float4(uAdapt.z > 0.5 ? prev + (cur - prev) * k : cur, 0.0, 0.0, 1.0);
}
)";

// ---------------------------------------------------------- depth of field ---
// Gather bokeh: 32 golden-angle taps over a disk of the largest radius; a tap counts when its own circle of confusion
// (CPU: dofCircleOfConfusion()) reaches this pixel, and taps behind the pixel never blur more than the pixel itself
// (no background bleeding over a sharp foreground).
inline const char* kDofGlsl = R"(
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uSrc;
uniform sampler2D uDepth;
uniform vec4 uDepthSize;
uniform vec4 uSrcSize;  // xy size, zw 1/size
uniform vec4 uDof;      // x focus distance, y focus range, z max radius (px), w unused
vec2 cocAt(vec2 uv) {   // x coc (px), y distance
    float d = depthAt(uDepth, uv, uDepthSize);
    float dist = d >= 1.0 ? 1e5 : length(worldFromDepth(uv, d) - uCameraPos.xyz);
    float off = abs(dist - uDof.x) - uDof.y * 0.5;
    return vec2(off <= 0.0 ? 0.0 : uDof.z * clamp(off / max(uDof.x, 0.1), 0.0, 1.0), dist);
}
void main() {
    vec2 c0 = cocAt(vUv);
    vec3 sum = fl_finiteOr0(texture(uSrc, vUv).rgb);  // 1.3.3: NaN / Inf taps -> 0
    float wsum = 1.0;
    for (int i = 0; i < 32; ++i) {
        float u = (float(i) + 0.5) / 32.0;
        float rho = sqrt(u) * uDof.z;
        float a = float(i) * 2.39996323;
        vec2 uv = vUv + vec2(cos(a), sin(a)) * rho * uSrcSize.zw;
        vec2 ci = cocAt(uv);
        float coc = ci.y > c0.y ? min(ci.x, c0.x) : ci.x;
        float w = clamp(coc - rho + 1.0, 0.0, 1.0);
        sum += fl_finiteOr0(textureLod(uSrc, uv, 0.0).rgb) * w;
        wsum += w;
    }
    oColor = vec4(sum / wsum, 1.0);
}
)";
inline const char* kDofHlsl = R"(
cbuffer Params : register(b1) { float4 uDepthSize; float4 uSrcSize; float4 uDof; };
Texture2D uSrc : register(t0);          SamplerState s0 : register(s0);
Texture2D<float> uDepth : register(t1); SamplerState s1 : register(s1);
float2 cocAt(float2 uv) {
    float d = depthAt(uDepth, uv, uDepthSize);
    float dist = d >= 1.0 ? 1e5 : length(worldFromDepth(uv, d) - uCameraPos.xyz);
    float off = abs(dist - uDof.x) - uDof.y * 0.5;
    return float2(off <= 0.0 ? 0.0 : uDof.z * saturate(off / max(uDof.x, 0.1)), dist);
}
float4 PSMain(FsOut i) : SV_Target {
    float2 c0 = cocAt(i.uv);
    float3 sum = fl_finiteOr0(uSrc.SampleLevel(s0, i.uv, 0.0).rgb);  // 1.3.3: NaN / Inf taps -> 0
    float wsum = 1.0;
    [loop] for (int k = 0; k < 32; ++k) {
        float u = (float(k) + 0.5) / 32.0;
        float rho = sqrt(u) * uDof.z;
        float a = float(k) * 2.39996323;
        float2 uv = i.uv + float2(cos(a), sin(a)) * rho * uSrcSize.zw;
        float2 ci = cocAt(uv);
        float coc = ci.y > c0.y ? min(ci.x, c0.x) : ci.x;
        float w = saturate(coc - rho + 1.0);
        sum += fl_finiteOr0(uSrc.SampleLevel(s0, uv, 0.0).rgb) * w;
        wsum += w;
    }
    return float4(sum / wsum, 1.0);
}
)";

// ------------------------------------------------------------- motion blur ---
// Samples along the pixel's motion vector (camera and object motion, the TAA velocity buffer), centred, jittered,
// length clamped; pixels that moved less than half a pixel are copied.
inline const char* kMotionBlurGlsl = R"(
in vec2 vUv;
out vec4 oColor;
uniform sampler2D uSrc;
uniform sampler2D uVelocity;
uniform vec4 uSrcSize;
uniform vec4 uMb;  // x strength, y samples, z max length (uv)
void main() {
    vec2 v = fl_finiteOr0(texture(uVelocity, vUv).xy) * uMb.x;  // 1.3.3: NaN / Inf velocity / colour -> 0
    float len = length(v);
    if (len > uMb.z) v *= uMb.z / len;
    if (len * uSrcSize.x < 0.5) { oColor = vec4(fl_finiteOr0(texture(uSrc, vUv).rgb), 1.0); return; }
    int n = int(uMb.y);
    float noise = ign(gl_FragCoord.xy);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 16; ++i) {
        if (i >= n) break;
        sum += fl_finiteOr0(textureLod(uSrc, vUv + v * ((float(i) + noise) / float(n) - 0.5), 0.0).rgb);
    }
    oColor = vec4(sum / float(n), 1.0);
}
)";
inline const char* kMotionBlurHlsl = R"(
cbuffer Params : register(b1) { float4 uSrcSize; float4 uMb; };
Texture2D uSrc : register(t0);      SamplerState s0 : register(s0);
Texture2D uVelocity : register(t1); SamplerState s1 : register(s1);
float4 PSMain(FsOut i) : SV_Target {
    float2 v = fl_finiteOr0(uVelocity.SampleLevel(s1, i.uv, 0.0).xy) * uMb.x;  // 1.3.3: NaN / Inf velocity / colour -> 0
    float len = length(v);
    if (len > uMb.z) v *= uMb.z / len;
    if (len * uSrcSize.x < 0.5) return float4(fl_finiteOr0(uSrc.SampleLevel(s0, i.uv, 0.0).rgb), 1.0);
    int n = int(uMb.y);
    float noise = ign(i.pos.xy);
    float3 sum = (0.0).xxx;
    [loop] for (int k = 0; k < 16; ++k) {
        if (k >= n) break;
        sum += fl_finiteOr0(uSrc.SampleLevel(s0, i.uv + v * ((float(k) + noise) / float(n) - 0.5), 0.0).rgb);
    }
    return float4(sum / float(n), 1.0);
}
)";

} // namespace fledge::shaders::effects
