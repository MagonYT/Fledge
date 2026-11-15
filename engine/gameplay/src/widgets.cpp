#include "fledge/gameplay/widgets.h"

#include <algorithm>
#include <cmath>

namespace fledge::gameplay {

namespace {
const char* kTypes[] = {"Panel", "Stack", "Text", "Button", "Bar", "Spacer", "Slider"};
const char* kAnchors[] = {"TopLeft", "Top", "TopRight", "Left", "Center", "Right", "BottomLeft", "Bottom", "BottomRight"};

Vec2 vec2(const Json& j, Vec2 def) {
    if (j.is_array() && j.size() >= 2 && j[0].is_number() && j[1].is_number()) return {j[0].get<float>(), j[1].get<float>()};
    return def;
}
Vec4 vec4(const Json& j, Vec4 def) {
    if (!j.is_array() || j.size() < 3) return def;
    for (auto& v : j)
        if (!v.is_number()) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j.size() > 3 ? j[3].get<float>() : 1.0f};
}
std::string resolve(const TextResolver& r, const std::string& s) { return r ? r(s) : s; }

struct UiStyle {
    Vec4 text, panel, button, buttonHover, focus, accent;
};

const UiStyle* uiStyle(const std::string& name) {
    static const UiStyle developerDark{{0.90f, 0.93f, 0.97f, 1}, {0.055f, 0.070f, 0.090f, 0.94f}, {0.12f, 0.15f, 0.19f, 0.98f},
                                       {0.18f, 0.23f, 0.29f, 1}, {1.0f, 0.55f, 0.18f, 1}, {1.0f, 0.55f, 0.18f, 1}};
    static const UiStyle gameDark{{0.96f, 0.96f, 0.98f, 1}, {0.06f, 0.075f, 0.11f, 0.92f}, {0.14f, 0.17f, 0.24f, 0.97f},
                                  {0.20f, 0.29f, 0.42f, 1}, {0.30f, 0.72f, 1.0f, 1}, {0.30f, 0.72f, 1.0f, 1}};
    static const UiStyle gameLight{{0.08f, 0.10f, 0.14f, 1}, {0.91f, 0.93f, 0.96f, 0.96f}, {0.78f, 0.82f, 0.87f, 1},
                                   {0.66f, 0.75f, 0.86f, 1}, {0.10f, 0.38f, 0.68f, 1}, {0.10f, 0.38f, 0.68f, 1}};
    if (name == "FledgeDev") return &developerDark;
    if (name == "FledgeGame") return &gameDark;
    if (name == "FledgeLight") return &gameLight;
    return nullptr;
}

UiStyle resolvedStyle(const std::string& name, const UiPalette& tokens, bool custom) {
    UiStyle s{{1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {1.0f, 0.8f, 0.25f, 1}, {1.0f, 0.55f, 0.18f, 1}};
    if (const UiStyle* p = uiStyle(name)) s = *p;
    if (custom) {
        if (tokens.text.w > 0) s.text = tokens.text;
        if (tokens.panel.w > 0) s.panel = tokens.panel;
        if (tokens.button.w > 0) s.button = tokens.button;
        if (tokens.buttonHover.w > 0) s.buttonHover = tokens.buttonHover;
        if (tokens.focus.w > 0) s.focus = tokens.focus;
        if (tokens.accent.w > 0) s.accent = tokens.accent;
    }
    return s;
}

Json colorJson(Vec4 c) { return Json::array({c.x, c.y, c.z, c.w}); }

bool interactive(const Widget& w) { return w.visible && w.enabled && w.type == WidgetType::Button; }
void collectButtons(Widget& w, std::vector<Widget*>& out) {
    if (!w.visible) return;
    if (interactive(w)) out.push_back(&w);
    for (auto& c : w.children) collectButtons(c, out);
}
void collectSliders(Widget& w, std::vector<Widget*>& out) {
    if (!w.visible) return;
    if (w.enabled && w.type == WidgetType::Slider) out.push_back(&w);
    for (auto& c : w.children) collectSliders(c, out);
}

bool hit(const Widget& w, Vec2 mouse) {
    return mouse.x >= w.pos.x && mouse.y >= w.pos.y && mouse.x < w.pos.x + w.box.x && mouse.y < w.pos.y + w.box.y;
}

Vec2 measure(Widget& w, const UiCanvas& cv, const TextResolver& tr) {
    Vec2 content{0, 0};
    float pad = w.padding;
    switch (w.type) {
        case WidgetType::Text: content = cv.measure(resolve(tr, w.text), w.textSize); break;
        case WidgetType::Button: {
            if (pad == 0) pad = 10;
            content = cv.measure(resolve(tr, w.text), w.textSize);
            break;
        }
        case WidgetType::Bar:
        case WidgetType::Slider: content = {200, 18}; break;
        case WidgetType::Spacer: break;
        case WidgetType::Stack: {
            int n = 0;
            for (auto& c : w.children) {
                if (!c.visible) continue;
                Vec2 m = measure(c, cv, tr);
                if (w.horizontal) {
                    content.x += m.x;
                    content.y = std::max(content.y, m.y);
                } else {
                    content.y += m.y;
                    content.x = std::max(content.x, m.x);
                }
                ++n;
            }
            if (n > 1) (w.horizontal ? content.x : content.y) += w.spacing * static_cast<float>(n - 1);
            break;
        }
        case WidgetType::Panel:
            for (auto& c : w.children) {
                if (!c.visible) continue;
                Vec2 m = measure(c, cv, tr);
                content.x = std::max(content.x, c.offset.x + m.x);
                content.y = std::max(content.y, c.offset.y + m.y);
            }
            break;
    }
    w.box = {w.size.x > 0 ? w.size.x : content.x + 2 * pad, w.size.y > 0 ? w.size.y : content.y + 2 * pad};
    return w.box;
}

void place(Widget& w, Vec2 pos, const UiCanvas& cv) {
    w.pos = pos;
    float pad = w.type == WidgetType::Button && w.padding == 0 ? 10 : w.padding;
    Vec2 inner{pos.x + pad, pos.y + pad}, innerSize{w.box.x - 2 * pad, w.box.y - 2 * pad};
    if (w.type == WidgetType::Stack) {
        Vec2 at = inner;
        for (auto& c : w.children) {
            if (!c.visible) continue;
            if (w.horizontal) {
                if (c.size.y <= 0) c.box.y = innerSize.y;
            } else if (c.size.x <= 0) {
                c.box.x = innerSize.x;
            }
            place(c, at, cv);
            (w.horizontal ? at.x : at.y) += (w.horizontal ? c.box.x : c.box.y) + w.spacing;
        }
    } else if (w.type == WidgetType::Panel) {
        for (auto& c : w.children) {
            if (!c.visible) continue;
            UiCanvas local(innerSize.x, innerSize.y);
            Vec2 p = local.place(c.anchor, c.offset, c.box);
            place(c, {inner.x + p.x, inner.y + p.y}, cv);
        }
    }
}

float ease(const std::string& name, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    if (name == "easeIn") return t * t;
    if (name == "easeOut") return 1.0f - (1.0f - t) * (1.0f - t);
    if (name == "easeInOut") return t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) * 0.5f;
    return t;
}

void applyAnim(Widget& w, const UiAnim& a, float u) {
    float v = a.from + (a.to - a.from) * ease(a.easing, u);
    if (a.property == "opacity") w.animOpacity = v;
    else if (a.property == "scale") w.animScale = v;
    else if (a.property == "rotation") w.animRotation = v;
    else if (a.property == "x") w.animOffset.x = v;
    else if (a.property == "y") w.animOffset.y = v;
    else if (a.property == "value") w.value = std::clamp(v, 0.0f, 1.0f);
}

void tickWidget(Widget& w, float dt) {
    w.animOpacity = 1;
    w.animScale = 1;
    w.animRotation = 0;
    w.animOffset = {};
    for (auto& a : w.animations) {
        float dur = std::max(a.duration, 0.0001f);
        if (a.play == "hover" || a.play == "press") {
            bool on = a.play == "hover" ? w.hovered : w.pressed;
            a.t = std::clamp(a.t + (on ? dt : -dt) / dur, 0.0f, 1.0f);
            applyAnim(w, a, a.t);
        } else if (a.play == "loop") {
            if (!w.visible) {
                a.t = 0;
                applyAnim(w, a, 0);
                continue;
            }
            a.t += dt;
            float cycle = a.pingPong ? dur * 2 : dur;
            float u = std::fmod(a.t / dur, a.pingPong ? 2.0f : 1.0f);
            if (a.pingPong && u > 1.0f) u = 2.0f - u;
            (void)cycle;
            applyAnim(w, a, u);
        } else {  // show / always
            if (a.play != "always" && !w.visible) {
                a.t = 0;
                applyAnim(w, a, 0);
                continue;
            }
            a.t += dt;
            float u = a.duration <= 0 ? 1.0f : std::clamp((a.t - a.delay) / dur, 0.0f, 1.0f);
            applyAnim(w, a, u);
        }
    }
    for (auto& c : w.children) tickWidget(c, dt);
}

void transformDrawn(std::vector<UiVertex>& verts, size_t begin, Vec2 center, float scale, float rotationRad, float opacity) {
    float cs = std::cos(rotationRad), sn = std::sin(rotationRad);
    for (size_t i = begin; i < verts.size(); ++i) {
        verts[i].color.w *= opacity;
        if (scale == 1 && rotationRad == 0) continue;
        Vec2 d = verts[i].pos - center;
        verts[i].pos = {center.x + (d.x * cs - d.y * sn) * scale, center.y + (d.x * sn + d.y * cs) * scale};
    }
}

void fill(UiCanvas& cv, float x, float y, float w, float h, float radius, Vec4 c) {
    if (c.w <= 0) return;
    if (radius > 0.5f) cv.roundedRect(x, y, w, h, radius, c);
    else cv.rect(x, y, w, h, c);
}

void drawWidget(const Widget& w, UiCanvas& cv, const TextResolver& tr, const std::string& focus, const UiStyle* style, Vec2 pan,
                float inheritedOpacity, bool root = false) {
    if (!w.visible) return;
    Vec2 off = pan + w.animOffset;
    float opacity = inheritedOpacity * std::clamp(w.opacity * w.animOpacity, 0.0f, 1.0f);
    if (opacity <= 0) return;
    bool focused = !focus.empty() && w.id == focus && interactive(w);
    Vec4 bg = w.background;
    if (style && !w.customBackground && (w.type == WidgetType::Panel || (root && w.type == WidgetType::Stack))) bg = style->panel;
    if (w.type == WidgetType::Button || w.type == WidgetType::Slider) {
        if (!w.customBackground && bg.w <= 0) bg = style ? style->button : Vec4{0.16f, 0.18f, 0.22f, 0.9f};
        if (!w.enabled) bg.w *= 0.4f;
        else if (w.hovered || focused) {
            if (style && !w.customBackground) bg = style->buttonHover;
            else bg = {std::min(1.0f, bg.x + 0.18f), std::min(1.0f, bg.y + 0.18f), std::min(1.0f, bg.z + 0.2f), bg.w};
        }
    }
    bg.w *= opacity;
    Vec4 textColor = style && !w.customColor ? style->text : w.color;
    textColor.w *= opacity;
    float x = w.pos.x + off.x, y = w.pos.y + off.y;
    size_t begin = cv.vertices().size();
    if (bg.w > 0) fill(cv, x, y, w.box.x, w.box.y, w.radius, bg);
    if (focused) cv.frame(x, y, w.box.x, w.box.y, 2, style ? style->focus : Vec4{1.0f, 0.8f, 0.25f, 1});
    switch (w.type) {
        case WidgetType::Text: {
            float pad = w.padding;
            cv.text(x + pad, y + pad, resolve(tr, w.text), textColor, w.textSize);
            break;
        }
        case WidgetType::Button: {
            std::string s = resolve(tr, w.text);
            Vec2 m = cv.measure(s, w.textSize);
            Vec4 col = textColor;
            if (!w.enabled) col.w *= 0.5f;
            cv.text(x + (w.box.x - m.x) * 0.5f, y + (w.box.y - m.y) * 0.5f, s, col, w.textSize);
            break;
        }
        case WidgetType::Bar:
        case WidgetType::Slider: {
            Vec4 fillCol = style && !w.customColor ? style->accent : w.color;
            fillCol.w *= opacity;
            cv.bar(x, y, w.box.x, w.box.y, w.value, fillCol, {0, 0, 0, 0.55f * opacity});
            break;
        }
        default: break;
    }
    Vec2 center{x + w.box.x * 0.5f, y + w.box.y * 0.5f};
    transformDrawn(cv.vertices(), begin, center, w.scale * w.animScale, radians(w.rotation + w.animRotation), 1);
    for (auto& c : w.children) drawWidget(c, cv, tr, focus, style, off, opacity);
}

bool widgetEnhanced(const Widget& w) {
    if (w.type == WidgetType::Slider || w.opacity != 1 || w.scale != 1 || w.rotation != 0 || w.radius != 0 || !w.animations.empty())
        return true;
    for (auto& c : w.children)
        if (widgetEnhanced(c)) return true;
    return false;
}

UiAnim readAnim(const Json& j) {
    UiAnim a;
    a.property = j.value("property", a.property);
    a.from = j.value("from", a.from);
    a.to = j.value("to", a.to);
    a.duration = j.value("duration", a.duration);
    a.delay = j.value("delay", a.delay);
    a.easing = j.value("easing", a.easing);
    a.play = j.value("play", a.play);
    a.pingPong = j.value("pingPong", a.pingPong);
    return a;
}

Json animJson(const UiAnim& a) {
    Json j = {{"property", a.property}, {"from", a.from}, {"to", a.to}, {"duration", a.duration}};
    if (a.delay != 0) j["delay"] = a.delay;
    if (a.easing != "linear") j["easing"] = a.easing;
    if (a.play != "show") j["play"] = a.play;
    if (a.pingPong) j["pingPong"] = true;
    return j;
}

Result<Widget> readWidget(const Json& j, unsigned depth, unsigned& remaining) {
    if (depth > 64 || remaining == 0) return Result<Widget>::failure("UI document exceeds the widget or nesting limit");
    --remaining;
    if (!j.is_object()) return Result<Widget>::failure("a widget must be a JSON object");
    Widget w;
    std::string type = j.value("type", std::string("Panel"));
    auto t = std::find(std::begin(kTypes), std::end(kTypes), type);
    if (t == std::end(kTypes)) return Result<Widget>::failure("unknown widget type '" + type + "' (Panel, Stack, Text, Button, Bar, Spacer, Slider)");
    w.type = static_cast<WidgetType>(t - std::begin(kTypes));
    w.id = j.value("id", std::string());
    w.anchor = anchorFromString(j.value("anchor", std::string("TopLeft")));
    w.offset = vec2(j.value("offset", Json()), w.offset);
    w.size = vec2(j.value("size", Json()), w.size);
    w.horizontal = j.value("direction", std::string("vertical")) == "horizontal";
    w.spacing = j.value("spacing", w.spacing);
    w.padding = j.value("padding", w.padding);
    w.text = j.value("text", std::string());
    w.textSize = j.value("textSize", w.textSize);
    w.customColor = j.contains("color");
    w.customBackground = j.contains("background");
    w.color = vec4(j.value("color", Json()), w.color);
    w.background = vec4(j.value("background", Json()), w.background);
    w.value = j.value("value", w.value);
    w.visible = j.value("visible", true);
    w.enabled = j.value("enabled", true);
    w.opacity = j.value("opacity", w.opacity);
    w.scale = j.value("scale", w.scale);
    w.rotation = j.value("rotation", w.rotation);
    w.radius = j.value("radius", w.radius);
    if (j.contains("animations")) {
        if (!j["animations"].is_array()) return Result<Widget>::failure("'animations' must be an array");
        for (auto& a : j["animations"]) {
            if (!a.is_object()) return Result<Widget>::failure("each animation must be an object");
            w.animations.push_back(readAnim(a));
        }
    }
    if (j.contains("children")) {
        if (!j["children"].is_array()) return Result<Widget>::failure("'children' must be an array");
        for (auto& c : j["children"]) {
            auto cw = readWidget(c, depth + 1, remaining);
            if (!cw) return cw;
            w.children.push_back(std::move(*cw));
        }
    }
    return w;
}
} // namespace

const char* toString(WidgetType t) { return kTypes[std::clamp(static_cast<int>(t), 0, 6)]; }

Result<Widget> Widget::fromJson(const Json& j) {
    unsigned remaining = 4096;
    try {
        return readWidget(j, 0, remaining);
    } catch (const Json::exception& error) {
        return Result<Widget>::failure(std::string("invalid UI widget field: ") + error.what());
    }
}

Json Widget::toJson() const {
    Json j = {{"type", toString(type)}};
    if (!id.empty()) j["id"] = id;
    if (anchor != Anchor::TopLeft) j["anchor"] = kAnchors[static_cast<int>(anchor)];
    if (offset.x != 0 || offset.y != 0) j["offset"] = {offset.x, offset.y};
    if (size.x != 0 || size.y != 0) j["size"] = {size.x, size.y};
    if (horizontal) j["direction"] = "horizontal";
    if (type == WidgetType::Stack) j["spacing"] = spacing;
    if (padding != 0) j["padding"] = padding;
    if (!text.empty()) j["text"] = text;
    if (textSize != 2) j["textSize"] = textSize;
    if (customColor || !(color == Vec4{1, 1, 1, 1})) j["color"] = {color.x, color.y, color.z, color.w};
    if (customBackground || background.w > 0) j["background"] = {background.x, background.y, background.z, background.w};
    if (type == WidgetType::Bar || type == WidgetType::Slider) j["value"] = value;
    if (!visible) j["visible"] = false;
    if (!enabled) j["enabled"] = false;
    if (opacity != 1) j["opacity"] = opacity;
    if (scale != 1) j["scale"] = scale;
    if (rotation != 0) j["rotation"] = rotation;
    if (radius != 0) j["radius"] = radius;
    if (!animations.empty()) {
        Json a = Json::array();
        for (auto& anim : animations) a.push_back(animJson(anim));
        j["animations"] = a;
    }
    if (!children.empty()) {
        Json c = Json::array();
        for (auto& w : children) c.push_back(w.toJson());
        j["children"] = c;
    }
    return j;
}

Result<UiDocument> UiDocument::fromJson(const Json& j) {
    auto root = Widget::fromJson(j.is_object() && j.contains("root") ? j["root"] : j);
    if (!root) return Result<UiDocument>::failure(root.error());
    UiDocument d;
    if (j.is_object() && j.contains("root")) {
        if (j.contains("style") && !j["style"].is_string()) return Result<UiDocument>::failure("'style' must be a string");
        if (j.contains("style")) d.m_style = j["style"].get<std::string>();
        if (!d.m_style.empty() && !uiStyle(d.m_style))
            return Result<UiDocument>::failure("unknown UI style '" + d.m_style + "' (FledgeDev, FledgeGame, FledgeLight)");
        if (j.contains("tokens")) {
            if (!j["tokens"].is_object()) return Result<UiDocument>::failure("'tokens' must be an object");
            const Json& t = j["tokens"];
            d.m_customTokens = true;
            d.m_tokens.text = vec4(t.value("text", Json()), d.m_tokens.text);
            d.m_tokens.panel = vec4(t.value("panel", Json()), d.m_tokens.panel);
            d.m_tokens.button = vec4(t.value("button", Json()), d.m_tokens.button);
            d.m_tokens.buttonHover = vec4(t.value("buttonHover", Json()), d.m_tokens.buttonHover);
            d.m_tokens.focus = vec4(t.value("focus", Json()), d.m_tokens.focus);
            d.m_tokens.accent = vec4(t.value("accent", Json()), d.m_tokens.accent);
        }
    }
    d.m_root = std::move(*root);
    return d;
}

Json UiDocument::toJson() const {
    int version = 1;
    if (!m_style.empty()) version = 2;
    if (m_customTokens || widgetEnhanced(m_root)) version = 3;
    Json j = {{"format", "fui"}, {"version", version}, {"root", m_root.toJson()}};
    if (!m_style.empty()) j["style"] = m_style;
    if (m_customTokens)
        j["tokens"] = {{"text", colorJson(m_tokens.text)},
                       {"panel", colorJson(m_tokens.panel)},
                       {"button", colorJson(m_tokens.button)},
                       {"buttonHover", colorJson(m_tokens.buttonHover)},
                       {"focus", colorJson(m_tokens.focus)},
                       {"accent", colorJson(m_tokens.accent)}};
    return j;
}

Widget* UiDocument::find(const std::string& id) {
    std::vector<Widget*> stack{&m_root};
    while (!stack.empty()) {
        Widget* w = stack.back();
        stack.pop_back();
        if (w->id == id) return w;
        for (auto& c : w->children) stack.push_back(&c);
    }
    return nullptr;
}

void UiDocument::layout(const UiCanvas& canvas, const TextResolver& text) {
    if (!m_root.visible) return;
    measure(m_root, canvas, text);
    Vec2 p = canvas.place(m_root.anchor, m_root.offset, m_root.box);
    place(m_root, p, canvas);
}

std::vector<std::string> UiDocument::handleInput(const UiInput& in) {
    std::vector<std::string> activated;
    std::vector<Widget*> buttons;
    collectButtons(m_root, buttons);
    auto focused = std::find_if(buttons.begin(), buttons.end(), [&](Widget* w) { return w->id == m_focus && !w->id.empty(); });
    if (focused == buttons.end()) m_focus.clear();
    if ((in.up || in.down) && !buttons.empty()) {
        long n = static_cast<long>(buttons.size());
        long i = focused == buttons.end() ? (in.down ? -1 : 0) : static_cast<long>(focused - buttons.begin());
        i = ((i + (in.down ? 1 : -1)) % n + n) % n;
        m_focus = buttons[static_cast<size_t>(i)]->id;
    }
    for (Widget* b : buttons) {
        b->hovered = hit(*b, in.mouse);
        b->pressed = b->hovered && in.held;
        if (b->hovered && in.click) {
            activated.push_back(b->id);
            m_focus = b->id;
        }
    }
    std::vector<Widget*> sliders;
    collectSliders(m_root, sliders);
    for (Widget* s : sliders) {
        s->hovered = hit(*s, in.mouse);
        s->pressed = s->hovered && in.held;
        if (s->pressed && s->box.x > 0) s->value = std::clamp((in.mouse.x - s->pos.x) / s->box.x, 0.0f, 1.0f);
    }
    if (in.accept && !m_focus.empty()) activated.push_back(m_focus);
    return activated;
}

void UiDocument::tick(float dt) {
    if (dt < 0) dt = 0;
    tickWidget(m_root, dt);
}

void UiDocument::draw(UiCanvas& canvas, const TextResolver& text) const {
    UiStyle style = resolvedStyle(m_style, m_tokens, m_customTokens);
    const UiStyle* p = (m_style.empty() && !m_customTokens) ? nullptr : &style;
    drawWidget(m_root, canvas, text, m_focus, p, {}, 1, true);
}

} // namespace fledge::gameplay
