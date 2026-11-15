#pragma once
// Graphics API selection: the per-project "graphics.api" setting, the runtime fallback chain,
// availability probes and the feature/capability matrix used by the Graphics panel to grey out
// features with a one-line reason. See docs/GRAPHICS.md.
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "fledge/render/rhi.h"

namespace fledge::rhi {

enum class GraphicsApi { Auto, D3D9, D3D10, D3D11, D3D12, OpenGL, Vulkan };
inline constexpr GraphicsApi kAllGraphicsApis[] = {GraphicsApi::Auto,  GraphicsApi::D3D9,   GraphicsApi::D3D10, GraphicsApi::D3D11,
                                                    GraphicsApi::D3D12, GraphicsApi::OpenGL, GraphicsApi::Vulkan};
const char* toString(GraphicsApi a);       // "auto", "d3d9", ..., "opengl", "vulkan"
const char* displayName(GraphicsApi a);    // "Auto", "Direct3D 9", ...
bool graphicsApiFromString(std::string_view s, GraphicsApi& out);  // accepts toString() names and rhi aliases (gl, dx12, ...)
Backend toBackend(GraphicsApi a);          // Auto -> D3D12
GraphicsApi toGraphicsApi(Backend b);

// Ordered backends to try for a requested API; the first one that creates a device wins.
// Auto: D3D12 -> D3D11 -> OpenGL -> D3D10 -> D3D9. Explicit choices fall back to the
// nearest full-featured API, ending in OpenGL (e.g. D3D12 -> D3D11 -> OpenGL).
std::vector<Backend> fallbackChain(GraphicsApi requested);

// Quick availability probe (loads the runtime DLLs and creates a throw-away device where cheap).
struct ApiProbe {
    bool available = false;
    std::string detail;       // adapter / reason
    bool rayTracing = false;  // D3D12: the GPU/driver reports DXR tier 1.0+
};
ApiProbe probeApi(Backend b);

struct VulkanProbe {
    bool loaderFound = false, deviceFound = false, rayTracingExtensions = false;
    bool vulkan13 = false;       // the renderer needs Vulkan 1.3 (dynamic rendering)
    bool shaderCompiler = false; // dxcompiler.dll (HLSL -> SPIR-V) next to the executable
    std::string deviceName, apiVersion, reason;
};
VulkanProbe probeVulkan();

// Feature level / tier label for UI ("Full", "Full raster", "Reduced").
const char* tierLabel(Backend b);

enum class GfxFeature { PbrIbl, Shadows, Msaa, PostFx, Taa, TemporalUpscale, Fsr, Dlss, XeSS, HdrOutput, RayTracing, PathTracing, Count };
const char* toString(GfxFeature f);
struct FeatureSupport {
    bool supported = false;
    std::string reason;  // one line, empty when supported
};
// caps: the live device caps when the backend is running (enables per-GPU checks: DXR, HDR display,
// MSAA); nullptr = static answer for the API.
FeatureSupport featureSupport(Backend b, GfxFeature f, const DeviceCaps* caps = nullptr);

// Persistent shader cache: compiled D3D bytecode is stored here keyed by source hash, so the
// second start skips HLSL compilation (D3D10/11/12). Empty = disabled. OpenGL relies on the driver's
// own program cache; D3D12 pipeline states are cached by the driver.
void setShaderCacheDirectory(const std::filesystem::path& dir);
std::filesystem::path shaderCacheDirectory();

} // namespace fledge::rhi
