// Intel XeSS Super Resolution on Direct3D 12 (C API of libxess.dll, loaded at runtime).
// Headers: intel/xess v3.0.2 inc/xess (fetched + SHA-256 pinned at configure time).
// The library comes from the upscaler version cache or sits next to a packaged game.
#ifdef _WIN32
#define __REQUIRED_RPCNDR_H_VERSION__ 475
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>

#include <algorithm>
#include <string>

#include "fledge/core/log.h"
#include "fledge/core/utility.h"
#include "d3d12_internal.h"

#ifdef FLEDGE_HAVE_XESS
#include <xess/xess_d3d12.h>
#endif

namespace fledge::rhi::d3d12 {

#ifndef FLEDGE_HAVE_XESS
std::unique_ptr<VendorUpscaler> createXessUpscaler(Internals&, const std::wstring&) { return nullptr; }
#else

namespace {
using PFN_CreateContext = xess_result_t (*)(ID3D12Device*, xess_context_handle_t*);
using PFN_Init = xess_result_t (*)(xess_context_handle_t, const xess_d3d12_init_params_t*);
using PFN_Execute = xess_result_t (*)(xess_context_handle_t, ID3D12GraphicsCommandList*, const xess_d3d12_execute_params_t*);
using PFN_Destroy = xess_result_t (*)(xess_context_handle_t);
using PFN_SetScale = xess_result_t (*)(xess_context_handle_t, float, float);
using PFN_ForceLegacy = xess_result_t (*)(xess_context_handle_t, bool);
using PFN_GetVersion = xess_result_t (*)(xess_version_t*);

class XessUpscaler final : public VendorUpscaler {
public:
    explicit XessUpscaler(Internals& d) : m_dev(d) {}
    ~XessUpscaler() override {
        if (m_ctx) {
            m_dev.waitGpuIdle();
            m_destroy(m_ctx);
        }
        if (m_dll) FreeLibrary(m_dll);
    }

    bool load(const std::wstring& path) {
        m_dll = LoadLibraryW(path.c_str());
        if (!m_dll) {
            FLEDGE_LOG_WARN("rhi", "XeSS: cannot load {}", narrow(path));
            return false;
        }
        auto get = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(m_dll, n)); };
        m_create = reinterpret_cast<PFN_CreateContext>(get("xessD3D12CreateContext"));
        m_init = reinterpret_cast<PFN_Init>(get("xessD3D12Init"));
        m_execute = reinterpret_cast<PFN_Execute>(get("xessD3D12Execute"));
        m_destroy = reinterpret_cast<PFN_Destroy>(get("xessDestroyContext"));
        m_velocityScale = reinterpret_cast<PFN_SetScale>(get("xessSetVelocityScale"));
        m_forceLegacy = reinterpret_cast<PFN_ForceLegacy>(get("xessForceLegacyScaleFactors"));
        auto version = reinterpret_cast<PFN_GetVersion>(get("xessGetVersion"));
        if (!m_create || !m_init || !m_execute || !m_destroy || !m_velocityScale) {
            FLEDGE_LOG_WARN("rhi", "XeSS: {} is missing D3D12 entry points", narrow(path));
            return false;
        }
        xess_version_t v{};
        if (version && version(&v) == XESS_RESULT_SUCCESS) m_name = "Intel XeSS " + std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
        if (m_create(m_dev.device(), &m_ctx) != XESS_RESULT_SUCCESS || !m_ctx) {
            FLEDGE_LOG_WARN("rhi", "XeSS: xessD3D12CreateContext failed");
            m_ctx = nullptr;
            return false;
        }
        if (m_forceLegacy) m_forceLegacy(m_ctx, true);  // Quality 1.5x, Balanced 1.7x, Performance 2x, Ultra Performance 3x
        FLEDGE_LOG_INFO("rhi", "{} loaded from {}", m_name, narrow(path));
        return true;
    }

    std::string name() const override { return m_name; }

    bool configure(const VendorUpscalerConfig& c) override {
        if (m_ready && c == m_config) return true;
        if (m_ready) m_dev.waitGpuIdle();  // re-initialisation must not overlap in-flight XeSS work
        xess_d3d12_init_params_t p{};
        p.outputResolution = {static_cast<uint32_t>(c.outputWidth), static_cast<uint32_t>(c.outputHeight)};
        static const xess_quality_settings_t q[] = {XESS_QUALITY_SETTING_AA, XESS_QUALITY_SETTING_QUALITY, XESS_QUALITY_SETTING_BALANCED,
                                                    XESS_QUALITY_SETTING_PERFORMANCE, XESS_QUALITY_SETTING_ULTRA_PERFORMANCE};
        p.qualitySetting = q[std::min(std::max(c.quality, 0), 4)];
        p.initFlags = XESS_INIT_FLAG_NONE;  // render-resolution motion vectors + depth
        p.creationNodeMask = 1;
        p.visibleNodeMask = 1;
        xess_result_t r = m_init(m_ctx, &p);
        if (r != XESS_RESULT_SUCCESS) {
            FLEDGE_LOG_WARN("rhi", "XeSS: xessD3D12Init failed ({})", static_cast<int>(r));
            m_ready = false;
            return false;
        }
        // renderer motion vectors: UV units, current - previous; XeSS: pixels, previous - current
        m_velocityScale(m_ctx, -static_cast<float>(c.renderWidth), -static_cast<float>(c.renderHeight));
        m_config = c;
        m_ready = true;
        return true;
    }

    bool evaluate(const VendorUpscalerInputs& in) override {
        ID3D12GraphicsCommandList* list = m_dev.commandList();
        if (!m_ready || !list) return false;
        constexpr auto srv =
            static_cast<D3D12_RESOURCE_STATES>(static_cast<int>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) | static_cast<int>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        for (TextureHandle t : {in.color, in.depth, in.velocity}) m_dev.transitionTexture(t, srv);
        m_dev.transitionTexture(in.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        xess_d3d12_execute_params_t e{};
        e.pColorTexture = m_dev.textureResource(in.color);
        e.pVelocityTexture = m_dev.textureResource(in.velocity);
        e.pDepthTexture = m_dev.textureResource(in.depth);
        e.pOutputTexture = m_dev.textureResource(in.output);
        if (!e.pColorTexture || !e.pVelocityTexture || !e.pDepthTexture || !e.pOutputTexture) return false;
        e.jitterOffsetX = in.jitterX;
        e.jitterOffsetY = in.jitterY;
        e.exposureScale = 1.0f;
        e.resetHistory = in.reset ? 1u : 0u;
        e.inputWidth = static_cast<uint32_t>(m_config.renderWidth);
        e.inputHeight = static_cast<uint32_t>(m_config.renderHeight);
        xess_result_t r = m_execute(m_ctx, list, &e);
        m_dev.restoreGraphicsState();  // XeSS binds its own root signature / descriptor heaps
        if (r != XESS_RESULT_SUCCESS) {
            if (!m_warned) FLEDGE_LOG_WARN("rhi", "XeSS: xessD3D12Execute failed ({})", static_cast<int>(r));
            m_warned = true;
            return false;
        }
        return true;
    }

private:
    Internals& m_dev;
    HMODULE m_dll = nullptr;
    PFN_CreateContext m_create = nullptr;
    PFN_Init m_init = nullptr;
    PFN_Execute m_execute = nullptr;
    PFN_Destroy m_destroy = nullptr;
    PFN_SetScale m_velocityScale = nullptr;
    PFN_ForceLegacy m_forceLegacy = nullptr;
    xess_context_handle_t m_ctx = nullptr;
    VendorUpscalerConfig m_config;
    bool m_ready = false, m_warned = false;
    std::string m_name = "Intel XeSS";
};
} // namespace

std::unique_ptr<VendorUpscaler> createXessUpscaler(Internals& device, const std::wstring& library) {
    auto x = std::make_unique<XessUpscaler>(device);
    if (!x->load(library)) return nullptr;
    return x;
}
#endif

} // namespace fledge::rhi::d3d12
#endif
