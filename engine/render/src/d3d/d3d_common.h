#pragma once
// Shared helpers for the Direct3D backends (COM pointer, HLSL compilation, shader reflection,
// format mapping). Windows only. Everything is loaded at runtime: the engine never links
// d3d*.lib / dxgi.lib / d3dcompiler.lib, so a missing component only disables that backend.
#ifdef _WIN32
#define WIDL_EXPLICIT_AGGREGATE_RETURNS  // correct ABI for struct-returning COM methods with GCC
#include <windows.h>
#include <d3dcommon.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "fledge/render/rhi.h"

namespace fledge::rhi::d3d {

template <typename T>
class Com {  // minimal ComPtr
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com& o) : m_p(o.m_p) {
        if (m_p) m_p->AddRef();
    }
    Com& operator=(const Com& o) {
        if (this != &o) {
            reset();
            m_p = o.m_p;
            if (m_p) m_p->AddRef();
        }
        return *this;
    }
    Com(Com&& o) noexcept : m_p(o.m_p) { o.m_p = nullptr; }
    Com& operator=(Com&& o) noexcept {
        if (this != &o) {
            reset();
            m_p = o.m_p;
            o.m_p = nullptr;
        }
        return *this;
    }
    void reset() {
        if (m_p) m_p->Release();
        m_p = nullptr;
    }
    T* get() const { return m_p; }
    T* operator->() const { return m_p; }
    T** put() {
        reset();
        return &m_p;
    }
    void** putVoid() { return reinterpret_cast<void**>(put()); }
    explicit operator bool() const { return m_p != nullptr; }

private:
    T* m_p = nullptr;
};

using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PFN_D3DReflect = HRESULT(WINAPI*)(const void*, SIZE_T, REFIID, void**);

// d3dcompiler_47.dll (ships with Windows 10+): compile + reflect. Reflection maps the "Params"
// cbuffer so setUniform(name) can write variables by name.
struct Compiler {
    HMODULE dll = nullptr;
    pD3DCompile compile = nullptr;
    PFN_D3DReflect reflect = nullptr;
    bool load();
    // Compiles with an on-disk bytecode cache (shader / pipeline cache, docs/GRAPHICS.md).
    bool compileCached(const std::string& name, const std::string& source, const char* entry, const char* target, std::vector<uint8_t>& out,
                       std::string& error);
};

struct CbVar {
    UINT offset = 0, size = 0;
};
// Reflects cbuffer "Params" (D3D10/11/12 shader models) into name -> offset/size; returns its size.
UINT reflectParams(Compiler& c, const std::vector<uint8_t>& blob, std::unordered_map<std::string, CbVar>& vars);
// Everything the Vulkan backend needs from a DXBC compile of the same HLSL: register masks (bN cbuffers, tN textures,
// sN samplers) and the vertex inputs (user semantics in declaration order = SPIR-V locations; system values skipped).
struct ShaderBindings {
    uint32_t cbuffers = 0, textures = 0, samplers = 0;
    uint8_t textureDim[32] = {};      // D3D_SRV_DIMENSION of each tN (2D, 2D array, cube ...)
    std::vector<std::string> inputs;  // "POSITION", "NORMAL", "TEXCOORD", "TANGENT", "COLOR" ...
};
bool reflectBindings(Compiler& c, const std::vector<uint8_t>& blob, ShaderBindings& out);  // merges into out
// Bit mask of the texture registers (tN) a shader reads.
uint32_t reflectTextureSlots(Compiler& c, const std::vector<uint8_t>& blob);

DXGI_FORMAT dxgiFormat(TextureFormat f, bool srgb);         // resource / view format
DXGI_FORMAT dxgiTypeless(TextureFormat f);                  // depth: typeless resource
DXGI_FORMAT dxgiSrvFormat(TextureFormat f, bool srgb);      // depth -> R32_FLOAT
bool isBlockCompressed(TextureFormat f);
UINT bytesPerPixel(TextureFormat f);
// Row pitch / row count of one mip for upload.
void uploadPitch(TextureFormat f, int width, int height, UINT& rowPitch, UINT& rows);

// The RHI takes 32-bit float data for the 16-bit float formats (like GL does); D3D needs halfs.
// Returns true and fills `out` when a conversion was needed.
bool convertFloatUpload(TextureFormat f, const void* data, size_t size, int w, int h, std::vector<uint8_t>& out);
uint16_t floatToHalf(float f);

// Copy one RGBA8 / BGRA8 / RGB10A2 row block into RGBA8 (readback).
void convertToRgba8(DXGI_FORMAT fmt, const uint8_t* src, UINT pitch, int w, int h, std::vector<uint8_t>& out);

// The renderer's shader cache folder (set by the host; empty = no disk cache).
void setShaderCacheDir(const std::wstring& dir);
std::wstring shaderCacheDir();

} // namespace fledge::rhi::d3d
#endif
