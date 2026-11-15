#pragma once
// Internal bridge between the D3D12 device (d3d12_device.cpp) and the DXR ray tracer
// (d3d12_raytracing.cpp). Include <directx/d3d12.h> before this header.
#include <memory>

#include "../d3d/d3d_common.h"
#include "fledge/render/raytracing.h"
#include "fledge/render/vendor_upscaler.h"

namespace fledge::rhi::d3d12 {

struct MeshGpu {
    ID3D12Resource* vertexBuffer = nullptr;  // fledge::Vertex stream (pos at offset 0)
    UINT vertexCount = 0, vertexStride = 0;
    ID3D12Resource* indexBuffer = nullptr;   // uint32 indices
    UINT indexCount = 0;
};

// What the ray tracer needs from the device.
class Internals {
public:
    virtual ~Internals() = default;
    virtual ID3D12Device* device() = 0;
    virtual ID3D12Device5* device5() = 0;
    virtual ID3D12GraphicsCommandList* commandList() = 0;  // the open frame list
    virtual d3d::Compiler& compiler() = 0;
    virtual bool meshBuffers(MeshHandle mesh, MeshGpu& out) = 0;
    virtual ID3D12Resource* textureResource(TextureHandle t) = 0;
    virtual bool textureInfo(TextureHandle t, TextureDesc& out) = 0;
    virtual void transitionTexture(TextureHandle t, D3D12_RESOURCE_STATES to) = 0;
    virtual void textureSrv(TextureHandle t, D3D12_CPU_DESCRIPTOR_HANDLE dst) = 0;  // copies the texture's SRV
    // Contiguous descriptors in the frame's shader-visible CBV/SRV/UAV ring.
    virtual bool allocGpuDescriptors(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu) = 0;
    virtual ID3D12DescriptorHeap* gpuSrvHeap() = 0;
    virtual D3D12_GPU_VIRTUAL_ADDRESS uploadTransient(const void* data, UINT64 bytes, UINT64 align) = 0;
    virtual D3D12_GPU_VIRTUAL_ADDRESS frameConstants() = 0;
    virtual void retireResource(d3d::Com<ID3D12Resource>& r) = 0;  // released once the GPU is done
    virtual void restoreGraphicsState() = 0;  // after recording compute/RT work
    virtual void executeAndWait() = 0;
    virtual uint64_t frameNumber() const = 0;
    virtual void waitGpuIdle() = 0;
};

class RayTracingBackend : public RayTracingDevice {};

bool rayTracingSupported(D3D12_RAYTRACING_TIER tier);
std::unique_ptr<RayTracingBackend> createRayTracingBackend(Internals& device);

// Intel XeSS bridge (d3d12_xess.cpp); nullptr when the library cannot be loaded/initialised.
std::unique_ptr<VendorUpscaler> createXessUpscaler(Internals& device, const std::wstring& library);

} // namespace fledge::rhi::d3d12
