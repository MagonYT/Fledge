// CPU-side render math shared with (and mirrored by) the shaders.
#include <algorithm>
#include <cmath>

#include "fledge/render/renderer.h"

namespace fledge {

const TypeInfo& RenderSettings::typeInfo() {
    static const TypeInfo info = TypeBuilder<RenderSettings>("RenderSettings", "Per-project rendering defaults")
                                     .enumeration("tonemapper", &RenderSettings::tonemapper, {"ACES", "AgX", "Linear"})
                                     .field("exposure", &RenderSettings::exposure, "exposure compensation (EV)", -6, 6)
                                     .field("bloom", &RenderSettings::bloom)
                                     .field("bloomIntensity", &RenderSettings::bloomIntensity, "", 0, 1)
                                     .field("bloomThreshold", &RenderSettings::bloomThreshold, "HDR luminance threshold", 0, 10)
                                     .field("fxaa", &RenderSettings::fxaa, "fast approximate anti-aliasing")
                                     .field("msaa", &RenderSettings::msaa, "MSAA samples: 1, 2 or 4")
                                     .field("shadows", &RenderSettings::shadows)
                                     .field("cascadeCount", &RenderSettings::cascadeCount, "directional shadow cascades (2-4)")
                                     .field("shadowResolution", &RenderSettings::shadowResolution, "shadow map size (512-4096)")
                                     .field("shadowDistance", &RenderSettings::shadowDistance, "meters covered by cascades", 5, 500)
                                     .field("environmentIntensity", &RenderSettings::environmentIntensity, "IBL + sky scale", 0, 4)
                                     .field("showSky", &RenderSettings::showSky)
                                     .assetRef("environment", &RenderSettings::environment, ".hdr environment texture (empty = procedural sky)")
                                     .enumeration("debugView", &RenderSettings::debugView,
                                                  {"None", "Albedo", "Normal", "Roughness", "Metallic", "Depth", "ShadowCascades", "Wireframe"})
                                     .field("fog", &RenderSettings::fog, "exponential height fog")
                                     .field("fogDensity", &RenderSettings::fogDensity, "extinction per meter at fogHeight", 0, 1)
                                     .field("fogHeight", &RenderSettings::fogHeight, "world height of fogDensity (m)", -1000, 10000)
                                     .field("fogHeightFalloff", &RenderSettings::fogHeightFalloff, "1/m, 0 = uniform", 0, 2)
                                     .field("fogStart", &RenderSettings::fogStart, "clear distance from the camera (m)", 0, 5000)
                                     .field("fogMaxOpacity", &RenderSettings::fogMaxOpacity, "", 0, 1)
                                     .color("fogColor", &RenderSettings::fogColor, "linear in-scattered colour")
                                     .field("fogSunScatter", &RenderSettings::fogSunScatter, "glow towards the sun", 0, 2)
                                     .field("pointShadows", &RenderSettings::pointShadows, "6-face shadow maps for up to 2 point lights with castShadows")
                                     .field("ssao", &RenderSettings::ssao, "screen-space ambient occlusion")
                                     .field("ssaoRadius", &RenderSettings::ssaoRadius, "occlusion radius (m)", 0.05f, 4)
                                     .field("ssaoIntensity", &RenderSettings::ssaoIntensity, "", 0, 2)
                                     .field("ssaoSamples", &RenderSettings::ssaoSamples, "samples per pixel (4-32)")
                                     .field("ssr", &RenderSettings::ssr, "screen-space reflections")
                                     .field("ssrMaxDistance", &RenderSettings::ssrMaxDistance, "ray length (m)", 1, 200)
                                     .field("ssrSteps", &RenderSettings::ssrSteps, "ray-march steps (8-96)")
                                     .field("ssrThickness", &RenderSettings::ssrThickness, "hit depth tolerance (m)", 0.01f, 4)
                                     .field("ssrIntensity", &RenderSettings::ssrIntensity, "", 0, 1)
                                     .field("wetness", &RenderSettings::wetness, "wet surfaces: 0 dry .. 1 soaked (rain drives it while playing)", 0, 1)
                                     .field("puddles", &RenderSettings::puddles, "puddle coverage of flat ground when wet", 0, 1)
                                     .field("volumetric", &RenderSettings::volumetric, "volumetric sun light (light shafts)")
                                     .field("volumetricDensity", &RenderSettings::volumetricDensity, "scattering per meter", 0, 0.5f)
                                     .field("volumetricIntensity", &RenderSettings::volumetricIntensity, "", 0, 4)
                                     .field("volumetricAnisotropy", &RenderSettings::volumetricAnisotropy, "forward scattering (Henyey-Greenstein g)", -0.9f, 0.9f)
                                     .field("volumetricSteps", &RenderSettings::volumetricSteps, "ray-march steps (8-64)")
                                     .field("volumetricDistance", &RenderSettings::volumetricDistance, "march distance (m)", 5, 500)
                                     .field("autoExposure", &RenderSettings::autoExposure, "eye adaptation; exposure becomes compensation")
                                     .field("autoExposureMinEv", &RenderSettings::autoExposureMinEv, "", -12, 12)
                                     .field("autoExposureMaxEv", &RenderSettings::autoExposureMaxEv, "", -12, 12)
                                     .field("autoExposureSpeed", &RenderSettings::autoExposureSpeed, "adaptation rate (1/s)", 0.05f, 20)
                                     .field("dof", &RenderSettings::dof, "depth of field")
                                     .field("dofFocusDistance", &RenderSettings::dofFocusDistance, "focus distance (m)", 0.1f, 1000)
                                     .field("dofFocusRange", &RenderSettings::dofFocusRange, "sharp band (m)", 0, 500)
                                     .field("dofMaxRadius", &RenderSettings::dofMaxRadius, "max blur (px at 1080p)", 0, 24)
                                     .field("motionBlur", &RenderSettings::motionBlur, "camera and object motion blur")
                                     .field("motionBlurStrength", &RenderSettings::motionBlurStrength, "shutter fraction", 0, 1)
                                     .field("motionBlurSamples", &RenderSettings::motionBlurSamples, "samples (4-16)")
                                     .field("giProbes", &RenderSettings::giProbes, "baked irradiance volume for static world lighting")
                                     .build();
    return info;
}

void RenderSettings::sanitize() {
    msaa = msaa >= 4 ? 4 : (msaa >= 2 ? 2 : 1);
    cascadeCount = std::clamp(cascadeCount, 2, 4);
    shadowResolution = std::clamp(shadowResolution, 512, 4096);
    shadowDistance = std::max(shadowDistance, 1.0f);
    fogDensity = std::max(fogDensity, 0.0f);
    fogHeightFalloff = std::max(fogHeightFalloff, 0.0f);
    fogStart = std::max(fogStart, 0.0f);
    fogMaxOpacity = std::clamp(fogMaxOpacity, 0.0f, 1.0f);
    ssaoRadius = std::clamp(ssaoRadius, 0.05f, 4.0f);
    ssaoIntensity = std::clamp(ssaoIntensity, 0.0f, 2.0f);
    ssaoSamples = std::clamp(ssaoSamples, 4, 32);
    ssrMaxDistance = std::clamp(ssrMaxDistance, 1.0f, 200.0f);
    ssrSteps = std::clamp(ssrSteps, 8, 96);
    ssrThickness = std::clamp(ssrThickness, 0.01f, 4.0f);
    ssrIntensity = std::clamp(ssrIntensity, 0.0f, 1.0f);
    wetness = std::clamp(wetness, 0.0f, 1.0f);
    puddles = std::clamp(puddles, 0.0f, 1.0f);
    volumetricDensity = std::clamp(volumetricDensity, 0.0f, 0.5f);
    volumetricIntensity = std::clamp(volumetricIntensity, 0.0f, 4.0f);
    volumetricAnisotropy = std::clamp(volumetricAnisotropy, -0.9f, 0.9f);
    volumetricSteps = std::clamp(volumetricSteps, 8, 64);
    volumetricDistance = std::clamp(volumetricDistance, 5.0f, 500.0f);
    autoExposureMinEv = std::clamp(autoExposureMinEv, -12.0f, 12.0f);
    autoExposureMaxEv = std::clamp(autoExposureMaxEv, autoExposureMinEv, 12.0f);
    autoExposureSpeed = std::clamp(autoExposureSpeed, 0.05f, 20.0f);
    dofFocusDistance = std::clamp(dofFocusDistance, 0.1f, 1000.0f);
    dofFocusRange = std::clamp(dofFocusRange, 0.0f, 500.0f);
    dofMaxRadius = std::clamp(dofMaxRadius, 0.0f, 24.0f);
    motionBlurStrength = std::clamp(motionBlurStrength, 0.0f, 1.0f);
    motionBlurSamples = std::clamp(motionBlurSamples, 4, 16);
}

// ---- 1.2.0 effects: CPU references of the shader math (shaders_effects.h, shaders.h / shaders_hlsl.h) ----
float autoExposureEv(float avgLog2Luma, float minEv, float maxEv) {
    // mid grey: the exposure that maps the scene's geometric mean luminance to 0.18
    return std::clamp(std::log2(0.18f) - avgLog2Luma, minEv, std::max(minEv, maxEv));
}

float adaptLuminance(float prevLog2, float currentLog2, float dt, float speed) {
    float k = 1.0f - std::exp(-std::max(dt, 0.0f) * std::max(speed, 0.0f));
    return prevLog2 + (currentLog2 - prevLog2) * k;
}

float dofCircleOfConfusion(float distance, float focusDistance, float focusRange, float maxRadius) {
    float off = std::fabs(distance - focusDistance) - focusRange * 0.5f;
    if (off <= 0) return 0;
    return maxRadius * std::clamp(off / std::max(focusDistance, 0.1f), 0.0f, 1.0f);
}

float henyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    float d = 1.0f + g2 - 2.0f * g * cosTheta;
    return (1.0f - g2) / (4.0f * 3.14159265f * d * std::sqrt(std::max(d, 1e-6f)));
}

int pointShadowFace(Vec3 v) {
    Vec3 a{std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)};
    if (a.x >= a.y && a.x >= a.z) return v.x > 0 ? 0 : 1;
    if (a.y >= a.z) return v.y > 0 ? 2 : 3;
    return v.z > 0 ? 4 : 5;
}

Mat4 pointShadowViewProj(Vec3 lightPos, int face, float range) {
    static const Vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const Vec3 ups[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    int f = std::clamp(face, 0, 5);
    // 90 degrees plus a small margin so the PCF taps near a face edge stay inside that face's map
    return Mat4::perspective(radians(96.0f), 1.0f, 0.05f, std::max(range, 0.1f)) * Mat4::lookAt(lightPos, lightPos + dirs[f], ups[f]);
}

WetSurface wetSurface(Vec3 albedo, float roughness, float metallic, float normalY, float wetness, float puddles, float puddleMask) {
    // exposure to rain: flat ground fully, walls partly, overhangs not at all (mirrors applyWetness() in the shaders)
    float exposure = normalY < -0.3f ? 0.0f : 0.4f + 0.6f * std::clamp(normalY, 0.0f, 1.0f);
    float wet = std::clamp(wetness, 0.0f, 1.0f) * exposure;
    WetSurface w;
    w.albedo = albedo * (1.0f - 0.4f * wet * (1.0f - std::clamp(metallic, 0.0f, 1.0f)));
    w.roughness = roughness + (0.1f - roughness) * wet * 0.85f;
    float flat = std::clamp((normalY - 0.9f) / 0.08f, 0.0f, 1.0f);
    float threshold = 1.0f - std::clamp(puddles, 0.0f, 1.0f) * std::clamp(wetness, 0.0f, 1.0f);
    float t = std::clamp((puddleMask - threshold) / 0.08f, 0.0f, 1.0f);
    w.puddle = flat * t * t * (3.0f - 2.0f * t);
    w.roughness = w.roughness + (0.03f - w.roughness) * w.puddle;
    w.albedo = w.albedo * (1.0f - 0.15f * w.puddle);
    return w;
}

float wetnessStep(float wetness, float rain, float dt) {
    float target = std::clamp(rain * 1.5f, 0.0f, 1.0f);
    if (target > wetness) return std::min(target, wetness + dt * 0.08f);
    return std::max(target, wetness - dt * 0.01f);
}

// Mirrors applyFog() in shaders.h / shaders_hlsl.h. Density at height y is fogDensity * exp(-k (y - fogHeight)); the
// optical depth along the ray is the closed-form integral of that over the ray (scaled to the part beyond fogStart).
float fogAmount(const RenderSettings& s, Vec3 camera, Vec3 p) {
    if (!s.fog || s.fogDensity <= 0) return 0;
    Vec3 d = p - camera;
    float dist = length(d);
    float t = std::max(dist - s.fogStart, 0.0f);
    if (t <= 0) return 0;
    float k = std::max(s.fogHeightFalloff, 0.0f);
    float atCamera = std::exp(std::clamp(-k * (camera.y - s.fogHeight), -80.0f, 80.0f));
    float kd = k * d.y;
    float f = std::fabs(kd) > 1e-4f ? (1.0f - std::exp(std::clamp(-kd, -80.0f, 80.0f))) / kd : 1.0f;
    float depth = s.fogDensity * atCamera * f * t;
    return std::min(1.0f - std::exp(-depth), std::clamp(s.fogMaxOpacity, 0.0f, 1.0f));
}

Vec3 fogInscatter(const RenderSettings& s, Vec3 viewDir, Vec3 toSun, Vec3 sunColor) {
    float c = std::max(dot(normalize(viewDir), normalize(toSun)), 0.0f);
    float c2 = c * c, c4 = c2 * c2;
    return s.fogColor + sunColor * (s.fogSunScatter * 0.1f * c4 * c4);  // pow(c, 8): a soft glow around the sun
}

std::array<Vec3, 4> particleQuad(const ParticleItem& p, Vec3 cameraPos, Vec3 cameraRight, Vec3 cameraUp) {
    Vec3 u = cameraRight, v = cameraUp;
    float halfW = p.size * 0.5f, halfL = p.size * 0.5f;
    float speed = length(p.velocity);
    if (p.stretch > 0 && speed > 1e-4f) {  // velocity-aligned: long axis along the motion, facing the camera
        Vec3 dir = p.velocity * (1.0f / speed);
        Vec3 side = cross(dir, cameraPos - p.position);
        if (length(side) > 1e-5f) {
            u = normalize(side);
            v = dir;
            halfL = (p.size + speed * p.stretch) * 0.5f;
        }
    }
    return {p.position - u * halfW - v * halfL, p.position + u * halfW - v * halfL, p.position + u * halfW + v * halfL,
            p.position - u * halfW + v * halfL};
}

Vec3 tonemapACES(Vec3 x) {
    Vec3 r;
    for (int i = 0; i < 3; ++i) {
        float v = x[i];
        r[i] = std::clamp((v * (2.51f * v + 0.03f)) / (v * (2.43f * v + 0.59f) + 0.14f), 0.0f, 1.0f);
    }
    return r;
}

Vec3 tonemapAgX(Vec3 c) {
    // Same constants as the shader (column-major mat3 -> rows written as GLSL columns).
    const float in[3][3] = {{0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f},
                            {0.0784335999999992f, 0.878468636469772f, 0.0784336f},
                            {0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f}};
    const float out[3][3] = {{1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f},
                             {-0.0980208811401368f, 1.15190312990417f, -0.0980434501171241f},
                             {-0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f}};
    auto mul = [](const float m[3][3], Vec3 v) {  // GLSL mat3(cols) * v
        return Vec3{m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z, m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z,
                    m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z};
    };
    const float minEv = -12.47393f, maxEv = 4.026069f;
    Vec3 v = mul(in, Vec3{std::max(c.x, 1e-10f), std::max(c.y, 1e-10f), std::max(c.z, 1e-10f)});
    for (int i = 0; i < 3; ++i) {
        float x = std::clamp(std::log2(std::max(v[i], 1e-10f)), minEv, maxEv);
        x = (x - minEv) / (maxEv - minEv);
        float x2 = x * x, x4 = x2 * x2;
        v[i] = 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
    }
    v = mul(out, v);
    for (int i = 0; i < 3; ++i) v[i] = std::pow(std::clamp(v[i], 0.0f, 1.0f), 2.2f);
    return v;
}

std::vector<Cascade> computeCascades(const Mat4& view, float fovY, float aspect, float nearPlane, float shadowDistance, Vec3 lightDir,
                                     int count, int resolution, float lambda) {
    count = std::clamp(count, 1, 4);
    std::vector<Cascade> out(static_cast<size_t>(count));
    Mat4 invView = view.inverse();
    float n = std::max(nearPlane, 0.01f), f = std::max(shadowDistance, n + 0.1f);
    float tanY = std::tan(fovY * 0.5f), tanX = tanY * aspect;
    Vec3 L = normalize(lightDir);
    Vec3 up = std::fabs(L.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    Mat4 lightRot = Mat4::lookAt({0, 0, 0}, L, up);  // light-space basis (stable across frames)
    float prev = n;
    for (int i = 0; i < count; ++i) {
        float t = static_cast<float>(i + 1) / static_cast<float>(count);
        float uniform = n + (f - n) * t;
        float logSplit = n * std::pow(f / n, t);
        float split = uniform + (logSplit - uniform) * lambda;
        Vec3 corners[8];
        int k = 0;
        for (float z : {prev, split})
            for (float sx : {-1.0f, 1.0f})
                for (float sy : {-1.0f, 1.0f}) corners[k++] = invView.transformPoint({sx * tanX * z, sy * tanY * z, -z});
        Vec3 center{0, 0, 0};
        for (auto& c : corners) center += c;
        center = center / 8.0f;
        float radius = 0;
        for (auto& c : corners) radius = std::max(radius, length(c - center));
        radius = std::ceil(radius * 16.0f) / 16.0f;  // quantized so it does not change while rotating
        float texel = 2.0f * radius / static_cast<float>(resolution);
        // Snap the center to whole texels in light space -> no shimmering when the camera moves.
        Vec3 ls = lightRot.transformPoint(center);
        ls.x = std::floor(ls.x / texel) * texel;
        ls.y = std::floor(ls.y / texel) * texel;
        Vec3 snapped = lightRot.inverse().transformPoint(ls);
        float depthRange = radius + 60.0f;  // extra room for casters between the light and the slice
        Mat4 lightView = Mat4::lookAt(snapped - L * depthRange, snapped, up);
        Mat4 proj = Mat4::ortho(-radius, radius, -radius, radius, 0.0f, depthRange + radius);
        Cascade& c = out[static_cast<size_t>(i)];
        c.viewProj = proj * lightView;
        c.splitNear = prev;
        c.splitFar = split;
        c.center = snapped;
        c.radius = radius;
        c.texelWorld = texel;
        prev = split;
    }
    return out;
}

bool sphereInClip(const Mat4& vp, Vec3 c, float r) {
    if (r <= 0) return true;
    Vec4 p = vp * Vec4(c, 1.0f);
    float rx = r * length(Vec3{vp.at(0, 0), vp.at(0, 1), vp.at(0, 2)});
    float ry = r * length(Vec3{vp.at(1, 0), vp.at(1, 1), vp.at(1, 2)});
    float rz = r * length(Vec3{vp.at(2, 0), vp.at(2, 1), vp.at(2, 2)});
    float w = std::fabs(p.w) < 1e-6f ? 1.0f : p.w;
    return p.x - rx <= w && p.x + rx >= -w && p.y - ry <= w && p.y + ry >= -w && p.z - rz <= w && p.z + rz >= -w;
}

std::vector<Mat4> shadowCasterVolumes(const ViewItem& view, const RenderSettings& rs, const std::vector<LightItem>& lights) {
    std::vector<Mat4> out;
    if (!rs.shadows || rs.debugView == DebugView::Wireframe) return out;
    const Mat4& P = view.projection;
    float m22 = P.at(2, 2), m23 = P.at(2, 3);
    float nearPlane = std::fabs(m22 - 1.0f) > 1e-6f ? m23 / (m22 - 1.0f) : 0.1f;
    float farPlane = std::fabs(m22 + 1.0f) > 1e-6f ? m23 / (m22 + 1.0f) : 1000.0f;
    float fovY = 2.0f * std::atan(1.0f / std::max(P.at(1, 1), 1e-6f));
    float aspect = P.at(1, 1) / std::max(P.at(0, 0), 1e-6f);
    bool dir = false;
    int spots = 0, points = 0, views = 0;
    constexpr int kMaxSpots = 2, kMaxPoints = 2, kMaxViews = 14;
    for (const LightItem& l : lights) {
        if (!l.castShadows) continue;
        if (l.kind == LightKind::Directional && !dir) {
            dir = true;
            for (const Cascade& c : computeCascades(view.view, fovY, aspect, nearPlane, std::min(rs.shadowDistance, farPlane), l.direction,
                                                    rs.cascadeCount, rs.shadowResolution))
                out.push_back(c.viewProj);
        } else if (l.kind == LightKind::Spot && spots < kMaxSpots && views + 1 <= kMaxViews) {
            Vec3 d = normalize(l.direction);
            Vec3 up = std::fabs(d.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
            out.push_back(Mat4::perspective(l.outerCone * 2.0f + radians(2), 1.0f, 0.05f, std::max(l.range, 0.1f)) *
                          Mat4::lookAt(l.position, l.position + d, up));
            ++spots;
            ++views;
        } else if (l.kind == LightKind::Point && rs.pointShadows && points < kMaxPoints && views + 6 <= kMaxViews) {
            for (int f = 0; f < 6; ++f) out.push_back(pointShadowViewProj(l.position, f, l.range));
            ++points;
            views += 6;
        }
    }
    return out;
}

std::vector<Vec2> computeBrdfLut(int size, int samples) {
    std::vector<Vec2> lut(static_cast<size_t>(size) * static_cast<size_t>(size));
    auto radicalInverse = [](uint32_t bits) {
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        return static_cast<float>(bits) * 2.3283064365386963e-10f;
    };
    for (int y = 0; y < size; ++y) {
        float rough = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
        float a = rough * rough;
        float k = a / 2.0f;  // IBL k
        for (int x = 0; x < size; ++x) {
            float NdotV = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            Vec3 V{std::sqrt(1.0f - NdotV * NdotV), 0, NdotV};
            float A = 0, B = 0;
            for (int i = 0; i < samples; ++i) {
                float xi1 = static_cast<float>(i) / static_cast<float>(samples), xi2 = radicalInverse(static_cast<uint32_t>(i));
                float phi = 2.0f * kPi * xi1;
                float cosT = std::sqrt((1.0f - xi2) / (1.0f + (a * a - 1.0f) * xi2));
                float sinT = std::sqrt(1.0f - cosT * cosT);
                Vec3 H{std::cos(phi) * sinT, std::sin(phi) * sinT, cosT};
                Vec3 L = H * (2.0f * dot(V, H)) - V;
                float NdotL = std::max(L.z, 0.0f), NdotH = std::max(H.z, 0.0f), VdotH = std::max(dot(V, H), 0.0f);
                if (NdotL > 0) {
                    float G = (NdotV / (NdotV * (1 - k) + k)) * (NdotL / (NdotL * (1 - k) + k));
                    float Gvis = G * VdotH / (NdotH * NdotV + 1e-7f);
                    float Fc = std::pow(1.0f - VdotH, 5.0f);
                    A += (1.0f - Fc) * Gvis;
                    B += Fc * Gvis;
                }
            }
            lut[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)] = {A / static_cast<float>(samples),
                                                                                                B / static_cast<float>(samples)};
        }
    }
    return lut;
}

Vec2 taaJitter(uint32_t index) {
    auto halton = [](uint32_t i, uint32_t b) {
        float f = 1.0f, r = 0.0f;
        while (i > 0) {
            f /= static_cast<float>(b);
            r += f * static_cast<float>(i % b);
            i /= b;
        }
        return r;
    };
    uint32_t k = index % 8 + 1;
    return {halton(k, 2) - 0.5f, halton(k, 3) - 0.5f};
}

void renderResolution(const GraphicsSettings& g, int outW, int outH, int& renderW, int& renderH) {
    float s = g.upscaler == UpscalerKind::Off ? g.renderScale : upscalerScale(g.upscalerQuality);
    renderW = std::max(1, static_cast<int>(std::lround(static_cast<float>(outW) * s)));
    renderH = std::max(1, static_cast<int>(std::lround(static_cast<float>(outH) * s)));
}

Vec3 rgbToYCoCg(Vec3 c) {
    return {0.25f * c.x + 0.5f * c.y + 0.25f * c.z, 0.5f * c.x - 0.5f * c.z, -0.25f * c.x + 0.5f * c.y - 0.25f * c.z};
}
Vec3 yCoCgToRgb(Vec3 c) { return {c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z}; }
Vec3 clipHistory(Vec3 h, Vec3 mean, Vec3 sigma, float gamma) {
    Vec3 r;
    for (int i = 0; i < 3; ++i) r[i] = std::clamp(h[i], mean[i] - gamma * sigma[i], mean[i] + gamma * sigma[i]);
    return r;
}

std::vector<LightItem> clusterLights(const ViewItem& view, const std::vector<LightItem>& lights, int maxLights) {
    maxLights = std::clamp(maxLights, 1, 8);
    std::vector<LightItem> out;
    struct Scored {
        size_t i = 0;
        float s = 0;
    };
    std::vector<Scored> local;
    for (size_t i = 0; i < lights.size(); ++i) {
        const LightItem& l = lights[i];
        if (l.kind == LightKind::Directional) {
            if (static_cast<int>(out.size()) < maxLights) out.push_back(l);
            continue;
        }
        float dist = length(l.position - view.position);
        if (dist > l.range + 12.0f) continue;
        local.push_back({i, l.intensity * l.range * l.range / (dist * dist + 1.0f)});
    }
    std::sort(local.begin(), local.end(), [](const Scored& a, const Scored& b) { return a.s > b.s; });
    for (const Scored& s : local) {
        if (static_cast<int>(out.size()) >= maxLights) break;
        out.push_back(lights[s.i]);
    }
    return out;
}

GiBake bakeGiProbes(Vec3 worldMin, Vec3 worldMax, std::span<const Vec3> centres, std::span<const float> radii, Vec3 sky) {
    GiBake b;
    Vec3 ext = worldMax - worldMin;
    if (length(ext) < 0.5f) ext = {8.0f, 4.0f, 8.0f};
    b.min = worldMin;
    b.extent = ext;
    const int n = b.gx * b.gy * b.gz;
    b.rgba.assign(static_cast<size_t>(n) * 4, 255);
    const Vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    auto occluded = [&](Vec3 p, Vec3 dir) {
        for (size_t i = 0; i < centres.size(); ++i) {
            Vec3 oc = p - centres[i];
            float rad = i < radii.size() ? radii[i] : 0.5f;
            if (dot(oc, oc) < rad * rad) return 0.18f;  // probe sits inside a caster
            float hb = dot(oc, dir);
            float c = dot(oc, oc) - rad * rad;
            float disc = hb * hb - c;
            if (disc < 0) continue;
            float t = -hb - std::sqrt(disc);
            if (t > 0.08f && t < 12.0f) return 0.18f;
        }
        return 1.0f;
    };
    auto enc = [](float x) { return static_cast<uint8_t>(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
    for (int z = 0; z < b.gz; ++z)
        for (int y = 0; y < b.gy; ++y)
            for (int x = 0; x < b.gx; ++x) {
                Vec3 t{(x + 0.5f) / static_cast<float>(b.gx), (y + 0.5f) / static_cast<float>(b.gy), (z + 0.5f) / static_cast<float>(b.gz)};
                Vec3 p = b.min + Vec3{t.x * ext.x, t.y * ext.y, t.z * ext.z};
                Vec3 acc{};
                for (const Vec3& d : dirs) acc += sky * occluded(p, d);
                acc *= 1.0f / 6.0f;
                int i = ((y + z * b.gy) * b.gx + x) * 4;
                b.rgba[static_cast<size_t>(i)] = enc(acc.x);
                b.rgba[static_cast<size_t>(i) + 1] = enc(acc.y);
                b.rgba[static_cast<size_t>(i) + 2] = enc(acc.z);
                b.rgba[static_cast<size_t>(i) + 3] = 255;
            }
    return b;
}

} // namespace fledge
