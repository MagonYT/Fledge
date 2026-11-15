#include "fledge/script/input_map.h"

#include <cctype>
#include <algorithm>
#include <cmath>
#include <format>

namespace fledge::script {

namespace {
struct KeyEntry {
    const char* name;
    int code;
};
const std::vector<KeyEntry>& keyTable() {
    static const std::vector<KeyEntry> t = [] {
        std::vector<KeyEntry> v = {{"Space", 32},       {"Apostrophe", 39}, {"Comma", 44},       {"Minus", 45},       {"Period", 46},
                                   {"Slash", 47},       {"Semicolon", 59},  {"Equal", 61},       {"LeftBracket", 91}, {"Backslash", 92},
                                   {"RightBracket", 93}, {"GraveAccent", 96}, {"Escape", 256},   {"Enter", 257},      {"Tab", 258},
                                   {"Backspace", 259},  {"Insert", 260},    {"Delete", 261},     {"Right", 262},      {"Left", 263},
                                   {"Down", 264},       {"Up", 265},        {"PageUp", 266},     {"PageDown", 267},   {"Home", 268},
                                   {"End", 269},        {"LeftShift", 340}, {"LeftControl", 341}, {"LeftAlt", 342},   {"RightShift", 344},
                                   {"RightControl", 345}, {"RightAlt", 346}, {"MouseLeft", 500}, {"MouseRight", 501}, {"MouseMiddle", 502},
                                   {"PadA", 400}, {"PadB", 401}, {"PadX", 402}, {"PadY", 403}, {"PadLB", 404}, {"PadRB", 405}, {"PadBack", 406},
                                   {"PadStart", 407}, {"PadGuide", 408}, {"PadLS", 409}, {"PadRS", 410}, {"PadUp", 411}, {"PadRight", 412},
                                   {"PadDown", 413}, {"PadLeft", 414}, {"PadRT", 416}, {"PadLT", 417}};
        static std::vector<std::string> names;  // storage for generated names
        names.reserve(64);
        for (char c = 'A'; c <= 'Z'; ++c) {
            names.emplace_back(1, c);
            v.push_back({nullptr, c});
        }
        for (char c = '0'; c <= '9'; ++c) {
            names.emplace_back(1, c);
            v.push_back({nullptr, c});
        }
        for (int i = 1; i <= 12; ++i) {
            names.push_back(std::format("F{}", i));
            v.push_back({nullptr, 289 + i});
        }
        size_t n = 0;
        for (auto& e : v)
            if (!e.name) e.name = names[n++].c_str();
        return v;
    }();
    return t;
}
std::string lower(std::string_view s) {
    std::string o(s);
    for (auto& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}
std::vector<int> codes(const Json& arr, std::string& err) {
    std::vector<int> out;
    if (!arr.is_array()) return out;
    for (auto& k : arr) {
        int c = k.is_string() ? InputMap::keyCode(k.get<std::string>()) : -1;
        if (c < 0) err = "unknown key " + k.dump();
        else out.push_back(c);
    }
    return out;
}
} // namespace

int InputMap::keyCode(std::string_view name) {
    std::string l = lower(name);
    if (l == "ctrl" || l == "control") l = "leftcontrol";
    if (l == "shift") l = "leftshift";
    if (l == "alt") l = "leftalt";
    if (l == "esc") l = "escape";
    if (l == "return") l = "enter";
    for (auto& e : keyTable())
        if (lower(e.name) == l) return e.code;
    return -1;
}

std::string InputMap::keyName(int code) {
    for (auto& e : keyTable())
        if (e.code == code) return e.name;
    return std::format("Key{}", code);
}

void InputMap::setDefaults() {
    m_actions = {{"Jump", {32, 400}},       {"Sprint", {340, 409}}, {"Interact", {'E', 403}}, {"Fire", {kMouseLeft, 416}},
                 {"Aim", {kMouseRight, 417}}, {"Pause", {256, 407}},  {"Crouch", {'C', 401}},    {"Reload", {'R', 402}},
                 {"QuickSave", {297}},        {"QuickLoad", {298}},   {"NextWeapon", {'Q', 405}},
                 {"UiUp", {265, 411}},           {"UiDown", {264, 413}}, {"UiAccept", {257, 335, 400}}};  // retained UI focus navigation
    m_axes.clear();
    Axis mx{{'D', 262}, {'A', 263}};
    mx.pad = 0;
    m_axes["MoveX"] = mx;
    Axis my{{'W', 265}, {'S', 264}};
    my.pad = 1;
    my.padScale = -1;  // stick up is negative in GLFW
    m_axes["MoveY"] = my;
    Axis lx;
    lx.mouse = 1;
    lx.pad = 2;
    lx.padScale = 12;
    m_axes["LookX"] = lx;
    Axis ly;
    ly.mouse = 2;
    ly.pad = 3;
    ly.padScale = 12;
    m_axes["LookY"] = ly;
}

Status InputMap::load(const Json& j) {
    if (j.is_null()) return {};
    if (!j.is_object()) return Status::failure("\"input\" must be an object");
    std::string err;
    if (j.contains("actions") && j["actions"].is_object())
        for (auto& [name, keys] : j["actions"].items()) m_actions[name] = codes(keys, err);
    if (j.contains("axes") && j["axes"].is_object())
        for (auto& [name, a] : j["axes"].items()) {
            Axis ax;
            if (!a.is_object()) {
                err = "axis " + name + " must be an object";
                continue;
            }
            ax.positive = codes(a.value("positive", Json::array()), err);
            ax.negative = codes(a.value("negative", Json::array()), err);
            std::string m = a.value("mouse", "");
            ax.mouse = m == "x" ? 1 : m == "y" ? 2 : 0;
            ax.scale = a.value("scale", 1.0f);
            static const char* padAxes[] = {"leftx", "lefty", "rightx", "righty", "lt", "rt"};
            std::string pa = a.value("gamepad", "");
            for (int i = 0; i < 6; ++i)
                if (pa == padAxes[i]) ax.pad = i;
            ax.padScale = a.value("padScale", 1.0f);
            m_axes[name] = ax;
        }
    return err.empty() ? Status::success() : Status::failure("input map: " + err);
}

Json InputMap::toJson() const {
    Json actions = Json::object(), axes = Json::object();
    for (auto& [n, keys] : m_actions) {
        Json a = Json::array();
        for (int k : keys) a.push_back(keyName(k));
        actions[n] = a;
    }
    for (auto& [n, ax] : m_axes) {
        Json a = Json::object();
        auto names = [](const std::vector<int>& v) {
            Json r = Json::array();
            for (int k : v) r.push_back(keyName(k));
            return r;
        };
        if (!ax.positive.empty()) a["positive"] = names(ax.positive);
        if (!ax.negative.empty()) a["negative"] = names(ax.negative);
        if (ax.mouse) a["mouse"] = ax.mouse == 1 ? "x" : "y";
        if (ax.scale != 1.0f) a["scale"] = ax.scale;
        static const char* padAxes[] = {"leftx", "lefty", "rightx", "righty", "lt", "rt"};
        if (ax.pad >= 0 && ax.pad < 6) a["gamepad"] = padAxes[ax.pad];
        if (ax.pad >= 0 && ax.padScale != 1.0f) a["padScale"] = ax.padScale;
        axes[n] = a;
    }
    return {{"actions", actions}, {"axes", axes}};
}

void InputMap::beginFrame(const InputFrame& frame) {
    m_prev = m_cur;
    m_cur = frame;
}

bool InputMap::action(const std::string& name, InputQuery q) const {
    auto it = m_actions.find(name);
    if (it == m_actions.end()) return false;
    bool now = false, before = false;
    for (int k : it->second) {
        if (k < 0 || k >= 512) continue;
        now |= m_cur.down[static_cast<size_t>(k)];
        before |= m_prev.down[static_cast<size_t>(k)];
    }
    switch (q) {
        case InputQuery::Down: return now;
        case InputQuery::Pressed: return now && !before;
        case InputQuery::Released: return !now && before;
    }
    return false;
}

bool InputMap::drive(const std::string& name, float value, InputFrame& f) const {
    auto press = [&](int k) {
        if (k < 0 || k >= 512) return false;
        f.down[static_cast<size_t>(k)] = true;
        return true;
    };
    if (auto it = m_actions.find(name); it != m_actions.end()) {
        if (value == 0) return true;
        for (int k : it->second)
            if (press(k)) return true;
        return false;
    }
    auto ax = m_axes.find(name);
    if (ax == m_axes.end()) return false;
    const Axis& a = ax->second;
    if (value == 0) return true;
    if (a.pad >= 0 && a.pad < 6 && a.padScale != 0 && std::fabs(value) < 1.0f) {  // analog: invert the dead zone mapping
        float p = value / a.padScale;
        p = p * 0.85f + std::copysign(0.15f, p);
        f.pad[static_cast<size_t>(a.pad)] = std::clamp(p, -1.0f, 1.0f);
        return true;
    }
    if (a.mouse != 0 && a.scale != 0 && a.positive.empty() && a.negative.empty()) {
        (a.mouse == 1 ? f.mouseDx : f.mouseDy) = value / a.scale;
        return true;
    }
    bool positive = (value > 0) == (a.scale >= 0);
    for (int k : positive ? a.positive : a.negative)
        if (press(k)) return true;
    if (a.pad >= 0 && a.pad < 6 && a.padScale != 0) {
        f.pad[static_cast<size_t>(a.pad)] = std::clamp(value / a.padScale, -1.0f, 1.0f);
        return true;
    }
    return false;
}
float InputMap::axis(const std::string& name) const {
    auto it = m_axes.find(name);
    if (it == m_axes.end()) return 0.0f;
    const Axis& a = it->second;
    float v = 0;
    for (int k : a.positive)
        if (k >= 0 && k < 512 && m_cur.down[static_cast<size_t>(k)]) {
            v += 1;
            break;
        }
    for (int k : a.negative)
        if (k >= 0 && k < 512 && m_cur.down[static_cast<size_t>(k)]) {
            v -= 1;
            break;
        }
    if (a.mouse == 1) v += m_cur.mouseDx;
    if (a.mouse == 2) v += m_cur.mouseDy;
    v *= a.scale;
    if (a.pad >= 0 && a.pad < 6) {
        float p = m_cur.pad[static_cast<size_t>(a.pad)];
        if (std::fabs(p) > 0.15f) v += (p - std::copysign(0.15f, p)) / 0.85f * a.padScale;  // dead zone
    }
    return v;
}

bool InputMap::rebind(const std::string& action, const std::string& key) {
    int c = keyCode(key);
    if (c < 0) return false;
    // keep the gamepad binding when rebinding a keyboard / mouse key (and vice versa)
    std::vector<int> keep;
    bool padKey = c >= 400 && c < 420;
    for (int k : m_actions[action])
        if ((k >= 400 && k < 420) != padKey) keep.push_back(k);
    keep.insert(keep.begin(), c);
    m_actions[action] = keep;
    return true;
}

std::vector<std::string> InputMap::bindings(const std::string& action) const {
    std::vector<std::string> out;
    auto it = m_actions.find(action);
    if (it != m_actions.end())
        for (int k : it->second) out.push_back(keyName(k));
    return out;
}

std::vector<std::string> InputMap::actionNames() const {
    std::vector<std::string> v;
    for (auto& [n, _] : m_actions) v.push_back(n);
    return v;
}
std::vector<std::string> InputMap::axisNames() const {
    std::vector<std::string> v;
    for (auto& [n, _] : m_axes) v.push_back(n);
    return v;
}

} // namespace fledge::script
