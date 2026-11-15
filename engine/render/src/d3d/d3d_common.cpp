#ifdef _WIN32
#include "d3d_common.h"

#include <d3d11shader.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "fledge/core/log.h"

namespace fledge::rhi::d3d {

namespace {
// IID_ID3D11ShaderReflection (MinGW's uuid lib does not always define it)
constexpr GUID kIIDShaderReflection11 = {0x8d536ca1, 0x0cca, 0x4956, {0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84}};
std::mutex g_cacheMutex;
std::wstring g_cacheDir;

uint64_t fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
} // namespace

void setShaderCacheDir(const std::wstring& dir) {
    std::lock_guard l(g_cacheMutex);
    g_cacheDir = dir;
}
std::wstring shaderCacheDir() {
    std::lock_guard l(g_cacheMutex);
    return g_cacheDir;
}

bool Compiler::load() {
    if (dll) return compile && reflect;
    dll = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!dll) return false;
    compile = reinterpret_cast<pD3DCompile>(reinterpret_cast<void*>(GetProcAddress(dll, "D3DCompile")));
    reflect = reinterpret_cast<PFN_D3DReflect>(reinterpret_cast<void*>(GetProcAddress(dll, "D3DReflect")));
    return compile && reflect;
}

bool Compiler::compileCached(const std::string& name, const std::string& source, const char* entry, const char* target,
                             std::vector<uint8_t>& out, std::string& error) {
    std::wstring dir = shaderCacheDir();
    std::filesystem::path file;
    if (!dir.empty()) {
        uint64_t h = fnv1a(source.data(), source.size());
        h = fnv1a(entry, std::strlen(entry), h);
        h = fnv1a(target, std::strlen(target), h);
        char buf[40];
        std::snprintf(buf, sizeof buf, "%016llx.dxbc", static_cast<unsigned long long>(h));
        file = std::filesystem::path(dir) / buf;
        std::ifstream f(file, std::ios::binary);
        if (f) {
            out.assign(std::istreambuf_iterator<char>(f), {});
            if (out.size() > 4 && std::memcmp(out.data(), "DXBC", 4) == 0) return true;
            out.clear();
        }
    }
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = compile(source.data(), source.size(), name.c_str(), nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr)) {
        error = err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) : "D3DCompile failed";
        if (err) err->Release();
        // append the offending source line: "name(LINE,COL): error ..."
        if (auto lp = error.find('('); lp != std::string::npos) {
            int line = std::atoi(error.c_str() + lp + 1);
            size_t pos = 0;
            for (int l = 1; l < line && pos != std::string::npos; ++l) pos = source.find('\n', pos) == std::string::npos ? std::string::npos : source.find('\n', pos) + 1;
            if (line > 0 && pos != std::string::npos) error += "\n  > " + source.substr(pos, source.find('\n', pos) - pos);
        }
        if (blob) blob->Release();
        return false;
    }
    if (err) err->Release();
    auto* p = static_cast<const uint8_t*>(blob->GetBufferPointer());
    out.assign(p, p + blob->GetBufferSize());
    blob->Release();
    if (!file.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream f(file, std::ios::binary);
        if (f) f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    }
    return true;
}

UINT reflectParams(Compiler& c, const std::vector<uint8_t>& blob, std::unordered_map<std::string, CbVar>& vars) {
    ID3D11ShaderReflection* refl = nullptr;
    if (FAILED(c.reflect(blob.data(), blob.size(), kIIDShaderReflection11, reinterpret_cast<void**>(&refl))) || !refl) return 0;
    UINT size = 0;
    if (ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByName("Params")) {
        D3D11_SHADER_BUFFER_DESC bd{};
        if (SUCCEEDED(cb->GetDesc(&bd))) {
            size = bd.Size;
            for (UINT v = 0; v < bd.Variables; ++v) {
                D3D11_SHADER_VARIABLE_DESC vd{};
                if (SUCCEEDED(cb->GetVariableByIndex(v)->GetDesc(&vd))) vars[vd.Name] = {vd.StartOffset, vd.Size};
            }
        }
    }
    refl->Release();
    return size;
}

bool reflectBindings(Compiler& c, const std::vector<uint8_t>& blob, ShaderBindings& out) {
    ID3D11ShaderReflection* refl = nullptr;
    if (FAILED(c.reflect(blob.data(), blob.size(), kIIDShaderReflection11, reinterpret_cast<void**>(&refl))) || !refl) return false;
    D3D11_SHADER_DESC sd{};
    if (SUCCEEDED(refl->GetDesc(&sd))) {
        for (UINT i = 0; i < sd.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC bd{};
            if (FAILED(refl->GetResourceBindingDesc(i, &bd))) continue;
            uint32_t* mask = bd.Type == D3D_SIT_CBUFFER ? &out.cbuffers : bd.Type == D3D_SIT_TEXTURE ? &out.textures : bd.Type == D3D_SIT_SAMPLER ? &out.samplers : nullptr;
            if (mask)
                for (UINT k = 0; k < std::max<UINT>(bd.BindCount, 1); ++k)
                    if (bd.BindPoint + k < 32) {
                        *mask |= 1u << (bd.BindPoint + k);
                        if (bd.Type == D3D_SIT_TEXTURE) out.textureDim[bd.BindPoint + k] = static_cast<uint8_t>(bd.Dimension);
                    }
        }
        if (out.inputs.empty())
            for (UINT i = 0; i < sd.InputParameters; ++i) {
                D3D11_SIGNATURE_PARAMETER_DESC pd{};
                if (SUCCEEDED(refl->GetInputParameterDesc(i, &pd)) && pd.SystemValueType == D3D_NAME_UNDEFINED && pd.SemanticName)
                    out.inputs.push_back(pd.SemanticName);
            }
    }
    refl->Release();
    return true;
}

uint32_t reflectTextureSlots(Compiler& c, const std::vector<uint8_t>& blob) {
    ID3D11ShaderReflection* refl = nullptr;
    if (FAILED(c.reflect(blob.data(), blob.size(), kIIDShaderReflection11, reinterpret_cast<void**>(&refl))) || !refl) return 0xFFFFFFFFu;
    D3D11_SHADER_DESC sd{};
    uint32_t mask = 0;
    if (SUCCEEDED(refl->GetDesc(&sd)))
        for (UINT i = 0; i < sd.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC bd{};
            if (SUCCEEDED(refl->GetResourceBindingDesc(i, &bd)) && bd.Type == D3D_SIT_TEXTURE)
                for (UINT k = 0; k < std::max<UINT>(bd.BindCount, 1); ++k)
                    if (bd.BindPoint + k < 32) mask |= 1u << (bd.BindPoint + k);
        }
    refl->Release();
    return mask;
}

DXGI_FORMAT dxgiFormat(TextureFormat f, bool srgb) {
    switch (f) {
        case TextureFormat::RGBA8: return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::BC1: return srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
        case TextureFormat::BC3: return srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
        case TextureFormat::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case TextureFormat::RG16F: return DXGI_FORMAT_R16G16_FLOAT;
        case TextureFormat::R16F: return DXGI_FORMAT_R16_FLOAT;
        case TextureFormat::RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case TextureFormat::Depth32F: return DXGI_FORMAT_D32_FLOAT;
    }
    return DXGI_FORMAT_UNKNOWN;
}
DXGI_FORMAT dxgiTypeless(TextureFormat f) { return f == TextureFormat::Depth32F ? DXGI_FORMAT_R32_TYPELESS : dxgiFormat(f, false); }
DXGI_FORMAT dxgiSrvFormat(TextureFormat f, bool srgb) { return f == TextureFormat::Depth32F ? DXGI_FORMAT_R32_FLOAT : dxgiFormat(f, srgb); }
bool isBlockCompressed(TextureFormat f) { return f == TextureFormat::BC1 || f == TextureFormat::BC3; }
UINT bytesPerPixel(TextureFormat f) {
    switch (f) {
        case TextureFormat::RGBA8: return 4;
        case TextureFormat::RGBA16F: return 8;
        case TextureFormat::RG16F: return 4;
        case TextureFormat::R16F: return 2;
        case TextureFormat::RGBA32F: return 16;
        case TextureFormat::Depth32F: return 4;
        default: return 0;
    }
}
void uploadPitch(TextureFormat f, int w, int h, UINT& rowPitch, UINT& rows) {
    if (isBlockCompressed(f)) {
        UINT bw = static_cast<UINT>(std::max(1, (w + 3) / 4));
        rowPitch = bw * (f == TextureFormat::BC1 ? 8u : 16u);
        rows = static_cast<UINT>(std::max(1, (h + 3) / 4));
    } else {
        rowPitch = static_cast<UINT>(std::max(1, w)) * bytesPerPixel(f);
        rows = static_cast<UINT>(std::max(1, h));
    }
}

uint16_t floatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFF) == 0xFF) return static_cast<uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));  // inf / nan
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);                                         // overflow -> inf
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        mant |= 0x800000u;
        uint32_t shift = static_cast<uint32_t>(14 - exp);
        uint32_t h = mant >> shift;
        if ((mant >> (shift - 1)) & 1u) ++h;  // round
        return static_cast<uint16_t>(sign | h);
    }
    uint32_t h = sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
    if (mant & 0x1000u) ++h;  // round to nearest
    return static_cast<uint16_t>(h);
}

bool convertFloatUpload(TextureFormat f, const void* data, size_t size, int w, int h, std::vector<uint8_t>& out) {
    int ch = f == TextureFormat::RGBA16F ? 4 : f == TextureFormat::RG16F ? 2 : f == TextureFormat::R16F ? 1 : 0;
    if (!ch || !data) return false;
    size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(ch);
    if (size != 0 && size < n * sizeof(float)) return false;  // already half data
    out.resize(n * 2);
    auto* src = static_cast<const float*>(data);
    auto* dst = reinterpret_cast<uint16_t*>(out.data());
    for (size_t i = 0; i < n; ++i) dst[i] = floatToHalf(src[i]);
    return true;
}

void convertToRgba8(DXGI_FORMAT fmt, const uint8_t* src, UINT pitch, int w, int h, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + static_cast<size_t>(y) * pitch;
        uint8_t* dst = out.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
        for (int x = 0; x < w; ++x) {
            if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM) {
                uint32_t v;
                std::memcpy(&v, row + x * 4, 4);
                dst[x * 4 + 0] = static_cast<uint8_t>(((v >> 0) & 1023) >> 2);
                dst[x * 4 + 1] = static_cast<uint8_t>(((v >> 10) & 1023) >> 2);
                dst[x * 4 + 2] = static_cast<uint8_t>(((v >> 20) & 1023) >> 2);
                dst[x * 4 + 3] = 255;
            } else if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8X8_UNORM) {
                dst[x * 4 + 0] = row[x * 4 + 2];
                dst[x * 4 + 1] = row[x * 4 + 1];
                dst[x * 4 + 2] = row[x * 4 + 0];
                dst[x * 4 + 3] = 255;
            } else {
                std::memcpy(dst + x * 4, row + x * 4, 4);
            }
        }
    }
}

} // namespace fledge::rhi::d3d
#endif
