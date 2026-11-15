#pragma once
// RHI - Render Hardware Interface.
//
// Small, immediate-style API. Backends (capability matrix: fledge/render/graphics_api.h, docs/GRAPHICS.md):
//   OpenGL 3.3 core  - full pipeline
//   Direct3D 12      - full pipeline (+ DXR ray tracing when the GPU supports it)
//   Direct3D 11      - full pipeline (feature level 11_0, SM 5.0)
//   Direct3D 10      - the D3D11 runtime at feature level 10_x (SM 4.0): full raster pipeline, no RT
//   Direct3D 9       - reduced: basic lit forward path only (SM 3.0)
//   Vulkan 1.3       - full pipeline (HLSL -> SPIR-V at runtime through dxcompiler.dll); no ray tracing
//   Null             - complete as a no-op recorder (headless/CI); counts draws
// Every native API is loaded at runtime (LoadLibrary), so a missing driver/DLL only makes that
// backend unavailable - the fallback chain then tries the next one.
//
// Shaders are dual-source (GLSL 330 + HLSL) per ShaderDesc; a backend compiles the language it
// understands (HLSL profiles: SM5 for D3D11/12, SM4 for D3D10, SM3 for D3D9).
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "fledge/core/math.h"
#include "fledge/core/mesh_data.h"

namespace fledge::rhi {

enum class Backend { Null, OpenGL, D3D9, D3D10, D3D11, D3D12, Vulkan };
const char* toString(Backend b);
Backend backendFromString(std::string_view s);  // "gl"/"opengl", "d3d9", "d3d10", "d3d11", "d3d12", "null", "vulkan"
bool isD3D(Backend b);

struct MeshHandle { uint32_t id = 0; explicit operator bool() const { return id != 0; } };
struct ShaderHandle { uint32_t id = 0; explicit operator bool() const { return id != 0; } };
struct TextureHandle {
    uint32_t id = 0;
    explicit operator bool() const { return id != 0; }
    bool operator==(const TextureHandle&) const = default;
};
struct RenderTargetHandle { uint32_t id = 0; explicit operator bool() const { return id != 0; } };
struct FramebufferHandle { uint32_t id = 0; explicit operator bool() const { return id != 0; } };
struct BufferHandle { uint32_t id = 0; explicit operator bool() const { return id != 0; } };

enum class Topology { Triangles, Lines };

// Texture formats. BC1/BC3 = S3TC (DXT1/DXT5). Depth32F is a depth attachment / shadow map.
enum class TextureFormat { RGBA8, BC1, BC3, RGBA16F, RG16F, R16F, RGBA32F, Depth32F };
const char* toString(TextureFormat f);
bool isDepthFormat(TextureFormat f);
enum class TextureType { Tex2D, Tex2DArray, Cube };
enum class Filter { Nearest, Linear };
enum class Wrap { Repeat, Clamp };

struct SamplerDesc {
    Filter filter = Filter::Linear;
    bool mipmaps = false;       // trilinear between mips
    Wrap wrap = Wrap::Clamp;
    bool depthCompare = false;  // sampler*Shadow (hardware PCF)
    bool operator==(const SamplerDesc&) const = default;
};

struct TextureDesc {
    TextureType type = TextureType::Tex2D;
    TextureFormat format = TextureFormat::RGBA8;
    bool srgb = false;
    int width = 1, height = 1;
    int layers = 1;   // Tex2DArray layers (cube: always 6 faces)
    int mips = 1;
    int samples = 1;  // >1 = multisampled render target (Tex2D only, no sampling - resolve first)
    bool storage = false;  // writable from ray-tracing / compute shaders (UAV); ignored where unsupported
    SamplerDesc sampler;
    bool operator==(const TextureDesc&) const = default;
};

// Initial data for one (mip, layer/face). Float formats take float data (RGBA16F/RG16F/R16F/RGBA32F).
struct TextureSubresource {
    int mip = 0, layer = 0;
    int width = 0, height = 0;
    const void* data = nullptr;
    size_t size = 0;
};

struct TextureMipDesc {  // legacy cooked-texture upload
    int width = 0, height = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

struct Attachment {
    TextureHandle texture;
    int mip = 0;
    int layer = 0;  // array layer or cube face
};
struct FramebufferDesc {
    std::array<Attachment, 4> colors{};
    int colorCount = 0;
    Attachment depth{};  // texture = {} -> no depth
};

struct ClearDesc {
    bool color = true;
    Vec4 colorValue{0, 0, 0, 1};
    bool depth = true;
    float depthValue = 1.0f;
};

struct LineVertex {
    Vec3 position;
    Vec3 color;
};

struct ShaderDesc {
    std::string name;
    std::string glslVertex;    // GLSL 330 core
    std::string glslFragment;
    std::string hlsl;          // HLSL with VSMain/PSMain: cbuffer Frame b0, Params b1, texture N = tN/sN (D3D10/11/12)
    std::vector<std::pair<std::string, int>> uniformBlocks;  // block name -> binding
    std::vector<std::pair<std::string, int>> samplers;       // sampler uniform -> texture unit
};

enum class BlendMode { None, Alpha, Additive };
enum class CullMode { None, Back, Front };
enum class DepthFunc { Less, LessEqual, Always };

struct DeviceCaps {
    bool fullPipeline = false;  // framebuffers, float/depth textures, cubemaps, arrays, UBOs, instancing
    bool bcTextures = false;
    int maxSamples = 1;
    bool depthZeroToOne = false;  // D3D-style clip space (z in [0,1]) and top-left texture origin
    bool rayTracing = false;      // DXR / Vulkan RT usable
    bool hdrOutput = false;       // HDR10 swapchain available on this display
    std::string featureLevel;     // e.g. "11_0", "SM 5.0", "GL 3.3"
    bool instancing = false;      // drawInstanced + a second uniform block (binding 1) for per-instance matrices
};

struct FrameStats {
    uint32_t drawCalls = 0;
    uint32_t triangles = 0;
    uint32_t lines = 0;
    uint32_t passes = 0;
};

class RayTracingDevice;  // fledge/render/raytracing.h
class VendorUpscaler;    // fledge/render/vendor_upscaler.h

class RenderDevice {
public:
    virtual ~RenderDevice() = default;
    virtual Backend backend() const = 0;
    virtual std::string description() const = 0;
    virtual DeviceCaps caps() const = 0;

    // ---- geometry (vertex layout: 0 pos3, 1 normal3, 2 uv2, 3 tangent4 [, lines: 0 pos3, 1 color3])
    virtual MeshHandle createMesh(const MeshData& data) = 0;
    virtual MeshHandle createLineMesh(std::span<const LineVertex> lines) = 0;  // dynamic
    virtual void updateLineMesh(MeshHandle mesh, std::span<const LineVertex> lines) = 0;
    virtual void destroyMesh(MeshHandle mesh) = 0;

    // ---- shaders
    virtual ShaderHandle createShader(const ShaderDesc& desc) = 0;
    virtual void destroyShader(ShaderHandle shader) = 0;

    // ---- textures
    virtual TextureHandle createTexture(const TextureDesc& desc, std::span<const TextureSubresource> initial = {}) = 0;
    virtual void generateMips(TextureHandle tex) = 0;
    virtual TextureHandle createTexture2D(int width, int height, const uint8_t* rgba8) = 0;  // RGBA8 sRGB, GPU mips, repeat
    // Cooked mip chain (RGBA8/BC1/BC3). Check supportsTextureFormat() for BC (decompress on the CPU otherwise).
    virtual TextureHandle createTexture(TextureFormat format, std::span<const TextureMipDesc> mips, bool srgb) = 0;
    virtual bool supportsTextureFormat(TextureFormat format) const = 0;
    // Replaces mip 0 of an existing 2D texture. False if the handle, size, or format does not match (the caller recreates).
    virtual bool updateTexture(TextureHandle tex, const TextureSubresource& mip0) = 0;
    virtual void destroyTexture(TextureHandle tex) = 0;
    virtual uint64_t nativeTexture(TextureHandle tex) const = 0;  // GL texture name (UI previews)

    // ---- framebuffers (render-to-texture)
    virtual FramebufferHandle createFramebuffer(const FramebufferDesc& desc) = 0;
    virtual void destroyFramebuffer(FramebufferHandle fb) = 0;
    // Copies (and resolves MSAA) color attachment 0 of src into dst.
    virtual void resolve(FramebufferHandle src, FramebufferHandle dst, int width, int height) = 0;

    // ---- legacy RGBA8 render targets (editor viewport, thumbnails)
    virtual RenderTargetHandle createRenderTarget(int width, int height) = 0;
    virtual void resizeRenderTarget(RenderTargetHandle rt, int width, int height) = 0;
    virtual void destroyRenderTarget(RenderTargetHandle rt) = 0;
    virtual uint64_t nativeColorTexture(RenderTargetHandle rt) const = 0;
    virtual FramebufferHandle framebufferOf(RenderTargetHandle rt) const = 0;  // {} for the backbuffer

    // ---- uniform buffers (std140)
    virtual BufferHandle createUniformBuffer(size_t bytes) = 0;
    virtual void updateBuffer(BufferHandle buffer, const void* data, size_t bytes, size_t offset = 0) = 0;
    virtual void bindUniformBuffer(int binding, BufferHandle buffer) = 0;
    virtual void destroyBuffer(BufferHandle buffer) = 0;

    // ---- passes
    virtual void beginPass(FramebufferHandle fb, int x, int y, int width, int height, const ClearDesc& clear) = 0;
    // legacy: rt = {} renders to the default framebuffer
    virtual void beginPass(RenderTargetHandle rt, int width, int height, Vec4 clearColor) = 0;
    virtual void endPass() = 0;

    // ---- state + draws
    virtual void setShader(ShaderHandle shader) = 0;
    virtual void setUniform(const char* name, const Mat4& m) = 0;
    virtual void setUniform(const char* name, Vec3 v) = 0;
    virtual void setUniform(const char* name, Vec4 v) = 0;
    virtual void setUniform(const char* name, float f) = 0;
    virtual void setUniform(const char* name, int i) = 0;
    virtual void bindTexture(int slot, TextureHandle tex) = 0;
    virtual void setDepthTest(bool enabled) = 0;
    virtual void setDepthWrite(bool enabled) = 0;
    virtual void setDepthFunc(DepthFunc f) = 0;
    virtual void setBlendMode(BlendMode mode) = 0;
    void setAlphaBlend(bool enabled) { setBlendMode(enabled ? BlendMode::Alpha : BlendMode::None); }
    virtual void setCullMode(CullMode mode) = 0;
    void setBackFaceCulling(bool enabled) { setCullMode(enabled ? CullMode::Back : CullMode::None); }
    virtual void setWireframe(bool enabled) = 0;
    virtual void draw(MeshHandle mesh, Topology topology) = 0;
    virtual void drawInstanced(MeshHandle mesh, int instanceCount) = 0;  // triangles; shader uses gl_InstanceID
    virtual void drawFullscreenTriangle() = 0;  // 3 vertices, no mesh; shader derives positions from gl_VertexID

    // ---- readback: RGBA8, top row first
    virtual bool readPixels(RenderTargetHandle rt, int width, int height, std::vector<uint8_t>& rgba) = 0;
    virtual bool readFramebuffer(FramebufferHandle fb, int width, int height, std::vector<uint8_t>& rgba) = 0;

    virtual FrameStats& stats() = 0;

    // Swapchain-owning backends (D3D/Vulkan) present/resize here; GL/Null: the window swaps.
    virtual void present() {}
    virtual void resizeSwapchain(int width, int height) { (void)width; (void)height; }
    virtual void setVsync(bool vsync, bool allowTearing) { (void)vsync; (void)allowTearing; }
    virtual bool hdrActive() const { return false; }  // HDR10 swapchain in use (tonemap must output PQ)
    // Native handles for optional integrations (ray tracing, upscalers, the editor UI): ID3D12Device*, ID3D11Device*, etc.
    virtual void* nativeDevice() const { return nullptr; }
    virtual void* nativeContext() const { return nullptr; }  // ID3D11DeviceContext* when this is a D3D11 device
    // Hardware ray tracing (DXR on D3D12); nullptr when the API/GPU cannot do it.
    virtual RayTracingDevice* rayTracing() { return nullptr; }
    // Vendor upscaler runtime ("xess") loaded from `runtimeLibrary`; nullptr when this backend cannot
    // drive it. The device owns the object (cached per vendor + library).
    virtual VendorUpscaler* vendorUpscaler(const std::string& vendor, const std::string& runtimeLibraryUtf8) {
        (void)vendor;
        (void)runtimeLibraryUtf8;
        return nullptr;
    }
};

using GlProcLoader = void* (*)(const char*);

struct DeviceCreateInfo {
    GlProcLoader glLoader = nullptr;  // OpenGL
    void* nativeWindow = nullptr;     // D3D / Vulkan: HWND
    int width = 0, height = 0;        // swapchain size
    bool vsync = true;
    bool allowTearing = false;        // vsync off on a VRR display (DXGI_PRESENT_ALLOW_TEARING)
    bool hdr = false;                 // request an HDR10 swapchain (falls back to SDR when unsupported)
};

// Returns nullptr if the backend is unavailable / not implemented.
std::unique_ptr<RenderDevice> createRenderDevice(Backend backend, const DeviceCreateInfo& info);
inline std::unique_ptr<RenderDevice> createRenderDevice(Backend backend, GlProcLoader glLoader = nullptr) {
    DeviceCreateInfo info;
    info.glLoader = glLoader;
    return createRenderDevice(backend, info);
}

} // namespace fledge::rhi
