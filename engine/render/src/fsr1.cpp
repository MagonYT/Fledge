// AMD FidelityFX Super Resolution 1 (EASU upscale + RCAS sharpening) as two fullscreen HLSL passes.
// The original ffx_a.h / ffx_fsr1.h (MIT, GPUOpen-Effects/FidelityFX-FSR v1.0.2, pinned in
// cmake/Dependencies.cmake) are embedded unmodified; only the texture callbacks and the pass entry
// points are written here. Shader model 5 (Gather) => Direct3D 11 / 12.
#include "fsr1.h"

#include "shaders_hlsl.h"

#ifdef FLEDGE_HAVE_FSR1
#include "ffx_a_text.h"
#include "ffx_fsr1_text.h"
#endif

namespace fledge::shaders {

#ifdef FLEDGE_HAVE_FSR1
namespace {
std::string text(const unsigned char* p, size_t n) { return std::string(reinterpret_cast<const char*>(p), n); }

const char* kEasuHead = R"(
cbuffer Params : register(b1) { float4 uInputSize; float4 uOutputSize; };
Texture2D InputTexture : register(t0); SamplerState s0 : register(s0);
#define A_GPU 1
#define A_HLSL 1
)";
const char* kEasuCallbacks = R"(
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return InputTexture.GatherRed(s0, p); }
AF4 FsrEasuGF(AF2 p) { return InputTexture.GatherGreen(s0, p); }
AF4 FsrEasuBF(AF2 p) { return InputTexture.GatherBlue(s0, p); }
)";
const char* kEasuMain = R"(
float4 PSMain(FsOut i) : SV_Target {
    AU4 con0, con1, con2, con3;
    FsrEasuCon(con0, con1, con2, con3, uInputSize.x, uInputSize.y, uInputSize.x, uInputSize.y, uOutputSize.x, uOutputSize.y);
    AF3 c;
    FsrEasuF(c, AU2(i.pos.xy), con0, con1, con2, con3);
    return float4(c, 1.0);
}
)";
const char* kRcasHead = R"(
cbuffer Params : register(b1) { float uSharpness; float3 padR; };
Texture2D InputTexture : register(t0); SamplerState s0 : register(s0);
#define A_GPU 1
#define A_HLSL 1
)";
const char* kRcasCallbacks = R"(
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return InputTexture.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
)";
const char* kRcasMain = R"(
float4 PSMain(FsOut i) : SV_Target {
    AU4 con;
    FsrRcasCon(con, (1.0 - saturate(uSharpness)) * 2.0);  // 0 stops = sharpest
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, AU2(i.pos.xy), con);
    return float4(c, 1.0);
}
)";
} // namespace

std::string fsr1EasuHlsl() {
    return std::string(hlsl::kFullscreen) + kEasuHead + text(kFfxAText, sizeof(kFfxAText)) + kEasuCallbacks + text(kFfxFsr1Text, sizeof(kFfxFsr1Text)) +
           kEasuMain;
}
std::string fsr1RcasHlsl() {
    return std::string(hlsl::kFullscreen) + kRcasHead + text(kFfxAText, sizeof(kFfxAText)) + kRcasCallbacks + text(kFfxFsr1Text, sizeof(kFfxFsr1Text)) +
           kRcasMain;
}
bool fsr1Available() { return true; }
#else
std::string fsr1EasuHlsl() { return {}; }
std::string fsr1RcasHlsl() { return {}; }
bool fsr1Available() { return false; }
#endif

} // namespace fledge::shaders
