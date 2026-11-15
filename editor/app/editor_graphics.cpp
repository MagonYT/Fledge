// Graphics & Platform panel, Project Settings window, upscaler licence dialog, auto-tune and
// "Preview in API" (editor_graphics.cpp). The editor draws with Direct3D 11 when it can; every
// setting here is a per-project value (the .fledge "graphics"/"render" blocks), changed
// through EditorSession::setGraphicsSettings so each click is one undoable step.
#include <format>

#include "fledge/packager/script_build.h"
#include "fledge/render/graphics_api.h"
#include "fledge/render/upscalers.h"
#include "editor_app.h"
#include "editor_icons.h"
#include <misc/cpp/imgui_stdlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fledge::editor {

namespace {
std::string mb(uint64_t bytes) { return std::format("{:.1f} MB", static_cast<double>(bytes) / 1e6); }

void tooltip(const std::string& text) {
    if (!text.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text.c_str());
}

// One-line "why is this greyed out" next to a disabled control.
void reasonLine(const std::string& reason) {
    if (reason.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s  %s", ICON_INFO, reason.c_str());
    ImGui::PopStyleColor();
}

const UpscalerKind kUpscalers[] = {UpscalerKind::Off, UpscalerKind::Taa, UpscalerKind::Fsr, UpscalerKind::XeSS, UpscalerKind::Dlss};
const char* upscalerLabel(UpscalerKind k) {
    switch (k) {
        case UpscalerKind::Off: return "Off";
        case UpscalerKind::Taa: return "TAA (built-in temporal AA / upscaler)";
        case UpscalerKind::Fsr: return "AMD FSR 1";
        case UpscalerKind::XeSS: return "Intel XeSS";
        case UpscalerKind::Dlss: return "NVIDIA DLSS";
    }
    return "?";
}
const char* qualityLabel(UpscalerQuality q) {
    switch (q) {
        case UpscalerQuality::Native: return "Native (AA only, 100%)";
        case UpscalerQuality::Quality: return "Quality (67%)";
        case UpscalerQuality::Balanced: return "Balanced (59%)";
        case UpscalerQuality::Performance: return "Performance (50%)";
        case UpscalerQuality::UltraPerformance: return "Ultra Performance (33%)";
    }
    return "?";
}
} // namespace

// ----------------------------------------------------------------- probes ---
const rhi::ApiProbe& EditorApp::apiProbe(rhi::Backend b) {
    if (!m_apiProbed) {
        m_apiProbed = true;
        for (rhi::Backend k : {rhi::Backend::D3D12, rhi::Backend::D3D11, rhi::Backend::D3D10, rhi::Backend::D3D9, rhi::Backend::OpenGL, rhi::Backend::Vulkan})
            m_apiProbes[k] = rhi::probeApi(k);
    }
    return m_apiProbes[b];
}

rhi::Backend EditorApp::gameBackend(const GraphicsSettings& g) {
    for (rhi::Backend b : rhi::fallbackChain(g.api))
        if (b == rhi::Backend::OpenGL || apiProbe(b).available) {
            if (g.api == rhi::GraphicsApi::Auto && b == rhi::Backend::D3D9) continue;
            return b;
        }
    return rhi::Backend::OpenGL;
}

void EditorApp::applyGraphics(const GraphicsSettings& g, const RenderSettings* r, const std::string& label) {
    if (auto st = m_session.setGraphicsSettings(g, r, label); !st) toast(ToastLevel::Error, st.error);
}

// Slider bound to a project value: edits a local copy while dragging, applies one undoable step on release.
bool EditorApp::gfxSlider(const char* label, float& buffer, float projectValue, float lo, float hi, const char* fmt, int flags) {
    if (m_gfxActive != label) buffer = projectValue;
    ImGui::SliderFloat(label, &buffer, lo, hi, fmt, flags);
    if (ImGui::IsItemActivated()) m_gfxActive = label;
    bool apply = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemDeactivated()) m_gfxActive.clear();
    return apply;
}

// ------------------------------------------------------------------ panel ---
void EditorApp::drawGraphicsPanel() {
    PanelDef* p = panel("Graphics & Platform");
    if (!ImGui::Begin("Graphics & Platform", p ? &p->open : nullptr)) {
        ImGui::End();
        return;
    }
    drawGraphicsContent();
    ImGui::End();
}

void EditorApp::drawGraphicsContent() {
    float s = effectiveScale();
    if (!m_session.hasProject()) {
        ImGui::TextDisabled("Open a project to edit its graphics settings.");
        return;
    }
    GraphicsSettings g = m_session.project().graphics;
    RenderSettings r = m_session.project().render;
    const rhi::Backend target = gameBackend(g);
    const bool targetIsD3d = target == rhi::Backend::D3D11 || target == rhi::Backend::D3D12;
    const rhi::ApiProbe& d12 = apiProbe(rhi::Backend::D3D12);
    rhi::DeviceCaps targetCaps;
    targetCaps.fullPipeline = target != rhi::Backend::D3D9 && target != rhi::Backend::Vulkan;
    targetCaps.maxSamples = 8;
    targetCaps.rayTracing = target == rhi::Backend::D3D12 && d12.rayTracing;
    targetCaps.hdrOutput = true;  // display mode is only known at runtime; the game reports it in its settings menu

    ImGui::TextDisabled("%s  The game renders with %s (%s).  The editor draws with %s.", ICON_INFO, rhi::toString(target), rhi::tierLabel(target),
                        m_device ? rhi::toString(m_device->backend()) : "OpenGL");

    // ---------------------------------------------------------------- presets (one click)
    ImGui::SeparatorText(ICON_WAND "  Quality preset");
    for (const auto& info : graphicsPresets()) {
        bool on = g.preset == info.preset;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(info.name, {92 * s, 0})) {
            GraphicsSettings ng = g;
            RenderSettings nr = r;
            applyGraphicsPreset(info.preset, nr, ng, target, &targetCaps);
            applyGraphics(ng, &nr, std::format("Graphics preset: {}", info.name));
        }
        if (on) ImGui::PopStyleColor();
        trackRect(std::string("gfx.preset.") + info.name);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nEstimated GPU cost: %s of Medium", info.summary, info.cost);
        ImGui::SameLine(0, 4 * s);
    }
    ImGui::BeginDisabled(m_autoTuneRunning || target == rhi::Backend::Vulkan);
    if (ImGui::Button(m_autoTuneRunning ? ICON_HOURGLASS " Measuring..." : ICON_GAUGE " Auto-tune")) startAutoTune();
    ImGui::EndDisabled();
    tooltip("Runs the game for a few seconds with each preset (separate window) and picks the best one that holds 60 FPS on this PC.");
    if (const PresetInfo* info = presetInfo(g.preset)) ImGui::TextWrapped("%s", info->summary);
    else ImGui::TextDisabled("Custom settings (changed after the last preset).");
    std::vector<std::string> skipped;
    {
        GraphicsSettings tg = g;
        RenderSettings tr = r;
        if (g.preset != GraphicsPreset::Custom) applyGraphicsPreset(g.preset, tr, tg, target, &targetCaps, &skipped);
    }
    ImGui::Text("Estimated GPU cost: %.2fx Medium", estimateGpuCost(r, g));
    for (auto& sk : skipped) reasonLine("Not on " + std::string(rhi::toString(target)) + ": " + sk);
    if (!m_autoTuneResult.empty()) ImGui::TextWrapped("%s %s", ICON_GAUGE, m_autoTuneResult.c_str());

    // ---------------------------------------------------------------- graphics API
    ImGui::SeparatorText(ICON_LAYERS "  Graphics API");
    ImGui::SetNextItemWidth(260 * s);
    std::string current = std::string(rhi::displayName(g.api)) + (g.api == rhi::GraphicsApi::Auto ? "" : std::string("  - ") + rhi::tierLabel(rhi::toBackend(g.api)));
    if (ImGui::BeginCombo("API", current.c_str())) {
        for (rhi::GraphicsApi a : rhi::kAllGraphicsApis) {
            bool avail = a == rhi::GraphicsApi::Auto || a == rhi::GraphicsApi::OpenGL || apiProbe(rhi::toBackend(a)).available;
            std::string label = std::string(rhi::displayName(a));
            if (a != rhi::GraphicsApi::Auto) label += std::string("  - ") + rhi::tierLabel(rhi::toBackend(a));
            if (!avail) label += "  (unavailable)";
            if (ImGui::Selectable(label.c_str(), g.api == a)) {
                GraphicsSettings ng = g;
                ng.api = a;
                applyGraphics(ng, nullptr, std::format("Graphics API: {}", rhi::displayName(a)));
            }
            if (a != rhi::GraphicsApi::Auto) tooltip(apiProbe(rhi::toBackend(a)).detail);
        }
        ImGui::EndCombo();
    }
    {
        std::string chain;
        for (rhi::Backend b : rhi::fallbackChain(g.api)) chain += (chain.empty() ? "" : " -> ") + std::string(rhi::toString(b));
        ImGui::TextDisabled("Fallback chain: %s", chain.c_str());
    }
    if (ImGui::Button((std::string(ICON_PLAY " Preview in ") + rhi::toString(target)).c_str())) launchPreview(rhi::toString(target), {});
    tooltip("Starts the game from the current project in a separate window using this API.");

    // ---------------------------------------------------------------- upscaling
    ImGui::SeparatorText(ICON_IMAGE "  Upscaling & anti-aliasing");
    ImGui::SetNextItemWidth(260 * s);
    if (ImGui::BeginCombo("Upscaler", upscalerLabel(g.upscaler))) {
        for (UpscalerKind k : kUpscalers) {
            rhi::FeatureSupport fs{true, {}};
            if (k == UpscalerKind::Taa) fs = rhi::featureSupport(target, rhi::GfxFeature::Taa, &targetCaps);
            if (k == UpscalerKind::Fsr) fs = rhi::featureSupport(target, rhi::GfxFeature::Fsr, &targetCaps);
            if (k == UpscalerKind::XeSS) fs = rhi::featureSupport(target, rhi::GfxFeature::XeSS, &targetCaps);
            if (k == UpscalerKind::Dlss) fs = rhi::featureSupport(target, rhi::GfxFeature::Dlss, &targetCaps);
            ImGui::BeginDisabled(!fs.supported);
            if (ImGui::Selectable(upscalerLabel(k), g.upscaler == k)) {
                GraphicsSettings ng = g;
                ng.upscaler = k;
                ng.preset = GraphicsPreset::Custom;
                applyGraphics(ng, nullptr, std::format("Upscaler: {}", upscalerLabel(k)));
            }
            ImGui::EndDisabled();
            tooltip(fs.reason);
        }
        ImGui::EndCombo();
    }
    trackRect("gfx.upscaler");
    ImGui::BeginDisabled(g.upscaler == UpscalerKind::Off);
    ImGui::SetNextItemWidth(260 * s);
    if (ImGui::BeginCombo("Quality mode", qualityLabel(g.upscalerQuality))) {
        for (UpscalerQuality q : {UpscalerQuality::Native, UpscalerQuality::Quality, UpscalerQuality::Balanced, UpscalerQuality::Performance,
                                  UpscalerQuality::UltraPerformance})
            if (ImGui::Selectable(qualityLabel(q), g.upscalerQuality == q)) {
                GraphicsSettings ng = g;
                ng.upscalerQuality = q;
                ng.preset = GraphicsPreset::Custom;
                applyGraphics(ng, nullptr, "Upscaler quality");
            }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(260 * s);
    if (gfxSlider("Sharpness", m_gfxSharpness, g.sharpness, 0.0f, 1.0f, "%.2f")) {
        GraphicsSettings ng = g;
        ng.sharpness = m_gfxSharpness;
        applyGraphics(ng, nullptr, "Upscaler sharpness");
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(g.upscaler != UpscalerKind::Off);
    ImGui::SetNextItemWidth(260 * s);
    if (gfxSlider("Render scale", m_gfxRenderScale, g.renderScale, 0.25f, 2.0f, "%.2fx")) {
        GraphicsSettings ng = g;
        ng.renderScale = m_gfxRenderScale;
        ng.preset = GraphicsPreset::Custom;
        applyGraphics(ng, nullptr, "Render scale");
    }
    ImGui::EndDisabled();
    if (g.upscaler != UpscalerKind::Off && g.upscaler != UpscalerKind::Taa) {
        auto res = upscalers::resolve(g, target, "", targetIsD3d, m_session.toolsDir);
        if (res.kind != g.upscaler)
            reasonLine(std::format("Will run {} instead: {}", upscalerLabel(res.kind), res.note));
        else if (res.version)
            ImGui::TextDisabled("%s %s  (version %s)", ICON_CHECK, upscalerLabel(res.kind), res.version->version.c_str());
    }

    // ---------------------------------------------------------------- ray tracing
    ImGui::SeparatorText(ICON_LIGHTBULB "  Ray tracing (DXR)");
    rhi::FeatureSupport rt = rhi::featureSupport(target, rhi::GfxFeature::RayTracing, &targetCaps);
    if (rt.supported && target == rhi::Backend::D3D12 && !d12.rayTracing) rt = {false, "this GPU/driver does not report DXR support"};
    ImGui::BeginDisabled(!rt.supported);
    bool rtOn = g.rt.enabled && rt.supported;
    if (ImGui::Checkbox("Enable ray tracing", &rtOn)) {
        GraphicsSettings ng = g;
        ng.rt.enabled = rtOn;
        ng.preset = GraphicsPreset::Custom;
        applyGraphics(ng, nullptr, rtOn ? "Ray tracing on" : "Ray tracing off");
    }
    trackRect("gfx.rt");
    ImGui::BeginDisabled(!rtOn);
    auto rtFlag = [&](const char* label, bool RayTracingSettings::*field) {
        bool v = g.rt.*field;
        if (ImGui::Checkbox(label, &v)) {
            GraphicsSettings ng = g;
            ng.rt.*field = v;
            ng.preset = GraphicsPreset::Custom;
            applyGraphics(ng, nullptr, std::format("Ray tracing: {} {}", label, v ? "on" : "off"));
        }
        ImGui::SameLine(0, 14 * s);
    };
    rtFlag("Shadows", &RayTracingSettings::shadows);
    rtFlag("Reflections", &RayTracingSettings::reflections);
    rtFlag("Ambient occlusion", &RayTracingSettings::ambientOcclusion);
    rtFlag("Global illumination", &RayTracingSettings::globalIllumination);
    ImGui::NewLine();
    const char* rq[] = {"Low", "Medium", "High", "Ultra"};
    int q = static_cast<int>(g.rt.quality);
    ImGui::SetNextItemWidth(160 * s);
    if (ImGui::Combo("RT quality", &q, rq, 4)) {
        GraphicsSettings ng = g;
        ng.rt.quality = static_cast<RtQuality>(q);
        ng.preset = GraphicsPreset::Custom;
        applyGraphics(ng, nullptr, std::format("Ray tracing quality: {}", rq[q]));
    }
    ImGui::SameLine();
    bool dn = g.rt.denoise;
    if (ImGui::Checkbox("Denoiser", &dn)) {
        GraphicsSettings ng = g;
        ng.rt.denoise = dn;
        applyGraphics(ng, nullptr, dn ? "Denoiser on" : "Denoiser off");
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    reasonLine(rt.supported ? std::string() : rt.reason);
    ImGui::BeginDisabled(!d12.available || !d12.rayTracing);
    if (ImGui::Button(ICON_CAMERA " Path-traced reference")) launchPreview("d3d12", {"--path-trace", "--freeze-time"});
    ImGui::EndDisabled();
    tooltip("Opens a Direct3D 12 window that accumulates a progressive path-traced image of the startup scene (needs DXR).");

    // ---------------------------------------------------------------- display
    ImGui::SeparatorText(ICON_VIDEO "  Display");
    rhi::FeatureSupport hdr = rhi::featureSupport(target, rhi::GfxFeature::HdrOutput, nullptr);
    ImGui::BeginDisabled(!hdr.supported);
    bool hdrOn = g.hdr && hdr.supported;
    if (ImGui::Checkbox("HDR output (HDR10)", &hdrOn)) {
        GraphicsSettings ng = g;
        ng.hdr = hdrOn;
        applyGraphics(ng, nullptr, hdrOn ? "HDR on" : "HDR off");
    }
    ImGui::BeginDisabled(!hdrOn);
    ImGui::SetNextItemWidth(200 * s);
    if (gfxSlider("Paper white", m_gfxPaperWhite, g.paperWhiteNits, 80, 500, "%.0f nits")) {
        GraphicsSettings ng = g;
        ng.paperWhiteNits = m_gfxPaperWhite;
        applyGraphics(ng, nullptr, "HDR paper white");
    }
    ImGui::SetNextItemWidth(200 * s);
    if (gfxSlider("Peak brightness", m_gfxPeak, g.peakNits, 400, 10000, "%.0f nits", ImGuiSliderFlags_Logarithmic)) {
        GraphicsSettings ng = g;
        ng.peakNits = m_gfxPeak;
        applyGraphics(ng, nullptr, "HDR peak brightness");
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    reasonLine(hdr.supported ? "HDR switches on only when Windows HDR is enabled for the display; otherwise the game stays SDR." : hdr.reason);
    const char* vs[] = {"VSync on", "VSync off", "Variable refresh (G-Sync / FreeSync)"};
    int vi = static_cast<int>(g.vsync);
    ImGui::SetNextItemWidth(260 * s);
    if (ImGui::Combo("Sync", &vi, vs, 3)) {
        GraphicsSettings ng = g;
        ng.vsync = static_cast<VsyncMode>(vi);
        applyGraphics(ng, nullptr, std::format("Sync: {}", vs[vi]));
    }
    const char* wm[] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};
    int wi = static_cast<int>(g.windowMode);
    ImGui::SetNextItemWidth(260 * s);
    if (ImGui::Combo("Window mode", &wi, wm, 3)) {
        GraphicsSettings ng = g;
        ng.windowMode = static_cast<WindowMode>(wi);
        applyGraphics(ng, nullptr, std::format("Window mode: {}", wm[wi]));
    }
    int res[2] = {g.width, g.height};
    ImGui::SetNextItemWidth(160 * s);
    ImGui::InputInt2("Resolution (0 = desktop)", res);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        GraphicsSettings ng = g;
        ng.width = res[0];
        ng.height = res[1];
        applyGraphics(ng, nullptr, "Fullscreen resolution");
    }

    // ---------------------------------------------------------------- advanced
    if (ImGui::CollapsingHeader(ICON_SLIDERS "  Advanced")) {
        ImGui::SeparatorText("Capabilities");
        if (ImGui::BeginTable("caps", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
            for (int f = 0; f < static_cast<int>(rhi::GfxFeature::Count); ++f) {
                auto fs = rhi::featureSupport(target, static_cast<rhi::GfxFeature>(f), &targetCaps);
                if (static_cast<rhi::GfxFeature>(f) == rhi::GfxFeature::RayTracing || static_cast<rhi::GfxFeature>(f) == rhi::GfxFeature::PathTracing)
                    if (fs.supported && !d12.rayTracing) fs = {false, "this GPU/driver does not report DXR support"};
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(rhi::toString(static_cast<rhi::GfxFeature>(f)));
                ImGui::TableNextColumn();
                if (fs.supported) ImGui::TextColored({0.4f, 0.8f, 0.45f, 1}, ICON_CHECK " available");
                else ImGui::TextDisabled(ICON_XMARK " %s", fs.reason.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Shader cache");
        bool sc = g.shaderCache;
        if (ImGui::Checkbox("Persistent shader cache", &sc)) {
            GraphicsSettings ng = g;
            ng.shaderCache = sc;
            applyGraphics(ng, nullptr, sc ? "Shader cache on" : "Shader cache off");
        }
        ImGui::SameLine();
        fs::path cache = enginePaths().userData / "Games" / pathFromUtf8(m_session.project().gameName) / "ShaderCache";
        if (ImGui::SmallButton("Clear")) {
            std::error_code ec;
            uintmax_t n = fs::remove_all(cache, ec);
            toast(ToastLevel::Info, std::format("Shader cache cleared ({} files)", n > 0 ? n - 1 : 0));
        }
        ImGui::TextDisabled("%s", narrow(cache.wstring()).c_str());
        drawUpscalerVersions();
    }
}

// ----------------------------------------------------------- versions table ---
void EditorApp::drawUpscalerVersions() {
    ImGui::SeparatorText(ICON_DOWNLOAD "  Upscaler versions");
    ImGui::TextDisabled("Cache: %s", narrow(upscalers::cacheRoot().wstring()).c_str());
    ImGui::TextWrapped("Vendor runtimes are downloaded only after you have read and accepted the vendor licence. Versions sit side by side; "
                       "packaging ships the version selected for the project.");
    GraphicsSettings g = m_session.project().graphics;
    for (UpscalerKind kind : {UpscalerKind::Dlss, UpscalerKind::XeSS, UpscalerKind::Fsr}) {
        auto hw = upscalers::hardwareCheck(kind, "");
        auto in = upscalers::runtimeIntegration(kind, rhi::Backend::D3D12);
        std::string header = std::format("{}##ver{}", upscalerLabel(kind), static_cast<int>(kind));
        if (!ImGui::TreeNodeEx(header.c_str(), kind == UpscalerKind::XeSS ? ImGuiTreeNodeFlags_DefaultOpen : 0)) continue;
        if (!hw.ok) reasonLine(hw.reason);
        if (!in.ok) reasonLine(in.reason);
        if (ImGui::BeginTable(header.c_str(), 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Version");
            ImGui::TableSetupColumn("Released");
            ImGui::TableSetupColumn("Size");
            ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (const auto* v : upscalers::versionsFor(kind)) {
                ImGui::PushID(v->id.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool selected = g.upscalerVersion == v->id;
                ImGui::BeginDisabled(!v->downloadable);
                ImGui::Text("%s%s", v->family.c_str(), v->family == v->version ? "" : (" " + v->version).c_str());
                ImGui::EndDisabled();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", v->date.empty() ? "-" : v->date.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", v->files.empty() ? "-" : mb(v->downloadSize()).c_str());
                ImGui::TableNextColumn();
                if (!v->downloadable) ImGui::TextDisabled("%s", v->unavailableReason.c_str());
                else ImGui::TextDisabled("%s", v->notes.c_str());
                ImGui::TableNextColumn();
                if (v->downloadable) {
                    bool installed = upscalers::isInstalled(*v);
                    bool busy = m_upscalerBusy.contains(v->id);
                    if (busy) {
                        ImGui::TextDisabled(ICON_HOURGLASS " working...");
                    } else if (installed) {
                        if (selected) ImGui::TextColored({0.4f, 0.8f, 0.45f, 1}, ICON_CHECK " used");
                        else if (ImGui::SmallButton("Use")) {
                            GraphicsSettings ng = g;
                            ng.upscaler = kind;
                            ng.upscalerVersion = v->id;
                            applyGraphics(ng, nullptr, "Upscaler version " + v->id);
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove")) {
                            editor::UpscalerManager mgr(nullptr);
                            auto st = mgr.remove(*v);
                            toast(st ? ToastLevel::Info : ToastLevel::Error, st ? v->id + " removed" : st.error);
                        }
                    } else if (ImGui::SmallButton(ICON_DOWNLOAD " Download...")) {
                        openLicenseDialog(v->id, false);
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
}

// ----------------------------------------------------------- licence dialog ---
void EditorApp::openLicenseDialog(const std::string& id, bool requestedByAi) {
    const auto* v = upscalers::findVersion(id);
    if (!v || !v->downloadable) return;
    m_license = {};
    m_license.id = id;
    m_license.open = true;
    m_license.loading = true;
    m_license.requestedByAi = requestedByAi;
    std::string vid = id;
    runInBackground("Fetching licence for " + id, [this, vid](BackgroundTask& t) {
        t.progress = -1;
        const auto* v = upscalers::findVersion(vid);
        editor::UpscalerManager mgr(std::shared_ptr<ai::HttpTransport>(ai::createDefaultHttpTransport()));
        auto preview = mgr.fetchLicense(*v);
        m_mainQueue.post([this, vid, preview] {
            if (m_license.id != vid) return;
            m_license.loading = false;
            m_license.text = preview.text;
            m_license.error = preview.error;
            m_license.hashOk = preview.hashMatches;
        });
        return preview.ok ? std::string("licence loaded") : preview.error;
    });
}

void EditorApp::drawLicenseDialog() {
    // AI tools may only *request* an install; each request becomes this dialog for the human user.
    if (!m_license.open && !m_session.upscalerInstallRequests.empty()) {
        std::string id = m_session.upscalerInstallRequests.front();
        m_session.upscalerInstallRequests.erase(m_session.upscalerInstallRequests.begin());
        openLicenseDialog(id, true);
    }
    if (!m_license.open) return;
    const auto* v = upscalers::findVersion(m_license.id);
    if (!v) {
        m_license.open = false;
        return;
    }
    float s = effectiveScale();
    ImGui::OpenPopup("Upscaler licence###upscaler_licence");
    ImGui::SetNextWindowSize({640 * s, 520 * s}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("Upscaler licence###upscaler_licence", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Text("%s %s", v->product.c_str(), v->version.c_str());
        ImGui::TextDisabled("Licence: %s   |   Download: %s   |   Source: %s", v->licenseName.c_str(), mb(v->downloadSize()).c_str(),
                            v->sourceUrl.c_str());
        if (m_license.requestedByAi) ImGui::TextColored({0.95f, 0.7f, 0.3f, 1}, ICON_ROBOT " Requested by an AI tool. Only you can accept this licence.");
        ImGui::Separator();
        ImGui::BeginChild("lic", {0, -ImGui::GetFrameHeightWithSpacing() * 2.2f}, ImGuiChildFlags_Borders);
        if (m_license.loading) ImGui::TextDisabled(ICON_HOURGLASS " Loading the licence text...");
        else if (!m_license.error.empty()) ImGui::TextColored({1, 0.45f, 0.4f, 1}, "%s", m_license.error.c_str());
        else ImGui::TextWrapped("%s", m_license.text.c_str());
        ImGui::EndChild();
        if (!m_license.loading && m_license.error.empty() && !m_license.hashOk)
            ImGui::TextColored({0.95f, 0.7f, 0.3f, 1}, ICON_WARNING " The licence text differs from the version catalogued with this Fledge build.");
        ImGui::BeginDisabled(m_license.loading || !m_license.error.empty());
        if (ImGui::Button(ICON_CHECK " Accept and download")) {
            std::string vid = v->id;
            m_upscalerBusy.insert(vid);
            m_license.open = false;
            runInBackground("Downloading " + vid, [this, vid](BackgroundTask& t) {
                const auto* ver = upscalers::findVersion(vid);
                editor::UpscalerManager mgr(std::shared_ptr<ai::HttpTransport>(ai::createDefaultHttpTransport()));
                uint64_t total = ver->downloadSize(), done = 0;
                auto st = mgr.install(*ver, true, [&](const std::string& file, uint64_t d, uint64_t size) {
                    if (d == size) done += size;
                    t.progress = total ? static_cast<float>(done) / static_cast<float>(total) : -1.0f;
                    std::lock_guard l(m_taskMutex);
                    t.detail = file;
                });
                m_mainQueue.post([this, vid, st] {
                    m_upscalerBusy.erase(vid);
                    toast(st ? ToastLevel::Success : ToastLevel::Error, st ? vid + " installed" : st.error);
                });
                return st ? vid + " installed" : st.error;
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Decline")) {
            m_license.open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ------------------------------------------------------------ project settings ---
void EditorApp::drawProjectSettings() {
    if (!m_showProjectSettings) return;
    float s = effectiveScale();
    ImGui::SetNextWindowSize({820 * s, 640 * s}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (!ImGui::Begin(ICON_SLIDERS "  Project Settings###ProjectSettings", &m_showProjectSettings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    const char* pages[] = {"General", "Graphics & Platform", "Packaging"};
    const char* icons[] = {ICON_HOUSE, ICON_GAUGE, ICON_BOX};
    ImGui::BeginChild("pages", {180 * s, 0}, ImGuiChildFlags_Borders);
    for (int i = 0; i < 3; ++i)
        if (ImGui::Selectable((std::string(icons[i]) + "  " + pages[i]).c_str(), m_projectSettingsPage == pages[i])) m_projectSettingsPage = pages[i];
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("page", {0, 0});
    if (!m_session.hasProject()) {
        ImGui::TextDisabled("No project is open.");
    } else if (m_projectSettingsPage == "General") {
        const Project& live = m_session.project();
        static Project draft;
        static fs::path draftFile;
        static bool dirty = false;
        if (draftFile != live.file) {
            draft = live;
            draftFile = live.file;
            dirty = false;
        } else if (!dirty) draft = live;
        ImGui::SeparatorText("Identity");
        ImGui::SetNextItemWidth(280 * s);
        dirty |= ImGui::InputText("Name", &draft.name);
        ImGui::SetNextItemWidth(280 * s);
        dirty |= ImGui::InputText("Game name", &draft.gameName);
        ImGui::TextDisabled("Used as the executable name. Letters, digits, '_' and '-' only.");
        ImGui::SetNextItemWidth(280 * s);
        dirty |= ImGui::InputText("Startup scene", &draft.startupScene);
        ImGui::TextDisabled("Content-relative, for example Scenes/Main.fscene");
        ImGui::SetNextItemWidth(280 * s);
        dirty |= ImGui::InputText("Icon", &draft.icon);
        ImGui::TextDisabled("Optional .ico next to the project file. Applied when packaging.");
        ImGui::SeparatorText("Modules");
        ImGui::TextDisabled("Native modules the packaged game loads, in addition to the script module.");
        int removeModule = -1;
        for (int i = 0; i < static_cast<int>(draft.modules.size()); ++i) {
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(280 * s);
            dirty |= ImGui::InputText("##module", &draft.modules[static_cast<size_t>(i)]);
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) removeModule = i;
            ImGui::PopID();
        }
        if (removeModule >= 0) {
            draft.modules.erase(draft.modules.begin() + removeModule);
            dirty = true;
        }
        if (ImGui::SmallButton("Add module")) {
            draft.modules.emplace_back();
            dirty = true;
        }
        ImGui::SeparatorText("Window");
        ImGui::SetNextItemWidth(280 * s);
        dirty |= ImGui::InputText("Title", &draft.windowTitle);
        ImGui::SetNextItemWidth(120 * s);
        dirty |= ImGui::InputInt("Width", &draft.windowWidth);
        ImGui::SetNextItemWidth(120 * s);
        dirty |= ImGui::InputInt("Height", &draft.windowHeight);
        dirty |= ImGui::Checkbox("GUI subsystem (no console window when the game is double-clicked)", &draft.guiSubsystem);
        ImGui::SeparatorText("UI");
        ImGui::SetNextItemWidth(280 * s);
        dirty |= ImGui::InputText("Font", &draft.ui.font);
        ImGui::TextDisabled("A .ttf or .otf under Content. Empty uses the built-in pixel font.");
        ImGui::SetNextItemWidth(120 * s);
        dirty |= ImGui::SliderFloat("Font size", &draft.ui.fontSize, 8, 96, "%.0f px");
        ImGui::SetNextItemWidth(120 * s);
        dirty |= ImGui::InputText("Language", &draft.ui.language);
        ImGui::SetNextItemWidth(120 * s);
        dirty |= ImGui::InputText("Fallback language", &draft.ui.fallbackLanguage);
        ImGui::SeparatorText("Cook");
        const char* cooks[] = {"auto", "bc1", "bc3", "none"};
        int cook = 0;
        for (int i = 0; i < 4; ++i)
            if (draft.textureCompression == cooks[i]) cook = i;
        ImGui::SetNextItemWidth(160 * s);
        if (ImGui::Combo("Texture compression", &cook, "Auto\0BC1\0BC3\0None\0")) {
            draft.textureCompression = cooks[cook];
            dirty = true;
        }
        dirty |= ImGui::Checkbox("Generate texture mipmaps", &draft.textureMips);
        dirty |= ImGui::Checkbox("Generate mesh tangents", &draft.meshTangents);
        std::string fhtml = draft.extra.contains("fhtmlPage") && draft.extra["fhtmlPage"].is_string() ? draft.extra["fhtmlPage"].get<std::string>() : "";
        ImGui::SetNextItemWidth(280 * s);
        if (ImGui::InputText("FHTML page", &fhtml)) {
            dirty = true;
            if (fhtml.empty()) draft.extra.erase("fhtmlPage");
            else draft.extra["fhtmlPage"] = fhtml;
        }
        ImGui::TextDisabled("Content-relative .fhtml. Empty disables the browser overlay.");
        ImGui::Spacing();
        ImGui::BeginDisabled(!dirty);
        if (ImGui::Button("Save project", {140 * s, 0})) {
            std::erase_if(draft.modules, [](const std::string& m) { return m.empty(); });
            if (auto st = m_session.commitProject(draft, "Project settings"); !st) toast(ToastLevel::Error, st.error);
            else {
                dirty = false;
                draft = m_session.project();
                toast(ToastLevel::Success, "Saved " + m_session.project().name);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            draft = live;
            dirty = false;
        }
        ImGui::TextDisabled("%s", narrow(live.file.wstring()).c_str());
    } else if (m_projectSettingsPage == "Graphics & Platform") {
        drawGraphicsContent();
    } else {
        const GraphicsSettings& g = m_session.project().graphics;
        bool splash = m_session.project().splash;
        if (ImGui::Checkbox("Show the Fledge boot splash when the game starts", &splash)) {
            if (auto st = m_session.setBootSplash(splash); !st) toast(ToastLevel::Error, st.error);
        }
        ImGui::TextDisabled("Both editions. Turn this off and the game opens straight on the first frame.");
        ImGui::SeparatorText("Package layout");
        if (ImGui::RadioButton("One portable .exe", m_prefs.singleExePackage)) {
            m_prefs.singleExePackage = true;
            saveSettings();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Folder (Game.fpak next to the .exe)", !m_prefs.singleExePackage)) {
            m_prefs.singleExePackage = false;
            saveSettings();
        }
        ImGui::TextDisabled("File > Package uses this layout immediately: cook used textures, bake environment lighting, then these options.");
        ImGui::SeparatorText("When packaging");
        if (ImGui::Checkbox("Run in background", &m_prefs.packageRunInBackground)) saveSettings();
        ImGui::SameLine();
        if (ImGui::Checkbox("Notify when it finishes", &m_prefs.notifyWhenPackageDone)) saveSettings();
        if (ImGui::Checkbox("Open the folder when it finishes", &m_prefs.openFolderWhenDone)) saveSettings();
        if (ImGui::Checkbox("Shut down the computer when packaging finishes", &m_prefs.shutdownAfterPackage)) saveSettings();
        if (m_prefs.shutdownAfterPackage)
            ImGui::TextDisabled("Only after a successful package. Windows waits 60 seconds. Cancel with: shutdown /a");
        ImGui::Spacing();
        ImGui::TextWrapped("Packaged games carry the graphics settings above (graphics API with its fallback chain, preset, upscaler, ray "
                           "tracing, HDR, display mode). Players can change them in the in-game settings menu (F10); their choices are "
                           "stored per user, never in the game folder.");
        if (g.upscaler == UpscalerKind::Dlss || g.upscaler == UpscalerKind::XeSS) {
            const auto* v = upscalers::selectVersion(g.upscaler, g.upscalerVersion);
            if (v) ImGui::Text("%s Ships %s %s and its licence next to the game.", ICON_CHECK, v->product.c_str(), v->version.c_str());
            else reasonLine(std::format("No {} version is installed: the packaged game falls back to FSR/TAA. Install one under Graphics & "
                                        "Platform > Advanced > Upscaler versions.",
                                        upscalerLabel(g.upscaler)));
        } else {
            ImGui::TextDisabled("No vendor runtime is shipped (upscaler: %s).", upscalerLabel(g.upscaler));
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// ------------------------------------------------------------ preview / auto-tune ---
void EditorApp::launchPreview(const std::string& api, const std::vector<std::string>& extra) {
    if (!m_session.hasProject()) return;
    if (m_session.dirty) (void)m_session.saveScene();
#ifdef _WIN32
    std::wstring cmd = L"\"" + (m_session.runtimeExe()).wstring() + L"\" --project \"" + m_session.project().file.wstring() +
                       L"\" --rhi " + widen(api);
    for (auto& e : extra) cmd += L" " + widen(e);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        toast(ToastLevel::Info, "Started the game preview (" + api + ")");
    } else {
        toast(ToastLevel::Error, "Cannot start fledge_runtime.exe");
    }
#else
    (void)api;
    (void)extra;
#endif
}

void EditorApp::startAutoTune() {
    if (!m_session.hasProject() || m_autoTuneRunning) return;
    if (m_session.dirty) (void)m_session.saveScene();
    m_autoTuneRunning = true;
    m_autoTuneResult.clear();
    fs::path runtime = m_session.runtimeExe(), project = m_session.project().file;
    std::string api = rhi::toString(gameBackend(m_session.project().graphics));
    runInBackground("Auto-tune graphics", [this, runtime, project, api](BackgroundTask& t) {
        const char* order[] = {"everything", "ultra", "high", "medium", "low"};
        std::string chosen = "low", report;
        double target = 1000.0 / 60.0 * 0.85;  // 60 FPS with headroom
        for (int i = 0; i < 5; ++i) {
            t.progress = static_cast<float>(i) / 5.0f;
            {
                std::lock_guard l(m_taskMutex);
                t.detail = std::string("measuring ") + order[i];
            }
            auto r = runProcess({narrow(runtime.wstring()), "--project", narrow(project.wstring()), "--rhi", api, "--preset", order[i],
                                 "--smoke-test", "--frames", "150", "--width", "1920", "--height", "1080", "--freeze-time"});
            double ms = -1;
            if (auto p = r.output.find("avgFrameMs="); p != std::string::npos) ms = std::atof(r.output.c_str() + p + 11);
            report += std::format("{} {:.1f} ms  ", order[i], ms);
            if (ms > 0 && ms <= target) {
                chosen = order[i];
                break;
            }
        }
        m_mainQueue.post([this, chosen, report] {
            m_autoTuneRunning = false;
            GraphicsPreset p;
            if (fromString(chosen, p) && m_session.hasProject()) {
                GraphicsSettings g = m_session.project().graphics;
                RenderSettings r = m_session.project().render;
                applyGraphicsPreset(p, r, g, gameBackend(g));
                applyGraphics(g, &r, "Auto-tune: " + chosen);
            }
            m_autoTuneResult = "Auto-tune picked '" + chosen + "' (1080p, 60 FPS target): " + report;
            toast(ToastLevel::Success, m_autoTuneResult);
        });
        return "picked " + chosen;
    });
}

} // namespace fledge::editor
