// MCP / assistant tools for the graphics suite: upscalers, graphics API, ray tracing, presets.
// Every mutating tool goes through EditorSession::setGraphicsSettings (one undoable [AI] step) and
// through the AI control gate in registerEngineTools' callWrapper (master switch + level + category).
// install_upscaler_version never accepts a vendor licence: it only queues a request that the editor
// shows to the human user as a licence dialog (Accept / Decline). Keep docs/MCP.md in sync.
#include <format>

#include "fledge/editor/editor_session.h"
#include "fledge/mcp/mcp_server.h"
#include "fledge/render/graphics_api.h"
#include "fledge/render/upscalers.h"

namespace fledge {

using mcp::Tool;
using mcp::ToolResult;

namespace {
Json schema(std::initializer_list<std::pair<const std::string, Json>> props, std::vector<std::string> required = {}) {
    Json p = Json::object();
    for (auto& [k, v] : props) p[k] = v;
    Json s = {{"type", "object"}, {"properties", p}, {"additionalProperties", false}};
    if (!required.empty()) s["required"] = required;
    return s;
}
Json enumOf(std::initializer_list<const char*> values, const char* description) {
    Json e = Json::array();
    for (const char* v : values) e.push_back(v);
    return {{"type", "string"}, {"enum", e}, {"description", description}};
}
Json versionJson(const upscalers::VersionEntry& v) {
    std::string why;
    bool installed = !v.files.empty() && upscalers::isInstalled(v, &why);
    Json j = {{"id", v.id},          {"product", v.product},       {"family", v.family},          {"version", v.version},
              {"date", v.date},      {"notes", v.notes},           {"downloadable", v.downloadable}, {"installed", installed},
              {"license", v.licenseName}, {"downloadBytes", v.downloadSize()}, {"source", v.sourceUrl}};
    if (!v.downloadable) j["unavailableReason"] = v.unavailableReason;
    return j;
}
ToolResult needProject() { return ToolResult::error(kNoProjectHelp); }
} // namespace

void registerGraphicsTools(mcp::ToolRegistry& reg, EditorSession& s) {
    reg.add({"list_upscalers", "List upscalers",
             "Lists the upscaler options (Off / TAA / FSR / XeSS / DLSS) with availability on this build and GPU, and every known vendor "
             "version (DLSS, XeSS, FidelityFX) with release date, download size, licence and whether it is installed. Versions that "
             "cannot be downloaded carry an unavailableReason.",
             schema({}), true, false, [&s](const Json&) {
                 Json kinds = Json::array();
                 for (UpscalerKind k : {UpscalerKind::Off, UpscalerKind::Taa, UpscalerKind::Fsr, UpscalerKind::XeSS, UpscalerKind::Dlss}) {
                     Json e = {{"upscaler", toString(k)}};
                     auto hw = upscalers::hardwareCheck(k, "");
                     auto in = upscalers::runtimeIntegration(k, rhi::Backend::D3D12);
                     e["hardwareOk"] = hw.ok;
                     if (!hw.ok) e["hardwareReason"] = hw.reason;
                     e["integrated"] = in.ok;
                     if (!in.ok) e["integrationReason"] = in.reason;
                     Json vs = Json::array();
                     for (const auto* v : upscalers::versionsFor(k)) vs.push_back(versionJson(*v));
                     e["versions"] = vs;
                     kinds.push_back(e);
                 }
                 Json out = {{"upscalers", kinds}, {"cache", narrow(upscalers::cacheRoot().wstring())}};
                 if (s.hasProject()) out["projectGraphics"] = s.project().graphics.toJson();
                 out["pendingInstallRequests"] = s.upscalerInstallRequests;
                 return ToolResult::json(out);
             }});

    reg.add({"install_upscaler_version", "Request an upscaler download",
             "Asks the human user to install a vendor upscaler version (see list_upscalers). The editor shows the vendor licence text "
             "and the download size; nothing is downloaded until the user clicks Accept. The AI cannot accept licences. Needs the "
             "'downloads' AI permission. Returns immediately; poll list_upscalers for 'installed'.",
             schema({{"id", {{"type", "string"}, {"description", "version id, e.g. xess-3.0.2"}}}}, {"id"}), false, false,
             [&s](const Json& a) {
                 const auto* v = upscalers::findVersion(a.value("id", ""));
                 if (!v) return ToolResult::error("unknown upscaler version '" + a.value("id", "") + "' (see list_upscalers)");
                 if (!v->downloadable) return ToolResult::error(v->id + " cannot be downloaded: " + v->unavailableReason);
                 if (upscalers::isInstalled(*v)) return ToolResult::json({{"id", v->id}, {"status", "already installed"}});
                 if (!s.hasLicenseUi)
                     return ToolResult::error("installing " + v->id + " requires a human to accept the " + v->licenseName +
                                              " in the Fledge editor (Graphics & Platform > Upscaler versions); this headless server cannot show it");
                 for (auto& r : s.upscalerInstallRequests)
                     if (r == v->id) return ToolResult::json({{"id", v->id}, {"status", "already waiting for the user's decision"}});
                 s.upscalerInstallRequests.push_back(v->id);
                 return ToolResult::json({{"id", v->id},
                                          {"status", "licence dialog shown to the user"},
                                          {"license", v->licenseName},
                                          {"downloadBytes", v->downloadSize()}});
             }});

    reg.add({"set_upscaler", "Set upscaler",
             "Sets the project's upscaler (off/taa/fsr/xess/dlss), quality mode (native/quality/balanced/performance/ultra-performance), "
             "preferred vendor version id and sharpness (0-1). Unavailable choices fall back at runtime (DLSS -> XeSS -> FSR -> TAA). "
             "Undoable.",
             schema({{"upscaler", enumOf({"off", "taa", "fsr", "xess", "dlss"}, "upscaler")},
                     {"quality", enumOf({"native", "quality", "balanced", "performance", "ultra-performance"}, "quality mode")},
                     {"version", {{"type", "string"}, {"description", "vendor version id (optional)"}}},
                     {"sharpness", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}},
                    {"upscaler"}),
             false, false, [&s](const Json& a) {
                 if (!s.hasProject()) return needProject();
                 GraphicsSettings g = s.project().graphics;
                 if (!fromString(a.value("upscaler", ""), g.upscaler)) return ToolResult::error("unknown upscaler");
                 if (a.contains("quality") && !fromString(a["quality"].get<std::string>(), g.upscalerQuality))
                     return ToolResult::error("unknown quality mode");
                 if (a.contains("version")) {
                     std::string id = a["version"].get<std::string>();
                     const auto* v = upscalers::findVersion(id);
                     if (!id.empty() && (!v || v->kind != g.upscaler)) return ToolResult::error("version '" + id + "' does not belong to " + toString(g.upscaler));
                     g.upscalerVersion = id;
                 }
                 if (a.contains("sharpness")) g.sharpness = a["sharpness"].get<float>();
                 g.preset = GraphicsPreset::Custom;
                 if (auto st = s.setGraphicsSettings(g, nullptr, "Set upscaler", EditSource::AI); !st) return ToolResult::error(st.error);
                 return ToolResult::json({{"graphics", s.project().graphics.toJson()}});
             }});

    reg.add({"set_graphics_api", "Set graphics API",
             "Sets the project's graphics API: auto, d3d12, d3d11, d3d10, d3d9, opengl or vulkan. The runtime falls back along the chain "
             "when the API is unavailable (e.g. d3d12 -> d3d11 -> opengl); d3d9 is a reduced feature level and vulkan a skeleton. "
             "The editor viewport stays on OpenGL. Undoable.",
             schema({{"api", enumOf({"auto", "d3d12", "d3d11", "d3d10", "d3d9", "opengl", "vulkan"}, "graphics API")}}, {"api"}), false, false,
             [&s](const Json& a) {
                 if (!s.hasProject()) return needProject();
                 GraphicsSettings g = s.project().graphics;
                 if (!rhi::graphicsApiFromString(a.value("api", ""), g.api)) return ToolResult::error("unknown graphics API");
                 if (auto st = s.setGraphicsSettings(g, nullptr, "Set graphics API", EditSource::AI); !st) return ToolResult::error(st.error);
                 Json chain = Json::array();
                 for (auto b : rhi::fallbackChain(g.api)) chain.push_back(rhi::toString(b));
                 return ToolResult::json({{"api", rhi::toString(g.api)}, {"fallbackChain", chain}, {"tier", rhi::tierLabel(rhi::toBackend(g.api))}});
             }});

    reg.add({"set_rt_options", "Set ray tracing options",
             "Turns hardware ray tracing (DXR, Direct3D 12) on/off and selects effects (shadows, reflections, ambientOcclusion, "
             "globalIllumination), quality (low/medium/high/ultra) and the denoiser. Off = pure raster. Undoable.",
             schema({{"enabled", {{"type", "boolean"}}},
                     {"shadows", {{"type", "boolean"}}},
                     {"reflections", {{"type", "boolean"}}},
                     {"ambientOcclusion", {{"type", "boolean"}}},
                     {"globalIllumination", {{"type", "boolean"}}},
                     {"denoise", {{"type", "boolean"}}},
                     {"quality", enumOf({"low", "medium", "high", "ultra"}, "ray tracing quality")}}),
             false, false, [&s](const Json& a) {
                 if (!s.hasProject()) return needProject();
                 GraphicsSettings g = s.project().graphics;
                 auto flag = [&](const char* k, bool& v) {
                     if (a.contains(k)) v = a[k].get<bool>();
                 };
                 flag("enabled", g.rt.enabled);
                 flag("shadows", g.rt.shadows);
                 flag("reflections", g.rt.reflections);
                 flag("ambientOcclusion", g.rt.ambientOcclusion);
                 flag("globalIllumination", g.rt.globalIllumination);
                 flag("denoise", g.rt.denoise);
                 if (a.contains("quality") && !fromString(a["quality"].get<std::string>(), g.rt.quality)) return ToolResult::error("unknown quality");
                 g.preset = GraphicsPreset::Custom;
                 if (auto st = s.setGraphicsSettings(g, nullptr, "Set ray tracing options", EditSource::AI); !st) return ToolResult::error(st.error);
                 Json out = {{"rayTracing", s.project().graphics.toJson()["rayTracing"]}};
                 auto sup = rhi::featureSupport(rhi::toBackend(g.api), rhi::GfxFeature::RayTracing);
                 if (g.rt.enabled && !sup.supported) out["note"] = "ray tracing will stay off on " + std::string(rhi::toString(g.api)) + ": " + sup.reason;
                 return ToolResult::json(out);
             }});

    reg.add({"apply_graphics_preset", "Apply graphics preset",
             "One-click quality preset: low, medium, high, ultra or everything (all ray-traced effects, maximum shadows, HDR). Sets the "
             "project's render + graphics blocks in one undoable step and returns the plain-language summary, the estimated GPU cost and "
             "features the selected API cannot do.",
             schema({{"preset", enumOf({"low", "medium", "high", "ultra", "everything"}, "preset")}}, {"preset"}), false, false,
             [&s](const Json& a) {
                 if (!s.hasProject()) return needProject();
                 GraphicsPreset p;
                 if (!fromString(a.value("preset", ""), p) || p == GraphicsPreset::Custom) return ToolResult::error("unknown preset");
                 GraphicsSettings g = s.project().graphics;
                 RenderSettings r = s.project().render;
                 std::vector<std::string> skipped;
                 rhi::Backend b = g.api == rhi::GraphicsApi::Auto ? rhi::Backend::D3D12 : rhi::toBackend(g.api);
                 applyGraphicsPreset(p, r, g, b, nullptr, &skipped);
                 if (auto st = s.setGraphicsSettings(g, &r, std::format("Apply graphics preset '{}'", toString(p)), EditSource::AI); !st)
                     return ToolResult::error(st.error);
                 const PresetInfo* info = presetInfo(p);
                 return ToolResult::json({{"preset", toString(p)},
                                          {"summary", info ? info->summary : ""},
                                          {"estimatedCost", estimateGpuCost(r, g)},
                                          {"unavailableOnThisApi", skipped},
                                          {"graphics", s.project().graphics.toJson()}});
             }});
}

} // namespace fledge
