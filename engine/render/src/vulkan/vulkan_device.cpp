// Vulkan backend (docs/GRAPHICS.md "Vulkan").
//
// Full raster pipeline on Vulkan 1.3, from the same HLSL as the Direct3D backends:
//   * shaders: DXC (dxcompiler.dll, loaded at runtime, shipped next to the exe) compiles HLSL to SPIR-V
//     (-spirv -fspv-target-env=vulkan1.3 -fvk-use-dx-layout) with register shifts b0..b3 -> bindings 0..3,
//     t0..t15 -> 16..31, s0..s15 -> 32..47 (one descriptor set). The SPIR-V is cached on disk next to the D3D
//     bytecode cache. The "Params" cbuffer layout, the used registers and the vertex input semantics come from a
//     DXBC compile of the same source through d3dcompiler_47 (DX layout == -fvk-use-dx-layout, so offsets match).
//   * conventions: D3D clip space (z 0..1, caps().depthZeroToOne) and a negative-height viewport, so render
//     targets have row 0 at the top exactly like Direct3D and the HLSL needs no Vulkan-specific changes.
//   * execution: one command buffer per frame, dynamic rendering (begun lazily on the first draw so transfers
//     can run between passes), every image in VK_IMAGE_LAYOUT_GENERAL with a full memory barrier after each
//     pass / transfer (simple and always correct; not the fastest), per-draw uniform data in a host-visible ring,
//     descriptor sets from per-frame pools, pipelines cached by shader + state + attachment formats.
//   * no window (DeviceCreateInfo::nativeWindow == nullptr): an offscreen RGBA8 backbuffer; present() just
//     finishes the frame. Used by the headless tests.
// Not implemented: ray tracing, HDR10 output, vendor upscalers, FSR 1 (SM 5 Gather path is D3D only).
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "fledge/core/log.h"
#include "fledge/core/utility.h"
#include "fledge/render/graphics_api.h"
#include "fledge/render/rhi.h"

#ifdef _WIN32
#include "../d3d/d3d_common.h"
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#endif

namespace fledge::rhi {

VulkanProbe probeVulkan() {
    VulkanProbe p;
#ifdef _WIN32
    HMODULE dll = LoadLibraryW(L"vulkan-1.dll");
    if (!dll) {
        p.reason = "vulkan-1.dll not found (no Vulkan driver installed)";
        return p;
    }
    p.loaderFound = true;
    auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(dll, "vkGetInstanceProcAddr")));
    auto createInstance = getProc ? reinterpret_cast<PFN_vkCreateInstance>(getProc(nullptr, "vkCreateInstance")) : nullptr;
    if (!createInstance) {
        p.reason = "Vulkan loader is missing vkCreateInstance";
        FreeLibrary(dll);
        return p;
    }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Fledge probe";
    app.pEngineName = "Fledge";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (createInstance(&ci, nullptr, &inst) != VK_SUCCESS) {
        p.reason = "vkCreateInstance failed";
        FreeLibrary(dll);
        return p;
    }
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(getProc(inst, "vkDestroyInstance"));
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(getProc(inst, "vkEnumeratePhysicalDevices"));
    auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(getProc(inst, "vkGetPhysicalDeviceProperties"));
    auto exts = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(getProc(inst, "vkEnumerateDeviceExtensionProperties"));
    uint32_t n = 0;
    if (enumerate && props && exts && enumerate(inst, &n, nullptr) == VK_SUCCESS && n > 0) {
        std::vector<VkPhysicalDevice> devs(n);
        enumerate(inst, &n, devs.data());
        VkPhysicalDevice best = devs[0];
        for (auto d : devs) {
            VkPhysicalDeviceProperties pr{};
            props(d, &pr);
            if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                best = d;
                break;
            }
        }
        VkPhysicalDeviceProperties pr{};
        props(best, &pr);
        p.deviceFound = true;
        p.deviceName = pr.deviceName;
        p.apiVersion = std::to_string(VK_API_VERSION_MAJOR(pr.apiVersion)) + "." + std::to_string(VK_API_VERSION_MINOR(pr.apiVersion)) + "." +
                       std::to_string(VK_API_VERSION_PATCH(pr.apiVersion));
        p.vulkan13 = VK_API_VERSION_MAJOR(pr.apiVersion) > 1 || VK_API_VERSION_MINOR(pr.apiVersion) >= 3;
        uint32_t en = 0;
        exts(best, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ep(en);
        exts(best, nullptr, &en, ep.data());
        bool accel = false, pipe = false;
        for (auto& e : ep) {
            if (std::strcmp(e.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0) accel = true;
            if (std::strcmp(e.extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) == 0) pipe = true;
        }
        p.rayTracingExtensions = accel && pipe;
    } else {
        p.reason = "no Vulkan physical device";
    }
    if (destroy) destroy(inst, nullptr);
    FreeLibrary(dll);
    if (p.deviceFound && !p.vulkan13) p.reason = "the GPU driver supports Vulkan " + p.apiVersion + "; Fledge needs Vulkan 1.3";
    // Look for dxcompiler.dll without loading it: unloading DXC again (FreeLibrary) leaves stale state behind, and a
    // later load in the same process then crashes inside Compile().
    wchar_t found[MAX_PATH];
    p.shaderCompiler = GetModuleHandleW(L"dxcompiler.dll") != nullptr || SearchPathW(nullptr, L"dxcompiler.dll", nullptr, MAX_PATH, found, nullptr) > 0;
    if (!p.shaderCompiler && p.deviceFound && p.reason.empty()) p.reason = "dxcompiler.dll (HLSL -> SPIR-V) is missing next to the executable";
#else
    p.reason = "Vulkan is only implemented on Windows";
#endif
    return p;
}

#ifdef _WIN32
// ------------------------------------------------------------------------- DXC (COM, declared here) ---
// Only the interfaces Fledge calls; vtable order as in dxcapi.h (which does not build with MinGW's GCC).
// NOT in an anonymous namespace: with internal linkage GCC -O2 sees "no implementation of these abstract classes
// anywhere" and devirtualizes the calls to __cxa_pure_virtual (a crash in Release builds only).
namespace dxcapi {
struct IDxcBlobV : IUnknown {
    virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
};
struct IDxcBlobEncodingV : IDxcBlobV {
    virtual HRESULT STDMETHODCALLTYPE GetEncoding(BOOL* known, UINT32* codePage) = 0;
};
struct IDxcOperationResultV : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetStatus(HRESULT* status) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResult(IDxcBlobV** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetErrorBuffer(IDxcBlobEncodingV** errors) = 0;
};
struct DxcBufferV {
    LPCVOID Ptr;
    SIZE_T Size;
    UINT Encoding;
};
struct IDxcCompiler3V : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Compile(const DxcBufferV* source, LPCWSTR* args, UINT32 argCount, IUnknown* includeHandler, REFIID riid, LPVOID* result) = 0;
};
constexpr GUID kClsidDxcCompiler = {0x73e22d93, 0xe6ce, 0x47f3, {0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0}};
constexpr GUID kIidDxcCompiler3 = {0x228B4687, 0x5A6A, 0x4730, {0x90, 0x0C, 0x97, 0x02, 0xB2, 0x20, 0x3F, 0x54}};
constexpr GUID kIidDxcOperationResult = {0xCEDB484A, 0xD4E9, 0x445A, {0xB9, 0x91, 0xCA, 0x21, 0xCA, 0x15, 0x7D, 0xC2}};
using PFN_DxcCreateInstance = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
} // namespace dxcapi

namespace {
using namespace dxcapi;

struct Dxc {
    HMODULE dll = nullptr;
    IDxcCompiler3V* compiler = nullptr;
    ~Dxc() {
        if (compiler) compiler->Release();
        // the DLL stays loaded (process lifetime), like d3dcompiler
    }
    bool load(std::string& error) {
        dll = LoadLibraryW(L"dxcompiler.dll");
        if (!dll) {
            error = "dxcompiler.dll not found next to the executable";
            return false;
        }
        auto create = reinterpret_cast<PFN_DxcCreateInstance>(reinterpret_cast<void*>(GetProcAddress(dll, "DxcCreateInstance")));
        if (!create || FAILED(create(kClsidDxcCompiler, kIidDxcCompiler3, reinterpret_cast<void**>(&compiler))) || !compiler) {
            error = "dxcompiler.dll: DxcCreateInstance(IDxcCompiler3) failed";
            return false;
        }
        return true;
    }
    // HLSL -> SPIR-V with an on-disk cache keyed by source + entry + profile + arguments.
    bool compile(const std::string& name, const std::string& src, const char* entry, const char* profile, std::vector<uint32_t>& out, std::string& error) {
        static const wchar_t* kFixed[] = {L"-spirv", L"-fspv-target-env=vulkan1.3", L"-fvk-use-dx-layout", L"-fvk-b-shift", L"0", L"0", L"-fvk-t-shift",
                                          L"16", L"0", L"-fvk-s-shift", L"32", L"0", L"-O3", L"-Wno-ignored-attributes"};
        std::string keySrc = src + '\x1f' + entry + '\x1f' + profile + "\x1f" "v1 spirv vulkan1.3 dx-layout b0 t16 s32";
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : keySrc) h = (h ^ c) * 1099511628211ull;
        fs::path cacheDir = shaderCacheDirectory(), cacheFile;
        if (!cacheDir.empty()) {
            char hex[17];
            std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
            cacheFile = cacheDir / (std::string("vk_") + hex + ".spv");
            if (auto bytes = readFileBytes(cacheFile); bytes && bytes->size() >= 20 && bytes->size() % 4 == 0) {
                out.resize(bytes->size() / 4);
                std::memcpy(out.data(), bytes->data(), bytes->size());
                if (out[0] == 0x07230203u) return true;  // SPIR-V magic
            }
        }
        std::wstring wEntry = widen(entry), wProfile = widen(profile), wName = widen(name);
        std::vector<LPCWSTR> args = {wName.c_str(), L"-E", wEntry.c_str(), L"-T", wProfile.c_str()};
        for (auto* a : kFixed) args.push_back(a);
        DxcBufferV buf{src.data(), src.size(), 65001 /* UTF-8 */};
        IDxcOperationResultV* res = nullptr;
        if (FAILED(compiler->Compile(&buf, args.data(), static_cast<UINT32>(args.size()), nullptr, kIidDxcOperationResult, reinterpret_cast<void**>(&res))) || !res) {
            error = "DXC Compile() failed";
            return false;
        }
        HRESULT status = E_FAIL;
        res->GetStatus(&status);
        if (FAILED(status)) {
            IDxcBlobEncodingV* err = nullptr;
            if (SUCCEEDED(res->GetErrorBuffer(&err)) && err) {
                error.assign(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
                err->Release();
            }
            res->Release();
            return false;
        }
        IDxcBlobV* blob = nullptr;
        res->GetResult(&blob);
        res->Release();
        if (!blob) {
            error = "DXC returned no SPIR-V";
            return false;
        }
        out.resize(blob->GetBufferSize() / 4);
        std::memcpy(out.data(), blob->GetBufferPointer(), out.size() * 4);
        blob->Release();
        if (!cacheFile.empty()) {
            std::error_code ec;
            fs::create_directories(cacheDir, ec);
            std::ofstream f(cacheFile, std::ios::binary);
            f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size() * 4));
        }
        return true;
    }
};

// --------------------------------------------------------------------------------- function table ---
#define FLEDGE_VK_INSTANCE_FNS(X)                                                                                                            \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures)                      \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR)                         \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)             \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define FLEDGE_VK_DEVICE_FNS(X)                                                                                                              \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCreateCommandPool)                    \
    X(vkDestroyCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence)   \
    X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateBuffer) X(vkDestroyBuffer)       \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory)                \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView)        \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout)                    \
    X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline)    \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)       \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers)                     \
    X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdPipelineBarrier)                   \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCreateSwapchainKHR)      \
    X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)

struct VkFns {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance vkCreateInstance = nullptr;
#define FLEDGE_VK_DECL(f) PFN_##f f = nullptr;
    FLEDGE_VK_INSTANCE_FNS(FLEDGE_VK_DECL)
    FLEDGE_VK_DEVICE_FNS(FLEDGE_VK_DECL)
#undef FLEDGE_VK_DECL
};

VkFormat vkFormat(TextureFormat f, bool srgb) {
    switch (f) {
        case TextureFormat::RGBA8: return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::BC1: return srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case TextureFormat::BC3: return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
        case TextureFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case TextureFormat::RG16F: return VK_FORMAT_R16G16_SFLOAT;
        case TextureFormat::R16F: return VK_FORMAT_R16_SFLOAT;
        case TextureFormat::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case TextureFormat::Depth32F: return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

constexpr int kSlots = 16;
constexpr uint32_t kTexBase = 16, kSamplerBase = 32;
// cbuffer registers b0..b(kCbSlots-1) -> bindings 0..kCbSlots-1: b0 Frame, b1 Params, b2 Instances, b3 Bones; Fledge
// Advanced builds (1.3.1, 1024-bone GPU skinning) add b4..b6 = Bones1..Bones3 (RHI uniform bindings 3..5).
#if FLEDGE_GPU_SKIN_EXTENDED
constexpr uint32_t kCbSlots = 7;
#else
constexpr uint32_t kCbSlots = 4;
#endif

class VulkanDevice final : public RenderDevice {
public:
    ~VulkanDevice() override { shutdown(); }

    bool init(const DeviceCreateInfo& info, std::string& error);

    Backend backend() const override { return Backend::Vulkan; }
    std::string description() const override { return m_desc; }
    DeviceCaps caps() const override {
        DeviceCaps c;
        c.fullPipeline = true;
        c.instancing = true;
        c.bcTextures = m_bc;
        c.maxSamples = m_maxSamples;
        c.depthZeroToOne = true;
        c.rayTracing = false;
        c.hdrOutput = false;
        c.featureLevel = "Vulkan " + m_apiVersion + " / SPIR-V (DXC)";
        return c;
    }
    void setVsync(bool vsync, bool tearing) override {
        if (m_vsync != vsync || m_tearing != tearing) m_swapDirty = true;
        m_vsync = vsync;
        m_tearing = tearing;
    }

    // ------------------------------------------------------------------------------------- meshes ---
    MeshHandle createMesh(const MeshData& d) override {
        if (d.empty()) return {};
        Mesh m;
        std::vector<Vec4> tang = d.tangents;
        if (tang.size() != d.vertices.size()) tang.assign(d.vertices.size(), Vec4{1, 0, 0, 1});
        if (!hostBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, d.vertices.data(), d.vertices.size() * sizeof(Vertex), m.vb) ||
            !hostBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, tang.data(), tang.size() * sizeof(Vec4), m.tb) ||
            !hostBuffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, d.indices.data(), d.indices.size() * sizeof(uint32_t), m.ib)) {
            freeMesh(m);
            return {};
        }
        m.count = static_cast<uint32_t>(d.indices.size());
        return MeshHandle{add(m_meshes, std::move(m))};
    }
    MeshHandle createLineMesh(std::span<const LineVertex> lines) override {
        Mesh m;
        m.lines = true;
        MeshHandle h{add(m_meshes, std::move(m))};
        updateLineMesh(h, lines);
        return h;
    }
    void updateLineMesh(MeshHandle h, std::span<const LineVertex> lines) override {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end()) return;
        Mesh& m = it->second;
        m.count = static_cast<uint32_t>(lines.size());
        if (lines.empty()) return;
        if (m.capacity < lines.size()) {  // grow: the old buffer may still be read by this frame's commands
            defer(m.vb);
            m.capacity = std::max<size_t>(lines.size(), 1024);
            std::vector<LineVertex> zero(m.capacity);
            if (!hostBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, zero.data(), zero.size() * sizeof(LineVertex), m.vb)) return;
        } else if (m.usedThisFrame == m_frame) {  // already drawn this frame: write into a fresh buffer
            defer(m.vb);
            std::vector<LineVertex> zero(m.capacity);
            if (!hostBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, zero.data(), zero.size() * sizeof(LineVertex), m.vb)) return;
        }
        std::memcpy(m.vb.mapped, lines.data(), lines.size() * sizeof(LineVertex));
    }
    void destroyMesh(MeshHandle h) override {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end()) return;
        defer(it->second.vb);
        defer(it->second.tb);
        defer(it->second.ib);
        m_meshes.erase(it);
    }

    // ------------------------------------------------------------------------------------ shaders ---
    ShaderHandle createShader(const ShaderDesc& desc) override;
    void destroyShader(ShaderHandle h) override {
        auto it = m_shaders.find(h.id);
        if (it == m_shaders.end()) return;
        if (m_current == &it->second) m_current = nullptr;
        for (auto pit = m_pipelines.begin(); pit != m_pipelines.end();)
            if (pit->first.shader == h.id) {
                m_deadPipelines.push_back(pit->second);
                pit = m_pipelines.erase(pit);
            } else {
                ++pit;
            }
        m_deadShaders.push_back(std::move(it->second));
        m_shaders.erase(it);
    }

    // ----------------------------------------------------------------------------------- textures ---
    TextureHandle createTexture(const TextureDesc& d, std::span<const TextureSubresource> initial) override;
    void generateMips(TextureHandle h) override;
    bool updateTexture(TextureHandle, const TextureSubresource&) override { return false; }
    TextureHandle createTexture2D(int w, int h, const uint8_t* rgba) override {
        TextureDesc d;
        d.width = w;
        d.height = h;
        d.srgb = true;
        int mips = 1;
        while ((std::max(w, h) >> mips) > 0) ++mips;
        d.mips = mips;
        d.sampler = {Filter::Linear, true, Wrap::Repeat, false};
        TextureSubresource s{0, 0, w, h, rgba, static_cast<size_t>(w) * static_cast<size_t>(h) * 4};
        TextureHandle t = createTexture(d, std::span(&s, 1));
        generateMips(t);
        return t;
    }
    TextureHandle createTexture(TextureFormat format, std::span<const TextureMipDesc> mips, bool srgb) override {
        if (mips.empty()) return {};
        TextureDesc d;
        d.format = format;
        d.srgb = srgb;
        d.width = mips[0].width;
        d.height = mips[0].height;
        d.mips = static_cast<int>(mips.size());
        d.sampler = {Filter::Linear, mips.size() > 1, Wrap::Repeat, false};
        std::vector<TextureSubresource> subs;
        for (size_t i = 0; i < mips.size(); ++i) subs.push_back({static_cast<int>(i), 0, mips[i].width, mips[i].height, mips[i].data, mips[i].size});
        return createTexture(d, subs);
    }
    bool supportsTextureFormat(TextureFormat f) const override { return m_bc || (f != TextureFormat::BC1 && f != TextureFormat::BC3); }
    void destroyTexture(TextureHandle h) override {
        auto it = m_textures.find(h.id);
        if (it == m_textures.end()) return;
        for (auto& b : m_bound)
            if (b == h.id) b = 0;
        m_deadTextures.push_back(std::move(it->second));
        m_textures.erase(it);
    }
    uint64_t nativeTexture(TextureHandle) const override { return 0; }

    // ------------------------------------------------------------------------------- framebuffers ---
    FramebufferHandle createFramebuffer(const FramebufferDesc& d) override { return FramebufferHandle{add(m_fbs, FramebufferDesc(d))}; }
    void destroyFramebuffer(FramebufferHandle h) override { m_fbs.erase(h.id); }
    void resolve(FramebufferHandle src, FramebufferHandle dst, int w, int h) override;

    RenderTargetHandle createRenderTarget(int w, int h) override {
        Rt r;
        makeRt(r, w, h);
        return RenderTargetHandle{add(m_rts, std::move(r))};
    }
    void resizeRenderTarget(RenderTargetHandle h, int w, int hh) override {
        auto it = m_rts.find(h.id);
        if (it == m_rts.end()) return;
        freeRt(it->second);
        makeRt(it->second, w, hh);
    }
    void destroyRenderTarget(RenderTargetHandle h) override {
        auto it = m_rts.find(h.id);
        if (it == m_rts.end()) return;
        freeRt(it->second);
        m_rts.erase(it);
    }
    uint64_t nativeColorTexture(RenderTargetHandle) const override { return 0; }
    FramebufferHandle framebufferOf(RenderTargetHandle h) const override {
        auto it = m_rts.find(h.id);
        return it == m_rts.end() ? FramebufferHandle{} : it->second.fb;
    }

    // ---------------------------------------------------------------------------- uniform buffers ---
    BufferHandle createUniformBuffer(size_t bytes) override {
        Ubo u;
        u.data.assign((bytes + 15) & ~size_t(15), 0);
        return BufferHandle{add(m_ubos, std::move(u))};
    }
    void updateBuffer(BufferHandle h, const void* data, size_t bytes, size_t offset) override {
        auto it = m_ubos.find(h.id);
        if (it == m_ubos.end() || offset >= it->second.data.size()) return;
        std::memcpy(it->second.data.data() + offset, data, std::min(bytes, it->second.data.size() - offset));
        it->second.uploadedFrame = ~0ull;  // the next draw that reads it copies a new version into the ring
    }
    void bindUniformBuffer(int binding, BufferHandle h) override {
        // 0 = Frame (b0), 1 = Instances (b2), 2 = Bones (b3), Fledge Advanced: 3..5 = Bones1..3 (b4..b6)
        if (binding >= 0 && binding < static_cast<int>(kCbSlots) - 1) m_uboBinding[static_cast<size_t>(binding)] = h.id;
    }
    void destroyBuffer(BufferHandle h) override { m_ubos.erase(h.id); }

    // ------------------------------------------------------------------------------------- passes ---
    void beginPass(FramebufferHandle fb, int x, int y, int w, int h, const ClearDesc& c) override;
    void beginPass(RenderTargetHandle rt, int w, int h, Vec4 clearColor) override {
        ClearDesc cd;
        cd.colorValue = clearColor;
        beginPass(rt ? framebufferOf(rt) : FramebufferHandle{}, 0, 0, w, h, cd);
    }
    void endPass() override;

    // -------------------------------------------------------------------------------------- state ---
    void setShader(ShaderHandle h) override {
        auto it = m_shaders.find(h.id);
        m_current = it == m_shaders.end() ? nullptr : &it->second;
        m_currentId = it == m_shaders.end() ? 0 : h.id;
    }
    void setRaw(const char* name, const void* data, uint32_t size) {
        if (!m_current) return;
        auto it = m_current->vars.find(name);
        if (it == m_current->vars.end()) return;
        std::memcpy(m_current->params.data() + it->second.offset, data, std::min<uint32_t>(size, it->second.size));
    }
    void setUniform(const char* n, const Mat4& m) override { setRaw(n, m.data(), 64); }
    void setUniform(const char* n, Vec3 v) override { setRaw(n, &v.x, 12); }
    void setUniform(const char* n, Vec4 v) override { setRaw(n, &v.x, 16); }
    void setUniform(const char* n, float f) override { setRaw(n, &f, 4); }
    void setUniform(const char* n, int i) override { setRaw(n, &i, 4); }
    void bindTexture(int slot, TextureHandle h) override {
        if (slot >= 0 && slot < kSlots) m_bound[static_cast<size_t>(slot)] = m_textures.count(h.id) ? h.id : 0;
    }
    void setDepthTest(bool e) override { m_state.depthTest = e; }
    void setDepthWrite(bool e) override { m_state.depthWrite = e; }
    void setDepthFunc(DepthFunc f) override { m_state.depthFunc = f; }
    void setBlendMode(BlendMode m) override { m_state.blend = m; }
    void setCullMode(CullMode m) override { m_state.cull = m; }
    void setWireframe(bool on) override { m_state.wire = on && m_fillNonSolid; }

    void draw(MeshHandle h, Topology t) override;
    void drawInstanced(MeshHandle h, int instances) override;
    void drawFullscreenTriangle() override;

    bool readPixels(RenderTargetHandle rt, int w, int h, std::vector<uint8_t>& rgba) override {
        if (rt) return readFramebuffer(framebufferOf(rt), w, h, rgba);
        return readBackbuffer(w, h, rgba);
    }
    bool readFramebuffer(FramebufferHandle fb, int w, int h, std::vector<uint8_t>& rgba) override;

    FrameStats& stats() override { return m_stats; }
    void present() override;
    void resizeSwapchain(int w, int h) override {
        if (w <= 0 || h <= 0 || (w == m_width && h == m_height)) return;
        m_width = w;
        m_height = h;
        m_swapDirty = true;
    }

private:
    // ----------------------------------------------------------------------------------- resources ---
    struct Buf {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };
    struct Mesh {
        Buf vb, tb, ib;
        uint32_t count = 0;
        size_t capacity = 0;
        bool lines = false;
        uint64_t usedThisFrame = ~0ull;
    };
    struct Tex {
        TextureDesc desc;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageView sampled = VK_NULL_HANDLE;
        std::map<std::pair<int, int>, VkImageView> attach;  // (mip, layer) -> 2D view
        bool external = false;                               // swapchain image (not owned)
    };
    struct Rt {
        TextureHandle color, depth;
        FramebufferHandle fb;
    };
    struct Ubo {
        std::vector<uint8_t> data;
        uint64_t uploadedFrame = ~0ull;
        VkDeviceSize ringOffset = 0;
    };
    struct Shader {
        std::string name;
        VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        d3d::ShaderBindings bind;
        std::unordered_map<std::string, d3d::CbVar> vars;
        std::vector<uint8_t> params;
        uint32_t paramsSize = 0;
    };
    struct State {
        bool depthTest = true, depthWrite = true, wire = false;
        DepthFunc depthFunc = DepthFunc::Less;
        BlendMode blend = BlendMode::None;
        CullMode cull = CullMode::Back;
    };
    enum class Input { None = 0, Mesh = 1, Lines = 2 };
    struct PipeKey {  // trivial (memset + memcmp)
        uint32_t shader;
        int depthTest, depthWrite, wire, depthFunc, blend, cull, topology, input, samples;
        VkFormat colors[4];
        int colorCount;
        VkFormat depth;
        bool operator<(const PipeKey& o) const { return std::memcmp(this, &o, sizeof(PipeKey)) < 0; }
    };
    struct Attach {
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        int samples = 1;
    };
    struct Pass {
        bool open = false, begun = false;
        std::array<Attach, 4> colors{};
        int colorCount = 0;
        Attach depth;
        int x = 0, y = 0, w = 1, h = 1, extentW = 1, extentH = 1;
        bool clearColor = false, clearDepth = false;
        Vec4 colorValue{0, 0, 0, 1};
        float depthValue = 1;
    };

    template <typename M, typename T>
    uint32_t add(M& map, T&& v) {
        uint32_t id = m_next++;
        map.emplace(id, std::forward<T>(v));
        return id;
    }
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
        for (uint32_t i = 0; i < m_mem.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (m_mem.memoryTypes[i].propertyFlags & flags) == flags) return i;
        return UINT32_MAX;
    }
    bool hostBuffer(VkBufferUsageFlags usage, const void* data, size_t size, Buf& out);
    void freeBuf(Buf& b) {
        if (b.buffer) m_vk.vkDestroyBuffer(m_device, b.buffer, nullptr);
        if (b.memory) m_vk.vkFreeMemory(m_device, b.memory, nullptr);
        b = {};
    }
    void freeMesh(Mesh& m) {
        freeBuf(m.vb);
        freeBuf(m.tb);
        freeBuf(m.ib);
    }
    void defer(Buf& b) {
        if (b.buffer) m_deadBuffers.push_back(b);
        b = {};
    }
    void freeTex(Tex& t) {
        for (auto& [k, v] : t.attach) m_vk.vkDestroyImageView(m_device, v, nullptr);
        if (t.sampled) m_vk.vkDestroyImageView(m_device, t.sampled, nullptr);
        if (!t.external) {
            if (t.image) m_vk.vkDestroyImage(m_device, t.image, nullptr);
            if (t.memory) m_vk.vkFreeMemory(m_device, t.memory, nullptr);
        }
        t = {};
    }
    void freeShader(Shader& s) {
        if (s.vs) m_vk.vkDestroyShaderModule(m_device, s.vs, nullptr);
        if (s.ps) m_vk.vkDestroyShaderModule(m_device, s.ps, nullptr);
        if (s.layout) m_vk.vkDestroyPipelineLayout(m_device, s.layout, nullptr);
        if (s.setLayout) m_vk.vkDestroyDescriptorSetLayout(m_device, s.setLayout, nullptr);
    }
    void makeRt(Rt& r, int w, int h) {
        TextureDesc c;
        c.width = std::max(1, w);
        c.height = std::max(1, h);
        r.color = createTexture(c, {});
        TextureDesc d = c;
        d.format = TextureFormat::Depth32F;
        r.depth = createTexture(d, {});
        FramebufferDesc fd;
        fd.colorCount = 1;
        fd.colors[0] = {r.color, 0, 0};
        fd.depth = {r.depth, 0, 0};
        r.fb = createFramebuffer(fd);
    }
    void freeRt(Rt& r) {
        destroyFramebuffer(r.fb);
        destroyTexture(r.color);
        destroyTexture(r.depth);
    }
    Tex* tex(uint32_t id) {
        auto it = m_textures.find(id);
        return it == m_textures.end() ? nullptr : &it->second;
    }
    VkImageView attachView(Tex& t, int mip, int layer);
    VkSampler sampler(const SamplerDesc& d);
    VkImageAspectFlags aspect(const Tex& t) const { return isDepthFormat(t.desc.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT; }

    // ----------------------------------------------------------------------------------- recording ---
    VkCommandBuffer oneShotBegin();
    void oneShotEnd(VkCommandBuffer cb);
    void barrier(VkCommandBuffer cb) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        m_vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    void imageBarrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags asp, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {asp, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        m_vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    VkCommandBuffer cmd();  // the frame's command buffer (begins the frame on first use)
    void beginRendering();
    void suspendRendering();  // before transfers inside a pass; the next draw resumes with LOAD
    bool prepareDraw(Input input, Topology topo);
    VkPipeline pipeline(const PipeKey& k);
    VkDeviceSize ringAlloc(size_t bytes, const void* data);
    void flush(bool waitAcquire);  // submit + wait; resets per-frame resources
    void beginFrameIfNeeded();
    bool acquire();
    bool createSwapchain();
    void destroySwapchain();
    bool createOffscreenBackbuffer();
    bool readBackbuffer(int w, int h, std::vector<uint8_t>& rgba);
    bool readImage(Tex& t, int mip, int layer, int w, int h, bool bgra, std::vector<uint8_t>& rgba);
    void collectGarbage();
    void shutdown();

    VkFns m_vk;
    HMODULE m_dll = nullptr;
    Dxc m_dxc;
    d3d::Compiler m_d3dc;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_gpu = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_family = 0;
    VkPhysicalDeviceMemoryProperties m_mem{};
    VkDeviceSize m_uboAlign = 256;
    std::string m_desc, m_apiVersion;
    bool m_bc = false, m_fillNonSolid = false;
    int m_maxSamples = 1;
    // frame
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cb = VK_NULL_HANDLE;
    bool m_recording = false;
    VkFence m_fence = VK_NULL_HANDLE;
    uint64_t m_frame = 1;
    Buf m_ring;
    VkDeviceSize m_ringHead = 0;
    std::vector<VkDescriptorPool> m_descPools;
    size_t m_descPool = 0;
    // window / swapchain
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
    std::vector<uint32_t> m_swapTex;  // texture ids of the swapchain images
    uint32_t m_swapIndex = 0;
    bool m_acquired = false, m_acquireWaited = false, m_swapDirty = false, m_vsync = true, m_tearing = false;
    VkSemaphore m_acquireSem = VK_NULL_HANDLE, m_renderSem = VK_NULL_HANDLE;
    uint32_t m_backDepth = 0, m_offscreen = 0;
    int m_width = 1, m_height = 1;
    // objects
    uint32_t m_next = 1;
    std::unordered_map<uint32_t, Mesh> m_meshes;
    std::unordered_map<uint32_t, Tex> m_textures;
    std::unordered_map<uint32_t, FramebufferDesc> m_fbs;
    std::unordered_map<uint32_t, Rt> m_rts;
    std::unordered_map<uint32_t, Ubo> m_ubos;
    std::unordered_map<uint32_t, Shader> m_shaders;
    std::map<PipeKey, VkPipeline> m_pipelines;
    std::map<std::tuple<int, bool, int, bool>, VkSampler> m_samplers;
    std::vector<Buf> m_deadBuffers;
    std::vector<Tex> m_deadTextures;
    std::vector<Shader> m_deadShaders;
    std::vector<VkPipeline> m_deadPipelines;
    uint32_t m_dummy2D = 0, m_dummyArray = 0, m_dummyCube = 0;
    // bindings + state
    Shader* m_current = nullptr;
    uint32_t m_currentId = 0;
    std::array<uint32_t, kSlots> m_bound{};
    std::array<uint32_t, kCbSlots - 1> m_uboBinding{};
    State m_state;
    Pass m_pass;
    FrameStats m_stats;
};

// ===================================================================================== init ===
bool VulkanDevice::init(const DeviceCreateInfo& info, std::string& error) {
    m_dll = LoadLibraryW(L"vulkan-1.dll");
    if (!m_dll) return (error = "vulkan-1.dll not found (no Vulkan driver)", false);
    m_vk.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(m_dll, "vkGetInstanceProcAddr")));
    if (!m_vk.vkGetInstanceProcAddr) return (error = "vkGetInstanceProcAddr missing", false);
    m_vk.vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(m_vk.vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (!m_dxc.load(error)) return false;
    if (!m_d3dc.load()) return (error = "d3dcompiler_47.dll (shader reflection) is missing", false);
    bool windowed = info.nativeWindow != nullptr;
    std::vector<const char*> instExt;
    if (windowed) instExt = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    std::vector<const char*> layers;
    if (const char* v = std::getenv("FLEDGE_VK_VALIDATION"); v && *v == '1') layers.push_back("VK_LAYER_KHRONOS_validation");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Fledge";
    app.pEngineName = "Fledge";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(instExt.size());
    ici.ppEnabledExtensionNames = instExt.data();
    ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    if (!m_vk.vkCreateInstance || m_vk.vkCreateInstance(&ici, nullptr, &m_instance) != VK_SUCCESS) {
        if (!layers.empty()) {  // validation layer missing: retry without
            ici.enabledLayerCount = 0;
            if (m_vk.vkCreateInstance(&ici, nullptr, &m_instance) != VK_SUCCESS) return (error = "vkCreateInstance failed", false);
        } else {
            return (error = "vkCreateInstance failed (Vulkan 1.3 instance)", false);
        }
    }
#define FLEDGE_VK_LOAD_I(f) m_vk.f = reinterpret_cast<PFN_##f>(m_vk.vkGetInstanceProcAddr(m_instance, #f));
    FLEDGE_VK_INSTANCE_FNS(FLEDGE_VK_LOAD_I)
#undef FLEDGE_VK_LOAD_I
    if (windowed) {
        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = GetModuleHandleW(nullptr);
        sci.hwnd = static_cast<HWND>(info.nativeWindow);
        if (!m_vk.vkCreateWin32SurfaceKHR || m_vk.vkCreateWin32SurfaceKHR(m_instance, &sci, nullptr, &m_surface) != VK_SUCCESS)
            return (error = "vkCreateWin32SurfaceKHR failed", false);
    }
    uint32_t n = 0;
    m_vk.vkEnumeratePhysicalDevices(m_instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    if (n) m_vk.vkEnumeratePhysicalDevices(m_instance, &n, gpus.data());
    int bestScore = -1;
    for (auto g : gpus) {
        VkPhysicalDeviceProperties pr{};
        m_vk.vkGetPhysicalDeviceProperties(g, &pr);
        if (VK_API_VERSION_MAJOR(pr.apiVersion) == 1 && VK_API_VERSION_MINOR(pr.apiVersion) < 3) continue;
        uint32_t qn = 0;
        m_vk.vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        m_vk.vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qs.data());
        for (uint32_t q = 0; q < qn; ++q) {
            if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_TRUE;
            if (m_surface) m_vk.vkGetPhysicalDeviceSurfaceSupportKHR(g, q, m_surface, &present);
            if (!present) continue;
            int score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > bestScore) {
                bestScore = score;
                m_gpu = g;
                m_family = q;
            }
            break;
        }
    }
    if (!m_gpu) return (error = "no Vulkan 1.3 GPU with a graphics (+ present) queue", false);
    VkPhysicalDeviceProperties pr{};
    m_vk.vkGetPhysicalDeviceProperties(m_gpu, &pr);
    m_vk.vkGetPhysicalDeviceMemoryProperties(m_gpu, &m_mem);
    VkPhysicalDeviceFeatures have{};
    m_vk.vkGetPhysicalDeviceFeatures(m_gpu, &have);
    m_bc = have.textureCompressionBC;
    m_fillNonSolid = have.fillModeNonSolid;
    m_uboAlign = std::max<VkDeviceSize>(16, pr.limits.minUniformBufferOffsetAlignment);
    VkSampleCountFlags sc = pr.limits.framebufferColorSampleCounts & pr.limits.framebufferDepthSampleCounts;
    m_maxSamples = (sc & VK_SAMPLE_COUNT_4_BIT) ? 4 : (sc & VK_SAMPLE_COUNT_2_BIT) ? 2 : 1;
    m_apiVersion = std::to_string(VK_API_VERSION_MAJOR(pr.apiVersion)) + "." + std::to_string(VK_API_VERSION_MINOR(pr.apiVersion));
    m_desc = std::string("Vulkan ") + m_apiVersion + " - " + pr.deviceName;
    float prio = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = m_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures feat{};
    feat.textureCompressionBC = have.textureCompressionBC;
    feat.fillModeNonSolid = have.fillModeNonSolid;
    feat.imageCubeArray = have.imageCubeArray;
    std::vector<const char*> devExt;
    if (m_surface) devExt.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f13;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(devExt.size());
    dci.ppEnabledExtensionNames = devExt.data();
    dci.pEnabledFeatures = &feat;
    if (m_vk.vkCreateDevice(m_gpu, &dci, nullptr, &m_device) != VK_SUCCESS) return (error = "vkCreateDevice failed", false);
#define FLEDGE_VK_LOAD_D(f) m_vk.f = reinterpret_cast<PFN_##f>(m_vk.vkGetDeviceProcAddr(m_device, #f));
    FLEDGE_VK_DEVICE_FNS(FLEDGE_VK_LOAD_D)
#undef FLEDGE_VK_LOAD_D
    if (!m_vk.vkCmdBeginRendering) return (error = "vkCmdBeginRendering missing (dynamic rendering)", false);
    m_vk.vkGetDeviceQueue(m_device, m_family, 0, &m_queue);
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = m_family;
    m_vk.vkCreateCommandPool(m_device, &cpi, nullptr, &m_pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = m_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    m_vk.vkAllocateCommandBuffers(m_device, &cai, &m_cb);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    m_vk.vkCreateFence(m_device, &fci, nullptr, &m_fence);
    VkSemaphoreCreateInfo sei{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    m_vk.vkCreateSemaphore(m_device, &sei, nullptr, &m_acquireSem);
    m_vk.vkCreateSemaphore(m_device, &sei, nullptr, &m_renderSem);
    std::vector<uint8_t> zero(16u << 20, 0);
    if (!hostBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, zero.data(), zero.size(), m_ring)) return (error = "uniform ring allocation failed", false);
    // dummies so every declared texture slot is valid
    const uint8_t white[4] = {255, 255, 255, 255};
    TextureDesc d1;
    TextureSubresource s1{0, 0, 1, 1, white, 4};
    m_dummy2D = createTexture(d1, std::span(&s1, 1)).id;
    TextureDesc da = d1;
    da.type = TextureType::Tex2DArray;
    m_dummyArray = createTexture(da, {}).id;
    TextureDesc dc = d1;
    dc.type = TextureType::Cube;
    m_dummyCube = createTexture(dc, {}).id;
    m_width = std::max(1, info.width);
    m_height = std::max(1, info.height);
    m_vsync = info.vsync;
    m_tearing = info.allowTearing;
    if (m_surface ? !createSwapchain() : !createOffscreenBackbuffer()) return (error = "swapchain / backbuffer creation failed", false);
    FLEDGE_LOG_INFO("rhi", "{} (SPIR-V via DXC, {} MSAA max)", m_desc, m_maxSamples);
    return true;
}

bool VulkanDevice::hostBuffer(VkBufferUsageFlags usage, const void* data, size_t size, Buf& out) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = std::max<size_t>(size, 16);
    bi.usage = usage;
    if (m_vk.vkCreateBuffer(m_device, &bi, nullptr, &out.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    m_vk.vkGetBufferMemoryRequirements(m_device, out.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || m_vk.vkAllocateMemory(m_device, &ai, nullptr, &out.memory) != VK_SUCCESS) return false;
    m_vk.vkBindBufferMemory(m_device, out.buffer, out.memory, 0);
    m_vk.vkMapMemory(m_device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped);
    out.size = bi.size;
    if (data && size) std::memcpy(out.mapped, data, size);
    return true;
}

// ============================================================================ swapchain ===
bool VulkanDevice::createOffscreenBackbuffer() {
    TextureDesc c;
    c.width = m_width;
    c.height = m_height;
    m_offscreen = createTexture(c, {}).id;
    TextureDesc d = c;
    d.format = TextureFormat::Depth32F;
    m_backDepth = createTexture(d, {}).id;
    return m_offscreen && m_backDepth;
}

void VulkanDevice::destroySwapchain() {
    if (m_device) m_vk.vkDeviceWaitIdle(m_device);
    for (uint32_t id : m_swapTex) {
        if (Tex* t = tex(id)) freeTex(*t);
        m_textures.erase(id);
    }
    m_swapTex.clear();
    if (m_backDepth) destroyTexture(TextureHandle{m_backDepth});
    m_backDepth = 0;
    collectGarbage();
    if (m_swapchain) m_vk.vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
}

bool VulkanDevice::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    m_vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_gpu, m_surface, &caps);
    uint32_t n = 0;
    m_vk.vkGetPhysicalDeviceSurfaceFormatsKHR(m_gpu, m_surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n);
    m_vk.vkGetPhysicalDeviceSurfaceFormatsKHR(m_gpu, m_surface, &n, fmts.data());
    VkSurfaceFormatKHR fmt = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts[0];
    for (auto& f : fmts)  // UNORM: the tonemap pass writes display-encoded values itself (like the D3D backends)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            fmt = f;
            break;
        }
    m_swapFormat = fmt.format;
    m_vk.vkGetPhysicalDeviceSurfacePresentModesKHR(m_gpu, m_surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    m_vk.vkGetPhysicalDeviceSurfacePresentModesKHR(m_gpu, m_surface, &n, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!m_vsync)
        for (auto m : modes)
            if (m == (m_tearing ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_MAILBOX_KHR) || m == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = m;
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height)};
    if (ext.width == 0 || ext.height == 0) return true;  // minimised: keep the old size, render offscreen later
    m_width = static_cast<int>(ext.width);
    m_height = static_cast<int>(ext.height);
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = m_surface;
    ci.minImageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount + 1, caps.maxImageCount);
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    if (m_vk.vkCreateSwapchainKHR(m_device, &ci, nullptr, &m_swapchain) != VK_SUCCESS) return false;
    m_vk.vkGetSwapchainImagesKHR(m_device, m_swapchain, &n, nullptr);
    std::vector<VkImage> imgs(n);
    m_vk.vkGetSwapchainImagesKHR(m_device, m_swapchain, &n, imgs.data());
    for (VkImage img : imgs) {
        Tex t;
        t.external = true;
        t.image = img;
        t.format = fmt.format;
        t.desc.width = m_width;
        t.desc.height = m_height;
        m_swapTex.push_back(add(m_textures, std::move(t)));
    }
    TextureDesc d;
    d.format = TextureFormat::Depth32F;
    d.width = m_width;
    d.height = m_height;
    m_backDepth = createTexture(d, {}).id;
    m_swapDirty = false;
    return true;
}

bool VulkanDevice::acquire() {
    if (!m_surface || m_acquired) return m_acquired || !m_surface;
    if (m_swapDirty) {
        destroySwapchain();
        if (!createSwapchain()) return false;
    }
    if (!m_swapchain) return false;
    VkResult r = m_vk.vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, m_acquireSem, VK_NULL_HANDLE, &m_swapIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        m_swapDirty = true;
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return false;
    m_acquired = true;
    m_acquireWaited = false;
    // fresh swapchain image: UNDEFINED -> GENERAL (rendered like any other image)
    imageBarrier(cmd(), m_textures[m_swapTex[m_swapIndex]].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    return true;
}

// ============================================================================ recording ===
VkCommandBuffer VulkanDevice::cmd() {
    if (!m_recording) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        m_vk.vkBeginCommandBuffer(m_cb, &bi);
        m_recording = true;
    }
    return m_cb;
}

VkCommandBuffer VulkanDevice::oneShotBegin() {
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = m_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    m_vk.vkAllocateCommandBuffers(m_device, &cai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    m_vk.vkBeginCommandBuffer(cb, &bi);
    return cb;
}

void VulkanDevice::oneShotEnd(VkCommandBuffer cb) {
    m_vk.vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    m_vk.vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE);
    m_vk.vkQueueWaitIdle(m_queue);
    PFN_vkFreeCommandBuffers freeCb = reinterpret_cast<PFN_vkFreeCommandBuffers>(m_vk.vkGetDeviceProcAddr(m_device, "vkFreeCommandBuffers"));
    if (freeCb) freeCb(m_device, m_pool, 1, &cb);
}

VkDeviceSize VulkanDevice::ringAlloc(size_t bytes, const void* data) {
    VkDeviceSize at = (m_ringHead + m_uboAlign - 1) & ~(m_uboAlign - 1);
    if (at + bytes > m_ring.size) {  // full: finish the frame so far and start over
        flush(false);
        at = 0;
    }
    std::memcpy(static_cast<uint8_t*>(m_ring.mapped) + at, data, bytes);
    m_ringHead = at + bytes;
    return at;
}

void VulkanDevice::collectGarbage() {
    for (auto& b : m_deadBuffers) freeBuf(b);
    m_deadBuffers.clear();
    for (auto& t : m_deadTextures) freeTex(t);
    m_deadTextures.clear();
    for (auto& s : m_deadShaders) freeShader(s);
    m_deadShaders.clear();
    for (auto p : m_deadPipelines) m_vk.vkDestroyPipeline(m_device, p, nullptr);
    m_deadPipelines.clear();
}

void VulkanDevice::flush(bool waitAcquire) {
    (void)waitAcquire;
    bool reopen = m_pass.open && m_pass.begun;
    suspendRendering();
    if (m_recording) {
        m_vk.vkEndCommandBuffer(m_cb);
        m_recording = false;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &m_cb;
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        if (m_acquired && !m_acquireWaited) {  // the first submit after acquiring waits for the image
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &m_acquireSem;
            si.pWaitDstStageMask = &stage;
            m_acquireWaited = true;
        }
        m_vk.vkResetFences(m_device, 1, &m_fence);
        m_vk.vkQueueSubmit(m_queue, 1, &si, m_fence);
        m_vk.vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);
    }
    m_ringHead = 0;
    for (auto p : m_descPools) m_vk.vkResetDescriptorPool(m_device, p, 0);
    m_descPool = 0;
    ++m_frame;  // every uniform buffer is re-uploaded on its next use
    collectGarbage();
    (void)reopen;
}

void VulkanDevice::beginPass(FramebufferHandle fb, int x, int y, int w, int h, const ClearDesc& c) {
    if (m_pass.open) endPass();
    Pass p;
    p.open = true;
    p.x = x;
    p.y = y;
    p.w = std::max(1, w);
    p.h = std::max(1, h);
    p.clearColor = c.color;
    p.clearDepth = c.depth;
    p.colorValue = c.colorValue;
    p.depthValue = c.depthValue;
    int extW = 0, extH = 0;
    auto attachTex = [&](const Attachment& a, Attach& out) {
        Tex* t = tex(a.texture.id);
        if (!t) return;
        out.view = attachView(*t, a.mip, a.layer);
        out.format = t->format;
        out.samples = std::max(1, t->desc.samples);
        extW = std::max(1, t->desc.width >> a.mip);
        extH = std::max(1, t->desc.height >> a.mip);
    };
    if (fb) {
        auto it = m_fbs.find(fb.id);
        if (it == m_fbs.end()) return;
        p.colorCount = it->second.colorCount;
        for (int i = 0; i < p.colorCount; ++i) attachTex(it->second.colors[static_cast<size_t>(i)], p.colors[static_cast<size_t>(i)]);
        if (it->second.depth.texture) attachTex(it->second.depth, p.depth);
    } else {  // the backbuffer (swapchain image or the offscreen one) + its depth
        uint32_t colorId = m_offscreen;
        if (m_surface) {
            if (!acquire()) return;
            colorId = m_swapTex[m_swapIndex];
        }
        Attachment a{TextureHandle{colorId}, 0, 0};
        p.colorCount = 1;
        attachTex(a, p.colors[0]);
        Attachment d{TextureHandle{m_backDepth}, 0, 0};
        attachTex(d, p.depth);
    }
    p.extentW = extW;
    p.extentH = extH;
    m_pass = p;
    m_state = State{};
    ++m_stats.passes;
}

void VulkanDevice::beginRendering() {
    if (!m_pass.open || m_pass.begun) return;
    VkRenderingAttachmentInfo colors[4]{};
    for (int i = 0; i < m_pass.colorCount; ++i) {
        auto& a = colors[i];
        a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        a.imageView = m_pass.colors[static_cast<size_t>(i)].view;
        a.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        a.loadOp = m_pass.clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.clearValue.color = {{m_pass.colorValue.x, m_pass.colorValue.y, m_pass.colorValue.z, m_pass.colorValue.w}};
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = m_pass.depth.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth.loadOp = m_pass.clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {m_pass.depthValue, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {static_cast<uint32_t>(m_pass.extentW), static_cast<uint32_t>(m_pass.extentH)}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = static_cast<uint32_t>(m_pass.colorCount);
    ri.pColorAttachments = colors;
    ri.pDepthAttachment = m_pass.depth.view ? &depth : nullptr;
    m_vk.vkCmdBeginRendering(cmd(), &ri);
    m_pass.begun = true;
    m_pass.clearColor = m_pass.clearDepth = false;  // a resumed pass keeps what was drawn
    // D3D conventions: y flipped through a negative-height viewport (row 0 = top)
    VkViewport vp{static_cast<float>(m_pass.x), static_cast<float>(m_pass.y + m_pass.h), static_cast<float>(m_pass.w), -static_cast<float>(m_pass.h), 0, 1};
    VkRect2D sc{{m_pass.x, m_pass.y}, {static_cast<uint32_t>(m_pass.w), static_cast<uint32_t>(m_pass.h)}};
    m_vk.vkCmdSetViewport(m_cb, 0, 1, &vp);
    m_vk.vkCmdSetScissor(m_cb, 0, 1, &sc);
}

void VulkanDevice::suspendRendering() {
    if (m_pass.open && m_pass.begun) {
        m_vk.vkCmdEndRendering(m_cb);
        m_pass.begun = false;
        barrier(m_cb);
    }
}

void VulkanDevice::endPass() {
    if (!m_pass.open) return;
    if (!m_pass.begun && (m_pass.clearColor || m_pass.clearDepth)) beginRendering();  // clear-only pass
    suspendRendering();
    m_pass.open = false;
}

// =============================================================================== shaders ===
ShaderHandle VulkanDevice::createShader(const ShaderDesc& desc) {
    if (desc.hlsl.empty()) {
        FLEDGE_LOG_WARN("rhi", "Vulkan: shader '{}' has no HLSL source - skipped", desc.name);
        return {};
    }
    Shader s;
    s.name = desc.name;
    // reflection from a DXBC compile of the same source (d3dcompiler, cached)
    std::vector<uint8_t> vsDxbc, psDxbc;
    std::string err;
    if (!m_d3dc.compileCached(desc.name, desc.hlsl, "VSMain", "vs_5_0", vsDxbc, err) || !m_d3dc.compileCached(desc.name, desc.hlsl, "PSMain", "ps_5_0", psDxbc, err)) {
        FLEDGE_LOG_ERROR("rhi", "Vulkan: HLSL (reflection) failed for '{}': {}", desc.name, err);
        return {};
    }
    d3d::ShaderBindings vsB, psB;
    d3d::reflectBindings(m_d3dc, vsDxbc, vsB);
    d3d::reflectBindings(m_d3dc, psDxbc, psB);
    s.bind = vsB;
    s.bind.cbuffers |= psB.cbuffers;
    s.bind.textures |= psB.textures;
    s.bind.samplers |= psB.samplers;
    for (int i = 0; i < 32; ++i)
        if (!s.bind.textureDim[i]) s.bind.textureDim[i] = psB.textureDim[i];
    s.paramsSize = std::max(d3d::reflectParams(m_d3dc, vsDxbc, s.vars), d3d::reflectParams(m_d3dc, psDxbc, s.vars));
    s.params.assign(std::max<uint32_t>(s.paramsSize, 16), 0);
    std::vector<uint32_t> vs, ps;
    if (!m_dxc.compile(desc.name, desc.hlsl, "VSMain", "vs_6_0", vs, err) || !m_dxc.compile(desc.name, desc.hlsl, "PSMain", "ps_6_0", ps, err)) {
        FLEDGE_LOG_ERROR("rhi", "Vulkan: HLSL -> SPIR-V failed for '{}': {}", desc.name, err);
        return {};
    }
    auto module = [&](const std::vector<uint32_t>& code) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = code.size() * 4;
        mi.pCode = code.data();
        VkShaderModule m = VK_NULL_HANDLE;
        m_vk.vkCreateShaderModule(m_device, &mi, nullptr, &m);
        return m;
    };
    s.vs = module(vs);
    s.ps = module(ps);
    std::vector<VkDescriptorSetLayoutBinding> b;
    for (uint32_t i = 0; i < kCbSlots; ++i)
        if (s.bind.cbuffers & (1u << i)) b.push_back({i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
    for (uint32_t i = 0; i < kSlots; ++i) {
        if (s.bind.textures & (1u << i)) b.push_back({kTexBase + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
        if (s.bind.samplers & (1u << i)) b.push_back({kSamplerBase + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = static_cast<uint32_t>(b.size());
    li.pBindings = b.data();
    m_vk.vkCreateDescriptorSetLayout(m_device, &li, nullptr, &s.setLayout);
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &s.setLayout;
    m_vk.vkCreatePipelineLayout(m_device, &pli, nullptr, &s.layout);
    if (!s.vs || !s.ps || !s.layout) {
        freeShader(s);
        FLEDGE_LOG_ERROR("rhi", "Vulkan: creating shader objects failed for '{}'", desc.name);
        return {};
    }
    return ShaderHandle{add(m_shaders, std::move(s))};
}

VkPipeline VulkanDevice::pipeline(const PipeKey& k) {
    if (auto it = m_pipelines.find(k); it != m_pipelines.end()) return it->second;
    Shader& s = m_shaders[k.shader];
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, s.vs, "VSMain", nullptr};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, s.ps, "PSMain", nullptr};
    // vertex input: user semantics in order are locations 0..n-1
    std::vector<VkVertexInputBindingDescription> binds;
    std::vector<VkVertexInputAttributeDescription> attrs;
    Input input = static_cast<Input>(k.input);
    if (input == Input::Mesh) {
        binds = {{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX}, {1, sizeof(Vec4), VK_VERTEX_INPUT_RATE_VERTEX}};
        for (uint32_t loc = 0; loc < s.bind.inputs.size(); ++loc) {
            const std::string& sem = s.bind.inputs[loc];
            if (sem == "POSITION") attrs.push_back({loc, 0, VK_FORMAT_R32G32B32_SFLOAT, 0});
            else if (sem == "NORMAL" || sem == "COLOR") attrs.push_back({loc, 0, VK_FORMAT_R32G32B32_SFLOAT, 12});
            else if (sem == "TEXCOORD") attrs.push_back({loc, 0, VK_FORMAT_R32G32_SFLOAT, 24});
            else if (sem == "TANGENT") attrs.push_back({loc, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
        }
    } else if (input == Input::Lines) {
        binds = {{0, sizeof(LineVertex), VK_VERTEX_INPUT_RATE_VERTEX}};
        for (uint32_t loc = 0; loc < s.bind.inputs.size(); ++loc) {
            const std::string& sem = s.bind.inputs[loc];
            if (sem == "POSITION") attrs.push_back({loc, 0, VK_FORMAT_R32G32B32_SFLOAT, 0});
            else attrs.push_back({loc, 0, VK_FORMAT_R32G32B32_SFLOAT, 12});
        }
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = static_cast<uint32_t>(binds.size());
    vi.pVertexBindingDescriptions = binds.data();
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = k.topology ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = k.wire ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k.cull == static_cast<int>(CullMode::Back) ? VK_CULL_MODE_BACK_BIT : k.cull == static_cast<int>(CullMode::Front) ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;  // engine meshes are CCW (GL convention); the flipped viewport keeps it
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(std::max(1, k.samples));
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.depthTest && k.depth != VK_FORMAT_UNDEFINED;
    ds.depthWriteEnable = k.depthWrite && k.depthTest && k.depth != VK_FORMAT_UNDEFINED;
    ds.depthCompareOp = k.depthFunc == static_cast<int>(DepthFunc::LessEqual) ? VK_COMPARE_OP_LESS_OR_EQUAL
                        : k.depthFunc == static_cast<int>(DepthFunc::Always) ? VK_COMPARE_OP_ALWAYS
                                                                              : VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cb[4]{};
    for (int i = 0; i < 4; ++i) {
        auto& a = cb[i];
        a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (k.blend == static_cast<int>(BlendMode::Alpha)) {
            a.blendEnable = VK_TRUE;
            a.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        } else if (k.blend == static_cast<int>(BlendMode::Additive)) {
            a.blendEnable = VK_TRUE;
            a.srcColorBlendFactor = a.dstColorBlendFactor = a.srcAlphaBlendFactor = a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        }
    }
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = static_cast<uint32_t>(k.colorCount);
    blend.pAttachments = cb;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rinfo.colorAttachmentCount = static_cast<uint32_t>(k.colorCount);
    rinfo.pColorAttachmentFormats = k.colors;
    rinfo.depthAttachmentFormat = k.depth;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &rinfo;
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &blend;
    pi.pDynamicState = &dy;
    pi.layout = s.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (m_vk.vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pi, nullptr, &p) != VK_SUCCESS)
        FLEDGE_LOG_ERROR("rhi", "Vulkan: pipeline creation failed for '{}'", s.name);
    m_pipelines[k] = p;
    return p;
}

VkSampler VulkanDevice::sampler(const SamplerDesc& d) {
    auto key = std::make_tuple(static_cast<int>(d.filter), d.mipmaps, static_cast<int>(d.wrap), d.depthCompare);
    if (auto it = m_samplers.find(key); it != m_samplers.end()) return it->second;
    bool lin = d.filter == Filter::Linear || d.depthCompare;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = lin && d.mipmaps ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = d.wrap == Wrap::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.compareEnable = d.depthCompare;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    si.maxLod = d.mipmaps ? VK_LOD_CLAMP_NONE : 0.0f;
    VkSampler s = VK_NULL_HANDLE;
    m_vk.vkCreateSampler(m_device, &si, nullptr, &s);
    return m_samplers[key] = s;
}

// ============================================================================== textures ===
TextureHandle VulkanDevice::createTexture(const TextureDesc& d, std::span<const TextureSubresource> initial) {
    Tex t;
    t.desc = d;
    t.desc.width = std::max(1, d.width);
    t.desc.height = std::max(1, d.height);
    t.desc.mips = std::max(1, d.mips);
    t.desc.samples = std::max(1, d.samples);
    bool depth = isDepthFormat(d.format), bc = d3d::isBlockCompressed(d.format);
    int layers = d.type == TextureType::Cube ? 6 : std::max(1, d.layers);
    t.desc.layers = layers;
    t.format = vkFormat(d.format, d.srgb);
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.flags = d.type == TextureType::Cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = t.format;
    ii.extent = {static_cast<uint32_t>(t.desc.width), static_cast<uint32_t>(t.desc.height), 1};
    ii.mipLevels = static_cast<uint32_t>(t.desc.mips);
    ii.arrayLayers = static_cast<uint32_t>(layers);
    ii.samples = static_cast<VkSampleCountFlagBits>(t.desc.samples);
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (t.desc.samples == 1) ii.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (depth) ii.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if (!bc) ii.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (m_vk.vkCreateImage(m_device, &ii, nullptr, &t.image) != VK_SUCCESS) {
        FLEDGE_LOG_ERROR("rhi", "Vulkan: vkCreateImage failed ({} {}x{} x{} layers, {} mips, {} samples)", toString(d.format), d.width, d.height, layers,
                         d.mips, d.samples);
        return {};
    }
    VkMemoryRequirements req{};
    m_vk.vkGetImageMemoryRequirements(m_device, t.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || m_vk.vkAllocateMemory(m_device, &ai, nullptr, &t.memory) != VK_SUCCESS) {
        m_vk.vkDestroyImage(m_device, t.image, nullptr);
        FLEDGE_LOG_ERROR("rhi", "Vulkan: out of GPU memory for a {}x{} texture", d.width, d.height);
        return {};
    }
    m_vk.vkBindImageMemory(m_device, t.image, t.memory, 0);
    if (t.desc.samples == 1) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t.image;
        vi.viewType = d.type == TextureType::Cube ? VK_IMAGE_VIEW_TYPE_CUBE : d.type == TextureType::Tex2DArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = t.format;
        vi.subresourceRange = {aspect(t), 0, static_cast<uint32_t>(t.desc.mips), 0, static_cast<uint32_t>(layers)};
        m_vk.vkCreateImageView(m_device, &vi, nullptr, &t.sampled);
    }
    // initial data through a staging buffer; the image ends in GENERAL either way
    VkCommandBuffer cb = oneShotBegin();
    imageBarrier(cb, t.image, aspect(t), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    Buf staging;
    if (!initial.empty()) {
        std::vector<std::vector<uint8_t>> converted(initial.size());
        size_t total = 0;
        std::vector<size_t> offsets;
        for (size_t k = 0; k < initial.size(); ++k) {
            auto& s = initial[k];
            UINT pitch, rows;
            d3d::uploadPitch(d.format, s.width, s.height, pitch, rows);
            if (!d3d::convertFloatUpload(d.format, s.data, s.size, s.width, s.height, converted[k]))
                converted[k].assign(static_cast<const uint8_t*>(s.data), static_cast<const uint8_t*>(s.data) + std::min<size_t>(s.size, size_t(pitch) * rows));
            total = (total + 15) & ~size_t(15);
            offsets.push_back(total);
            total += converted[k].size();
        }
        std::vector<uint8_t> all(total, 0);
        for (size_t k = 0; k < initial.size(); ++k) std::memcpy(all.data() + offsets[k], converted[k].data(), converted[k].size());
        if (hostBuffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, all.data(), all.size(), staging)) {
            std::vector<VkBufferImageCopy> copies;
            for (size_t k = 0; k < initial.size(); ++k) {
                auto& s = initial[k];
                VkBufferImageCopy c{};
                c.bufferOffset = offsets[k];
                c.imageSubresource = {aspect(t), static_cast<uint32_t>(s.mip), static_cast<uint32_t>(s.layer), 1};
                c.imageExtent = {static_cast<uint32_t>(std::max(1, s.width)), static_cast<uint32_t>(std::max(1, s.height)), 1};
                copies.push_back(c);
            }
            m_vk.vkCmdCopyBufferToImage(cb, staging.buffer, t.image, VK_IMAGE_LAYOUT_GENERAL, static_cast<uint32_t>(copies.size()), copies.data());
        }
    }
    oneShotEnd(cb);
    freeBuf(staging);
    return TextureHandle{add(m_textures, std::move(t))};
}

void VulkanDevice::generateMips(TextureHandle h) {
    Tex* t = tex(h.id);
    if (!t || t->desc.mips <= 1 || isDepthFormat(t->desc.format) || d3d::isBlockCompressed(t->desc.format)) return;
    suspendRendering();
    VkCommandBuffer cb = cmd();
    barrier(cb);
    for (int m = 1; m < t->desc.mips; ++m) {
        VkImageBlit b{};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(m - 1), 0, static_cast<uint32_t>(t->desc.layers)};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(m), 0, static_cast<uint32_t>(t->desc.layers)};
        b.srcOffsets[1] = {std::max(1, t->desc.width >> (m - 1)), std::max(1, t->desc.height >> (m - 1)), 1};
        b.dstOffsets[1] = {std::max(1, t->desc.width >> m), std::max(1, t->desc.height >> m), 1};
        m_vk.vkCmdBlitImage(cb, t->image, VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
        barrier(cb);
    }
}

VkImageView VulkanDevice::attachView(Tex& t, int mip, int layer) {
    auto key = std::make_pair(mip, layer);
    if (auto it = t.attach.find(key); it != t.attach.end()) return it->second;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t.format;
    vi.subresourceRange = {aspect(t), static_cast<uint32_t>(mip), 1, static_cast<uint32_t>(layer), 1};
    VkImageView v = VK_NULL_HANDLE;
    m_vk.vkCreateImageView(m_device, &vi, nullptr, &v);
    return t.attach[key] = v;
}

void VulkanDevice::resolve(FramebufferHandle src, FramebufferHandle dst, int w, int h) {
    auto s = m_fbs.find(src.id);
    if (s == m_fbs.end() || s->second.colorCount == 0) return;
    Tex* st = tex(s->second.colors[0].texture.id);
    Tex* dt = nullptr;
    int dmip = 0, dlayer = 0;
    if (dst) {
        auto d = m_fbs.find(dst.id);
        if (d == m_fbs.end() || d->second.colorCount == 0) return;
        dt = tex(d->second.colors[0].texture.id);
        dmip = d->second.colors[0].mip;
        dlayer = d->second.colors[0].layer;
    } else {
        suspendRendering();
        if (m_surface && !acquire()) return;
        dt = tex(m_surface ? m_swapTex[m_swapIndex] : m_offscreen);
    }
    if (!st || !dt) return;
    suspendRendering();
    VkCommandBuffer cb = cmd();
    barrier(cb);
    VkImageSubresourceLayers sl{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, dl{VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(dmip), static_cast<uint32_t>(dlayer), 1};
    VkExtent3D ext{static_cast<uint32_t>(std::max(1, w)), static_cast<uint32_t>(std::max(1, h)), 1};
    if (st->desc.samples > 1) {
        VkImageResolve r{sl, {0, 0, 0}, dl, {0, 0, 0}, ext};
        m_vk.vkCmdResolveImage(cb, st->image, VK_IMAGE_LAYOUT_GENERAL, dt->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else if (st->format == dt->format) {
        VkImageCopy c{sl, {0, 0, 0}, dl, {0, 0, 0}, ext};
        m_vk.vkCmdCopyImage(cb, st->image, VK_IMAGE_LAYOUT_GENERAL, dt->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    } else {  // e.g. RGBA8 -> the BGRA8 swapchain
        VkImageBlit b{};
        b.srcSubresource = sl;
        b.dstSubresource = dl;
        b.srcOffsets[1] = b.dstOffsets[1] = {static_cast<int32_t>(ext.width), static_cast<int32_t>(ext.height), 1};
        m_vk.vkCmdBlitImage(cb, st->image, VK_IMAGE_LAYOUT_GENERAL, dt->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    }
    barrier(cb);
}

// ================================================================================= draws ===
bool VulkanDevice::prepareDraw(Input input, Topology topo) {
    if (!m_current || !m_pass.open) return false;
    beginRendering();
    if (!m_pass.begun) return false;
    PipeKey k;
    std::memset(&k, 0, sizeof(k));  // compared bytewise: no stray padding
    k.shader = m_currentId;
    k.depthTest = m_state.depthTest;
    k.depthWrite = m_state.depthWrite;
    k.wire = m_state.wire;
    k.depthFunc = static_cast<int>(m_state.depthFunc);
    k.blend = static_cast<int>(m_state.blend);
    k.cull = static_cast<int>(m_state.cull);
    k.topology = topo == Topology::Lines ? 1 : 0;
    k.input = static_cast<int>(input);
    k.colorCount = m_pass.colorCount;
    for (int i = 0; i < m_pass.colorCount; ++i) k.colors[i] = m_pass.colors[static_cast<size_t>(i)].format;
    k.depth = m_pass.depth.format;
    k.samples = m_pass.colorCount ? m_pass.colors[0].samples : m_pass.depth.samples;
    VkPipeline p = pipeline(k);
    if (!p) return false;
    VkCommandBuffer cb = m_cb;
    m_vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    // descriptor set for this draw
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_current->setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (;;) {
        if (m_descPool >= m_descPools.size()) {
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096 * 4}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096 * 16}, {VK_DESCRIPTOR_TYPE_SAMPLER, 4096 * 16}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = 4096;
            pi.poolSizeCount = 3;
            pi.pPoolSizes = sizes;
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (m_vk.vkCreateDescriptorPool(m_device, &pi, nullptr, &pool) != VK_SUCCESS) return false;
            m_descPools.push_back(pool);
        }
        ai.descriptorPool = m_descPools[m_descPool];
        if (m_vk.vkAllocateDescriptorSets(m_device, &ai, &set) == VK_SUCCESS) break;
        ++m_descPool;  // full: next pool
    }
    std::vector<VkWriteDescriptorSet> writes;
    std::array<VkDescriptorBufferInfo, kCbSlots> bufs{};
    std::array<VkDescriptorImageInfo, kSlots> imgs{}, smps{};
    const d3d::ShaderBindings& b = m_current->bind;
    for (uint32_t i = 0; i < kCbSlots; ++i) {
        if (!(b.cbuffers & (1u << i))) continue;
        VkDeviceSize off = 0, range = 16;
        if (i == 1) {  // Params: this draw's values
            range = std::max<uint32_t>(m_current->paramsSize, 16);
            off = ringAlloc(m_current->params.size(), m_current->params.data());
        } else {
            uint32_t id = m_uboBinding[i == 0 ? 0 : i - 1];
            auto it = m_ubos.find(id);
            if (it == m_ubos.end()) {
                static const uint8_t zeros[256] = {};
                off = ringAlloc(sizeof(zeros), zeros);
                range = sizeof(zeros);
            } else {
                Ubo& u = it->second;
                if (u.uploadedFrame != m_frame) {
                    u.ringOffset = ringAlloc(u.data.size(), u.data.data());
                    u.uploadedFrame = m_frame;
                }
                off = u.ringOffset;
                range = u.data.size();
            }
        }
        bufs[i] = {m_ring.buffer, off, range};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = i;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bufs[i];
        writes.push_back(w);
    }
    for (uint32_t i = 0; i < kSlots; ++i) {
        Tex* t = tex(m_bound[i]);
        if (b.textures & (1u << i)) {
            int dim = b.textureDim[i];  // D3D_SRV_DIMENSION: 4 = 2D, 5 = 2D array, 9 = cube
            uint32_t fallback = dim == 9 ? m_dummyCube : dim == 5 ? m_dummyArray : m_dummy2D;
            bool ok = t && t->sampled &&
                      ((dim == 9) == (t->desc.type == TextureType::Cube)) && ((dim == 5) == (t->desc.type == TextureType::Tex2DArray));
            Tex* use = ok ? t : tex(fallback);
            imgs[i] = {VK_NULL_HANDLE, use->sampled, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = kTexBase + i;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w.pImageInfo = &imgs[i];
            writes.push_back(w);
        }
        if (b.samplers & (1u << i)) {
            smps[i] = {t ? sampler(t->desc.sampler) : sampler(SamplerDesc{}), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = kSamplerBase + i;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            w.pImageInfo = &smps[i];
            writes.push_back(w);
        }
    }
    m_vk.vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    m_vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_current->layout, 0, 1, &set, 0, nullptr);
    return true;
}

void VulkanDevice::draw(MeshHandle h, Topology t) {
    auto it = m_meshes.find(h.id);
    if (it == m_meshes.end() || it->second.count == 0 || !it->second.vb.buffer) return;
    Mesh& m = it->second;
    if (!prepareDraw(m.lines ? Input::Lines : Input::Mesh, t)) return;
    VkDeviceSize zero = 0;
    m.usedThisFrame = m_frame;
    if (m.lines) {
        m_vk.vkCmdBindVertexBuffers(m_cb, 0, 1, &m.vb.buffer, &zero);
        m_vk.vkCmdDraw(m_cb, m.count, 1, 0, 0);
        m_stats.lines += m.count / 2;
    } else {
        VkBuffer vbs[2] = {m.vb.buffer, m.tb.buffer};
        VkDeviceSize offs[2] = {0, 0};
        m_vk.vkCmdBindVertexBuffers(m_cb, 0, 2, vbs, offs);
        m_vk.vkCmdBindIndexBuffer(m_cb, m.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
        m_vk.vkCmdDrawIndexed(m_cb, m.count, 1, 0, 0, 0);
        m_stats.triangles += m.count / 3;
    }
    ++m_stats.drawCalls;
}

void VulkanDevice::drawInstanced(MeshHandle h, int instances) {
    auto it = m_meshes.find(h.id);
    if (it == m_meshes.end() || it->second.lines || it->second.count == 0 || instances <= 0) return;
    Mesh& m = it->second;
    if (!prepareDraw(Input::Mesh, Topology::Triangles)) return;
    VkBuffer vbs[2] = {m.vb.buffer, m.tb.buffer};
    VkDeviceSize offs[2] = {0, 0};
    m_vk.vkCmdBindVertexBuffers(m_cb, 0, 2, vbs, offs);
    m_vk.vkCmdBindIndexBuffer(m_cb, m.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
    m_vk.vkCmdDrawIndexed(m_cb, m.count, static_cast<uint32_t>(instances), 0, 0, 0);
    ++m_stats.drawCalls;
    m_stats.triangles += m.count / 3 * static_cast<uint32_t>(instances);
}

void VulkanDevice::drawFullscreenTriangle() {
    if (!prepareDraw(Input::None, Topology::Triangles)) return;
    m_vk.vkCmdDraw(m_cb, 3, 1, 0, 0);
    ++m_stats.drawCalls;
}

// ============================================================================== readback ===
bool VulkanDevice::readImage(Tex& t, int mip, int layer, int w, int h, bool bgra, std::vector<uint8_t>& rgba) {
    suspendRendering();
    Buf b;
    size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    if (!hostBuffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT, nullptr, bytes, b)) return false;
    VkCommandBuffer cb = cmd();
    barrier(cb);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(mip), static_cast<uint32_t>(layer), 1};
    c.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    m_vk.vkCmdCopyImageToBuffer(cb, t.image, VK_IMAGE_LAYOUT_GENERAL, b.buffer, 1, &c);
    bool reopen = m_pass.open;
    flush(true);
    (void)reopen;
    rgba.assign(static_cast<const uint8_t*>(b.mapped), static_cast<const uint8_t*>(b.mapped) + bytes);
    if (bgra)
        for (size_t i = 0; i < bytes; i += 4) std::swap(rgba[i], rgba[i + 2]);
    freeBuf(b);
    return true;
}

bool VulkanDevice::readBackbuffer(int w, int h, std::vector<uint8_t>& rgba) {
    if (m_surface) {
        if (!m_acquired) return false;
        Tex& t = m_textures[m_swapTex[m_swapIndex]];
        return readImage(t, 0, 0, std::min(w, m_width), std::min(h, m_height), m_swapFormat == VK_FORMAT_B8G8R8A8_UNORM, rgba);
    }
    Tex* t = tex(m_offscreen);
    return t && readImage(*t, 0, 0, std::min(w, m_width), std::min(h, m_height), false, rgba);
}

bool VulkanDevice::readFramebuffer(FramebufferHandle fb, int w, int h, std::vector<uint8_t>& rgba) {
    if (!fb) return readBackbuffer(w, h, rgba);
    auto it = m_fbs.find(fb.id);
    if (it == m_fbs.end() || it->second.colorCount == 0) return false;
    Tex* t = tex(it->second.colors[0].texture.id);
    if (!t || t->desc.format != TextureFormat::RGBA8) return false;
    return readImage(*t, it->second.colors[0].mip, it->second.colors[0].layer, w, h, false, rgba);
}

void VulkanDevice::present() {
    if (m_pass.open) endPass();
    if (m_surface && m_acquired) {
        imageBarrier(cmd(), m_textures[m_swapTex[m_swapIndex]].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        m_vk.vkEndCommandBuffer(m_cb);
        m_recording = false;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &m_cb;
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        if (!m_acquireWaited) {
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &m_acquireSem;
            si.pWaitDstStageMask = &stage;
        }
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &m_renderSem;
        m_vk.vkResetFences(m_device, 1, &m_fence);
        m_vk.vkQueueSubmit(m_queue, 1, &si, m_fence);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &m_renderSem;
        pi.swapchainCount = 1;
        pi.pSwapchains = &m_swapchain;
        pi.pImageIndices = &m_swapIndex;
        VkResult r = m_vk.vkQueuePresentKHR(m_queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) m_swapDirty = true;
        m_vk.vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);
        m_acquired = false;
        m_ringHead = 0;
        for (auto p : m_descPools) m_vk.vkResetDescriptorPool(m_device, p, 0);
        m_descPool = 0;
        ++m_frame;
        collectGarbage();
        return;
    }
    flush(false);
}

void VulkanDevice::shutdown() {
    if (!m_device) {
        if (m_instance && m_vk.vkDestroyInstance) m_vk.vkDestroyInstance(m_instance, nullptr);
        return;
    }
    if (m_recording) {
        m_vk.vkEndCommandBuffer(m_cb);
        m_recording = false;
    }
    m_vk.vkDeviceWaitIdle(m_device);
    destroySwapchain();
    for (auto& [id, m] : m_meshes) freeMesh(m);
    for (auto& [id, t] : m_textures) freeTex(t);
    for (auto& [id, s] : m_shaders) freeShader(s);
    for (auto& [k, p] : m_pipelines) m_vk.vkDestroyPipeline(m_device, p, nullptr);
    for (auto& [k, s] : m_samplers) m_vk.vkDestroySampler(m_device, s, nullptr);
    collectGarbage();
    freeBuf(m_ring);
    for (auto p : m_descPools) m_vk.vkDestroyDescriptorPool(m_device, p, nullptr);
    m_vk.vkDestroySemaphore(m_device, m_acquireSem, nullptr);
    m_vk.vkDestroySemaphore(m_device, m_renderSem, nullptr);
    m_vk.vkDestroyFence(m_device, m_fence, nullptr);
    m_vk.vkDestroyCommandPool(m_device, m_pool, nullptr);
    m_vk.vkDestroyDevice(m_device, nullptr);
    m_device = VK_NULL_HANDLE;
    if (m_surface) m_vk.vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    m_vk.vkDestroyInstance(m_instance, nullptr);
    m_instance = VK_NULL_HANDLE;
}

} // namespace
#endif

std::unique_ptr<RenderDevice> createVulkanDevice(const DeviceCreateInfo& info) {
#ifdef _WIN32
    auto dev = std::make_unique<VulkanDevice>();
    std::string err;
    if (dev->init(info, err)) return dev;
    FLEDGE_LOG_WARN("rhi", "Vulkan unavailable: {}", err);
#else
    (void)info;
    FLEDGE_LOG_WARN("rhi", "Vulkan unavailable: only implemented on Windows");
#endif
    return nullptr;
}

} // namespace fledge::rhi
