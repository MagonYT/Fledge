#include "settings_menu.h"

#include <format>

#include "fledge/core/log.h"
#include "fledge/render/graphics_api.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include <stb_easy_font.h>  // public domain / MIT (stb)
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fledge {

namespace {
enum ItemId { kPreset, kUpscaler, kQuality, kScale, kRt, kRtQuality, kSync, kWindow, kHdr, kApi, kControls, kApply, kClose, kCount };
constexpr int kF10 = 299, kEnter = 257, kEsc = 256, kRight = 262, kLeft = 263, kDown = 264, kUp = 265;

using Verts = std::vector<ForwardRenderer::OverlayVertex>;
void rect(Verts& v, float x, float y, float w, float h, Vec4 c) {
    Vec2 a{x, y}, b{x + w, y}, d{x, y + h}, e{x + w, y + h};
    for (Vec2 p : {a, b, e, a, e, d}) v.push_back({p, c});
}
void text(Verts& v, float x, float y, const std::string& s, Vec4 c, float scale) {
    static char buf[60000];
    int quads = stb_easy_font_print(0, 0, const_cast<char*>(s.c_str()), nullptr, buf, sizeof(buf));
    auto* f = reinterpret_cast<const float*>(buf);
    for (int q = 0; q < quads; ++q) {
        Vec2 p[4];
        for (int k = 0; k < 4; ++k) {
            const float* vert = f + (q * 4 + k) * 4;  // x, y, z, color(4 bytes)
            p[k] = {x + vert[0] * scale, y + vert[1] * scale};
        }
        for (int k : {0, 1, 2, 0, 2, 3}) v.push_back({p[k], c});
    }
}
} // namespace

PlayerSettingsMenu::PlayerSettingsMenu(GraphicsSettings& g, RenderSettings& r, fs::path file, rhi::RenderDevice& device, Window& window)
    : m_g(g), m_r(r), m_file(std::move(file)), m_device(device), m_window(window) {
    rebuild();
}

GraphicsSettings PlayerSettingsMenu::loadOverrides(const fs::path& file, const GraphicsSettings& base) {
    auto text = readFileText(file);
    if (!text) return base;
    Json j = Json::parse(*text, nullptr, false);
    if (!j.is_object()) return base;
    Json merged = base.toJson();
    for (auto& [k, v] : j.items()) {
        if (k == "input") continue;  // key bindings (loadInputOverrides)
        if (k == "rayTracing" && v.is_object())
            for (auto& [rk, rv] : v.items()) merged["rayTracing"][rk] = rv;
        else
            merged[k] = v;
    }
    auto g = GraphicsSettings::fromJson(merged);
    if (!g) {
        FLEDGE_LOG_WARN("runtime", "Ignoring player settings {}: {}", narrow(file.wstring()), g.error());
        return base;
    }
    FLEDGE_LOG_INFO("runtime", "Player graphics settings loaded from {}", narrow(file.wstring()));
    return g.value();
}

void PlayerSettingsMenu::rebuild() {
    rhi::DeviceCaps caps = m_device.caps();
    rhi::Backend b = m_device.backend();
    auto reason = [&](rhi::GfxFeature f) {
        auto s = rhi::featureSupport(b, f, &caps);
        return s.supported ? std::string() : s.reason;
    };
    m_items.assign(kCount, {});
    m_items[kPreset] = {"Quality preset", {"Custom", "Low", "Medium", "High", "Ultra", "Everything"}, static_cast<int>(m_g.preset)};
    m_items[kUpscaler] = {"Upscaler", {"Off", "TAA", "FSR 1", "XeSS", "DLSS"}, static_cast<int>(m_g.upscaler), false, reason(rhi::GfxFeature::Taa)};
    m_items[kQuality] = {"Upscaler quality", {"Native", "Quality", "Balanced", "Performance", "Ultra Performance"}, static_cast<int>(m_g.upscalerQuality)};
    if (m_g.upscaler == UpscalerKind::Off) m_items[kQuality].disabled = "upscaler is off";
    std::vector<std::string> scales;
    for (int p = 50; p <= 200; p += 10) scales.push_back(std::to_string(p) + "%");
    m_items[kScale] = {"Render scale", scales, std::clamp(static_cast<int>(std::lround(m_g.renderScale * 10.0f)) - 5, 0, 15)};
    if (m_g.upscaler != UpscalerKind::Off) m_items[kScale].disabled = "set by the upscaler quality";
    m_items[kRt] = {"Ray tracing", {"Off", "On"}, m_g.rt.enabled ? 1 : 0, false, reason(rhi::GfxFeature::RayTracing)};
    m_items[kRtQuality] = {"Ray tracing quality", {"Low", "Medium", "High", "Ultra"}, static_cast<int>(m_g.rt.quality), false,
                           m_items[kRt].disabled.empty() && !m_g.rt.enabled ? "ray tracing is off" : m_items[kRt].disabled};
    m_items[kSync] = {"Sync", {"VSync", "Off", "Variable refresh"}, static_cast<int>(m_g.vsync)};
    m_items[kWindow] = {"Display mode", {"Windowed", "Borderless", "Fullscreen"}, static_cast<int>(m_g.windowMode)};
    m_items[kHdr] = {"HDR output", {"Off", "On"}, m_g.hdr ? 1 : 0, true, reason(rhi::GfxFeature::HdrOutput)};
    std::vector<std::string> apis;
    for (auto a : rhi::kAllGraphicsApis) apis.push_back(rhi::displayName(a));
    m_items[kApi] = {"Graphics API", apis, static_cast<int>(m_g.api), true};
    m_items[kControls] = {"Controls...", {}, 0, false, m_inputMap ? "" : "no input map"};
    m_items[kApply] = {"Apply & save", {}, 0};
    m_items[kClose] = {"Close (F10)", {}, 0};
}

bool PlayerSettingsMenu::pressed(const InputState& in, int key) { return in.raw[static_cast<size_t>(key)] && !m_prev[static_cast<size_t>(key)]; }

Json PlayerSettingsMenu::loadInputOverrides(const fs::path& file) {
    auto text = readFileText(file);
    if (!text) return Json();
    Json j = Json::parse(*text, nullptr, false);
    return j.is_object() && j.contains("input") ? j["input"] : Json();
}

std::vector<std::string> PlayerSettingsMenu::controlActions() const {
    std::vector<std::string> out;
    if (!m_inputMap) return out;
    for (const char* a : {"Jump", "Sprint", "Interact", "Fire", "Aim", "Crouch", "Reload", "NextWeapon", "QuickSave", "QuickLoad", "Pause"})
        out.push_back(a);
    return out;
}

void PlayerSettingsMenu::updateControls(const InputState& in) {
    auto actions = controlActions();
    int n = static_cast<int>(actions.size()) + 1;  // + Back
    if (m_waiting) {
        if (pressed(in, kEsc)) {
            m_waiting = false;
            m_ateEscape = true;
            return;
        }
        for (int k = 0; k < 512; ++k)
            if (pressed(in, k) && k != kEnter && k != kF10) {
                std::string key = script::InputMap::keyName(k);
                if (!key.empty() && key != "?" && m_inputMap->rebind(actions[static_cast<size_t>(m_csel)], key)) {
                    m_status = actions[static_cast<size_t>(m_csel)] + " -> " + key + "  (Apply & save keeps it)";
                    m_statusTime = m_frame;
                }
                m_waiting = false;
                break;
            }
        return;
    }
    if (pressed(in, kEsc)) {
        m_controls = false;
        m_ateEscape = true;
        return;
    }
    if (pressed(in, kDown)) m_csel = (m_csel + 1) % n;
    if (pressed(in, kUp)) m_csel = (m_csel + n - 1) % n;
    if (pressed(in, kEnter)) {
        if (m_csel == n - 1) m_controls = false;
        else m_waiting = true;
    }
}

void PlayerSettingsMenu::update(const InputState& in) {
    m_ateEscape = false;
    ++m_frame;
    if (pressed(in, kF10)) {
        m_open = !m_open;
        m_controls = m_waiting = false;
        if (m_open) rebuild();
    }
    if (m_open && m_controls) {
        updateControls(in);
    } else if (m_open) {
        if (pressed(in, kEsc)) {
            m_open = false;
            m_ateEscape = true;
        }
        if (pressed(in, kDown)) m_sel = (m_sel + 1) % kCount;
        if (pressed(in, kUp)) m_sel = (m_sel + kCount - 1) % kCount;
        if (pressed(in, kLeft)) change(m_sel, -1);
        if (pressed(in, kRight)) change(m_sel, +1);
        if (pressed(in, kEnter)) {
            if (m_sel == kApply) save();
            else if (m_sel == kClose) m_open = false;
            else if (m_sel == kControls && m_inputMap) {
                m_controls = true;
                m_csel = 0;
            }
            else change(m_sel, +1);
        }
    }
    m_prev = in.raw;
}

void PlayerSettingsMenu::change(int item, int delta) {
    Item& it = m_items[static_cast<size_t>(item)];
    if (it.values.empty() || !it.disabled.empty()) return;
    int n = static_cast<int>(it.values.size());
    it.index = (it.index + delta + n) % n;
    apply(item);
    rebuild();
}

void PlayerSettingsMenu::apply(int item) {
    int v = m_items[static_cast<size_t>(item)].index;
    switch (item) {
        case kPreset:
            if (v > 0) applyGraphicsPreset(static_cast<GraphicsPreset>(v), m_r, m_g, m_device.backend());
            break;
        case kUpscaler:
            m_g.upscaler = static_cast<UpscalerKind>(v);
            m_g.preset = GraphicsPreset::Custom;
            break;
        case kQuality: m_g.upscalerQuality = static_cast<UpscalerQuality>(v); break;
        case kScale: m_g.renderScale = static_cast<float>(v + 5) / 10.0f; break;
        case kRt:
            m_g.rt.enabled = v == 1;
            m_g.preset = GraphicsPreset::Custom;
            break;
        case kRtQuality: m_g.rt.quality = static_cast<RtQuality>(v); break;
        case kSync:
            m_g.vsync = static_cast<VsyncMode>(v);
            m_device.setVsync(m_g.vsync == VsyncMode::On, m_g.vsync == VsyncMode::Vrr);
            m_window.setVsync(m_g.vsync == VsyncMode::On);
            break;
        case kWindow:
            m_g.windowMode = static_cast<WindowMode>(v);
            m_window.setDisplayMode(v, m_g.width, m_g.height);
            break;
        case kHdr: m_g.hdr = v == 1; break;
        case kApi: m_g.api = static_cast<rhi::GraphicsApi>(v); break;
        default: break;
    }
    m_g.sanitize();
}

bool PlayerSettingsMenu::save() {
    std::error_code ec;
    fs::create_directories(m_file.parent_path(), ec);
    Json out = m_g.toJson();
    if (m_inputMap) out["input"] = m_inputMap->toJson();  // the player's key / gamepad bindings
    std::string s = out.dump(2);
    bool ok = writeFileBytes(m_file, Bytes(s.begin(), s.end()));
    m_status = ok ? "Saved. Graphics API / HDR changes apply on the next start." : "Could not save " + narrow(m_file.wstring());
    m_statusTime = m_frame;
    return ok;
}

void PlayerSettingsMenu::draw(ForwardRenderer& renderer, int width, int height) {
    if (!m_open || width <= 0 || height <= 0) return;
    Verts v;
    if (m_controls) {  // key / gamepad rebinding page
        float scale = std::clamp(static_cast<float>(height) / 360.0f, 1.5f, 4.0f);
        float rowH = 12.0f * scale, pad = 10.0f * scale;
        auto actions = controlActions();
        float w = std::min(static_cast<float>(width) - 2 * pad, 330.0f * scale), h = rowH * (static_cast<float>(actions.size()) + 6) + pad * 2;
        float x0 = (static_cast<float>(width) - w) * 0.5f, y0 = std::max(pad, (static_cast<float>(height) - h) * 0.5f);
        const Vec4 bg{0.08f, 0.09f, 0.11f, 0.92f}, accent{0.95f, 0.53f, 0.42f, 1.0f}, fg{0.92f, 0.93f, 0.95f, 1}, dim{0.55f, 0.57f, 0.62f, 1};
        rect(v, x0, y0, w, h, bg);
        rect(v, x0, y0, w, 3 * scale, accent);
        text(v, x0 + pad, y0 + pad, "CONTROLS", accent, scale * 1.2f);
        float y = y0 + pad + rowH * 2.0f;
        for (size_t i = 0; i <= actions.size(); ++i) {
            bool sel = static_cast<int>(i) == m_csel;
            if (sel) rect(v, x0 + pad * 0.5f, y - 2 * scale, w - pad, rowH, Vec4{1, 1, 1, 0.08f});
            Vec4 c = sel ? accent : fg;
            if (i == actions.size()) {
                text(v, x0 + pad, y, "Back", c, scale);
            } else {
                text(v, x0 + pad, y, actions[i], c, scale);
                std::string keys;
                for (auto& k : m_inputMap->bindings(actions[i])) keys += (keys.empty() ? "" : ", ") + k;
                if (sel && m_waiting) keys = "press a key / button...";
                text(v, x0 + w * 0.45f, y, keys, sel && m_waiting ? accent : dim, scale * 0.9f);
            }
            y += rowH;
        }
        std::string hint = !m_status.empty() && m_frame - m_statusTime < 240 ? m_status : "Enter: rebind  Esc: back (or cancel)  Apply & save on the main page";
        text(v, x0 + pad, y + rowH * 0.5f, hint, dim, scale * 0.7f);
        renderer.setOverlay(std::move(v));
        return;
    }
    float scale = std::clamp(static_cast<float>(height) / 360.0f, 1.5f, 4.0f);
    float rowH = 12.0f * scale, pad = 10.0f * scale;
    float w = std::min(static_cast<float>(width) - 2 * pad, 330.0f * scale), h = rowH * (kCount + 5) + pad * 2;
    float x0 = (static_cast<float>(width) - w) * 0.5f, y0 = std::max(pad, (static_cast<float>(height) - h) * 0.5f);
    const Vec4 bg{0.08f, 0.09f, 0.11f, 0.92f}, accent{0.95f, 0.53f, 0.42f, 1.0f}, fg{0.92f, 0.93f, 0.95f, 1}, dim{0.55f, 0.57f, 0.62f, 1};
    rect(v, x0, y0, w, h, bg);
    rect(v, x0, y0, w, 3 * scale, accent);
    text(v, x0 + pad, y0 + pad, "GRAPHICS SETTINGS", accent, scale * 1.2f);
    const auto& fi = renderer.lastFrame();
    std::string info = std::format("{} | render {}x{} | {} | RT {}", rhi::toString(m_device.backend()), fi.renderWidth, fi.renderHeight,
                                   toString(fi.upscaler), fi.rayTracing ? "on" : "off");
    text(v, x0 + pad, y0 + pad + rowH * 1.3f, info, dim, scale * 0.8f);
    float y = y0 + pad + rowH * 2.6f;
    for (int i = 0; i < kCount; ++i) {
        const Item& it = m_items[static_cast<size_t>(i)];
        bool sel = i == m_sel;
        if (sel) rect(v, x0 + pad * 0.5f, y - 2 * scale, w - pad, rowH, Vec4{1, 1, 1, 0.08f});
        Vec4 c = it.disabled.empty() ? (sel ? accent : fg) : dim;
        text(v, x0 + pad, y, it.label + (it.restart ? " *" : ""), c, scale);
        if (!it.values.empty()) {
            std::string val = it.disabled.empty() ? "< " + it.values[static_cast<size_t>(it.index)] + " >" : "n/a";
            text(v, x0 + w * 0.55f, y, val, c, scale);
        }
        y += rowH;
    }
    const Item& cur = m_items[static_cast<size_t>(m_sel)];
    std::string hint = !cur.disabled.empty() ? cur.disabled : (!fi.upscalerNote.empty() ? fi.upscalerNote : fi.rayTracingNote);
    if (!m_status.empty() && m_frame - m_statusTime < 240) hint = m_status;
    text(v, x0 + pad, y + rowH * 0.3f, hint.substr(0, 90), dim, scale * 0.75f);
    text(v, x0 + pad, y + rowH * 1.1f, "Arrows: choose/change  Enter: apply  F10/Esc: close  * = after restart", dim, scale * 0.7f);
    renderer.setOverlay(std::move(v));
}

bool DeveloperOverlay::update(const InputState& input, bool available) {
    constexpr int f8 = 297, f9 = 298, backspace = 259;
    auto pressed = [&](size_t key) { return input.raw[key] && !m_previous[key]; };
    const bool ownedInput = m_console;
    if (available) {
        if (pressed(f8)) m_console = !m_console;
        if (m_console && pressed(kEsc)) m_console = false;
        if (!m_console && pressed(f9)) m_hud = !m_hud;
        if (m_console && ownedInput) {
            m_command.append(input.asciiText.substr(0, 120 - m_command.size()));
            if (pressed(backspace) && !m_command.empty()) m_command.pop_back();
            if (pressed(kEnter) && !m_command.empty()) {
                if (m_command == "help") FLEDGE_LOG_INFO("dev", "Commands: help, hud, stats");
                else if (m_command == "hud") m_hud = !m_hud;
                else if (m_command == "stats") FLEDGE_LOG_INFO("dev", "{}", m_stats);
                else FLEDGE_LOG_WARN("dev", "Unknown command. Type help for commands.");
                m_command.clear();
            }
        }
    }
    m_previous = input.raw;
    return available && (m_console || ownedInput);
}

void DeveloperOverlay::draw(ForwardRenderer& renderer, int width, int height, float dt, size_t entities,
                            uint32_t draws, uint32_t triangles) {
    if ((!m_console && !m_hud) || width < 48 || height < 48) return;
    m_stats = std::format("{:.1f} ms | {} entities | {} draws | {} tris", dt * 1000.0f, entities, draws, triangles);
    Verts v;
    const float panelWidth = std::min(920.0f, static_cast<float>(width) - 16.0f);
    const size_t columns = static_cast<size_t>(std::max(1.0f, (panelWidth - 16.0f) / 8.0f));
    const Vec4 bg{0.08f, 0.09f, 0.11f, 0.94f}, accent{0.95f, 0.53f, 0.42f, 1}, fg{0.92f, 0.93f, 0.95f, 1};
    if (m_console) {
        const int rows = std::clamp((height - 52) / 14, 0, 12);
        rect(v, 8, 8, panelWidth, 42.0f + rows * 14.0f, bg);
        text(v, 16, 15, std::string("Developer console | F8 or Esc closes").substr(0, columns), accent, 1.0f);
        float y = 30.0f;
        for (const auto& entry : Logger::get().recent(static_cast<size_t>(rows))) {
            std::string line = (entry.category + ": " + entry.message).substr(0, columns);
            for (char& c : line) if (c < 32 || c > 126) c = '?';
            text(v, 16, y, line, fg, 1.0f);
            y += 14.0f;
        }
        const std::string prompt = "> " + m_command;
        text(v, 16, 30.0f + rows * 14.0f, prompt.substr(prompt.size() > columns ? prompt.size() - columns : 0), accent, 1.0f);
    } else {
        rect(v, 8, 8, panelWidth, 25, bg);
        text(v, 16, 16, m_stats.substr(0, columns), fg, 1.0f);
    }
    renderer.setOverlay(std::move(v));
}

} // namespace fledge
