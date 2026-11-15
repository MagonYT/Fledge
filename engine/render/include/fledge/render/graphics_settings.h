#pragma once
// Per-project graphics & platform settings ("graphics" in the .fledge; baked into packaged
// games). Players may override them at runtime through the in-game settings menu, which writes a
// per-user file (never the project). See docs/GRAPHICS.md.
#include <string>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/utility.h"
#include "fledge/render/graphics_api.h"

namespace fledge {

struct RenderSettings;

enum class VsyncMode { On, Off, Vrr };  // Vrr: no vsync + tearing allowed (G-Sync/FreeSync)
enum class WindowMode { Windowed, Borderless, Fullscreen };
enum class UpscalerKind { Off, Taa, Fsr, XeSS, Dlss };
enum class UpscalerQuality { Native, Quality, Balanced, Performance, UltraPerformance };
enum class RtQuality { Low, Medium, High, Ultra };
enum class GraphicsPreset { Custom, Low, Medium, High, Ultra, Everything };

const char* toString(VsyncMode m);
const char* toString(WindowMode m);
const char* toString(UpscalerKind k);
const char* toString(UpscalerQuality q);
const char* toString(RtQuality q);
const char* toString(GraphicsPreset p);
bool fromString(std::string_view s, VsyncMode& out);
bool fromString(std::string_view s, WindowMode& out);
bool fromString(std::string_view s, UpscalerKind& out);
bool fromString(std::string_view s, UpscalerQuality& out);
bool fromString(std::string_view s, RtQuality& out);
bool fromString(std::string_view s, GraphicsPreset& out);
// Render-resolution scale of an upscaler quality mode (Quality = 1/1.5, Balanced = 1/1.7, ...).
float upscalerScale(UpscalerQuality q);

struct RayTracingSettings {
    bool enabled = false;      // master toggle; off = pure raster (no RT resources are created)
    bool shadows = true;
    bool reflections = true;
    bool ambientOcclusion = true;
    bool globalIllumination = false;
    RtQuality quality = RtQuality::Medium;
    bool denoise = true;       // spatial + temporal
    bool operator==(const RayTracingSettings&) const = default;
};

struct GraphicsSettings {
    rhi::GraphicsApi api = rhi::GraphicsApi::Auto;
    GraphicsPreset preset = GraphicsPreset::Custom;  // projects without a "graphics" block keep their look
    VsyncMode vsync = VsyncMode::On;
    WindowMode windowMode = WindowMode::Windowed;
    int width = 0, height = 0;   // fullscreen/borderless resolution; 0 = desktop / project window size
    float renderScale = 1.0f;    // 0.5 - 2.0 (manual scale when no upscaler; with an upscaler the quality mode decides)
    UpscalerKind upscaler = UpscalerKind::Off;  // presets switch on TAA / FSR
    UpscalerQuality upscalerQuality = UpscalerQuality::Quality;
    std::string upscalerVersion;  // e.g. "dlss-310.9.16"; empty = newest installed (see upscalers.h)
    float sharpness = 0.3f;       // RCAS / upscaler sharpening 0..1
    bool hdr = false;             // HDR10 output when the display supports it
    float paperWhiteNits = 200.0f;
    float peakNits = 1000.0f;
    RayTracingSettings rt;
    bool shaderCache = true;      // persistent shader/pipeline cache (D3D bytecode, GL program binaries)

    Json toJson() const;
    static Result<GraphicsSettings> fromJson(const Json& j);
    void sanitize();
    bool operator==(const GraphicsSettings&) const = default;
};

// ---- presets -------------------------------------------------------------------------------
struct PresetInfo {
    GraphicsPreset preset;
    const char* name;
    const char* summary;  // plain language
    const char* cost;     // estimated GPU cost
};
const std::vector<PresetInfo>& graphicsPresets();
const PresetInfo* presetInfo(GraphicsPreset p);
// Applies a preset to both settings blocks. Features the active backend cannot do are left on in
// the settings (they switch on automatically on a capable API) but reported in `skipped`.
void applyGraphicsPreset(GraphicsPreset p, RenderSettings& render, GraphicsSettings& gfx, rhi::Backend backend = rhi::Backend::D3D12,
                         const rhi::DeviceCaps* caps = nullptr, std::vector<std::string>* skipped = nullptr);
// Relative GPU cost estimate of a configuration (1.0 = the "Medium" preset at 1080p).
float estimateGpuCost(const RenderSettings& render, const GraphicsSettings& gfx);

} // namespace fledge
