// Direct3D 9 RHI backend - REDUCED feature level (honest by design).
//
// What works: meshes, RGBA8 + BC1/BC3 textures, the basic lit forward shader (SM 3.0, compiled
// at runtime from an embedded source), legacy RGBA8 render targets, line meshes, readback,
// present/resize, vsync. caps().fullPipeline = false, so the renderer uses its basic path:
// no PBR/IBL, no shadows, no float targets, no post-processing, no MSAA, no HDR, no RT.
// d3d9.dll and d3dcompiler_47.dll are loaded at runtime.
#ifdef _WIN32
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "fledge/core/log.h"
#include "fledge/render/rhi.h"

namespace fledge::rhi {

namespace {

// Constant registers for the SM3 basic shader (vertex: c0-c11; pixel: c0-c3).
const char* kBasic9 = R"(
#ifdef FLEDGE_VS
float4x4 uModel : register(c0);
float4x4 uViewProj : register(c4);
#else
sampler2D uAlbedo : register(s0);
float4 uColor : register(c0);
float4 uAmbient : register(c1);
float4 uLightDir : register(c2);
float4 uLightColor : register(c3);
#endif
struct VSIn { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct PSIn { float4 pos : POSITION; float3 normal : TEXCOORD1; float2 uv : TEXCOORD0; float3 wpos : TEXCOORD2; };
#ifdef FLEDGE_VS
PSIn VSMain(VSIn i) {
    PSIn o;
    float4 wp = mul(uModel, float4(i.pos, 1.0));
    o.wpos = wp.xyz;
    o.pos = mul(uViewProj, wp);
    o.normal = mul(uModel, float4(i.normal, 0.0)).xyz;
    o.uv = i.uv;
    return o;
}
#else
float4 PSMain(PSIn i) : COLOR0 {
    float3 albedo = uColor.rgb * tex2D(uAlbedo, i.uv).rgb;
    // 1.3.3: zero-length / NaN / Inf normal -> the face normal from derivatives (screen y down: -cross faces the camera),
    // else straight up. SM 3 has no asuint: comparison tests (NaN fails every comparison here, fxc keeps them).
    float l2 = dot(i.normal, i.normal);
    float3 fn = -cross(ddx(i.wpos), ddy(i.wpos));
    float f2 = dot(fn, fn);
    float3 n = l2 > 1e-30 && l2 < 1e30 ? normalize(i.normal) : (f2 > 1e-30 && f2 < 1e30 ? normalize(fn) : float3(0.0, 1.0, 0.0));
    float ndl = max(dot(n, -normalize(uLightDir.xyz)), 0.0);
    float3 c = albedo * (uAmbient.rgb + uLightColor.rgb * ndl);
    return float4(pow(abs(c), 1.0 / 2.2), 1.0);
}
#endif
)";

const char* kLines9 = R"(
float4x4 uViewProj : register(c4);
struct VSIn { float3 pos : POSITION; float3 color : COLOR0; };
struct PSIn { float4 pos : POSITION; float3 color : TEXCOORD0; };
PSIn VSMain(VSIn i) { PSIn o; o.pos = mul(uViewProj, float4(i.pos, 1.0)); o.color = i.color; return o; }
float4 PSMain(PSIn i) : COLOR0 { return float4(pow(abs(i.color), 1.0 / 2.2), 1.0); }
)";

const char* kOverlay9 = R"(
#ifdef FLEDGE_VS
float4 uScreen : register(c12);
struct VSIn { float3 pos : POSITION; float3 color : NORMAL; float2 alpha : TEXCOORD0; };
struct PSIn { float4 pos : POSITION; float4 color : TEXCOORD0; float2 uv : TEXCOORD1; float browser : TEXCOORD2; };
PSIn VSMain(VSIn i) { PSIn o; o.color = float4(i.color, i.alpha.x); o.uv = float2(i.pos.z, i.alpha.y); o.browser = uScreen.z; o.pos = float4(i.pos.x / uScreen.x * 2.0 - 1.0 - 1.0 / uScreen.x, 1.0 - i.pos.y / uScreen.y * 2.0 + 1.0 / uScreen.y, 0.0, 1.0); return o; }
#else
sampler2D uOverlayTex : register(s0);  // UI font atlas (alpha = coverage) or white
struct PSIn { float4 pos : POSITION; float4 color : TEXCOORD0; float2 uv : TEXCOORD1; float browser : TEXCOORD2; };
float4 PSMain(PSIn i) : COLOR0 { float4 t = tex2D(uOverlayTex, i.uv); return float4(lerp(i.color.rgb, t.rgb, i.browser), i.color.a * t.a); }
#endif
)";

using PFN_Direct3DCreate9 = IDirect3D9*(WINAPI*)(UINT);

template <typename T>
void rel(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

struct Mesh9 {
    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DIndexBuffer9* ib = nullptr;
    UINT count = 0, vertices = 0, capacity = 0;
    bool lines = false;
};
struct Tex9 {
    IDirect3DTexture9* tex = nullptr;
    SamplerDesc sampler;
};
struct Shader9 {
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    bool lines = false;
};
struct Rt9 {
    IDirect3DTexture9* color = nullptr;
    IDirect3DSurface9* depth = nullptr;
    int w = 0, h = 0;
};

class D3D9Device final : public RenderDevice {
public:
    bool init(const DeviceCreateInfo& info) {
        m_hwnd = static_cast<HWND>(info.nativeWindow);
        if (!m_hwnd) return fail("no native window handle");
        m_dll = LoadLibraryW(L"d3d9.dll");
        m_compilerDll = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!m_dll || !m_compilerDll) return fail("d3d9.dll / d3dcompiler_47.dll not found");
        auto create = reinterpret_cast<PFN_Direct3DCreate9>(reinterpret_cast<void*>(GetProcAddress(m_dll, "Direct3DCreate9")));
        m_compile = reinterpret_cast<pD3DCompile>(reinterpret_cast<void*>(GetProcAddress(m_compilerDll, "D3DCompile")));
        if (!create || !m_compile) return fail("missing entry points");
        m_d3d = create(D3D_SDK_VERSION);
        if (!m_d3d) return fail("Direct3DCreate9 failed");
        D3DADAPTER_IDENTIFIER9 id{};
        m_d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
        m_desc = std::string("Direct3D 9 (reduced: basic forward path, SM 3.0) / ") + id.Description;
        m_width = std::max(1, info.width);
        m_height = std::max(1, info.height);
        m_vsync = info.vsync;
        fillPresent();
        HRESULT hr = m_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, m_hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
                                         &m_pp, &m_dev);
        if (FAILED(hr)) return fail("CreateDevice failed");
        D3DVERTEXELEMENT9 mesh[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                    {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
                                    {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                                    D3DDECL_END()};
        D3DVERTEXELEMENT9 line[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                    {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
                                    D3DDECL_END()};
        m_dev->CreateVertexDeclaration(mesh, &m_meshDecl);
        m_dev->CreateVertexDeclaration(line, &m_lineDecl);
        FLEDGE_LOG_INFO("rhi", "{}", m_desc);
        return true;
    }
    ~D3D9Device() override {
        for (auto& [i, m] : m_meshes) freeMesh(m);
        for (auto& [i, t] : m_textures) rel(t.tex);
        for (auto& [i, s] : m_shaders) {
            rel(s.vs);
            rel(s.ps);
        }
        for (auto& [i, r] : m_rts) freeRt(r);
        rel(m_meshDecl);
        rel(m_lineDecl);
        rel(m_dev);
        rel(m_d3d);
    }

    Backend backend() const override { return Backend::D3D9; }
    std::string description() const override { return m_desc; }
    DeviceCaps caps() const override {
        DeviceCaps c;
        c.fullPipeline = false;
        c.bcTextures = true;
        c.maxSamples = 1;
        c.depthZeroToOne = true;
        c.featureLevel = "9_3 / SM 3.0 (reduced)";
        return c;
    }
    void* nativeDevice() const override { return m_dev; }
    void setVsync(bool v, bool) override {
        if (v == m_vsync) return;
        m_vsync = v;
        reset();
    }

    MeshHandle createMesh(const MeshData& d) override {
        if (d.empty()) return {};
        Mesh9 m;
        UINT vbytes = static_cast<UINT>(d.vertices.size() * sizeof(Vertex)), ibytes = static_cast<UINT>(d.indices.size() * 4);
        if (FAILED(m_dev->CreateVertexBuffer(vbytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &m.vb, nullptr)) ||
            FAILED(m_dev->CreateIndexBuffer(ibytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_MANAGED, &m.ib, nullptr))) {
            freeMesh(m);
            return {};
        }
        void* p = nullptr;
        if (SUCCEEDED(m.vb->Lock(0, vbytes, &p, 0))) {
            std::memcpy(p, d.vertices.data(), vbytes);
            m.vb->Unlock();
        }
        if (SUCCEEDED(m.ib->Lock(0, ibytes, &p, 0))) {
            std::memcpy(p, d.indices.data(), ibytes);
            m.ib->Unlock();
        }
        m.count = static_cast<UINT>(d.indices.size());
        m.vertices = static_cast<UINT>(d.vertices.size());
        m_meshes[m_next] = m;
        return MeshHandle{m_next++};
    }
    MeshHandle createLineMesh(std::span<const LineVertex> lines) override {
        Mesh9 m;
        m.lines = true;
        m_meshes[m_next] = m;
        MeshHandle h{m_next++};
        updateLineMesh(h, lines);
        return h;
    }
    void updateLineMesh(MeshHandle h, std::span<const LineVertex> lines) override {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end()) return;
        Mesh9& m = it->second;
        m.count = static_cast<UINT>(lines.size());
        if (lines.empty()) return;
        if (m.capacity < lines.size()) {
            rel(m.vb);
            m.capacity = static_cast<UINT>(std::max<size_t>(lines.size(), 1024));
            if (FAILED(m_dev->CreateVertexBuffer(m.capacity * sizeof(LineVertex), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &m.vb, nullptr))) {
                m.capacity = 0;
                return;
            }
        }
        void* p = nullptr;
        if (SUCCEEDED(m.vb->Lock(0, static_cast<UINT>(lines.size() * sizeof(LineVertex)), &p, 0))) {
            std::memcpy(p, lines.data(), lines.size() * sizeof(LineVertex));
            m.vb->Unlock();
        }
    }
    void destroyMesh(MeshHandle h) override {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end()) return;
        freeMesh(it->second);
        m_meshes.erase(it);
    }

    // Only the renderer's "basic" and "lines" shaders exist at this feature level; their SM 3.0
    // sources are embedded here (desc.hlsl targets SM 4+ cbuffers).
    ShaderHandle createShader(const ShaderDesc& desc) override {
        const char* src = desc.name == "basic" ? kBasic9 : desc.name == "lines" ? kLines9 : desc.name == "overlay" ? kOverlay9 : nullptr;
        if (!src) {
            FLEDGE_LOG_WARN("rhi", "D3D9: shader '{}' is not available at this reduced feature level", desc.name);
            return {};
        }
        std::vector<uint8_t> vs, ps;
        if (!compile(src, "VSMain", "vs_3_0", vs) || !compile(src, "PSMain", "ps_3_0", ps)) return {};
        Shader9 s;
        s.lines = desc.name == "lines";
        if (FAILED(m_dev->CreateVertexShader(reinterpret_cast<const DWORD*>(vs.data()), &s.vs)) ||
            FAILED(m_dev->CreatePixelShader(reinterpret_cast<const DWORD*>(ps.data()), &s.ps))) {
            rel(s.vs);
            rel(s.ps);
            return {};
        }
        m_shaders[m_next] = s;
        return ShaderHandle{m_next++};
    }
    void destroyShader(ShaderHandle h) override {
        auto it = m_shaders.find(h.id);
        if (it == m_shaders.end()) return;
        rel(it->second.vs);
        rel(it->second.ps);
        if (m_current == h.id) m_current = 0;
        m_shaders.erase(it);
    }

    TextureHandle createTexture(const TextureDesc& d, std::span<const TextureSubresource> initial) override {
        if (d.type != TextureType::Tex2D || d.samples > 1 || (d.format != TextureFormat::RGBA8 && d.format != TextureFormat::BC1 && d.format != TextureFormat::BC3)) {
            FLEDGE_LOG_WARN("rhi", "D3D9: texture format/type '{}' not supported at this reduced feature level", toString(d.format));
            return {};
        }
        Tex9 t;
        t.sampler = d.sampler;
        D3DFORMAT fmt = d.format == TextureFormat::BC1 ? D3DFMT_DXT1 : d.format == TextureFormat::BC3 ? D3DFMT_DXT5 : D3DFMT_A8R8G8B8;
        bool autoMips = d.mips > 1 && d.format == TextureFormat::RGBA8 && initial.size() <= 1;
        UINT levels = autoMips ? 0 : static_cast<UINT>(std::max(1, d.mips));
        if (FAILED(m_dev->CreateTexture(static_cast<UINT>(d.width), static_cast<UINT>(d.height), levels, autoMips ? D3DUSAGE_AUTOGENMIPMAP : 0, fmt,
                                        D3DPOOL_MANAGED, &t.tex, nullptr)))
            return {};
        for (auto& s : initial) upload(t.tex, d.format, s);
        m_srgb[m_next] = d.srgb;
        m_textures[m_next] = t;
        return TextureHandle{m_next++};
    }
    void generateMips(TextureHandle h) override {
        auto it = m_textures.find(h.id);
        if (it != m_textures.end()) it->second.tex->GenerateMipSubLevels();
    }
    bool updateTexture(TextureHandle h, const TextureSubresource& mip0) override {
        auto it = m_textures.find(h.id);
        if (it == m_textures.end() || !it->second.tex || !mip0.data) return false;
        D3DSURFACE_DESC desc{};
        if (FAILED(it->second.tex->GetLevelDesc(0, &desc))) return false;
        if (desc.Width != static_cast<UINT>(mip0.width) || desc.Height != static_cast<UINT>(mip0.height)) return false;
        if (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8) return false;
        if (mip0.size < static_cast<size_t>(mip0.width) * static_cast<size_t>(mip0.height) * 4) return false;
        upload(it->second.tex, TextureFormat::RGBA8, mip0);
        return true;
    }
    TextureHandle createTexture2D(int w, int h, const uint8_t* rgba) override {
        TextureDesc d;
        d.width = w;
        d.height = h;
        d.srgb = true;
        d.mips = 2;  // autogen
        d.sampler = {Filter::Linear, true, Wrap::Repeat, false};
        TextureSubresource s{0, 0, w, h, rgba, static_cast<size_t>(w) * static_cast<size_t>(h) * 4};
        return createTexture(d, std::span(&s, 1));
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
        if (mips.size() == 1) d.mips = 1;
        return createTexture(d, subs);
    }
    bool supportsTextureFormat(TextureFormat f) const override {
        return f == TextureFormat::RGBA8 || f == TextureFormat::BC1 || f == TextureFormat::BC3;
    }
    void destroyTexture(TextureHandle h) override {
        auto it = m_textures.find(h.id);
        if (it == m_textures.end()) return;
        for (DWORD i = 0; i < 8; ++i) m_dev->SetTexture(i, nullptr);
        rel(it->second.tex);
        m_textures.erase(it);
    }
    uint64_t nativeTexture(TextureHandle) const override { return 0; }

    // Framebuffers / UBOs are part of the full pipeline, which this level does not provide.
    FramebufferHandle createFramebuffer(const FramebufferDesc&) override { return {}; }
    void destroyFramebuffer(FramebufferHandle) override {}
    void resolve(FramebufferHandle, FramebufferHandle, int, int) override {}

    RenderTargetHandle createRenderTarget(int w, int h) override {
        Rt9 r;
        if (!makeRt(r, w, h)) return {};
        m_rts[m_next] = r;
        return RenderTargetHandle{m_next++};
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
    FramebufferHandle framebufferOf(RenderTargetHandle) const override { return {}; }

    BufferHandle createUniformBuffer(size_t) override { return {}; }
    void updateBuffer(BufferHandle, const void*, size_t, size_t) override {}
    void bindUniformBuffer(int, BufferHandle) override {}
    void destroyBuffer(BufferHandle) override {}

    void beginPass(FramebufferHandle, int x, int y, int w, int h, const ClearDesc& c) override {
        beginScene({});
        D3DVIEWPORT9 vp{static_cast<DWORD>(x), static_cast<DWORD>(y), static_cast<DWORD>(w), static_cast<DWORD>(h), 0.0f, 1.0f};
        m_dev->SetViewport(&vp);
        clear(c.color, c.colorValue, c.depth, c.depthValue);
    }
    void beginPass(RenderTargetHandle rt, int w, int h, Vec4 clearColor) override {
        beginScene(rt);
        D3DVIEWPORT9 vp{0, 0, static_cast<DWORD>(w), static_cast<DWORD>(h), 0.0f, 1.0f};
        m_dev->SetViewport(&vp);
        clear(true, clearColor, true, 1.0f);
        ++m_stats.passes;
    }
    void endPass() override {
        if (!m_inScene) return;
        m_dev->EndScene();
        m_inScene = false;
        if (m_savedBack) {  // restore the backbuffer
            m_dev->SetRenderTarget(0, m_savedBack);
            m_dev->SetDepthStencilSurface(m_savedDepth);
            rel(m_savedBack);
            rel(m_savedDepth);
        }
    }

    void setShader(ShaderHandle h) override {
        m_current = m_shaders.count(h.id) ? h.id : 0;
        if (!m_current) return;
        m_dev->SetVertexShader(m_shaders[m_current].vs);
        m_dev->SetPixelShader(m_shaders[m_current].ps);
    }
    void setUniform(const char* n, const Mat4& m) override {
        std::string s(n);
        if (s == "uModel") m_dev->SetVertexShaderConstantF(0, m.data(), 4);
        else if (s == "uViewProj") m_dev->SetVertexShaderConstantF(4, m.data(), 4);
    }
    void setUniform(const char* n, Vec3 v) override { setUniform(n, Vec4(v, 1.0f)); }
    void setUniform(const char* n, Vec4 v) override {
        std::string s(n);
        float f[4] = {v.x, v.y, v.z, v.w};
        int reg = s == "uColor" ? 0 : s == "uAmbient" ? 1 : s == "uLightDir" ? 2 : s == "uLightColor" ? 3 : -1;
        if (reg >= 0) m_dev->SetPixelShaderConstantF(static_cast<UINT>(reg), f, 1);
        if (s == "uScreen") m_dev->SetVertexShaderConstantF(12, f, 1);
    }
    void setUniform(const char*, float) override {}
    void setUniform(const char*, int) override {}
    void bindTexture(int slot, TextureHandle h) override {
        if (slot < 0 || slot > 7) return;
        auto it = m_textures.find(h.id);
        DWORD s = static_cast<DWORD>(slot);
        if (it == m_textures.end()) {
            m_dev->SetTexture(s, nullptr);
            return;
        }
        m_dev->SetTexture(s, it->second.tex);
        const SamplerDesc& sd = it->second.sampler;
        D3DTEXTUREFILTERTYPE f = sd.filter == Filter::Linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
        m_dev->SetSamplerState(s, D3DSAMP_MINFILTER, f);
        m_dev->SetSamplerState(s, D3DSAMP_MAGFILTER, f);
        m_dev->SetSamplerState(s, D3DSAMP_MIPFILTER, sd.mipmaps ? D3DTEXF_LINEAR : D3DTEXF_NONE);
        DWORD addr = sd.wrap == Wrap::Repeat ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
        m_dev->SetSamplerState(s, D3DSAMP_ADDRESSU, addr);
        m_dev->SetSamplerState(s, D3DSAMP_ADDRESSV, addr);
        m_dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, m_srgb[h.id] ? TRUE : FALSE);
    }
    void setDepthTest(bool e) override { m_dev->SetRenderState(D3DRS_ZENABLE, e ? D3DZB_TRUE : D3DZB_FALSE); }
    void setDepthWrite(bool e) override { m_dev->SetRenderState(D3DRS_ZWRITEENABLE, e ? TRUE : FALSE); }
    void setDepthFunc(DepthFunc f) override {
        m_dev->SetRenderState(D3DRS_ZFUNC, f == DepthFunc::Less ? D3DCMP_LESS : f == DepthFunc::LessEqual ? D3DCMP_LESSEQUAL : D3DCMP_ALWAYS);
    }
    void setBlendMode(BlendMode m) override {
        m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, m != BlendMode::None);
        m_dev->SetRenderState(D3DRS_SRCBLEND, m == BlendMode::Alpha ? D3DBLEND_SRCALPHA : D3DBLEND_ONE);
        m_dev->SetRenderState(D3DRS_DESTBLEND, m == BlendMode::Alpha ? D3DBLEND_INVSRCALPHA : D3DBLEND_ONE);
    }
    void setCullMode(CullMode m) override {  // engine meshes are CCW-front: cull clockwise (back) faces
        m_dev->SetRenderState(D3DRS_CULLMODE, m == CullMode::Back ? D3DCULL_CW : m == CullMode::Front ? D3DCULL_CCW : D3DCULL_NONE);
    }
    void setWireframe(bool on) override { m_dev->SetRenderState(D3DRS_FILLMODE, on ? D3DFILL_WIREFRAME : D3DFILL_SOLID); }

    void draw(MeshHandle h, Topology t) override {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end() || !m_current || !it->second.vb || it->second.count == 0) return;
        Mesh9& m = it->second;
        if (m.lines) {
            m_dev->SetVertexDeclaration(m_lineDecl);
            m_dev->SetStreamSource(0, m.vb, 0, sizeof(LineVertex));
            m_dev->DrawPrimitive(D3DPT_LINELIST, 0, m.count / 2);
            m_stats.lines += m.count / 2;
        } else {
            (void)t;
            m_dev->SetVertexDeclaration(m_meshDecl);
            m_dev->SetStreamSource(0, m.vb, 0, sizeof(Vertex));
            m_dev->SetIndices(m.ib);
            m_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, m.vertices, 0, m.count / 3);
            m_stats.triangles += m.count / 3;
        }
        ++m_stats.drawCalls;
    }
    void drawInstanced(MeshHandle h, int instances) override {
        for (int i = 0; i < instances; ++i) draw(h, Topology::Triangles);
    }
    void drawFullscreenTriangle() override {}

    bool readPixels(RenderTargetHandle rt, int w, int h, std::vector<uint8_t>& rgba) override {
        IDirect3DSurface9* src = nullptr;
        if (rt) {
            auto it = m_rts.find(rt.id);
            if (it == m_rts.end()) return false;
            it->second.color->GetSurfaceLevel(0, &src);
        } else {
            m_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &src);
        }
        if (!src) return false;
        D3DSURFACE_DESC sd{};
        src->GetDesc(&sd);
        IDirect3DSurface9* sys = nullptr;
        bool ok = false;
        if (SUCCEEDED(m_dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
            SUCCEEDED(m_dev->GetRenderTargetData(src, sys))) {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
                int cw = std::min<int>(w, static_cast<int>(sd.Width)), ch = std::min<int>(h, static_cast<int>(sd.Height));
                for (int y = 0; y < ch; ++y) {
                    auto* row = static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y) * static_cast<size_t>(lr.Pitch);
                    uint8_t* dst = rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
                    for (int x = 0; x < cw; ++x) {  // BGRA -> RGBA
                        dst[x * 4 + 0] = row[x * 4 + 2];
                        dst[x * 4 + 1] = row[x * 4 + 1];
                        dst[x * 4 + 2] = row[x * 4 + 0];
                        dst[x * 4 + 3] = 255;
                    }
                }
                sys->UnlockRect();
                ok = true;
            }
        }
        rel(sys);
        rel(src);
        return ok;
    }
    bool readFramebuffer(FramebufferHandle, int w, int h, std::vector<uint8_t>& rgba) override { return readPixels({}, w, h, rgba); }

    FrameStats& stats() override { return m_stats; }
    void present() override {
        if (m_dev->Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST) m_lost = true;
        if (m_lost && m_dev->TestCooperativeLevel() == D3DERR_DEVICENOTRESET) reset();
    }
    void resizeSwapchain(int w, int h) override {
        if (w <= 0 || h <= 0 || (w == m_width && h == m_height)) return;
        m_width = w;
        m_height = h;
        reset();
    }

private:
    bool fail(const std::string& m) {
        FLEDGE_LOG_ERROR("rhi", "D3D9 init: {}", m);
        return false;
    }
    void fillPresent() {
        m_pp = {};
        m_pp.Windowed = TRUE;
        m_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        m_pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        m_pp.BackBufferWidth = static_cast<UINT>(m_width);
        m_pp.BackBufferHeight = static_cast<UINT>(m_height);
        m_pp.EnableAutoDepthStencil = TRUE;
        m_pp.AutoDepthStencilFormat = D3DFMT_D24X8;
        m_pp.hDeviceWindow = m_hwnd;
        m_pp.PresentationInterval = m_vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    }
    void reset() {
        // D3DPOOL_DEFAULT render targets must be released before Reset.
        for (auto& [i, r] : m_rts) {
            int w = r.w, h = r.h;
            freeRt(r);
            r.w = w;
            r.h = h;
        }
        fillPresent();
        if (SUCCEEDED(m_dev->Reset(&m_pp))) m_lost = false;
        for (auto& [i, r] : m_rts) makeRt(r, r.w, r.h);
    }
    bool compile(const char* src, const char* entry, const char* target, std::vector<uint8_t>& out) {
        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        D3D_SHADER_MACRO defs[] = {{target[0] == 'v' ? "FLEDGE_VS" : "FLEDGE_PS", "1"}, {nullptr, nullptr}};
        HRESULT hr = m_compile(src, std::strlen(src), "d3d9", defs, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
        if (FAILED(hr)) {
            FLEDGE_LOG_ERROR("rhi", "D3D9 shader compile failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            if (err) err->Release();
            return false;
        }
        if (err) err->Release();
        auto* p = static_cast<const uint8_t*>(blob->GetBufferPointer());
        out.assign(p, p + blob->GetBufferSize());
        blob->Release();
        return true;
    }
    void upload(IDirect3DTexture9* tex, TextureFormat f, const TextureSubresource& s) {
        D3DLOCKED_RECT lr{};
        if (FAILED(tex->LockRect(static_cast<UINT>(s.mip), &lr, nullptr, 0))) return;
        auto* src = static_cast<const uint8_t*>(s.data);
        if (f == TextureFormat::RGBA8) {
            for (int y = 0; y < s.height; ++y) {
                uint8_t* dst = static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * static_cast<size_t>(lr.Pitch);
                const uint8_t* row = src + static_cast<size_t>(y) * static_cast<size_t>(s.width) * 4;
                for (int x = 0; x < s.width; ++x) {  // RGBA -> BGRA
                    dst[x * 4 + 0] = row[x * 4 + 2];
                    dst[x * 4 + 1] = row[x * 4 + 1];
                    dst[x * 4 + 2] = row[x * 4 + 0];
                    dst[x * 4 + 3] = row[x * 4 + 3];
                }
            }
        } else {
            size_t block = f == TextureFormat::BC1 ? 8 : 16;
            size_t rowBytes = static_cast<size_t>(std::max(1, (s.width + 3) / 4)) * block;
            int rows = std::max(1, (s.height + 3) / 4);
            for (int y = 0; y < rows; ++y)
                std::memcpy(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * static_cast<size_t>(lr.Pitch), src + static_cast<size_t>(y) * rowBytes,
                            rowBytes);
        }
        tex->UnlockRect(static_cast<UINT>(s.mip));
    }
    bool makeRt(Rt9& r, int w, int h) {
        r.w = std::max(1, w);
        r.h = std::max(1, h);
        return SUCCEEDED(m_dev->CreateTexture(static_cast<UINT>(r.w), static_cast<UINT>(r.h), 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                              &r.color, nullptr)) &&
               SUCCEEDED(m_dev->CreateDepthStencilSurface(static_cast<UINT>(r.w), static_cast<UINT>(r.h), D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE,
                                                          &r.depth, nullptr));
    }
    void freeRt(Rt9& r) {
        rel(r.color);
        rel(r.depth);
    }
    void freeMesh(Mesh9& m) {
        rel(m.vb);
        rel(m.ib);
    }
    void beginScene(RenderTargetHandle rt) {
        if (m_inScene) endPass();
        if (rt) {
            auto it = m_rts.find(rt.id);
            if (it != m_rts.end() && it->second.color) {
                m_dev->GetRenderTarget(0, &m_savedBack);
                m_dev->GetDepthStencilSurface(&m_savedDepth);
                IDirect3DSurface9* s = nullptr;
                it->second.color->GetSurfaceLevel(0, &s);
                m_dev->SetRenderTarget(0, s);
                m_dev->SetDepthStencilSurface(it->second.depth);
                rel(s);
            }
        }
        m_dev->BeginScene();
        m_inScene = true;
        m_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        m_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        m_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
        m_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        m_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    }
    void clear(bool color, Vec4 c, bool depth, float z) {
        auto to8 = [](float f) { return static_cast<DWORD>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
        DWORD flags = (color ? D3DCLEAR_TARGET : 0) | (depth ? D3DCLEAR_ZBUFFER : 0);
        if (flags) m_dev->Clear(0, nullptr, flags, D3DCOLOR_ARGB(to8(c.w), to8(c.x), to8(c.y), to8(c.z)), z, 0);
    }

    HMODULE m_dll = nullptr, m_compilerDll = nullptr;
    pD3DCompile m_compile = nullptr;
    IDirect3D9* m_d3d = nullptr;
    IDirect3DDevice9* m_dev = nullptr;
    IDirect3DVertexDeclaration9 *m_meshDecl = nullptr, *m_lineDecl = nullptr;
    IDirect3DSurface9 *m_savedBack = nullptr, *m_savedDepth = nullptr;
    D3DPRESENT_PARAMETERS m_pp{};
    HWND m_hwnd = nullptr;
    int m_width = 1, m_height = 1;
    bool m_vsync = true, m_inScene = false, m_lost = false;
    std::string m_desc;
    std::unordered_map<uint32_t, Mesh9> m_meshes;
    std::unordered_map<uint32_t, Tex9> m_textures;
    std::unordered_map<uint32_t, bool> m_srgb;
    std::unordered_map<uint32_t, Shader9> m_shaders;
    std::unordered_map<uint32_t, Rt9> m_rts;
    uint32_t m_current = 0, m_next = 1;
    FrameStats m_stats;
};

} // namespace

std::unique_ptr<RenderDevice> createD3D9Device(const DeviceCreateInfo& info) {
    auto d = std::make_unique<D3D9Device>();
    if (!d->init(info)) return nullptr;
    return d;
}

} // namespace fledge::rhi
#else
#include "fledge/core/log.h"
#include "fledge/render/rhi.h"
namespace fledge::rhi {
std::unique_ptr<RenderDevice> createD3D9Device(const DeviceCreateInfo&) {
    FLEDGE_LOG_ERROR("rhi", "Direct3D 9 is only available on Windows");
    return nullptr;
}
} // namespace fledge::rhi
#endif
