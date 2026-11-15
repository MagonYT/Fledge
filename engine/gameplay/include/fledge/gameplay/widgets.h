#pragma once
// Retained game UI (docs/GAMEPLAY.md "UI"): a widget tree loaded from a JSON document (.fui), laid out every frame
// (anchors inside panels, vertical / horizontal stacks with padding and spacing, sizes fitted to text when 0),
// drawn into a UiCanvas, with mouse hover / click and keyboard / gamepad focus navigation between buttons.
// Text goes through a resolver: localisation ("@menu.play") and HUD bindings ({money}).
// Widgets can round corners, override style tokens, and run property animations (opacity, scale, slide) so a native
// HUD can move like an FHTML overlay without a browser.
//
//   {"root": {"type": "Stack", "anchor": "Center", "padding": 16, "background": [0, 0, 0, 0.7],
//     "animations": [{"property": "opacity", "from": 0, "to": 1, "duration": 0.25, "easing": "easeOut"}],
//     "children": [
//       {"type": "Text", "text": "@menu.title", "textSize": 3},
//       {"type": "Button", "id": "play", "text": "@menu.play",
//         "animations": [{"property": "scale", "from": 1, "to": 1.04, "duration": 0.12, "play": "hover"}]},
//       {"type": "Bar", "id": "loading", "value": 0.4, "size": [240, 14]}]}}
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/utility.h"
#include "fledge/gameplay/ui.h"

namespace fledge::gameplay {

enum class WidgetType { Panel = 0, Stack = 1, Text = 2, Button = 3, Bar = 4, Spacer = 5, Slider = 6 };
const char* toString(WidgetType t);

// Named colour set used by version 2+ documents. Empty style keeps the v1 look; tokens overlay a named preset.
struct UiPalette {
    Vec4 text{1, 1, 1, 1}, panel{0, 0, 0, 0}, button{0, 0, 0, 0}, buttonHover{0, 0, 0, 0}, focus{1.0f, 0.8f, 0.25f, 1},
        accent{1.0f, 0.55f, 0.18f, 1};
};

// One property track on a widget. `play`: show (once when visible), loop, hover, press, always.
struct UiAnim {
    std::string property = "opacity";  // opacity, scale, rotation, x, y, value
    float from = 0, to = 1;
    float duration = 0.25f, delay = 0;
    std::string easing = "linear";  // linear | easeIn | easeOut | easeInOut
    std::string play = "show";
    bool pingPong = false;
    float t = 0;  // runtime clock (seconds, or 0..1 for hover/press)
};

struct Widget {
    std::string id;
    WidgetType type = WidgetType::Panel;
    Anchor anchor = Anchor::TopLeft;  // inside a Panel (ignored by stacks, which place children in order)
    Vec2 offset{0, 0}, size{0, 0};    // pixels; a 0 size component fits the content (stack children stretch across)
    bool horizontal = false;          // Stack direction
    float spacing = 6, padding = 0;
    std::string text;                 // "@key" = localised
    float textSize = 2;               // UiCanvas units (2 ~ 15 px lines)
    Vec4 color{1, 1, 1, 1}, background{0, 0, 0, 0};
    bool customColor = false, customBackground = false;  // preserve explicit values, even when equal to defaults
    float value = 0;                  // Bar / Slider fill 0..1
    bool visible = true, enabled = true;
    float opacity = 1, scale = 1, rotation = 0, radius = 0;  // rotation in degrees; radius = rounded corners
    std::vector<UiAnim> animations;
    std::vector<Widget> children;
    // layout results (pixels) and state
    Vec2 pos{0, 0}, box{0, 0};
    bool hovered = false, pressed = false;
    float animOpacity = 1, animScale = 1, animRotation = 0;
    Vec2 animOffset{0, 0};

    static Result<Widget> fromJson(const Json& j);
    Json toJson() const;
};

struct UiInput {
    Vec2 mouse{-1, -1};      // pixels; negative = no pointer
    bool click = false;      // primary button pressed this frame
    bool held = false;       // primary button down (sliders, press animations)
    bool up = false, down = false, accept = false;  // focus navigation (UiUp / UiDown / UiAccept actions)
};

using TextResolver = std::function<std::string(const std::string&)>;

class UiDocument {
public:
    static Result<UiDocument> fromJson(const Json& j);  // {"root": {...}} or a bare widget
    Json toJson() const;
    Widget& root() { return m_root; }
    const Widget& root() const { return m_root; }
    const std::string& style() const { return m_style; }
    void setStyle(std::string style) { m_style = std::move(style); }
    const UiPalette& tokens() const { return m_tokens; }
    void setTokens(UiPalette tokens) {
        m_tokens = tokens;
        m_customTokens = true;
    }
    bool customTokens() const { return m_customTokens; }
    Widget* find(const std::string& id);
    // Sizes and positions every visible widget for the canvas (its size and text measurement / font).
    void layout(const UiCanvas& canvas, const TextResolver& text = {});
    // Hover / click / focus. Returns the ids of buttons activated this frame (click or accept on the focus).
    std::vector<std::string> handleInput(const UiInput& in);
    // Advance widget animations (seconds). Call after handleInput so hover / press tracks see this frame's pointer.
    void tick(float dt);
    void draw(UiCanvas& canvas, const TextResolver& text = {}) const;
    const std::string& focus() const { return m_focus; }
    void setFocus(const std::string& id) { m_focus = id; }

private:
    Widget m_root;
    std::string m_focus;
    std::string m_style;  // built-in style token set; empty preserves the v1 appearance
    UiPalette m_tokens;
    bool m_customTokens = false;
};

} // namespace fledge::gameplay
