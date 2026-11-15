#include "fledge/render/rhi.h"

#include <cctype>
#include <string>

#include "fledge/core/log.h"

namespace fledge::rhi {

std::unique_ptr<RenderDevice> createNullDevice();
std::unique_ptr<RenderDevice> createGlDevice(GlProcLoader loader);
std::unique_ptr<RenderDevice> createD3D12Device(const DeviceCreateInfo& info);  // d3d12/d3d12_device.cpp
std::unique_ptr<RenderDevice> createD3D11Device(const DeviceCreateInfo& info, bool featureLevel10);  // d3d11/d3d11_device.cpp
std::unique_ptr<RenderDevice> createD3D9Device(const DeviceCreateInfo& info);  // d3d9/d3d9_device.cpp
std::unique_ptr<RenderDevice> createVulkanDevice(const DeviceCreateInfo& info);  // vulkan/vulkan_device.cpp

const char* toString(Backend b) {
    switch (b) {
        case Backend::Null: return "Null";
        case Backend::OpenGL: return "OpenGL";
        case Backend::D3D9: return "D3D9";
        case Backend::D3D10: return "D3D10";
        case Backend::D3D11: return "D3D11";
        case Backend::D3D12: return "D3D12";
        case Backend::Vulkan: return "Vulkan";
    }
    return "?";
}

bool isD3D(Backend b) { return b == Backend::D3D9 || b == Backend::D3D10 || b == Backend::D3D11 || b == Backend::D3D12; }

Backend backendFromString(std::string_view in) {
    std::string s(in);
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (s == "d3d12" || s == "dx12" || s == "direct3d12") return Backend::D3D12;
    if (s == "d3d11" || s == "dx11" || s == "direct3d11") return Backend::D3D11;
    if (s == "d3d10" || s == "dx10" || s == "direct3d10") return Backend::D3D10;
    if (s == "d3d9" || s == "dx9" || s == "direct3d9") return Backend::D3D9;
    if (s == "null") return Backend::Null;
    if (s == "vulkan" || s == "vk") return Backend::Vulkan;
    return Backend::OpenGL;
}

const char* toString(TextureFormat f) {
    switch (f) {
        case TextureFormat::RGBA8: return "RGBA8";
        case TextureFormat::BC1: return "BC1";
        case TextureFormat::BC3: return "BC3";
        case TextureFormat::RGBA16F: return "RGBA16F";
        case TextureFormat::RG16F: return "RG16F";
        case TextureFormat::R16F: return "R16F";
        case TextureFormat::RGBA32F: return "RGBA32F";
        case TextureFormat::Depth32F: return "Depth32F";
    }
    return "?";
}

bool isDepthFormat(TextureFormat f) { return f == TextureFormat::Depth32F; }

std::unique_ptr<RenderDevice> createRenderDevice(Backend backend, const DeviceCreateInfo& info) {
    switch (backend) {
        case Backend::Null: return createNullDevice();
        case Backend::OpenGL:
            if (!info.glLoader) {
                FLEDGE_LOG_ERROR("rhi", "OpenGL backend requires a GL proc loader (window with GL context)");
                return nullptr;
            }
            return createGlDevice(info.glLoader);
        case Backend::D3D12: return createD3D12Device(info);
        case Backend::D3D11: return createD3D11Device(info, false);
        case Backend::D3D10: return createD3D11Device(info, true);
        case Backend::D3D9: return createD3D9Device(info);
        case Backend::Vulkan: return createVulkanDevice(info);
    }
    return nullptr;
}

} // namespace fledge::rhi
