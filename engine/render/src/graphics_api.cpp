#include "fledge/render/graphics_api.h"

#include <cctype>
#include <string>

#ifdef _WIN32
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d9.h>
#endif

namespace fledge::rhi {

const char* toString(GraphicsApi a) {
    switch (a) {
        case GraphicsApi::Auto: return "auto";
        case GraphicsApi::D3D9: return "d3d9";
        case GraphicsApi::D3D10: return "d3d10";
        case GraphicsApi::D3D11: return "d3d11";
        case GraphicsApi::D3D12: return "d3d12";
        case GraphicsApi::OpenGL: return "opengl";
        case GraphicsApi::Vulkan: return "vulkan";
    }
    return "auto";
}

const char* displayName(GraphicsApi a) {
    switch (a) {
        case GraphicsApi::Auto: return "Auto";
        case GraphicsApi::D3D9: return "Direct3D 9";
        case GraphicsApi::D3D10: return "Direct3D 10";
        case GraphicsApi::D3D11: return "Direct3D 11";
        case GraphicsApi::D3D12: return "Direct3D 12";
        case GraphicsApi::OpenGL: return "OpenGL";
        case GraphicsApi::Vulkan: return "Vulkan";
    }
    return "Auto";
}

bool graphicsApiFromString(std::string_view in, GraphicsApi& out) {
    std::string s(in);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "auto" || s.empty()) {
        out = GraphicsApi::Auto;
        return true;
    }
    for (GraphicsApi a : kAllGraphicsApis)
        if (s == toString(a)) {
            out = a;
            return true;
        }
    if (s == "gl") {
        out = GraphicsApi::OpenGL;
        return true;
    }
    Backend b = backendFromString(s);
    if (b == Backend::Null) return false;
    if (b == Backend::OpenGL && s != "opengl" && s != "gl") return false;  // backendFromString's default
    out = toGraphicsApi(b);
    return true;
}

Backend toBackend(GraphicsApi a) {
    switch (a) {
        case GraphicsApi::Auto: return Backend::D3D12;
        case GraphicsApi::D3D9: return Backend::D3D9;
        case GraphicsApi::D3D10: return Backend::D3D10;
        case GraphicsApi::D3D11: return Backend::D3D11;
        case GraphicsApi::D3D12: return Backend::D3D12;
        case GraphicsApi::OpenGL: return Backend::OpenGL;
        case GraphicsApi::Vulkan: return Backend::Vulkan;
    }
    return Backend::OpenGL;
}

GraphicsApi toGraphicsApi(Backend b) {
    switch (b) {
        case Backend::D3D9: return GraphicsApi::D3D9;
        case Backend::D3D10: return GraphicsApi::D3D10;
        case Backend::D3D11: return GraphicsApi::D3D11;
        case Backend::D3D12: return GraphicsApi::D3D12;
        case Backend::Vulkan: return GraphicsApi::Vulkan;
        default: return GraphicsApi::OpenGL;
    }
}

std::vector<Backend> fallbackChain(GraphicsApi requested) {
#ifdef _WIN32
    switch (requested) {
        case GraphicsApi::Auto: return {Backend::D3D12, Backend::D3D11, Backend::OpenGL, Backend::D3D10, Backend::D3D9};
        case GraphicsApi::D3D12: return {Backend::D3D12, Backend::D3D11, Backend::OpenGL};
        case GraphicsApi::D3D11: return {Backend::D3D11, Backend::D3D12, Backend::OpenGL};
        case GraphicsApi::D3D10: return {Backend::D3D10, Backend::D3D11, Backend::OpenGL, Backend::D3D9};
        case GraphicsApi::D3D9: return {Backend::D3D9, Backend::D3D10, Backend::OpenGL};
        case GraphicsApi::OpenGL: return {Backend::OpenGL, Backend::D3D11, Backend::D3D12};
        case GraphicsApi::Vulkan: return {Backend::Vulkan, Backend::D3D12, Backend::D3D11, Backend::OpenGL};
    }
    return {Backend::OpenGL};
#else
    if (requested == GraphicsApi::Vulkan) return {Backend::Vulkan, Backend::OpenGL};
    return {Backend::OpenGL};
#endif
}

ApiProbe probeApi(Backend b) {
    ApiProbe p;
    switch (b) {
        case Backend::Null:
            p.available = true;
            p.detail = "headless";
            return p;
        case Backend::OpenGL:
            p.available = true;  // GLFW reports a missing GL 3.3 driver at window creation; the chain then moves on
            p.detail = "OpenGL 3.3 core";
            return p;
        case Backend::Vulkan: {
            VulkanProbe v = probeVulkan();
            p.available = v.deviceFound && v.vulkan13 && v.shaderCompiler;
            p.detail = p.available ? v.deviceName + ", Vulkan " + v.apiVersion : v.reason;
            return p;
        }
        default: break;
    }
#ifdef _WIN32
    if (b == Backend::D3D12) {
        HMODULE dll = LoadLibraryW(L"d3d12.dll");
        if (!dll) {
            p.detail = "d3d12.dll not found";
            return p;
        }
        using PFN = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
        auto create = reinterpret_cast<PFN>(reinterpret_cast<void*>(GetProcAddress(dll, "D3D12CreateDevice")));
        // IID_ID3D12Device; a null output pointer only tests support.
        static const GUID iid = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
        p.available = create && SUCCEEDED(create(nullptr, D3D_FEATURE_LEVEL_11_0, iid, nullptr));
        p.detail = p.available ? "feature level 11_0+" : "no D3D12 capable adapter";
        if (p.available) {
            // DXR tier: D3D12_FEATURE_D3D12_OPTIONS5 (27) -> RaytracingTier (declared locally: MinGW's d3d12.h predates it)
            struct Options5 {
                BOOL srvOnlyTiledResourceTier3;
                int renderPassesTier;
                int raytracingTier;
            } o5{};
            ID3D12Device* dev = nullptr;
            if (SUCCEEDED(create(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&dev))) && dev) {
                if (SUCCEEDED(dev->CheckFeatureSupport(static_cast<D3D12_FEATURE>(27), &o5, sizeof(o5))))
                    p.rayTracing = o5.raytracingTier >= 10;  // D3D12_RAYTRACING_TIER_1_0 = 10
                dev->Release();
            }
            if (p.rayTracing) p.detail += ", DXR";
        }
        FreeLibrary(dll);
        return p;
    }
    if (b == Backend::D3D11 || b == Backend::D3D10) {
        HMODULE dll = LoadLibraryW(L"d3d11.dll");
        if (!dll) {
            p.detail = "d3d11.dll not found";
            return p;
        }
        auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(reinterpret_cast<void*>(GetProcAddress(dll, "D3D11CreateDevice")));
        D3D_FEATURE_LEVEL l11[] = {D3D_FEATURE_LEVEL_11_0}, l10[] = {D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL got{};
        bool ten = b == Backend::D3D10;
        p.available = create && SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, ten ? l10 : l11, ten ? 2 : 1, D3D11_SDK_VERSION,
                                                 nullptr, &got, nullptr));
        p.detail = p.available ? (ten ? (got == D3D_FEATURE_LEVEL_10_1 ? "feature level 10_1" : "feature level 10_0") : "feature level 11_0")
                               : "no adapter for this feature level";
        FreeLibrary(dll);
        return p;
    }
    if (b == Backend::D3D9) {
        HMODULE dll = LoadLibraryW(L"d3d9.dll");
        if (!dll) {
            p.detail = "d3d9.dll not found";
            return p;
        }
        using PFN = IDirect3D9*(WINAPI*)(UINT);
        auto create = reinterpret_cast<PFN>(reinterpret_cast<void*>(GetProcAddress(dll, "Direct3DCreate9")));
        IDirect3D9* d = create ? create(D3D_SDK_VERSION) : nullptr;
        p.available = d != nullptr;
        p.detail = p.available ? "reduced (SM 3.0 basic path)" : "Direct3DCreate9 failed";
        if (d) d->Release();
        FreeLibrary(dll);
        return p;
    }
#endif
    p.detail = "not available on this platform";
    return p;
}

#ifdef _WIN32
namespace d3d {
void setShaderCacheDir(const std::wstring& dir);  // d3d/d3d_common.cpp
std::wstring shaderCacheDir();
} // namespace d3d
void setShaderCacheDirectory(const std::filesystem::path& dir) { d3d::setShaderCacheDir(dir.wstring()); }
std::filesystem::path shaderCacheDirectory() { return std::filesystem::path(d3d::shaderCacheDir()); }
#else
void setShaderCacheDirectory(const std::filesystem::path&) {}
std::filesystem::path shaderCacheDirectory() { return {}; }
#endif

const char* tierLabel(Backend b) {
    switch (b) {
        case Backend::OpenGL:
        case Backend::D3D11:
        case Backend::D3D12: return "Full";
        case Backend::D3D10: return "Full raster (SM 4.0)";
        case Backend::D3D9: return "Reduced (SM 3.0)";
        case Backend::Vulkan: return "Full raster";
        case Backend::Null: return "Headless";
    }
    return "";
}

const char* toString(GfxFeature f) {
    switch (f) {
        case GfxFeature::PbrIbl: return "PBR + image-based lighting";
        case GfxFeature::Shadows: return "Shadows";
        case GfxFeature::Msaa: return "MSAA";
        case GfxFeature::PostFx: return "Bloom / tonemapping / FXAA";
        case GfxFeature::Taa: return "TAA";
        case GfxFeature::TemporalUpscale: return "Temporal upscaling";
        case GfxFeature::Fsr: return "AMD FSR";
        case GfxFeature::Dlss: return "NVIDIA DLSS";
        case GfxFeature::XeSS: return "Intel XeSS";
        case GfxFeature::HdrOutput: return "HDR output";
        case GfxFeature::RayTracing: return "Ray tracing";
        case GfxFeature::PathTracing: return "Path-traced reference";
        case GfxFeature::Count: break;
    }
    return "?";
}

FeatureSupport featureSupport(Backend b, GfxFeature f, const DeviceCaps* caps) {
    auto no = [](std::string r) { return FeatureSupport{false, std::move(r)}; };
    const FeatureSupport yes{true, {}};
    if (b == Backend::Null) return no("headless renderer (no GPU)");
    if (b == Backend::D3D9) return no("Direct3D 9 runs the reduced basic forward path (SM 3.0) only");
    if (caps && !caps->fullPipeline) return no("this device runs the basic forward path only");
    switch (f) {
        case GfxFeature::PbrIbl:
        case GfxFeature::Shadows:
        case GfxFeature::PostFx:
        case GfxFeature::Taa:
        case GfxFeature::TemporalUpscale: return yes;
        case GfxFeature::Fsr:
            if (b != Backend::D3D11 && b != Backend::D3D12) return no("FSR 1 shaders need Shader Model 5 (Direct3D 11/12); TAA upscaling is used instead");
            return yes;
        case GfxFeature::Msaa:
            if (caps && caps->maxSamples < 2) return no("the GPU reports no MSAA support for RGBA16F targets");
            return yes;
        case GfxFeature::Dlss:
            if (b != Backend::D3D11 && b != Backend::D3D12) return no("DLSS needs Direct3D 11/12 (and an NVIDIA RTX GPU)");
            return yes;  // vendor runtime + GPU checks: fledge/render/upscalers.h
        case GfxFeature::XeSS:
            if (b != Backend::D3D11 && b != Backend::D3D12) return no("XeSS needs Direct3D 11/12");
            return yes;
        case GfxFeature::HdrOutput:
            if (b != Backend::D3D11 && b != Backend::D3D12) return no("HDR10 output uses a DXGI swapchain (Direct3D 11/12)");
            if (caps && !caps->hdrOutput) return no("the display is not in HDR mode (enable HDR in Windows display settings)");
            return yes;
        case GfxFeature::RayTracing:
        case GfxFeature::PathTracing:
            if (b != Backend::D3D12) return no("ray tracing uses DXR (Direct3D 12)");
            if (caps && !caps->rayTracing) return no("the GPU/driver does not support DXR tier 1.1");
            return yes;
        case GfxFeature::Count: break;
    }
    return no("unknown feature");
}

} // namespace fledge::rhi
