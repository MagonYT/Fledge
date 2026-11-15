#pragma once
// Hardware ray tracing interface (DXR 1.1 on Direct3D 12). The renderer drives it when
// GraphicsSettings::rt.enabled is on and RenderDevice::rayTracing() is non-null; otherwise the
// frame is pure raster and no RT resources exist. See docs/GRAPHICS.md "Ray tracing".
#include <span>
#include <string>

#include "fledge/render/rhi.h"

namespace fledge::rhi {

struct RtInstance {
    MeshHandle mesh;
    Mat4 transform;
    uint32_t material = 0;  // index into the material array
};

// Materials as seen by rays (factors only: hit shaders do not sample textures).
struct RtMaterial {
    Vec4 baseColor{1, 1, 1, 1};
    Vec4 emissiveRoughness{0, 0, 0, 0.5f};  // xyz emissive, w roughness
    Vec4 params{0, 0, 0, 0};                // x metallic
};

struct RtLight {  // same layout as the renderer's GPU light
    Vec4 posType, dirRange, colorIntensity, cone;
};

struct RtTraceDesc {
    int width = 0, height = 0;
    TextureHandle depth;        // Depth32F, single-sampled, D3D clip depth
    TextureHandle normals;      // RGBA16F: world normal xyz, roughness w
    TextureHandle environment;  // radiance cube (sky / IBL source)
    // Outputs (storage textures, width x height). {} = effect off.
    TextureHandle shadow;       // R16F: sun visibility 0..1
    TextureHandle ao;           // R16F: ambient occlusion 0..1
    TextureHandle reflection;   // RGBA16F: reflected radiance (a = hit distance)
    TextureHandle gi;           // RGBA16F: one-bounce diffuse irradiance
    TextureHandle pathTrace;    // RGBA32F: progressive path-traced accumulation (reference mode)
    Mat4 invViewProj;
    Vec3 cameraPos;
    Vec3 sunDirection{0, 1, 0};  // towards the sun
    Vec3 sunColor{1, 1, 1};      // color * intensity
    float sunAngularRadius = 0.0093f;  // soft shadows (radians)
    float environmentIntensity = 1.0f;
    float aoRadius = 1.0f;
    int raysPerPixel = 1;
    int quality = 1;        // 0 low .. 3 ultra (secondary shadow rays from Ultra/High)
    int maxBounces = 4;     // path tracer
    uint32_t frameIndex = 0;  // noise seed
    uint32_t accumulatedFrames = 0;  // path tracer: frames already in the accumulation buffer
    std::span<const RtLight> lights;
};

class RayTracingDevice {
public:
    virtual ~RayTracingDevice() = default;
    // Builds BLASes (cached per mesh) and rebuilds the TLAS for this frame.
    virtual bool setScene(std::span<const RtInstance> instances, std::span<const RtMaterial> materials) = 0;
    // Dispatches the enabled effects into the output textures.
    virtual bool trace(const RtTraceDesc& desc) = 0;
    virtual std::string status() const = 0;  // human readable ("DXR 1.1, 12 BLAS, 140 instances")
};

} // namespace fledge::rhi
