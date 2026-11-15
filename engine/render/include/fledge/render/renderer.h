#pragma once
// Scene renderer.
//
// Full pipeline (devices with caps().fullPipeline: OpenGL, D3D10/11/12, Null), built every frame as
// a RenderGraph:
//   shadow.cascades (directional CSM, 2-4 cascades, PCF) -> shadow.spot<N> (up to 2 spot lights + 6 faces for each of up to
//      2 point lights, one Depth32F array)
//   [-> rt.gbuffer -> rt.trace (DXR: shadows / AO / reflections / GI) -> rt.denoise.*]
//   [-> depth.prepass -> ssao + ssao.blur (half res) -> ssr (half res, previous frame's colour)]  (1.2.0 screen-space effects)
//   -> main (HDR RGBA16F + RG16F motion vectors, optional MSAA, at the render resolution): PBR
//      metallic-roughness (Cook-Torrance GGX), normal/MR/occlusion/emissive maps, IBL, sky,
//      grid/lines, sorted alpha blending, particles (sorted alpha + additive); exponential height
//      fog on lit surfaces, sky and particles -> resolve (MSAA)
//   [-> taa (temporal AA; with an upscaler quality mode also the temporal upscale to the output size)]
//   [-> volumetric + volumetric.apply (sun shafts, half res)] [-> ssr.history]
//   [-> exposure.luma/reduce/adapt (auto-exposure)] [-> dof] [-> motion.blur]
//   -> bloom (down/up chain) -> tonemap (ACES/AgX, exposure, HDR10/PQ output)
//   -> fxaa | fsr.easu + fsr.rcas | upscale -> output (backbuffer or render target).
// Debug views replace the lit result. GraphicsSettings (RenderOptions::graphics) selects the
// upscaler, render scale and ray-traced effects; see docs/GRAPHICS.md.
// Basic path (devices without fullPipeline, i.e. Direct3D 9): single forward pass with a simple lit
// shader straight into the backbuffer (no shadows/IBL/post).
//
// Wet surfaces (rain-driven darkening, gloss and puddles) are part of the PBR shader (RenderSettings.wetness, driven by
// the weather while playing). Screen-space effects: docs/GRAPHICS.md "Screen-space and post effects (1.2.0)".
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fledge/core/features.h"
#include "fledge/core/json.h"
#include "fledge/core/reflection.h"
#include "fledge/render/graphics_settings.h"
#include "fledge/render/render_graph.h"
#include "fledge/render/rhi.h"

namespace fledge {

// GPU skinning bone cap (1.2.1: 256, was 128). The "Bones" uniform block holds kMaxGpuSkinBones float4x4 = 16 KB, the
// guaranteed minimum uniform block size on OpenGL 3.3 (GL_MAX_UNIFORM_BLOCK_SIZE) and Vulkan (maxUniformBufferRange);
// D3D11 / D3D10 (incl. feature level 10_0) allow 4096 float4 = 64 KB per constant buffer. Characters with more bones are
// skinned on the CPU (AnimationSystem). ForwardRenderer::kMaxBones is the same value.
//
// 1.3.1: the cap depends on the edition (docs/GRAPHICS.md "GPU skinning"). Fledge keeps 256 (kMaxGpuSkinBones, the
// single 16 KB block, exactly as in 1.3.0). Fledge Advanced skins up to 1024 bones (kMaxGpuSkinBonesAdvanced) on the GPU:
// a rig with more than 256 bones uses a second shader variant whose palette is split over 4 uniform blocks of 256 bones
// (Bones, Bones1, Bones2, Bones3 = 4 x 16 KB), so no block ever exceeds the guaranteed 16 KB. The extended path is only
// compiled into Fledge Advanced builds (FLEDGE_GPU_SKIN_EXTENDED, set by CMake); at run time it also needs
// currentEdition() == Advanced, so fledge_runtime (the core runtime, Standard edition in both builds) keeps 256.
inline constexpr int kMaxGpuSkinBones = 256;
inline constexpr int kMaxGpuSkinBonesAdvanced = 1024;
inline constexpr int kGpuSkinBonesPerBlock = 256;  // bones per "Bones*" uniform block (16 KB)
// The GPU skinning cap an edition allows (policy only: Standard 256, Advanced 1024).
constexpr int maxGpuSkinBones(Edition e) { return e == Edition::Advanced ? kMaxGpuSkinBonesAdvanced : kMaxGpuSkinBones; }
// The cap of this executable: maxGpuSkinBones(currentEdition()) in a Fledge Advanced build, always 256 in Fledge.
#if FLEDGE_GPU_SKIN_EXTENDED
inline int maxGpuSkinBones() { return maxGpuSkinBones(currentEdition()); }
#else
constexpr int maxGpuSkinBones() { return kMaxGpuSkinBones; }
#endif
// 1.3.2 Fledge Advanced features (docs/EDITIONS.md, docs/GRAPHICS.md, docs/ANIMATION.md): leader pose, multi-material
// characters and projected decals. Compiled into Fledge Advanced builds only (FLEDGE_ADVANCED_RENDER, set by CMake) and active
// when the executable runs as Fledge Advanced (like the 1024-bone path: fledge_runtime, the core runtime, has none of them).
// Elsewhere the data loads and is ignored with one warning: followers animate on their own, multi-material characters draw
// with one material, decals are not drawn (impacts keep the 1.3.0 box marks).
#if FLEDGE_ADVANCED_RENDER
inline bool advancedRenderFeatures() { return currentEdition() == Edition::Advanced; }
#else
constexpr bool advancedRenderFeatures() { return false; }
#endif
// The warning text for a feature that is off here ("leader pose", "multi-material characters", "projected decals").
std::string advancedRenderNote(const char* feature);

// Why a character of `bones` bones is skinned where it is, for tools (get_animation_info, inspector):
// {"path": "gpu" | "cpu", "reason": ..., "bones", "limit", "edition"}. gpuLimit = ForwardRenderer::gpuSkinBoneLimit()
// (0 = no GPU skinning on this backend / renderer).
struct SkinningDecision {
    bool gpu = false;
    int bones = 0, limit = 0;
    std::string reason;
};
SkinningDecision skinningDecision(int bones, int gpuLimit, bool crowd = false);

struct DrawItem {
    rhi::MeshHandle mesh;
    Mat4 model;
    Vec3 color{1, 1, 1};         // base color factor
    rhi::TextureHandle texture;  // base color (sRGB)
    bool selected = false;       // editor highlight tint
    bool unlit = false;
    bool twoSided = false;
    int alphaMode = 0;  // 0 opaque, 1 mask (cutoff), 2 blend (sorted back-to-front)
    float opacity = 1.0f;
    float alphaCutoff = 0.5f;
    float roughness = 0.5f;
    float metallic = 0.0f;
    rhi::TextureHandle normalTexture;             // tangent-space, linear
    rhi::TextureHandle metallicRoughnessTexture;  // G = roughness, B = metallic (glTF), linear
    rhi::TextureHandle occlusionTexture;          // R, linear
    rhi::TextureHandle emissiveTexture;           // sRGB
    Vec3 emissive{0, 0, 0};                       // linear color * intensity
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    bool castShadows = true;  // false: left out of every shadow map (MeshRenderer / Animator castShadows)
    bool shadowOnly = false;  // true: drawn into the shadow maps only (not the main view, depth prepass or the RT scene)
    bool offCamera = false;   // outside the camera this frame: skipped in the view, still drawn into shadow maps and the RT scene
    bool staticCaster = true; // false: this caster moves every frame, so a cached cascade that contains it is redrawn
    uint32_t objectId = 0;  // stable id (entity) for motion vectors; 0 = static / unknown
    // World-space bounding sphere (radius 0 = unknown: never culled). Used for shadow-cascade culling.
    Vec3 boundsCenter{0, 0, 0};
    float boundsRadius = 0;
    bool instanceable = true;  // false: never merged into an instanced draw (e.g. unique per-frame data)
    // GPU skinning (renderer.gpuSkinning()): mesh is the bind pose; joints/weights come from skinTexture (RGBA32F,
    // 2 texels per vertex) and bones[0..boneCount) are model-space skin matrices (<= renderer.gpuSkinBoneLimit():
    // 256, or 1024 in Fledge Advanced).
    rhi::TextureHandle skinTexture;
    const Mat4* bones = nullptr;
    int boneCount = 0;
    int skinInfluences = 4;  // 1.3.2: 8 = skinTexture has 4 texels per vertex (influences 5..8 in texels 2 and 3)
    // 1.3.2 viewmodel (first-person arms / weapon, docs/GRAPHICS.md "Viewmodel"): with ViewItem::viewmodelPass the item is
    // drawn after the world with the view's viewmodelProjection in the front slice of the depth range, so it never clips
    // into walls and keeps its size when the world FOV changes. It casts no world shadow and is left out of the depth
    // prepass (SSAO / SSR), the RT scene and SSAO / SSR / RT lookups. Without viewmodelPass (editor free camera) it is an
    // ordinary item.
    bool viewmodel = false;
    // 1.3.2 Fledge Advanced projected decal piece (GameWorld builds the clipped mesh; tangent.w = angle fade): drawn right after
    // the opaque world with alpha blending and no depth writes, sorted by decalOrder; never in shadow maps / prepass / RT.
    bool decal = false;
    int64_t decalOrder = 0;
};

enum class LightKind : int { Directional = 0, Point = 1, Spot = 2 };

struct LightItem {
    LightKind kind = LightKind::Directional;
    Vec3 direction{0, -1, 0};  // direction the light travels (directional/spot)
    Vec3 position{};           // point/spot
    Vec3 color{1, 1, 1};
    float intensity = 1.0f;
    float range = 10.0f;
    float innerCone = radians(20.0f);  // spot, half-angles in radians
    float outerCone = radians(30.0f);
    bool castShadows = false;
    float shadowBias = 0.0005f;  // constant depth bias (NDC depth units)
    float normalBias = 1.0f;     // receiver offset along the normal, in shadow texels
};

struct ViewItem {
    Mat4 view;
    Mat4 projection;  // perspective; near/far/fov are recovered from it for cascades
    Vec3 position;
    uint32_t camera = 0;  // entity of the Camera this view renders from (0 = editor / free camera); shadowOnly ownership
    // 1.3.2 viewmodel pass: DrawItem::viewmodel items use this projection (Camera viewmodelFov / viewmodelNear) instead of
    // `projection`, remapped into the front kViewmodelDepthSlice of the depth range. false = no viewmodel pass (they are
    // drawn like any item).
    bool viewmodelPass = false;
    Mat4 viewmodelProjection;
};
// Depth range the viewmodel pass uses (NDC depth [0, slice] in D3D terms; the world keeps the rest). World geometry closer
// than ~near / (1 - slice) to the camera would overlap it: about 0.1 mm past a 0.1 m near plane.
inline constexpr float kViewmodelDepthSlice = 1.0f / 512.0f;
// The clip-space remap that squeezes a GL-style projection's depth into [-1, -1 + 2 * slice] (D3D: [0, slice]).
Mat4 viewmodelDepthRemap(float slice = kViewmodelDepthSlice);

enum class Tonemapper : int { ACES = 0, AgX = 1, Linear = 2 };
enum class DebugView : int { None = 0, Albedo, Normal, Roughness, Metallic, Depth, ShadowCascades, Wireframe };

// Per-project defaults live in the .fledge "render" block; the editor's Render Settings panel edits them.
struct RenderSettings {
    Tonemapper tonemapper = Tonemapper::ACES;
    float exposure = 0.0f;  // EV
    bool bloom = true;
    float bloomIntensity = 0.05f;
    float bloomThreshold = 1.0f;
    bool fxaa = true;
    int msaa = 1;  // 1, 2, 4
    bool shadows = true;
    int cascadeCount = 3;  // 2..4
    int shadowResolution = 2048;
    float shadowDistance = 40.0f;
    float environmentIntensity = 1.0f;
    bool showSky = true;
    std::string environment;  // .hdr texture asset (GUID/path); empty = procedural sky
    DebugView debugView = DebugView::None;
    // Exponential height fog (full pipeline; the weather system drives it while playing, see docs/GRAPHICS.md "Fog").
    bool fog = false;
    float fogDensity = 0.02f;         // extinction per meter at fogHeight
    float fogHeight = 0.0f;           // world height (m) where the density is fogDensity
    float fogHeightFalloff = 0.15f;   // 1/m: density *= exp(-falloff * (y - fogHeight)); 0 = uniform
    float fogStart = 0.0f;            // meters from the camera before fog begins
    float fogMaxOpacity = 1.0f;       // cap so distant geometry / sky never fully vanish (0..1)
    Vec3 fogColor{0.62f, 0.68f, 0.76f};  // linear in-scattered colour
    float fogSunScatter = 0.5f;       // forward scattering towards the sun (0..2)
    // ---- 1.2.0 effects (full pipeline; all off by default except point shadows and wet surfaces, docs/GRAPHICS.md) ----
    bool pointShadows = true;          // point lights with castShadows get 6-face shadow maps (up to 2 lights)
    bool ssao = false;                 // screen-space ambient occlusion (applied to ambient light)
    float ssaoRadius = 0.6f;           // world-space radius (m)
    float ssaoIntensity = 1.0f;        // 0..2
    int ssaoSamples = 12;              // 4..32
    bool ssr = false;                  // screen-space reflections (blended over the IBL on smooth surfaces)
    float ssrMaxDistance = 25.0f;      // ray length (m)
    int ssrSteps = 32;                 // 8..96
    float ssrThickness = 0.35f;        // depth tolerance of a hit (m)
    float ssrIntensity = 1.0f;         // 0..1
    float wetness = 0.0f;              // 0 dry .. 1 soaked (the weather raises it while it rains, then it dries)
    float puddles = 0.5f;              // how much of the flat ground collects puddles when wet (0..1)
    bool volumetric = false;           // volumetric sun light (shafts through the cascade shadows)
    float volumetricDensity = 0.015f;  // scattering medium per meter
    float volumetricIntensity = 1.0f;  // 0..4
    float volumetricAnisotropy = 0.6f; // Henyey-Greenstein g (forward scattering), -0.9..0.9
    int volumetricSteps = 24;          // 8..64
    float volumetricDistance = 80.0f;  // max march distance (m)
    bool autoExposure = false;         // average log luminance + temporal adaptation; "exposure" becomes compensation
    float autoExposureMinEv = -4.0f;   // limits of the automatic exposure (EV)
    float autoExposureMaxEv = 4.0f;
    float autoExposureSpeed = 1.5f;    // adaptation rate (1/s)
    bool dof = false;                  // depth of field (gather bokeh)
    float dofFocusDistance = 8.0f;     // m
    float dofFocusRange = 3.0f;        // fully sharp band around the focus distance (m)
    float dofMaxRadius = 8.0f;         // largest blur radius (px at 1080p, scaled with the output height)
    bool motionBlur = false;           // per-pixel blur along the motion vectors
    float motionBlurStrength = 0.5f;   // fraction of the frame's motion (shutter), 0..1
    int motionBlurSamples = 8;         // 4..16
    bool giProbes = true;              // baked irradiance volume (static world), sampled in the PBR ambient term

    static const TypeInfo& typeInfo();
    Json toJson() const { return typeInfo().toJson(this); }
    Status fromJson(const Json& j) { return typeInfo().fromJson(this, j); }
    void sanitize();
};

struct RenderOptions {
    Vec4 clearColor{0.08f, 0.09f, 0.11f, 1.0f};
    Vec3 ambient{0.15f, 0.15f, 0.18f};  // basic path only (full pipeline uses IBL)
    bool drawGrid = true;
    RenderSettings settings;
    GraphicsSettings graphics;          // upscaler, render scale, ray tracing, HDR (see graphics_settings.h)
    bool pathTraceReference = false;    // editor: progressive path-traced reference (needs DXR)
    bool temporal = true;               // false: one-off renders (thumbnails) skip TAA history
};

// Camera-facing particle (weather, ParticleEmitter components), drawn in the main HDR pass after transparent
// geometry: depth-tested, no depth writes, fogged. Alpha particles are sorted back-to-front.
struct ParticleItem {
    Vec3 position;
    Vec3 velocity;       // m/s; with stretch > 0 the sprite is elongated along it (rain streaks, sparks)
    float size = 0.1f;   // width (m)
    float stretch = 0;   // seconds of motion added to the length: length = size + |velocity| * stretch
    Vec4 color{1, 1, 1, 1};  // linear rgb (may exceed 1 for glowing particles), a = opacity
    bool additive = false;   // fire / sparks: added to the scene instead of blended
};

// ---- math helpers (CPU reference versions of shader code; unit-tested) ----
// Exponential height fog: opacity 0..fogMaxOpacity of the fog between the camera and p (shader applyFog()).
float fogAmount(const RenderSettings& s, Vec3 camera, Vec3 p);
// Fog colour seen along a view direction (adds forward scattering towards the sun).
Vec3 fogInscatter(const RenderSettings& s, Vec3 viewDir, Vec3 toSun, Vec3 sunColor);
// Camera-facing (or velocity-stretched) quad corners of a particle, in the order the renderer emits them.
std::array<Vec3, 4> particleQuad(const ParticleItem& p, Vec3 cameraPos, Vec3 cameraRight, Vec3 cameraUp);
// ---- 1.2.0 effects: CPU references of the shader math (unit-tested) ----
// Exposure (EV) the auto-exposure applies for a scene whose average log2 luminance is avgLog2 (mid grey 0.18), clamped.
float autoExposureEv(float avgLog2Luma, float minEv, float maxEv);
// One adaptation step in log2 luminance space: exponential approach with rate speed (1/s).
float adaptLuminance(float prevLog2, float currentLog2, float dt, float speed);
// Depth-of-field circle of confusion (px) at a distance (m).
float dofCircleOfConfusion(float distance, float focusDistance, float focusRange, float maxRadius);
// Henyey-Greenstein phase function (normalised over the sphere).
float henyeyGreenstein(float cosTheta, float g);
// Point-light shadow face (0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z) for a direction from the light, as the shader picks it.
int pointShadowFace(Vec3 fromLight);
// View-projection of one point-light shadow face (OpenGL clip space; the renderer applies its clip fix).
Mat4 pointShadowViewProj(Vec3 lightPos, int face, float range);
// Wet surface: the material after rain (porous darkening, water-film gloss, puddles on flat ground). normalY = world normal
// y, puddleMask = 0..1 noise at that spot (the shader uses value noise of the world position).
struct WetSurface {
    Vec3 albedo;
    float roughness;
    float puddle;  // 0..1 how much of the shading uses the flat puddle normal
};
WetSurface wetSurface(Vec3 albedo, float roughness, float metallic, float normalY, float wetness, float puddles, float puddleMask);
// Weather -> surface wetness: rises while it rains (about 12 s to soak), dries slowly after (about 100 s).
float wetnessStep(float wetness, float rain, float dt);
Vec3 tonemapACES(Vec3 c);
Vec3 tonemapAgX(Vec3 c);  // returns linear (pre display-encode) like the shader
struct Cascade {
    Mat4 viewProj;
    float splitNear = 0, splitFar = 0;  // view-space distances
    Vec3 center;
    float radius = 0;
    float texelWorld = 0;  // world size of one shadow-map texel
};
std::vector<Cascade> computeCascades(const Mat4& view, float fovY, float aspect, float nearPlane, float shadowDistance, Vec3 lightDir,
                                     int count, int resolution, float lambda = 0.75f);
// Sphere vs a view-projection's clip volume (x/y exact for ortho shadow maps; z tested with a margin).
bool sphereInClip(const Mat4& vp, Vec3 center, float radius);
// Shadow-map projections this view will fill (first shadowing directional's cascades, then spot and point faces).
// GL clip space. Empty when shadows are off. Matches the renderer's light limits (2 spots, 2 point lights).
std::vector<Mat4> shadowCasterVolumes(const ViewItem& view, const RenderSettings& settings, const std::vector<LightItem>& lights);
// Picks the sun plus the local lights that matter in this view (range vs camera, then intensity). The shader still
// loops 8 lights; extra lights in the scene are kept out of that loop instead of being silently dropped as "first 8".
std::vector<LightItem> clusterLights(const ViewItem& view, const std::vector<LightItem>& lights, int maxLights = 8);
struct GiBake {
    Vec3 min{0, 0, 0}, extent{1, 1, 1};
    int gx = 8, gy = 4, gz = 8;
    std::vector<uint8_t> rgba;  // gx * (gy * gz) * 4, RGBA8
};
// Cheap irradiance volume: 6-tap AO against caster spheres, unoccluded faces take `sky`.
GiBake bakeGiProbes(Vec3 worldMin, Vec3 worldMax, std::span<const Vec3> centres, std::span<const float> radii, Vec3 sky);
// Split-sum BRDF integration (Karis): returns size*size (scale, bias) pairs; x = N.V, y = roughness.
std::vector<Vec2> computeBrdfLut(int size, int samples);

// ---- temporal helpers (CPU reference versions of the TAA math; unit-tested) ----
// Sub-pixel jitter of frame `index` (Halton 2,3 over 8 frames), in pixels, each axis in [-0.5, 0.5).
Vec2 taaJitter(uint32_t index);
// Internal render resolution for an output size: the upscaler's quality mode, or renderScale
// when no upscaler is active. At least 1x1.
void renderResolution(const GraphicsSettings& g, int outW, int outH, int& renderW, int& renderH);
// YCoCg transform used by the TAA neighbourhood clipping, and the clamp it applies.
Vec3 rgbToYCoCg(Vec3 c);
Vec3 yCoCgToRgb(Vec3 c);
Vec3 clipHistory(Vec3 historyYCoCg, Vec3 mean, Vec3 sigma, float gamma = 1.25f);

class ForwardRenderer {
public:
    explicit ForwardRenderer(rhi::RenderDevice& device);
    ~ForwardRenderer();

    rhi::RenderDevice& device() { return m_device; }
    // Built-in meshes: "builtin:cube", "builtin:plane", "builtin:sphere" (tangents generated).
    rhi::MeshHandle builtinMesh(const std::string& name);

    void addLine(Vec3 a, Vec3 b, Vec3 color);
    // Particles for the next render() (then cleared). Full pipeline only; the basic path (D3D9) skips them.
    void addParticles(std::span<const ParticleItem> particles);
    static constexpr size_t kMaxParticles = 65536;  // per frame; the rest are dropped (farthest first)
    // 2D overlay (in-game menus): triangles in pixels (top-left origin), sRGB colors with alpha, drawn
    // on top of the final image by the next render() and then cleared.
    struct OverlayVertex {
        Vec2 pos;
        Vec4 color;
        Vec2 uv{0, 0};  // into the overlay texture (font atlas); (0, 0) must be opaque (solid shapes)
    };
    // Texture the overlay samples (alpha multiplies the vertex alpha): a UI font atlas. {} = solid white.
    void setOverlayTexture(rhi::TextureHandle texture) { m_overlayTexture = texture; }
    void setBrowserOverlay(rhi::TextureHandle texture) { m_browserOverlay = texture; }
    void setOverlay(std::vector<OverlayVertex> triangles) { m_overlay = std::move(triangles); }
    // Adds triangles to the overlay of the next frame; behind = drawn under what is already there (HUD under menus).
    void addOverlay(const std::vector<OverlayVertex>& triangles, bool behind = false) {
        m_overlay.insert(behind ? m_overlay.begin() : m_overlay.end(), triangles.begin(), triangles.end());
    }
    // target = {} renders to the backbuffer.
    void render(rhi::RenderTargetHandle target, int width, int height, const ViewItem& view, const std::vector<DrawItem>& items,
                const std::vector<LightItem>& lights, const RenderOptions& options);

    // Environment for IBL + sky. Equirectangular linear HDR (RGBA float); empty = procedural sky.
    void setEnvironmentEquirect(int width, int height, std::vector<float> rgba);
    void setProceduralSky();
    void setGiBake(const GiBake& bake);

    struct FrameInfo {
        std::vector<std::string> passes;   // executed render-graph passes in order
        std::vector<std::string> culled;
        int cascades = 0;
        bool fullPipeline = false;
        int renderWidth = 0, renderHeight = 0;  // internal resolution (before upscaling)
        UpscalerKind upscaler = UpscalerKind::Off;  // the one actually used
        std::string upscalerNote;                   // why a fallback happened (empty = as requested)
        bool rayTracing = false;                    // RT effects ran this frame
        std::string rayTracingNote;                 // why RT is off although requested
        uint32_t pathTraceSamples = 0;              // reference mode: accumulated samples per pixel
        int instancedDraws = 0, instancedItems = 0;  // GPU instancing: draw calls issued / items they covered
        int shadowCulled = 0;                         // shadow-caster draws skipped by per-cascade culling
        int shadowCacheHits = 0;                      // cascades reused from the previous frame (same light matrix, no moving caster)
        int clusteredLights = 0;                      // lights packed into the 8-slot shader this frame
        bool giProbes = false;
        int gpuSkinned = 0;                           // items skinned in the vertex shader
        int particles = 0;                            // particles drawn (alpha + additive)
        bool fog = false;                             // height fog applied
        // 1.2.0 effects that ran this frame
        bool ssao = false, ssr = false, volumetric = false, autoExposure = false, dof = false, motionBlur = false, depthPrepass = false;
        int pointShadowLights = 0;                    // point lights with 6-face shadow maps
        int viewmodelItems = 0;                       // 1.3.2: items drawn in the viewmodel pass
        int decals = 0;                               // 1.3.2 (Fledge Advanced): projected decal meshes drawn
        float wetness = 0;                            // wetness the PBR shader used
        // 1.3.3 debug counter (setNonFiniteCheck): pixels holding NaN / +-Inf in the HDR scene colour as the opaque, sky and
        // transparent passes left it (nonFiniteHdr; how many of them are Inf: nonFiniteHdrInf) and in the image bloom reads
        // after TAA / DoF / motion blur (nonFinitePost). -1 = not checked this frame (the check is off, or the basic path).
        int nonFiniteHdr = -1, nonFiniteHdrInf = -1, nonFinitePost = -1;
    };
    const FrameInfo& lastFrame() const { return m_frame; }
    // 1.3.3: counts NaN / Inf pixels every frame into lastFrame().nonFinite* (docs/GRAPHICS.md "Robustness rules"). Off by
    // default: it reads a mask back to the CPU (a GPU sync) and costs nothing while off. fledge_runtime --count-nonfinite,
    // MCP describe_render / set_render_settings "countNonFinite".
    void setNonFiniteCheck(bool on) { m_nonFiniteCheck = on; }
    bool nonFiniteCheck() const { return m_nonFiniteCheck; }
    // 1.3.3 tests only (fledge_runtime --inject-nonfinite): writes NaN into a 2x2 block at the image centre and +Inf into one
    // pixel at 3/4 width of the HDR scene colour after the scene passes, so the post chain (TAA, auto-exposure, DoF, motion
    // blur, bloom, tonemap) can be checked against a bad pixel on hardware whose shaders never produce one. Off by default.
    void setNonFiniteInjection(bool on) { m_injectNonFinite = on; }

private:
    void renderFull(rhi::FramebufferHandle out, int width, int height, const ViewItem& view, const std::vector<DrawItem>& items,
                    const std::vector<LightItem>& lights, const RenderOptions& options);
    void renderBasic(rhi::RenderTargetHandle target, int width, int height, const ViewItem& view, const std::vector<DrawItem>& items,
                     const std::vector<LightItem>& lights, const RenderOptions& options);
    void ensureEnvironment(Vec3 sunDir);
    void bindMaterial(const DrawItem& item);
    // depthPrepass: every opaque / masked item (shadow casting or not) for the camera's depth prepass
    void drawShadowCasters(const std::vector<DrawItem>& items, const Mat4& lightVP, bool depthPrepass = false);
    // Opaque items grouped by mesh + material for instanced draws (caps().instancing).
    void drawOpaque(const std::vector<const DrawItem*>& items, const std::function<const Mat4&(const DrawItem&)>& prevModel, int rtFlags, Vec4 screen);
    rhi::ShaderHandle m_pbrInst, m_shadowInst, m_pbrSkin, m_shadowSkin;
    rhi::ShaderHandle m_pbrSkin8, m_shadowSkin8;  // 1.3.2: 8 skin influences (4 skin texels per vertex)
    rhi::ShaderHandle m_pbrDecal;                  // 1.3.2 Fledge Advanced: PBR with alpha x angle fade (tangent.w), depth-biased
    rhi::BufferHandle m_instUbo, m_boneUbo;
#if FLEDGE_GPU_SKIN_EXTENDED
    // Fledge Advanced: > 256 bones, palette in 4 uniform blocks (m_boneUbo + these 3), docs/GRAPHICS.md "GPU skinning"
    rhi::ShaderHandle m_pbrSkinXL, m_shadowSkinXL, m_pbrSkinXL8, m_shadowSkinXL8;
    rhi::BufferHandle m_boneUboXL[3];
    std::unordered_set<int> m_skinWarned;  // bone counts already warned about (one warning each)
    void bindBones(const DrawItem& item, bool shadow);
#endif
    // The skinning shader of a GPU-skinned item (bone count, influences), {} when there is none for it.
    rhi::ShaderHandle skinShader(const DrawItem& item, bool shadow, bool& xl) const;
public:
    static constexpr int kMaxBones = kMaxGpuSkinBones;
    bool gpuSkinning() const { return gpuSkinningEnabled && m_pbrSkin && m_shadowSkin && m_boneUbo; }
    // Most bones a character may have to be skinned in the vertex shader here (0 = no GPU skinning): 256, or in Fledge
    // Advanced (edition Advanced at run time) 1024 on the backends that have the 4-block variant (OpenGL, D3D11, D3D10,
    // Vulkan, Null). AnimationSystem skins larger rigs on the CPU.
    int gpuSkinBoneLimit() const;
    // Why the backend has the limit it has ("" = full limit of the edition), e.g. "D3D12 has no GPU skinning path".
    std::string gpuSkinNote() const;
    // 1.3.2: most skin influences per vertex skinned in the vertex shader (8 where the 8-influence variants exist, 4 when only the
    // 4-influence ones do, 0 = no GPU skinning). AnimationSystem skins characters with more on the CPU.
    int gpuSkinInfluences() const;
    bool gpuSkinningEnabled = true;  // tests compare GPU vs CPU skinning
    // 1.3.2 projected decals: the decal shader exists (full pipeline, Fledge Advanced at run time). The basic path (D3D9) has none.
    bool decalsSupported() const { return static_cast<bool>(m_pbrDecal); }
    static constexpr int kMaxInstances = 128;
    bool instancingEnabled = true;  // tests compare instanced vs per-item draws
private:

    // Per output target: TAA / denoiser histories, previous matrices, path-trace accumulation.
    struct ViewHistory {
        int outW = 0, outH = 0, renderW = 0, renderH = 0;
        rhi::TextureHandle taa[2];
        int taaIndex = 0;
        bool taaValid = false;
        rhi::TextureHandle rt[4][2];  // shadow, ao, reflection, gi
        int rtIndex = 0;
        bool rtValid[4] = {};
        rhi::TextureHandle pathTrace;
        rhi::TextureHandle ssrColor[2];  // previous frame's HDR colour for SSR (render size)
        int ssrIndex = 0;
        bool ssrValid = false;
        rhi::TextureHandle exposure[2];  // adapted log2 luminance (1x1, auto-exposure)
        int exposureIndex = 0;
        bool exposureValid = false;
        double lastTime = 0;  // seconds (steady clock) of the previous frame: auto-exposure adaptation

        uint32_t ptFrames = 0;
        uint64_t ptKey = 0;  // hash of camera + scene; accumulation restarts when it changes
        Mat4 prevViewProj;
        bool prevValid = false;
        Mat4 prevVmViewProj;  // 1.3.2 viewmodel pass (motion vectors of the first-person rig)
        bool prevVmValid = false;
        uint64_t frame = 0, lastUsed = 0;
        std::unordered_map<uint32_t, Mat4> prevModels, models;
    };
    void releaseHistory(ViewHistory& h);
    rhi::TextureHandle historyTexture(rhi::TextureHandle& slot, const rhi::TextureDesc& d);

    rhi::RenderDevice& m_device;
    bool m_full = false;
    rhi::ShaderHandle m_pbr, m_shadow, m_sky, m_lines, m_toCube, m_skyCube, m_irradiance, m_prefilter, m_bloomDown, m_bloomUp, m_tonemap,
        m_fxaa, m_basic, m_taa, m_blit, m_rtGbuffer, m_rtDenoise, m_fsrEasu, m_fsrRcas;
    // 1.2.0 effects (shaders_effects.h)
    rhi::ShaderHandle m_ssao, m_ssaoBlur, m_ssr, m_volumetric, m_apply, m_lumaDown, m_lumaReduce, m_lumaAdapt, m_dof, m_motionBlur;
    std::unordered_map<uint64_t, ViewHistory> m_views;
    std::unordered_map<uint32_t, rhi::TextureDesc> m_historyDescs;  // persistent history textures by id
    uint64_t m_frameCounter = 0;
    // upscaler resolution cache (see upscalers::resolve)
    std::string m_upscalerKey, m_upscalerNoteCache, m_vendorLibrary;
    UpscalerKind m_upscalerResolved = UpscalerKind::Off;
    uint64_t m_upscalerCheckFrame = 0;
    std::unordered_set<std::string> m_vendorFailed;
    rhi::MeshHandle m_gridMesh, m_debugMesh;
    rhi::TextureHandle m_white, m_flatNormal, m_brdfLut;
    rhi::TextureHandle m_envCube, m_irradianceCube, m_prefilteredCube;
    rhi::BufferHandle m_frameUbo;
    rhi::BufferHandle m_vmFrameUbo;  // 1.3.2: the Frame block of the viewmodel pass (its own projection)
    // 1.3.3 non-finite pixel counter (debug, off by default): mask shader + persistent RGBA8 mask read back after the frame
    bool m_nonFiniteCheck = false, m_injectNonFinite = false;
    rhi::ShaderHandle m_nonFinite, m_nfInject;
    rhi::TextureHandle m_nfMask;
    std::unique_ptr<TransientTexturePool> m_pool;
    std::unordered_map<std::string, rhi::MeshHandle> m_builtins;
    std::vector<rhi::LineVertex> m_debugLines;
    std::vector<OverlayVertex> m_overlay;
    rhi::MeshHandle m_overlayMesh;
    rhi::ShaderHandle m_overlayShader;
    rhi::TextureHandle m_overlayTexture;
    rhi::TextureHandle m_browserOverlay;
    rhi::MeshHandle m_browserOverlayMesh;
    void drawOverlay(int width, int height);
    std::vector<ParticleItem> m_particles;
    rhi::MeshHandle m_particleMesh[2];  // alpha, additive (rebuilt every frame)
    rhi::ShaderHandle m_particleShader;
    int drawParticles(const ViewItem& view);

    // environment state
    std::vector<float> m_equirect;
    int m_equirectW = 0, m_equirectH = 0;
    bool m_envDirty = true;
    bool m_procedural = true;
    Vec3 m_envSun{0, 1, 0};
    FrameInfo m_frame;
    Mat4 m_cascadeCacheVP[4]{};
    uint64_t m_cascadeCacheKey[4]{};
    bool m_cascadeCacheValid[4]{};
    int m_shadowCacheRes = 0, m_shadowCacheLayers = 0;
    rhi::TextureHandle m_giTex;
    Vec4 m_giMin{0, 0, 0, 1}, m_giExt{1, 1, 1, 0};  // w=0 disables probe GI
};

} // namespace fledge
