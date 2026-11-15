#pragma once
// Vendor upscaler runtimes (XeSS today; DLSS / FidelityFX SDK are not integrated - see upscalers.h).
// A backend returns one from RenderDevice::vendorUpscaler() when it can drive the given runtime
// library; the renderer feeds it the HDR frame at the render resolution and receives the upscaled
// frame at the output resolution (it replaces the TAA pass).
#include <string>

#include "fledge/render/rhi.h"

namespace fledge::rhi {

struct VendorUpscalerConfig {
    int renderWidth = 0, renderHeight = 0;
    int outputWidth = 0, outputHeight = 0;
    int quality = 1;  // fledge::UpscalerQuality as int (0 native/AA .. 4 ultra performance)
    bool operator==(const VendorUpscalerConfig&) const = default;
};

struct VendorUpscalerInputs {
    TextureHandle color;     // linear HDR (RGBA16F), render resolution, single-sampled
    TextureHandle depth;     // Depth32F, render resolution, single-sampled
    TextureHandle velocity;  // RG16F motion in UV: current - previous (renderer convention)
    TextureHandle output;    // storage RGBA16F at the output resolution
    float jitterX = 0, jitterY = 0;  // projection jitter in render pixels (x right, y down)
    bool reset = false;              // camera cut / first frame
};

class VendorUpscaler {
public:
    virtual ~VendorUpscaler() = default;
    virtual std::string name() const = 0;  // "Intel XeSS 2.1.1"
    virtual bool configure(const VendorUpscalerConfig& c) = 0;
    virtual bool evaluate(const VendorUpscalerInputs& in) = 0;
};

} // namespace fledge::rhi
