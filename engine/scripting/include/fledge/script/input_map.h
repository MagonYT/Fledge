#pragma once
// Named input actions and axes for Fledge Script files (and anything else in the runtime).
//
// Project file:
//   "input": {
//     "actions": { "Jump": ["Space"], "Fire": ["MouseLeft", "F"] },
//     "axes":    { "MoveX": { "positive": ["D", "Right"], "negative": ["A", "Left"] },
//                  "LookX": { "mouse": "x", "scale": 0.1, "gamepad": "rightx", "padScale": 12 } }
// Gamepad buttons are keys named PadA PadB PadX PadY PadLB PadRB PadBack PadStart PadGuide PadLS PadRS
// PadUp PadRight PadDown PadLeft PadRT PadLT; axes are leftx lefty rightx righty lt rt (dead zone 0.15).
//   }
// Entries replace the defaults of the same name; other defaults stay.
// Key codes are GLFW key codes (letters = ASCII), mouse buttons are 500 + button.
#include <array>
#include <map>
#include <string>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/utility.h"

namespace fledge::script {

struct InputFrame {
    std::array<bool, 512> down{};  // GLFW key codes; 500..502 = mouse left/right/middle; 400..414 gamepad buttons, 416/417 RT/LT
    float mouseDx = 0, mouseDy = 0;
    std::array<float, 6> pad{};    // gamepad axes: left x, left y, right x, right y, LT, RT
};

enum class InputQuery { Down = 0, Pressed = 1, Released = 2 };

class InputMap {
public:
    static constexpr int kMouseLeft = 500, kMouseRight = 501, kMouseMiddle = 502;

    InputMap() { setDefaults(); }
    void setDefaults();
    Status load(const Json& j);  // merges over the current map
    Json toJson() const;

    void beginFrame(const InputFrame& frame);
    bool action(const std::string& name, InputQuery q) const;
    float axis(const std::string& name) const;
    float mouseDx() const { return m_cur.mouseDx; }
    float mouseDy() const { return m_cur.mouseDy; }
    bool rebind(const std::string& action, const std::string& key);  // false for unknown keys
    std::vector<std::string> bindings(const std::string& action) const;  // key names
    // Scripted input (MCP play {input}): sets what "name" (an action, or an axis at "value") needs in "frame" - the first bound
    // key, a gamepad axis for fractional values, or mouse motion. false if the name is unknown or has nothing to drive.
    bool drive(const std::string& name, float value, InputFrame& frame) const;

    std::vector<std::string> actionNames() const;
    std::vector<std::string> axisNames() const;

    static int keyCode(std::string_view name);  // -1 if unknown (case-insensitive)
    static std::string keyName(int code);

private:
    struct Axis {
        std::vector<int> positive, negative;
        int mouse = 0;  // 1 = x, 2 = y
        float scale = 1.0f;
        int pad = -1;   // gamepad axis index
        float padScale = 1.0f;
    };
    std::map<std::string, std::vector<int>> m_actions;
    std::map<std::string, Axis> m_axes;
    InputFrame m_cur, m_prev;
};

} // namespace fledge::script
