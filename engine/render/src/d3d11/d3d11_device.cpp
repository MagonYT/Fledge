// Direct3D 11 RHI backend - full pipeline (feature level 11_0, SM 5.0).
// The same code drives "Direct3D 10": the D3D11 runtime at feature level 10_1 / 10_0 with
// SM 4.0 shaders (no MSAA depth reads, max 4x MSAA, no ray tracing, no HDR10 output).
//
// d3d11.dll / dxgi.dll / d3dcompiler_47.dll are loaded at runtime.
#ifdef _WIN32
#include "../d3d/d3d_common.h"

#include <d3d11_1.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "fledge/core/log.h"
#include "fledge/render/rhi.h"

namespace fledge::rhi {

namespace {
using d3d::Com;

using PFN_D3D11CreateDevice_ = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

constexpr int kMaxSlots = 16;
constexpr UINT kParamsSize = 4096;

struct Mesh11 {
    Com<ID3D11Buffer> vb, tb, ib;
    UINT count = 0, vertexCount = 0, capacity = 0;
    bool indexed = true, lines = false;
};
struct Tex11 {
    Com<ID3D11Resource> res;
    Com<ID3D11ShaderResourceView> srv;
    TextureDesc desc;
    std::map<std::pair<int, int>, Com<ID3D11RenderTargetView>> rtvs;
    std::map<std::pair<int, int>, Com<ID3D11DepthStencilView>> dsvs;
};
struct Fb11 {
    std::array<ID3D11RenderTargetView*, 4> rtv{};
    ID3D11DepthStencilView* dsv = nullptr;
    int count = 0;
    FramebufferDesc desc;
};
struct Shader11 {
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    std::vector<uint8_t> vsBlob;
    std::unordered_map<std::string, d3d::CbVar> vars;
    std::vector<uint8_t> staging;
    Com<ID3D11InputLayout> meshLayout, lineLayout;
    bool triedMesh = false, triedLine = false;
};
struct Rt11 {
    TextureHandle color, depth;
    FramebufferHandle fb;
    int w = 0, h = 0;
};

class D3D11Device final : public RenderDevice {
public:
    bool init(const DeviceCreateInfo& info, bool fl10) {
        m_fl10 = fl10;
        m_hwnd = static_cast<HWND>(info.nativeWindow);
        m_width = std::max(1, info.width);
        m_height = std::max(1, info.height);
        m_vsync = info.vsync;
        m_wantTearing = info.allowTearing;
        if (!m_hwnd) return fail("no native window handle");
        m_d3d11 = LoadLibraryW(L"d3d11.dll");
        m_dxgi = LoadLibraryW(L"dxgi.dll");
        if (!m_d3d11 || !m_dxgi || !m_compiler.load()) return fail("d3d11.dll / dxgi.dll / d3dcompiler_47.dll not found");
        auto create = reinterpret_cast<PFN_D3D11CreateDevice_>(reinterpret_cast<void*>(GetProcAddress(m_d3d11, "D3D11CreateDevice")));
        auto createFactory = reinterpret_cast<d3d::PFN_CreateDXGIFactory2>(reinterpret_cast<void*>(GetProcAddress(m_dxgi, "CreateDXGIFactory2")));
        if (!create || !createFactory) return fail("missing D3D11/DXGI entry points");
        if (FAILED(createFactory(0, __uuidof(IDXGIFactory2), m_factory.putVoid()))) return fail("CreateDXGIFactory2 failed");
        D3D_FEATURE_LEVEL levels11[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL levels10[] = {D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        if (std::getenv("FLEDGE_D3D_DEBUG")) flags |= D3D11_CREATE_DEVICE_DEBUG;
        Com<IDXGIAdapter1> adapter;
        for (UINT i = 0; m_factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 ad;
            adapter->GetDesc1(&ad);
            if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            D3D_FEATURE_LEVEL got{};
            HRESULT hr = create(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, fl10 ? levels10 : levels11, 2, D3D11_SDK_VERSION,
                                m_device.put(), &got, m_ctx.put());
            if (FAILED(hr) && !fl10)
                hr = create(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels11 + 1, 1, D3D11_SDK_VERSION, m_device.put(), &got,
                            m_ctx.put());
            if (SUCCEEDED(hr)) {
                char name[128];
                WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
                m_level = got;
                m_fl = got >= D3D_FEATURE_LEVEL_11_0 ? "11_0" : got == D3D_FEATURE_LEVEL_10_1 ? "10_1" : "10_0";
                m_desc = std::string(fl10 ? "Direct3D 10 (D3D11 runtime, feature level " : "Direct3D 11 (feature level ") + m_fl + ") / " + name;
                break;
            }
        }
        if (!m_device) return fail("no Direct3D 11 capable adapter");
        // MSAA support for the HDR target
        for (UINT s : {8u, 4u, 2u}) {
            UINT q = 0;
            if (SUCCEEDED(m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_R16G16B16A16_FLOAT, s, &q)) && q > 0) {
                m_maxSamples = static_cast<int>(s);
                break;
            }
        }
        if (fl10) m_maxSamples = std::min(m_maxSamples, 4);
        Com<IDXGIFactory5> f5;
        if (SUCCEEDED(m_factory->QueryInterface(__uuidof(IDXGIFactory5), f5.putVoid()))) {
            BOOL allow = FALSE;
            if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) m_tearingSupported = allow;
        }
        if (!createSwapchain(info.hdr)) return false;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kParamsSize;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_device->CreateBuffer(&bd, nullptr, m_params.put()))) return fail("params buffer");
        FLEDGE_LOG_INFO("rhi", "{}{}", m_desc, m_hdr ? " [HDR10 output]" : "");
        return true;
    }

    ~D3D11Device() override {
        if (m_ctx) m_ctx->ClearState();
    }

    Backend backend() const override { return m_fl10 ? Backend::D3D10 : Backend::D3D11; }
    std::string description() const override { return m_desc; }
    DeviceCaps caps() const override {
        DeviceCaps c;
        c.fullPipeline = true;
        c.instancing = true;  // cbuffer Instances at b2 (binding 1)
        c.bcTextures = true;
        c.maxSamples = m_maxSamples;
        c.depthZeroToOne = true;
        c.rayTracing = false;
        c.hdrOutput = m_hdrCapable;
        c.featureLevel = m_fl + (!m_fl10 ? " / SM 5.0" : m_level >= D3D_FEATURE_LEVEL_10_1 ? " / SM 4.1" : " / SM 4.0 (no shadows)");
        return c;
    }
    void* nativeDevice() const override { return m_device.get(); }
    void* nativeContext() const override { return m_ctx.get(); }
    bool hdrActive() const override { return m_hdr; }
    void setVsync(bool v, bool tearing) override {
        m_vsync = v;
        m_wantTearing = tearing;
    }

    // ------------------------------------------------------------ meshes ---
    MeshHandle createMesh(const MeshData& d) override {
        if (d.empty()) return {};
        Mesh11 m;
        if (!makeBuffer(D3D11_BIND_VERTEX_BUFFER, d.vertices.data(), static_cast<UINT>(d.vertices.size() * sizeof(Vertex)), m.vb)) return {};
        std::vector<Vec4> zero;
        const void* tdata = d.tangents.data();
        if (d.tangents.size() != d.vertices.size()) {
            zero.assign(d.vertices.size(), Vec4{1, 0, 0, 1});
            tdata = zero.data();
        }
        if (!makeBuffer(D3D11_BIND_VERTEX_BUFFER, tdata, static_cast<UINT>(d.vertices.size() * sizeof(Vec4)), m.tb)) return {};
        if (!makeBuffer(D3D11_BIND_INDEX_BUFFER, d.indices.data(), static_cast<UINT>(d.indices.size() * sizeof(uint32_t)), m.ib)) return {};
        m.count = static_cast<UINT>(d.indices.size());
        m.vertexCount = static_cast<UINT>(d.vertices.size());
        return MeshHandle{add(m_meshes, std::move(m))};
    }
    MeshHandle createLineMesh(std::span<const LineVertex> lines) override {
        Mesh11 m;
        m.indexed = false;
        m.lines = true;
        MeshHandle h = MeshHandle{add(m_meshes, std::move(m))};
        updateLineMesh(h, lines);
        return h;
    }
    void updateLineMesh(MeshHandle h, std::span<const LineVertex> lines) override {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end()) return;
        Mesh11& m = it->second;
        m.count = static_cast<UINT>(lines.size());
        if (lines.empty()) return;
        if (m.capacity < lines.size()) {
            m.capacity = static_cast<UINT>(std::max<size_t>(lines.size(), 1024));
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = static_cast<UINT>(m.capacity * sizeof(LineVertex));
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            m.vb.reset();
            if (FAILED(m_device->CreateBuffer(&bd, nullptr, m.vb.put()))) return;
        }
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (SUCCEEDED(m_ctx->Map(m.vb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, lines.data(), lines.size() * sizeof(LineVertex));
            m_ctx->Unmap(m.vb.get(), 0);
        }
    }
    void destroyMesh(MeshHandle h) override { m_meshes.erase(h.id); }

    // ----------------------------------------------------------- shaders ---
    ShaderHandle createShader(const ShaderDesc& desc) override {
        if (desc.hlsl.empty()) {
            FLEDGE_LOG_WARN("rhi", "D3D11: shader '{}' has no HLSL source - skipped", desc.name);
            return {};
        }
        Shader11 s;
        std::vector<uint8_t> ps;
        std::string err;
        // SM 5.0 at FL 11; SM 4.1 at FL 10_1; SM 4.0 at FL 10_0 (no comparison sampling of arrays -> no shadows)
        const char* vsTarget = !m_fl10 ? "vs_5_0" : m_level >= D3D_FEATURE_LEVEL_10_1 ? "vs_4_1" : "vs_4_0";
        const char* psTarget = !m_fl10 ? "ps_5_0" : m_level >= D3D_FEATURE_LEVEL_10_1 ? "ps_4_1" : "ps_4_0";
        std::string src = m_level < D3D_FEATURE_LEVEL_10_1 ? std::string("#define FLEDGE_FL10_0 1") + '\n' + desc.hlsl : desc.hlsl;
        if (!m_compiler.compileCached(desc.name, src, "VSMain", vsTarget, s.vsBlob, err) ||
            !m_compiler.compileCached(desc.name, src, "PSMain", psTarget, ps, err)) {
            FLEDGE_LOG_ERROR("rhi", "{}: HLSL compile failed for '{}': {}", m_fl10 ? "D3D10" : "D3D11", desc.name, err);
            return {};
        }
        UINT size = std::max(d3d::reflectParams(m_compiler, s.vsBlob, s.vars), d3d::reflectParams(m_compiler, ps, s.vars));
        s.staging.assign(std::max<UINT>(size, 16), 0);
        if (FAILED(m_device->CreateVertexShader(s.vsBlob.data(), s.vsBlob.size(), nullptr, s.vs.put())) ||
            FAILED(m_device->CreatePixelShader(ps.data(), ps.size(), nullptr, s.ps.put()))) {
            FLEDGE_LOG_ERROR("rhi", "D3D11: creating shader objects failed for '{}'", desc.name);
            return {};
        }
        return ShaderHandle{add(m_shaders, std::move(s))};
    }
    void destroyShader(ShaderHandle h) override {
        if (m_current == &m_shaders[h.id]) m_current = nullptr;
        m_shaders.erase(h.id);
    }

    // ---------------------------------------------------------- textures ---
    TextureHandle createTexture(const TextureDesc& d, std::span<const TextureSubresource> initial) override {
        Tex11 t;
        t.desc = d;
        bool depth = isDepthFormat(d.format);
        bool bc = d3d::isBlockCompressed(d.format);
        int layers = d.type == TextureType::Cube ? 6 : std::max(1, d.layers);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(std::max(1, d.width));
        td.Height = static_cast<UINT>(std::max(1, d.height));
        td.MipLevels = static_cast<UINT>(std::max(1, d.mips));
        td.ArraySize = static_cast<UINT>(layers);
        td.Format = depth ? d3d::dxgiTypeless(d.format) : d3d::dxgiFormat(d.format, d.srgb);
        td.SampleDesc.Count = static_cast<UINT>(std::max(1, d.samples));
        td.Usage = D3D11_USAGE_DEFAULT;
        bool srvOk = !(depth && d.samples > 1 && m_level < D3D_FEATURE_LEVEL_10_1);
        td.BindFlags = (srvOk ? D3D11_BIND_SHADER_RESOURCE : 0) | (depth ? D3D11_BIND_DEPTH_STENCIL : (bc ? 0 : D3D11_BIND_RENDER_TARGET));
        if (d.type == TextureType::Cube) td.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
        if (d.mips > 1 && !bc && !depth) td.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
        std::vector<D3D11_SUBRESOURCE_DATA> init;
        std::vector<std::vector<uint8_t>> converted(initial.size());
        bool complete = !initial.empty() && initial.size() == static_cast<size_t>(td.MipLevels) * static_cast<size_t>(layers);
        if (complete) {
            init.resize(initial.size());
            for (size_t k = 0; k < initial.size(); ++k) {
                auto& s = initial[k];
                UINT idx = static_cast<UINT>(s.mip) + static_cast<UINT>(s.layer) * td.MipLevels;
                if (idx >= init.size()) continue;
                UINT pitch, rows;
                d3d::uploadPitch(d.format, s.width, s.height, pitch, rows);
                const void* src = d3d::convertFloatUpload(d.format, s.data, s.size, s.width, s.height, converted[k]) ? converted[k].data() : s.data;
                init[idx] = {src, pitch, pitch * rows};
            }
        }
        Com<ID3D11Texture2D> tex;
        if (FAILED(m_device->CreateTexture2D(&td, complete ? init.data() : nullptr, tex.put()))) {
            FLEDGE_LOG_ERROR("rhi", "D3D11: CreateTexture2D failed ({} {}x{} x{} mips {} samples)", toString(d.format), d.width, d.height, layers,
                           d.samples);
            return {};
        }
        if (!complete && !initial.empty())  // partial initial data (e.g. mip 0 only)
            for (size_t k = 0; k < initial.size(); ++k) {
                auto& s = initial[k];
                UINT pitch, rows;
                d3d::uploadPitch(d.format, s.width, s.height, pitch, rows);
                const void* src = d3d::convertFloatUpload(d.format, s.data, s.size, s.width, s.height, converted[k]) ? converted[k].data() : s.data;
                m_ctx->UpdateSubresource(tex.get(), static_cast<UINT>(s.mip) + static_cast<UINT>(s.layer) * td.MipLevels, nullptr, src, pitch,
                                         pitch * rows);
            }
        if (srvOk) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = d3d::dxgiSrvFormat(d.format, d.srgb);
            if (d.samples > 1) {
                sv.ViewDimension = layers > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D11_SRV_DIMENSION_TEXTURE2DMS;
                sv.Texture2DMSArray.ArraySize = static_cast<UINT>(layers);
            } else if (d.type == TextureType::Cube) {
                sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
                sv.TextureCube.MipLevels = td.MipLevels;
            } else if (d.type == TextureType::Tex2DArray) {
                sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                sv.Texture2DArray.MipLevels = td.MipLevels;
                sv.Texture2DArray.ArraySize = static_cast<UINT>(layers);
            } else {
                sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sv.Texture2D.MipLevels = td.MipLevels;
            }
            if (FAILED(m_device->CreateShaderResourceView(tex.get(), &sv, t.srv.put()))) FLEDGE_LOG_WARN("rhi", "D3D11: SRV creation failed");
        }
        tex->QueryInterface(__uuidof(ID3D11Resource), t.res.putVoid());
        return TextureHandle{add(m_textures, std::move(t))};
    }
    void generateMips(TextureHandle h) override {
        auto it = m_textures.find(h.id);
        if (it != m_textures.end() && it->second.srv && it->second.desc.mips > 1) m_ctx->GenerateMips(it->second.srv.get());
    }
    bool updateTexture(TextureHandle h, const TextureSubresource& mip0) override {
        auto it = m_textures.find(h.id);
        if (it == m_textures.end() || !it->second.res || !mip0.data) return false;
        const TextureDesc& desc = it->second.desc;
        if (desc.type != TextureType::Tex2D || desc.format != TextureFormat::RGBA8 || desc.samples > 1) return false;
        if (mip0.width != desc.width || mip0.height != desc.height) return false;
        if (mip0.size < static_cast<size_t>(mip0.width) * static_cast<size_t>(mip0.height) * 4) return false;
        m_ctx->UpdateSubresource(it->second.res.get(), 0, nullptr, mip0.data, static_cast<UINT>(mip0.width * 4), 0);
        return true;
    }
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
    bool supportsTextureFormat(TextureFormat) const override { return true; }
    void destroyTexture(TextureHandle h) override {
        unbindAll();
        m_textures.erase(h.id);
    }
    uint64_t nativeTexture(TextureHandle h) const override {
        auto it = m_textures.find(h.id);
        return it == m_textures.end() || !it->second.srv ? 0 : reinterpret_cast<uint64_t>(it->second.srv.get());
    }

    // ------------------------------------------------------ framebuffers ---
    FramebufferHandle createFramebuffer(const FramebufferDesc& d) override {
        Fb11 f;
        f.desc = d;
        f.count = d.colorCount;
        for (int i = 0; i < d.colorCount; ++i) f.rtv[static_cast<size_t>(i)] = rtvFor(d.colors[static_cast<size_t>(i)]);
        if (d.depth.texture) f.dsv = dsvFor(d.depth);
        return FramebufferHandle{add(m_fbs, std::move(f))};
    }
    void destroyFramebuffer(FramebufferHandle h) override { m_fbs.erase(h.id); }
    void resolve(FramebufferHandle src, FramebufferHandle dst, int w, int h) override {
        (void)w;
        (void)h;
        auto s = m_fbs.find(src.id);
        if (s == m_fbs.end() || s->second.desc.colorCount == 0) return;
        Tex11* st = tex(s->second.desc.colors[0].texture);
        ID3D11Resource* dstRes = nullptr;
        UINT dstSub = 0;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        if (dst) {
            auto d = m_fbs.find(dst.id);
            if (d == m_fbs.end() || d->second.desc.colorCount == 0) return;
            Tex11* dt = tex(d->second.desc.colors[0].texture);
            if (!dt) return;
            dstRes = dt->res.get();
            dstSub = static_cast<UINT>(d->second.desc.colors[0].mip) + static_cast<UINT>(d->second.desc.colors[0].layer) * static_cast<UINT>(dt->desc.mips);
            fmt = d3d::dxgiFormat(dt->desc.format, dt->desc.srgb);
        } else {
            dstRes = m_backbuffer.get();
            fmt = m_backFormat;
        }
        if (!st || !dstRes) return;
        unbindAll();
        if (st->desc.samples > 1) m_ctx->ResolveSubresource(dstRes, dstSub, st->res.get(), 0, fmt);
        else m_ctx->CopySubresourceRegion(dstRes, dstSub, 0, 0, 0, st->res.get(), 0, nullptr);
    }

    // ------------------------------------------- legacy render targets ---
    RenderTargetHandle createRenderTarget(int w, int h) override {
        Rt11 r;
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
    uint64_t nativeColorTexture(RenderTargetHandle h) const override {
        auto it = m_rts.find(h.id);
        return it == m_rts.end() ? 0 : nativeTexture(it->second.color);
    }
    FramebufferHandle framebufferOf(RenderTargetHandle h) const override {
        auto it = m_rts.find(h.id);
        return it == m_rts.end() ? FramebufferHandle{} : it->second.fb;
    }

    // ------------------------------------------------- uniform buffers ---
    BufferHandle createUniformBuffer(size_t bytes) override {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>((bytes + 15) & ~size_t(15));
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Com<ID3D11Buffer> b;
        if (FAILED(m_device->CreateBuffer(&bd, nullptr, b.put()))) return {};
        return BufferHandle{add(m_buffers, std::move(b))};
    }
    void updateBuffer(BufferHandle h, const void* data, size_t bytes, size_t offset) override {
        auto it = m_buffers.find(h.id);
        if (it == m_buffers.end()) return;
        if (offset == 0) {
            D3D11_BUFFER_DESC bd;
            it->second->GetDesc(&bd);
            if (bytes >= bd.ByteWidth || bytes == bd.ByteWidth) {
                m_ctx->UpdateSubresource(it->second.get(), 0, nullptr, data, 0, 0);
                return;
            }
            std::vector<uint8_t> tmp(bd.ByteWidth, 0);
            std::memcpy(tmp.data(), data, bytes);
            m_ctx->UpdateSubresource(it->second.get(), 0, nullptr, tmp.data(), 0, 0);
            return;
        }
        D3D11_BOX box{static_cast<UINT>(offset), 0, 0, static_cast<UINT>(offset + bytes), 1, 1};
        m_ctx->UpdateSubresource(it->second.get(), 0, &box, data, 0, 0);
    }
    void bindUniformBuffer(int binding, BufferHandle h) override {
        auto it = m_buffers.find(h.id);
        ID3D11Buffer* b = it == m_buffers.end() ? nullptr : it->second.get();
        UINT slot = static_cast<UINT>(binding == 0 ? 0 : binding + 1);  // b1 is reserved for Params
        m_ctx->VSSetConstantBuffers(slot, 1, &b);
        m_ctx->PSSetConstantBuffers(slot, 1, &b);
    }
    void destroyBuffer(BufferHandle h) override { m_buffers.erase(h.id); }

    // ------------------------------------------------------------ passes ---
    void beginPass(FramebufferHandle fb, int x, int y, int w, int h, const ClearDesc& c) override {
        unbindAll();
        std::array<ID3D11RenderTargetView*, 4> rtv{};
        ID3D11DepthStencilView* dsv = nullptr;
        int count = 1;
        if (fb) {
            auto it = m_fbs.find(fb.id);
            if (it == m_fbs.end()) return;
            rtv = it->second.rtv;
            dsv = it->second.dsv;
            count = it->second.count;
        } else {
            rtv[0] = m_backRtv.get();
            dsv = m_backDsv.get();
        }
        m_ctx->OMSetRenderTargets(static_cast<UINT>(count), rtv.data(), dsv);
        if (c.color) {
            float col[4] = {c.colorValue.x, c.colorValue.y, c.colorValue.z, c.colorValue.w};
            for (int i = 0; i < count; ++i)
                if (rtv[static_cast<size_t>(i)]) m_ctx->ClearRenderTargetView(rtv[static_cast<size_t>(i)], col);
        }
        if (c.depth && dsv) m_ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, c.depthValue, 0);
        D3D11_VIEWPORT vp{static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        m_ctx->RSSetViewports(1, &vp);
        m_depthTest = m_depthWrite = true;
        m_depthFunc = DepthFunc::Less;
        m_blend = BlendMode::None;
        ++m_stats.passes;
    }
    void beginPass(RenderTargetHandle rt, int w, int h, Vec4 clearColor) override {
        ClearDesc cd;
        cd.colorValue = clearColor;
        beginPass(rt ? framebufferOf(rt) : FramebufferHandle{}, 0, 0, w, h, cd);
    }
    void endPass() override {}

    // ------------------------------------------------------------- state ---
    void setShader(ShaderHandle h) override {
        auto it = m_shaders.find(h.id);
        m_current = it == m_shaders.end() ? nullptr : &it->second;
    }
    void setRaw(const char* name, const void* data, UINT size) {
        if (!m_current) return;
        auto it = m_current->vars.find(name);
        if (it == m_current->vars.end()) return;
        std::memcpy(m_current->staging.data() + it->second.offset, data, std::min(size, it->second.size));
    }
    void setUniform(const char* n, const Mat4& m) override { setRaw(n, m.data(), 64); }
    void setUniform(const char* n, Vec3 v) override { setRaw(n, &v.x, 12); }
    void setUniform(const char* n, Vec4 v) override { setRaw(n, &v.x, 16); }
    void setUniform(const char* n, float f) override { setRaw(n, &f, 4); }
    void setUniform(const char* n, int i) override { setRaw(n, &i, 4); }
    void bindTexture(int slot, TextureHandle h) override {
        if (slot < 0 || slot >= kMaxSlots) return;
        Tex11* t = tex(h);
        ID3D11ShaderResourceView* srv = t ? t->srv.get() : nullptr;
        ID3D11SamplerState* smp = t ? sampler(t->desc.sampler) : sampler(SamplerDesc{});
        m_ctx->PSSetShaderResources(static_cast<UINT>(slot), 1, &srv);
        m_ctx->PSSetSamplers(static_cast<UINT>(slot), 1, &smp);
        if (slot == 14) m_ctx->VSSetShaderResources(14, 1, &srv);  // vertex texture fetch (GPU skinning data)
        m_boundSrvs = std::max(m_boundSrvs, slot + 1);
    }
    void setDepthTest(bool e) override { m_depthTest = e; }
    void setDepthWrite(bool e) override { m_depthWrite = e; }
    void setDepthFunc(DepthFunc f) override { m_depthFunc = f; }
    void setBlendMode(BlendMode m) override { m_blend = m; }
    void setCullMode(CullMode m) override { m_cull = m; }
    void setWireframe(bool on) override { m_wire = on; }

    void draw(MeshHandle h, Topology t) override {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end() || !m_current) return;
        Mesh11& m = it->second;
        if (m.count == 0 || !m.vb) return;
        if (!applyState(m.lines)) return;
        m_ctx->IASetPrimitiveTopology(t == Topology::Lines ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (m.lines) {
            UINT stride = sizeof(LineVertex), off = 0;
            ID3D11Buffer* vb = m.vb.get();
            m_ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
            m_ctx->Draw(m.count, 0);
            m_stats.lines += m.count / 2;
        } else {
            bindMeshBuffers(m);
            m_ctx->DrawIndexed(m.count, 0, 0);
            m_stats.triangles += m.count / 3;
        }
        ++m_stats.drawCalls;
    }
    void drawInstanced(MeshHandle h, int instances) override {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end() || !m_current || it->second.lines) return;
        if (!applyState(false)) return;
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        bindMeshBuffers(it->second);
        m_ctx->DrawIndexedInstanced(it->second.count, static_cast<UINT>(instances), 0, 0, 0);
        ++m_stats.drawCalls;
        m_stats.triangles += it->second.count / 3 * static_cast<uint32_t>(instances);
    }
    void drawFullscreenTriangle() override {
        if (!m_current || !applyState(false, true)) return;
        m_ctx->IASetInputLayout(nullptr);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->Draw(3, 0);
        ++m_stats.drawCalls;
    }

    // ---------------------------------------------------------- readback ---
    bool readPixels(RenderTargetHandle rt, int w, int h, std::vector<uint8_t>& rgba) override {
        if (rt) return readFramebuffer(framebufferOf(rt), w, h, rgba);
        return readResource(m_backbuffer.get(), 0, m_backFormat, w, h, rgba);
    }
    bool readFramebuffer(FramebufferHandle fb, int w, int h, std::vector<uint8_t>& rgba) override {
        if (!fb) return readPixels({}, w, h, rgba);
        auto it = m_fbs.find(fb.id);
        if (it == m_fbs.end() || it->second.desc.colorCount == 0) return false;
        Tex11* t = tex(it->second.desc.colors[0].texture);
        if (!t || t->desc.format != TextureFormat::RGBA8) return false;
        return readResource(t->res.get(), static_cast<UINT>(it->second.desc.colors[0].mip), d3d::dxgiFormat(t->desc.format, t->desc.srgb), w, h, rgba);
    }

    FrameStats& stats() override { return m_stats; }

    void present() override {
        UINT flags = (!m_vsync && m_wantTearing && m_tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;
        m_swapchain->Present(m_vsync ? 1 : 0, flags);
    }
    void resizeSwapchain(int w, int h) override {
        if (w <= 0 || h <= 0 || (w == m_width && h == m_height)) return;
        m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        m_backRtv.reset();
        m_backDsv.reset();
        m_backbuffer.reset();
        m_backDepth.reset();
        m_ctx->Flush();
        if (FAILED(m_swapchain->ResizeBuffers(2, static_cast<UINT>(w), static_cast<UINT>(h), m_backFormat,
                                              m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0))) {
            FLEDGE_LOG_ERROR("rhi", "D3D11: ResizeBuffers failed");
            return;
        }
        m_width = w;
        m_height = h;
        createBackbufferViews();
    }

private:
    template <typename M, typename T>
    uint32_t add(M& map, T&& v) {
        uint32_t id = m_next++;
        map.emplace(id, std::forward<T>(v));
        return id;
    }
    bool fail(const std::string& msg) {
        FLEDGE_LOG_ERROR("rhi", "{} init: {}", m_fl10 ? "D3D10" : "D3D11", msg);
        return false;
    }
    bool makeBuffer(UINT bind, const void* data, UINT size, Com<ID3D11Buffer>& out) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = std::max<UINT>(size, 16);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA sd{data, 0, 0};
        return SUCCEEDED(m_device->CreateBuffer(&bd, size ? &sd : nullptr, out.put()));
    }
    bool createSwapchain(bool hdr) {
        m_backFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = static_cast<UINT>(m_width);
        sd.Height = static_cast<UINT>(m_height);
        sd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        Com<IDXGISwapChain1> sc;
        // HDR10: R10G10B10A2 + ST.2084 / BT.2020 colour space when the output supports it
        if (SUCCEEDED(m_factory->CreateSwapChainForHwnd(m_device.get(), m_hwnd, &sd, nullptr, nullptr, sc.put()))) {
            Com<IDXGISwapChain3> sc3;
            UINT support = 0;
            if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), sc3.putVoid())) &&
                SUCCEEDED(sc3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &support)) &&
                (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
                m_hdrCapable = !m_fl10;
            if (hdr && m_hdrCapable && SUCCEEDED(sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020))) {
                m_hdr = true;
                m_backFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
            }
            if (!m_hdr) sc.reset();  // recreate as plain 8-bit SDR
        }
        if (!m_hdr) {
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            if (FAILED(m_factory->CreateSwapChainForHwnd(m_device.get(), m_hwnd, &sd, nullptr, nullptr, sc.put())))
                return fail("CreateSwapChainForHwnd failed");
        }
        m_swapchain = std::move(sc);
        m_factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
        return createBackbufferViews();
    }
    bool createBackbufferViews() {
        if (FAILED(m_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), m_backbuffer.putVoid()))) return fail("GetBuffer failed");
        if (FAILED(m_device->CreateRenderTargetView(m_backbuffer.get(), nullptr, m_backRtv.put()))) return fail("backbuffer RTV failed");
        D3D11_TEXTURE2D_DESC dd{};
        dd.Width = static_cast<UINT>(m_width);
        dd.Height = static_cast<UINT>(m_height);
        dd.MipLevels = 1;
        dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.SampleDesc.Count = 1;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(m_device->CreateTexture2D(&dd, nullptr, m_backDepth.put()))) return fail("depth buffer");
        if (FAILED(m_device->CreateDepthStencilView(m_backDepth.get(), nullptr, m_backDsv.put()))) return fail("DSV");
        return true;
    }
    Tex11* tex(TextureHandle h) {
        auto it = m_textures.find(h.id);
        return it == m_textures.end() ? nullptr : &it->second;
    }
    ID3D11RenderTargetView* rtvFor(const Attachment& a) {
        Tex11* t = tex(a.texture);
        if (!t) return nullptr;
        auto key = std::make_pair(a.mip, a.layer);
        if (auto it = t->rtvs.find(key); it != t->rtvs.end()) return it->second.get();
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = d3d::dxgiFormat(t->desc.format, t->desc.srgb);
        if (t->desc.samples > 1) {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
        } else if (t->desc.type == TextureType::Tex2D) {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rd.Texture2D.MipSlice = static_cast<UINT>(a.mip);
        } else {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rd.Texture2DArray.MipSlice = static_cast<UINT>(a.mip);
            rd.Texture2DArray.FirstArraySlice = static_cast<UINT>(a.layer);
            rd.Texture2DArray.ArraySize = 1;
        }
        Com<ID3D11RenderTargetView> v;
        if (FAILED(m_device->CreateRenderTargetView(t->res.get(), &rd, v.put()))) return nullptr;
        return (t->rtvs[key] = std::move(v)).get();
    }
    ID3D11DepthStencilView* dsvFor(const Attachment& a) {
        Tex11* t = tex(a.texture);
        if (!t) return nullptr;
        auto key = std::make_pair(a.mip, a.layer);
        if (auto it = t->dsvs.find(key); it != t->dsvs.end()) return it->second.get();
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        if (t->desc.samples > 1) {
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        } else if (t->desc.type == TextureType::Tex2D) {
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            dd.Texture2D.MipSlice = static_cast<UINT>(a.mip);
        } else {
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            dd.Texture2DArray.MipSlice = static_cast<UINT>(a.mip);
            dd.Texture2DArray.FirstArraySlice = static_cast<UINT>(a.layer);
            dd.Texture2DArray.ArraySize = 1;
        }
        Com<ID3D11DepthStencilView> v;
        if (FAILED(m_device->CreateDepthStencilView(t->res.get(), &dd, v.put()))) return nullptr;
        return (t->dsvs[key] = std::move(v)).get();
    }
    void makeRt(Rt11& r, int w, int h) {
        r.w = std::max(1, w);
        r.h = std::max(1, h);
        TextureDesc cd;
        cd.width = r.w;
        cd.height = r.h;
        cd.sampler = {Filter::Linear, false, Wrap::Clamp, false};
        r.color = createTexture(cd, {});
        TextureDesc dd = cd;
        dd.format = TextureFormat::Depth32F;
        r.depth = createTexture(dd, {});
        FramebufferDesc fd;
        fd.colorCount = 1;
        fd.colors[0] = {r.color, 0, 0};
        fd.depth = {r.depth, 0, 0};
        r.fb = createFramebuffer(fd);
    }
    void freeRt(Rt11& r) {
        destroyFramebuffer(r.fb);
        destroyTexture(r.color);
        destroyTexture(r.depth);
    }
    void unbindAll() {
        if (m_boundSrvs == 0) return;
        ID3D11ShaderResourceView* nulls[kMaxSlots] = {};
        m_ctx->PSSetShaderResources(0, static_cast<UINT>(m_boundSrvs), nulls);
        m_boundSrvs = 0;
    }
    ID3D11SamplerState* sampler(const SamplerDesc& d) {
        auto key = std::make_tuple(static_cast<int>(d.filter), d.mipmaps, static_cast<int>(d.wrap), d.depthCompare);
        if (auto it = m_samplers.find(key); it != m_samplers.end()) return it->second.get();
        D3D11_SAMPLER_DESC sd{};
        bool lin = d.filter == Filter::Linear;
        if (d.depthCompare) sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        else sd.Filter = lin ? (d.mipmaps ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT) : D3D11_FILTER_MIN_MAG_MIP_POINT;
        D3D11_TEXTURE_ADDRESS_MODE am = d.wrap == Wrap::Repeat ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressU = sd.AddressV = sd.AddressW = am;
        sd.ComparisonFunc = d.depthCompare ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_NEVER;
        sd.MaxLOD = d.mipmaps ? D3D11_FLOAT32_MAX : 0.0f;
        sd.MaxAnisotropy = 1;
        Com<ID3D11SamplerState> s;
        m_device->CreateSamplerState(&sd, s.put());
        return (m_samplers[key] = std::move(s)).get();
    }
    ID3D11InputLayout* layoutFor(Shader11& s, bool lines) {
        bool& tried = lines ? s.triedLine : s.triedMesh;
        Com<ID3D11InputLayout>& l = lines ? s.lineLayout : s.meshLayout;
        if (tried) return l.get();
        tried = true;
        if (lines) {
            D3D11_INPUT_ELEMENT_DESC e[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                            {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
            m_device->CreateInputLayout(e, 2, s.vsBlob.data(), s.vsBlob.size(), l.put());
        } else {
            D3D11_INPUT_ELEMENT_DESC e[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                            {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
            m_device->CreateInputLayout(e, 4, s.vsBlob.data(), s.vsBlob.size(), l.put());
        }
        return l.get();
    }
    void bindMeshBuffers(Mesh11& m) {
        ID3D11Buffer* vbs[2] = {m.vb.get(), m.tb.get()};
        UINT strides[2] = {sizeof(Vertex), sizeof(Vec4)}, offs[2] = {0, 0};
        m_ctx->IASetVertexBuffers(0, 2, vbs, strides, offs);
        m_ctx->IASetIndexBuffer(m.ib.get(), DXGI_FORMAT_R32_UINT, 0);
    }
    bool applyState(bool lines, bool noInput = false) {
        Shader11& s = *m_current;
        if (!noInput) {
            ID3D11InputLayout* l = layoutFor(s, lines);
            if (!l) return false;
            m_ctx->IASetInputLayout(l);
        }
        m_ctx->VSSetShader(s.vs.get(), nullptr, 0);
        m_ctx->PSSetShader(s.ps.get(), nullptr, 0);
        // per-draw Params (b1)
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (SUCCEEDED(m_ctx->Map(m_params.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, s.staging.data(), std::min<size_t>(s.staging.size(), kParamsSize));
            m_ctx->Unmap(m_params.get(), 0);
        }
        ID3D11Buffer* pb = m_params.get();
        m_ctx->VSSetConstantBuffers(1, 1, &pb);
        m_ctx->PSSetConstantBuffers(1, 1, &pb);
        // rasterizer
        auto rkey = std::make_pair(static_cast<int>(m_cull), m_wire);
        auto rit = m_raster.find(rkey);
        if (rit == m_raster.end()) {
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = m_wire ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
            rd.CullMode = m_cull == CullMode::Back ? D3D11_CULL_BACK : m_cull == CullMode::Front ? D3D11_CULL_FRONT : D3D11_CULL_NONE;
            rd.FrontCounterClockwise = TRUE;  // engine meshes are CCW (GL convention)
            rd.DepthClipEnable = TRUE;
            rd.MultisampleEnable = TRUE;
            Com<ID3D11RasterizerState> r;
            m_device->CreateRasterizerState(&rd, r.put());
            rit = m_raster.emplace(rkey, std::move(r)).first;
        }
        m_ctx->RSSetState(rit->second.get());
        auto dkey = std::make_tuple(m_depthTest, m_depthWrite, static_cast<int>(m_depthFunc));
        auto dit = m_depth.find(dkey);
        if (dit == m_depth.end()) {
            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable = m_depthTest;
            dd.DepthWriteMask = m_depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.DepthFunc = m_depthFunc == DepthFunc::Less ? D3D11_COMPARISON_LESS : m_depthFunc == DepthFunc::LessEqual ? D3D11_COMPARISON_LESS_EQUAL
                                                                                                                      : D3D11_COMPARISON_ALWAYS;
            Com<ID3D11DepthStencilState> d;
            m_device->CreateDepthStencilState(&dd, d.put());
            dit = m_depth.emplace(dkey, std::move(d)).first;
        }
        m_ctx->OMSetDepthStencilState(dit->second.get(), 0);
        auto bit = m_blendStates.find(static_cast<int>(m_blend));
        if (bit == m_blendStates.end()) {
            D3D11_BLEND_DESC bd{};
            auto& rt = bd.RenderTarget[0];
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (m_blend != BlendMode::None) {
                rt.BlendEnable = TRUE;
                rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                if (m_blend == BlendMode::Alpha) {
                    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
                    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
                    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
                    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
                } else {
                    rt.SrcBlend = rt.DestBlend = rt.SrcBlendAlpha = rt.DestBlendAlpha = D3D11_BLEND_ONE;
                }
            }
            Com<ID3D11BlendState> b;
            m_device->CreateBlendState(&bd, b.put());
            bit = m_blendStates.emplace(static_cast<int>(m_blend), std::move(b)).first;
        }
        float factor[4] = {0, 0, 0, 0};
        m_ctx->OMSetBlendState(bit->second.get(), factor, 0xFFFFFFFFu);
        return true;
    }
    bool readResource(ID3D11Resource* res, UINT sub, DXGI_FORMAT fmt, int w, int h, std::vector<uint8_t>& rgba) {
        if (!res) return false;
        Com<ID3D11Texture2D> src;
        res->QueryInterface(__uuidof(ID3D11Texture2D), src.putVoid());
        D3D11_TEXTURE2D_DESC td{};
        src->GetDesc(&td);
        UINT mip = sub % std::max<UINT>(td.MipLevels, 1);
        int tw = static_cast<int>(std::max<UINT>(td.Width >> mip, 1)), th = static_cast<int>(std::max<UINT>(td.Height >> mip, 1));
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = static_cast<UINT>(tw);
        sd.Height = static_cast<UINT>(th);
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = td.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Texture2D> staging;
        if (FAILED(m_device->CreateTexture2D(&sd, nullptr, staging.put()))) return false;
        m_ctx->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, src.get(), sub, nullptr);
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (FAILED(m_ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &ms))) return false;
        std::vector<uint8_t> tmp;
        d3d::convertToRgba8(fmt, static_cast<const uint8_t*>(ms.pData), ms.RowPitch, std::min(w, tw), std::min(h, th), tmp);
        m_ctx->Unmap(staging.get(), 0);
        rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
        int cw = std::min(w, tw);
        for (int y = 0; y < std::min(h, th); ++y)
            std::memcpy(rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4, tmp.data() + static_cast<size_t>(y) * static_cast<size_t>(cw) * 4,
                        static_cast<size_t>(cw) * 4);
        return true;
    }

    bool m_fl10 = false;
    HMODULE m_d3d11 = nullptr, m_dxgi = nullptr;
    d3d::Compiler m_compiler;
    HWND m_hwnd = nullptr;
    int m_width = 1, m_height = 1;
    bool m_vsync = true, m_wantTearing = false, m_tearingSupported = false, m_hdr = false, m_hdrCapable = false;
    D3D_FEATURE_LEVEL m_level = D3D_FEATURE_LEVEL_11_0;
    std::string m_fl, m_desc = "D3D11";
    int m_maxSamples = 1;
    Com<IDXGIFactory2> m_factory;
    Com<ID3D11Device> m_device;
    Com<ID3D11DeviceContext> m_ctx;
    Com<IDXGISwapChain1> m_swapchain;
    DXGI_FORMAT m_backFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    Com<ID3D11Texture2D> m_backbuffer, m_backDepth;
    Com<ID3D11RenderTargetView> m_backRtv;
    Com<ID3D11DepthStencilView> m_backDsv;
    Com<ID3D11Buffer> m_params;
    std::unordered_map<uint32_t, Mesh11> m_meshes;
    std::unordered_map<uint32_t, Shader11> m_shaders;
    std::unordered_map<uint32_t, Tex11> m_textures;
    std::unordered_map<uint32_t, Fb11> m_fbs;
    std::unordered_map<uint32_t, Rt11> m_rts;
    std::unordered_map<uint32_t, Com<ID3D11Buffer>> m_buffers;
    std::map<std::tuple<int, bool, int, bool>, Com<ID3D11SamplerState>> m_samplers;
    std::map<std::pair<int, bool>, Com<ID3D11RasterizerState>> m_raster;
    std::map<std::tuple<bool, bool, int>, Com<ID3D11DepthStencilState>> m_depth;
    std::map<int, Com<ID3D11BlendState>> m_blendStates;
    Shader11* m_current = nullptr;
    bool m_depthTest = true, m_depthWrite = true, m_wire = false;
    DepthFunc m_depthFunc = DepthFunc::Less;
    BlendMode m_blend = BlendMode::None;
    CullMode m_cull = CullMode::Back;
    int m_boundSrvs = 0;
    uint32_t m_next = 1;
    FrameStats m_stats;
};

} // namespace

std::unique_ptr<RenderDevice> createD3D11Device(const DeviceCreateInfo& info, bool featureLevel10) {
    auto d = std::make_unique<D3D11Device>();
    if (!d->init(info, featureLevel10)) return nullptr;
    return d;
}

} // namespace fledge::rhi

#else
#include "fledge/core/log.h"
#include "fledge/render/rhi.h"
namespace fledge::rhi {
std::unique_ptr<RenderDevice> createD3D11Device(const DeviceCreateInfo&, bool) {
    FLEDGE_LOG_ERROR("rhi", "Direct3D is only available on Windows");
    return nullptr;
}
} // namespace fledge::rhi
#endif
