#include "fledge/render/graphics_settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "fledge/render/renderer.h"

namespace fledge {

namespace {
std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}
template <typename E, size_t N>
bool parseEnum(std::string_view s, E& out, const E (&all)[N]) {
    std::string l = lower(s);
    for (E e : all)
        if (l == lower(toString(e))) {
            out = e;
            return true;
        }
    return false;
}
constexpr VsyncMode kVsync[] = {VsyncMode::On, VsyncMode::Off, VsyncMode::Vrr};
constexpr WindowMode kWindow[] = {WindowMode::Windowed, WindowMode::Borderless, WindowMode::Fullscreen};
constexpr UpscalerKind kUpscalers[] = {UpscalerKind::Off, UpscalerKind::Taa, UpscalerKind::Fsr, UpscalerKind::XeSS, UpscalerKind::Dlss};
constexpr UpscalerQuality kQualities[] = {UpscalerQuality::Native, UpscalerQuality::Quality, UpscalerQuality::Balanced, UpscalerQuality::Performance,
                                          UpscalerQuality::UltraPerformance};
constexpr RtQuality kRtQualities[] = {RtQuality::Low, RtQuality::Medium, RtQuality::High, RtQuality::Ultra};
constexpr GraphicsPreset kPresets[] = {GraphicsPreset::Custom, GraphicsPreset::Low,   GraphicsPreset::Medium,
                                       GraphicsPreset::High,   GraphicsPreset::Ultra, GraphicsPreset::Everything};
} // namespace

const char* toString(VsyncMode m) { return m == VsyncMode::On ? "on" : m == VsyncMode::Off ? "off" : "vrr"; }
const char* toString(WindowMode m) { return m == WindowMode::Windowed ? "windowed" : m == WindowMode::Borderless ? "borderless" : "fullscreen"; }
const char* toString(UpscalerKind k) {
    switch (k) {
        case UpscalerKind::Off: return "off";
        case UpscalerKind::Taa: return "taa";
        case UpscalerKind::Fsr: return "fsr";
        case UpscalerKind::XeSS: return "xess";
        case UpscalerKind::Dlss: return "dlss";
    }
    return "off";
}
const char* toString(UpscalerQuality q) {
    switch (q) {
        case UpscalerQuality::Native: return "native";
        case UpscalerQuality::Quality: return "quality";
        case UpscalerQuality::Balanced: return "balanced";
        case UpscalerQuality::Performance: return "performance";
        case UpscalerQuality::UltraPerformance: return "ultra-performance";
    }
    return "quality";
}
const char* toString(RtQuality q) {
    switch (q) {
        case RtQuality::Low: return "low";
        case RtQuality::Medium: return "medium";
        case RtQuality::High: return "high";
        case RtQuality::Ultra: return "ultra";
    }
    return "medium";
}
const char* toString(GraphicsPreset p) {
    switch (p) {
        case GraphicsPreset::Custom: return "custom";
        case GraphicsPreset::Low: return "low";
        case GraphicsPreset::Medium: return "medium";
        case GraphicsPreset::High: return "high";
        case GraphicsPreset::Ultra: return "ultra";
        case GraphicsPreset::Everything: return "everything";
    }
    return "custom";
}
bool fromString(std::string_view s, VsyncMode& o) { return parseEnum(s, o, kVsync); }
bool fromString(std::string_view s, WindowMode& o) { return parseEnum(s, o, kWindow); }
bool fromString(std::string_view s, UpscalerKind& o) { return parseEnum(s, o, kUpscalers); }
bool fromString(std::string_view s, UpscalerQuality& o) { return parseEnum(s, o, kQualities); }
bool fromString(std::string_view s, RtQuality& o) { return parseEnum(s, o, kRtQualities); }
bool fromString(std::string_view s, GraphicsPreset& o) { return parseEnum(s, o, kPresets); }

float upscalerScale(UpscalerQuality q) {
    switch (q) {
        case UpscalerQuality::Native: return 1.0f;
        case UpscalerQuality::Quality: return 1.0f / 1.5f;
        case UpscalerQuality::Balanced: return 1.0f / 1.7f;
        case UpscalerQuality::Performance: return 0.5f;
        case UpscalerQuality::UltraPerformance: return 1.0f / 3.0f;
    }
    return 1.0f;
}

Json GraphicsSettings::toJson() const {
    Json j;
    j["api"] = rhi::toString(api);
    j["preset"] = toString(preset);
    j["vsync"] = toString(vsync);
    j["windowMode"] = toString(windowMode);
    j["width"] = width;
    j["height"] = height;
    j["renderScale"] = renderScale;
    j["upscaler"] = toString(upscaler);
    j["upscalerQuality"] = toString(upscalerQuality);
    j["upscalerVersion"] = upscalerVersion;
    j["sharpness"] = sharpness;
    j["hdr"] = hdr;
    j["paperWhiteNits"] = paperWhiteNits;
    j["peakNits"] = peakNits;
    j["shaderCache"] = shaderCache;
    j["rayTracing"] = {{"enabled", rt.enabled},
                       {"shadows", rt.shadows},
                       {"reflections", rt.reflections},
                       {"ambientOcclusion", rt.ambientOcclusion},
                       {"globalIllumination", rt.globalIllumination},
                       {"quality", toString(rt.quality)},
                       {"denoise", rt.denoise}};
    return j;
}

Result<GraphicsSettings> GraphicsSettings::fromJson(const Json& j) {
    GraphicsSettings g;
    if (!j.is_object()) return Result<GraphicsSettings>::failure("graphics: expected an object");
    auto str = [&](const char* k) -> std::string { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
    auto bad = [](const char* k, const std::string& v) { return Result<GraphicsSettings>::failure(std::string("graphics.") + k + ": unknown value '" + v + "'"); };
    if (auto s = str("api"); !s.empty() && !rhi::graphicsApiFromString(s, g.api)) return bad("api", s);
    if (auto s = str("preset"); !s.empty() && !fromString(s, g.preset)) return bad("preset", s);
    if (auto s = str("vsync"); !s.empty() && !fromString(s, g.vsync)) return bad("vsync", s);
    if (auto s = str("windowMode"); !s.empty() && !fromString(s, g.windowMode)) return bad("windowMode", s);
    if (auto s = str("upscaler"); !s.empty() && !fromString(s, g.upscaler)) return bad("upscaler", s);
    if (auto s = str("upscalerQuality"); !s.empty() && !fromString(s, g.upscalerQuality)) return bad("upscalerQuality", s);
    g.upscalerVersion = str("upscalerVersion");
    auto num = [&](const char* k, auto& v) {
        if (j.contains(k) && j[k].is_number()) v = j[k].get<std::decay_t<decltype(v)>>();
    };
    auto flag = [&](const Json& o, const char* k, bool& v) {
        if (o.contains(k) && o[k].is_boolean()) v = o[k].get<bool>();
    };
    num("width", g.width);
    num("height", g.height);
    num("renderScale", g.renderScale);
    num("sharpness", g.sharpness);
    num("paperWhiteNits", g.paperWhiteNits);
    num("peakNits", g.peakNits);
    flag(j, "hdr", g.hdr);
    flag(j, "shaderCache", g.shaderCache);
    if (j.contains("rayTracing") && j["rayTracing"].is_object()) {
        const Json& r = j["rayTracing"];
        flag(r, "enabled", g.rt.enabled);
        flag(r, "shadows", g.rt.shadows);
        flag(r, "reflections", g.rt.reflections);
        flag(r, "ambientOcclusion", g.rt.ambientOcclusion);
        flag(r, "globalIllumination", g.rt.globalIllumination);
        flag(r, "denoise", g.rt.denoise);
        if (r.contains("quality") && r["quality"].is_string() && !fromString(r["quality"].get<std::string>(), g.rt.quality))
            return bad("rayTracing.quality", r["quality"].get<std::string>());
    }
    g.sanitize();
    return g;
}

void GraphicsSettings::sanitize() {
    width = std::clamp(width, 0, 16384);
    height = std::clamp(height, 0, 16384);
    if (!(renderScale > 0)) renderScale = 1.0f;
    renderScale = std::clamp(renderScale, 0.25f, 2.0f);
    sharpness = std::clamp(sharpness, 0.0f, 1.0f);
    paperWhiteNits = std::clamp(paperWhiteNits, 80.0f, 500.0f);
    peakNits = std::clamp(peakNits, 400.0f, 10000.0f);
}

// ------------------------------------------------------------------------------- presets ---
const std::vector<PresetInfo>& graphicsPresets() {
    static const std::vector<PresetInfo> p = {
        {GraphicsPreset::Low, "Low",
         "Fastest. Renders at half resolution and upscales with FSR; small shadow maps, no bloom. For integrated GPUs and older laptops.",
         "~0.2x"},
        {GraphicsPreset::Medium, "Medium",
         "Balanced. Native resolution with TAA, three shadow cascades, bloom. Runs on most dedicated GPUs from the last eight years.", "1.0x"},
        {GraphicsPreset::High, "High",
         "Sharper shadows over a longer distance and TAA at native resolution. Recommended for current mid-range GPUs.", "~1.1x"},
        {GraphicsPreset::Ultra, "Ultra",
         "4K shadow maps, four cascades, plus ray-traced shadows and ambient occlusion where the GPU supports DXR.", "~1.4x"},
        {GraphicsPreset::Everything, "Everything",
         "Every feature on: all ray-traced effects (shadows, reflections, AO, GI) at Ultra quality, maximum shadows, HDR output when the display "
         "supports it. Expect high GPU load; pair it with an upscaler on 4K screens.",
         "~3.0x"},
    };
    return p;
}

const PresetInfo* presetInfo(GraphicsPreset preset) {
    for (auto& i : graphicsPresets())
        if (i.preset == preset) return &i;
    return nullptr;
}

void applyGraphicsPreset(GraphicsPreset p, RenderSettings& r, GraphicsSettings& g, rhi::Backend backend, const rhi::DeviceCaps* caps,
                         std::vector<std::string>* skipped) {
    if (p == GraphicsPreset::Custom) return;
    g.preset = p;
    g.rt = {};
    r.msaa = 1;
    r.fxaa = false;
    r.shadows = true;
    r.bloom = true;
    // 1.2.0 screen-space effects by quality (the artistic ones - auto-exposure, DoF, motion blur, wetness - are left alone)
    r.ssao = r.ssr = r.volumetric = false;
    r.pointShadows = p != GraphicsPreset::Low;
    r.ssaoSamples = p == GraphicsPreset::Medium ? 8 : p == GraphicsPreset::High ? 12 : 16;
    r.ssrSteps = p == GraphicsPreset::Everything ? 64 : 32;
    r.volumetricSteps = p == GraphicsPreset::Everything ? 32 : 24;
    g.sharpness = 0.3f;
    g.renderScale = 1.0f;
    switch (p) {
        case GraphicsPreset::Low:
            r.shadowResolution = 1024;
            r.cascadeCount = 2;
            r.shadowDistance = 25;
            r.bloom = false;
            r.fxaa = true;
            g.upscaler = UpscalerKind::Fsr;
            g.upscalerQuality = UpscalerQuality::Performance;
            break;
        case GraphicsPreset::Medium:
            r.shadowResolution = 2048;
            r.cascadeCount = 3;
            r.shadowDistance = 40;
            r.ssao = true;
            g.upscaler = UpscalerKind::Taa;
            g.upscalerQuality = UpscalerQuality::Native;
            break;
        case GraphicsPreset::High:
            r.shadowResolution = 2048;
            r.cascadeCount = 4;
            r.shadowDistance = 60;
            r.ssao = r.ssr = true;
            g.upscaler = UpscalerKind::Taa;
            g.upscalerQuality = UpscalerQuality::Native;
            break;
        case GraphicsPreset::Ultra:
            r.shadowResolution = 4096;
            r.cascadeCount = 4;
            r.shadowDistance = 100;
            r.ssao = r.ssr = r.volumetric = true;  // the RT effects below replace SSAO / SSR where DXR runs
            g.upscaler = UpscalerKind::Taa;
            g.upscalerQuality = UpscalerQuality::Native;
            g.rt.enabled = true;
            g.rt.shadows = true;
            g.rt.ambientOcclusion = true;
            g.rt.reflections = false;
            g.rt.globalIllumination = false;
            g.rt.quality = RtQuality::High;
            break;
        case GraphicsPreset::Everything:
            r.shadowResolution = 4096;
            r.cascadeCount = 4;
            r.shadowDistance = 150;
            r.ssao = r.ssr = r.volumetric = true;
            g.upscaler = UpscalerKind::Taa;
            g.upscalerQuality = UpscalerQuality::Native;
            g.rt.enabled = true;
            g.rt.shadows = g.rt.reflections = g.rt.ambientOcclusion = g.rt.globalIllumination = true;
            g.rt.quality = RtQuality::Ultra;
            g.hdr = true;
            break;
        case GraphicsPreset::Custom: break;
    }
    r.sanitize();
    g.sanitize();
    if (skipped) {
        auto check = [&](bool used, rhi::GfxFeature f) {
            if (!used) return;
            auto s = rhi::featureSupport(backend, f, caps);
            if (!s.supported) skipped->push_back(std::string(rhi::toString(f)) + ": " + s.reason);
        };
        check(g.rt.enabled, rhi::GfxFeature::RayTracing);
        check(g.hdr, rhi::GfxFeature::HdrOutput);
        check(g.upscaler != UpscalerKind::Off, rhi::GfxFeature::Taa);
    }
}

float estimateGpuCost(const RenderSettings& r, const GraphicsSettings& g) {
    float scale = g.upscaler == UpscalerKind::Off ? g.renderScale : (g.upscaler == UpscalerKind::Taa ? 1.0f : upscalerScale(g.upscalerQuality));
    if (g.upscaler == UpscalerKind::Taa && g.upscalerQuality != UpscalerQuality::Native) scale = upscalerScale(g.upscalerQuality);
    float pixels = scale * scale;
    float shading = 1.0f + (r.msaa > 1 ? 0.25f * static_cast<float>(r.msaa - 1) : 0.0f);
    float shadows = r.shadows ? 0.12f * static_cast<float>(r.cascadeCount) * (static_cast<float>(r.shadowResolution) / 2048.0f) *
                                    (static_cast<float>(r.shadowResolution) / 2048.0f)
                              : 0.0f;
    float post = (r.bloom ? 0.06f : 0.0f) + (g.upscaler != UpscalerKind::Off ? 0.08f : 0.0f) + (r.fxaa ? 0.02f : 0.0f);
    // 1.2.0 effects (measured shares, docs/GRAPHICS.md "Screen-space and post effects"); SSAO / SSR need the depth prepass
    bool ssao = r.ssao && !(g.rt.enabled && g.rt.ambientOcclusion), ssr = r.ssr && !(g.rt.enabled && g.rt.reflections);
    post += (ssao || ssr ? 0.08f : 0.0f) + (ssao ? 0.06f * static_cast<float>(r.ssaoSamples) / 12.0f : 0.0f) +
            (ssr ? 0.1f * static_cast<float>(r.ssrSteps) / 32.0f : 0.0f) + (r.volumetric ? 0.05f * static_cast<float>(r.volumetricSteps) / 24.0f : 0.0f) +
            (r.autoExposure ? 0.01f : 0.0f) + (r.dof ? 0.06f : 0.0f) + (r.motionBlur ? 0.03f : 0.0f);
    if (g.rt.enabled && g.rt.shadows) shadows *= 0.1f;  // RT replaces the sun's cascades (spot maps stay)
    float rt = 0.0f;
    if (g.rt.enabled) {
        float q = g.rt.quality == RtQuality::Low ? 0.5f : g.rt.quality == RtQuality::Medium ? 1.0f : g.rt.quality == RtQuality::High ? 1.6f : 2.4f;
        rt = q * (0.25f * g.rt.shadows + 0.35f * g.rt.reflections + 0.25f * g.rt.ambientOcclusion + 0.5f * g.rt.globalIllumination);
    }
    // normalised so that the Medium preset (3 x 2048 cascades, bloom, native TAA, SSAO 8 samples) = 1.0
    constexpr float kMedium = 1.0f + 0.36f + 0.14f + 0.08f + 0.04f;
    return (pixels * (shading + post + rt) + shadows) / kMedium;
}

} // namespace fledge
