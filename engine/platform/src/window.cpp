#include "fledge/platform/window.h"

#include <GLFW/glfw3.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

#include <chrono>

#include "fledge/core/log.h"

namespace fledge {

// ---------------------------------------------------------------- Clock ----
static int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
Clock::Clock() : m_start(nowNs()), m_last(m_start) {}
double Clock::now() const { return static_cast<double>(nowNs() - m_start) * 1e-9; }
float Clock::tick() {
    int64_t t = nowNs();
    float dt = static_cast<float>(static_cast<double>(t - m_last) * 1e-9);
    m_last = t;
    return dt > 0.25f ? 0.25f : dt;
}

// ----------------------------------------------------------- NullWindow ----
namespace {

class NullWindow final : public Window {
public:
    explicit NullWindow(const WindowDesc& d) : m_w(d.width), m_h(d.height) {}
    bool shouldClose() const override { return m_close; }
    void requestClose() override { m_close = true; }
    void pollEvents() override {}
    void swapBuffers() override {}
    int width() const override { return m_w; }
    int height() const override { return m_h; }
    const InputState& input() const override { return m_input; }
    void setTitle(const std::string&) override {}
    bool isHeadless() const override { return true; }
    GlProcLoader glLoader() const override { return nullptr; }

private:
    int m_w, m_h;
    bool m_close = false;
    InputState m_input;
};

// ----------------------------------------------------------- GlfwWindow ----
int g_glfwRefs = 0;

void* glfwProc(const char* name) { return reinterpret_cast<void*>(glfwGetProcAddress(name)); }

Key mapKey(int k) {
    switch (k) {
        case GLFW_KEY_W: return Key::W;
        case GLFW_KEY_A: return Key::A;
        case GLFW_KEY_S: return Key::S;
        case GLFW_KEY_D: return Key::D;
        case GLFW_KEY_Q: return Key::Q;
        case GLFW_KEY_E: return Key::E;
        case GLFW_KEY_F: return Key::F;
        case GLFW_KEY_SPACE: return Key::Space;
        case GLFW_KEY_ESCAPE: return Key::Escape;
        case GLFW_KEY_LEFT_SHIFT: return Key::LeftShift;
        case GLFW_KEY_UP: return Key::Up;
        case GLFW_KEY_DOWN: return Key::Down;
        case GLFW_KEY_LEFT: return Key::Left;
        case GLFW_KEY_RIGHT: return Key::Right;
        case GLFW_KEY_DELETE: return Key::Delete;
        default: return Key::Unknown;
    }
}

class GlfwWindow final : public Window {
public:
    GlfwWindow(GLFWwindow* w, bool gl) : m_window(w), m_gl(gl) {
        glfwSetWindowUserPointer(w, this);
        // NOTE: callbacks are chained by ImGui's GLFW backend when installed later.
        glfwSetScrollCallback(w, [](GLFWwindow* win, double, double y) {
            static_cast<GlfwWindow*>(glfwGetWindowUserPointer(win))->m_scrollAccum += static_cast<float>(y);
        });
        glfwSetCharCallback(w, [](GLFWwindow* win, unsigned int codepoint) {
            auto& text = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(win))->m_input.asciiText;
            if (codepoint >= 32 && codepoint <= 126 && text.size() < 256)
                text.push_back(static_cast<char>(codepoint));
        });
    }
    ~GlfwWindow() override {
        glfwDestroyWindow(m_window);
        if (--g_glfwRefs == 0) glfwTerminate();
    }
    bool shouldClose() const override { return glfwWindowShouldClose(m_window); }
    void requestClose() override { glfwSetWindowShouldClose(m_window, GLFW_TRUE); }
    void pollEvents() override {
        m_input.asciiText.clear();
        glfwPollEvents();
        InputState prev = m_input;
        for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST; ++k) {
            Key mk = mapKey(k);
            bool down = glfwGetKey(m_window, k) == GLFW_PRESS;
            m_input.raw[static_cast<size_t>(k)] = down;
            if (mk != Key::Unknown) m_input.keys[static_cast<size_t>(mk)] = down;
        }
        for (int b = 0; b < 3; ++b) {
            m_input.mouse[static_cast<size_t>(b)] = glfwGetMouseButton(m_window, b) == GLFW_PRESS;
            m_input.raw[static_cast<size_t>(500 + b)] = m_input.mouse[static_cast<size_t>(b)];
        }
        double x, y;
        glfwGetCursorPos(m_window, &x, &y);
        m_input.mousePos = {static_cast<float>(x), static_cast<float>(y)};
        m_input.mouseDelta = m_firstPoll || m_cursorJump ? Vec2{0, 0} : m_input.mousePos - prev.mousePos;  // no jump on the first frame / capture change
        m_firstPoll = m_cursorJump = false;
        m_input.scroll = m_scrollAccum;
        m_scrollAccum = 0;
        GLFWgamepadstate pad;
        m_input.padConnected = glfwJoystickIsGamepad(GLFW_JOYSTICK_1) && glfwGetGamepadState(GLFW_JOYSTICK_1, &pad);
        if (m_input.padConnected) {
            for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b) m_input.raw[static_cast<size_t>(400 + b)] = pad.buttons[b] == GLFW_PRESS;
            for (int a = 0; a <= GLFW_GAMEPAD_AXIS_LAST; ++a) m_input.padAxes[static_cast<size_t>(a)] = pad.axes[a];
            m_input.raw[416] = pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0.0f;
            m_input.raw[417] = pad.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] > 0.0f;
        } else {
            m_input.padAxes = {};
        }
    }
    bool focused() const override { return glfwGetWindowAttrib(m_window, GLFW_FOCUSED) == GLFW_TRUE; }
    void requestFocus() override { glfwFocusWindow(m_window); }
    bool cursorCaptured() const override { return m_captured; }
    void setCursorCaptured(bool captured) override {
        const int want = captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL;
        const int have = glfwGetInputMode(m_window, GLFW_CURSOR);
        if (captured && !focused()) {
            if (have != GLFW_CURSOR_NORMAL) {
                glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                if (glfwRawMouseMotionSupported()) glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
#ifdef _WIN32
                ClipCursor(nullptr);
#endif
            }
            if (m_captured) m_cursorJump = true;
            m_captured = false;
            return;
        }
        if (have != want) {
            glfwSetInputMode(m_window, GLFW_CURSOR, want);
            if (glfwRawMouseMotionSupported()) glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
            if (captured != m_captured) m_cursorJump = true;
        }
        m_captured = captured && glfwGetInputMode(m_window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED;
#ifdef _WIN32
        if (m_captured) {
            HWND hwnd = glfwGetWin32Window(m_window);
            RECT cr{};
            GetClientRect(hwnd, &cr);
            POINT a{cr.left, cr.top}, b{cr.right, cr.bottom};
            ClientToScreen(hwnd, &a);
            ClientToScreen(hwnd, &b);
            RECT clip{a.x, a.y, b.x, b.y};
            ClipCursor(&clip);
        } else if (!captured) {
            ClipCursor(nullptr);
        }
#endif
    }
    void swapBuffers() override {
        if (m_gl) glfwSwapBuffers(m_window);
    }
    int width() const override { int w, h; glfwGetFramebufferSize(m_window, &w, &h); return w; }
    int height() const override { int w, h; glfwGetFramebufferSize(m_window, &w, &h); return h; }
    const InputState& input() const override { return m_input; }
    void setTitle(const std::string& t) override { glfwSetWindowTitle(m_window, t.c_str()); }
    bool isHeadless() const override { return false; }
    GlProcLoader glLoader() const override { return m_gl ? &glfwProc : nullptr; }
    GLFWwindow* glfwHandle() const override { return m_window; }
    float contentScale() const override {
        float x = 1, y = 1;
        glfwGetWindowContentScale(m_window, &x, &y);
        return x > 0 ? x : 1.0f;
    }
    void* nativeHandle() const override {
#ifdef _WIN32
        return glfwGetWin32Window(m_window);
#else
        return nullptr;
#endif
    }
    void setDisplayMode(int mode, int width, int height) override {
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
        if (mode == 0 || !mon || !vm) {
            if (glfwGetWindowMonitor(m_window) || m_borderless) {  // leave fullscreen: restore the windowed rectangle
                glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_TRUE);
                glfwSetWindowMonitor(m_window, nullptr, m_savedX, m_savedY, m_savedW, m_savedH, 0);
                m_borderless = false;
            } else if (width > 0 && height > 0) {
                glfwSetWindowSize(m_window, width, height);
            }
            return;
        }
        if (!glfwGetWindowMonitor(m_window) && !m_borderless) {
            glfwGetWindowPos(m_window, &m_savedX, &m_savedY);
            glfwGetWindowSize(m_window, &m_savedW, &m_savedH);
        }
        if (mode == 1) {  // borderless: undecorated window covering the monitor
            int mx = 0, my = 0;
            glfwGetMonitorPos(mon, &mx, &my);
            glfwSetWindowMonitor(m_window, nullptr, mx, my, vm->width, vm->height, 0);
            glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_FALSE);
            glfwSetWindowPos(m_window, mx, my);
            glfwSetWindowSize(m_window, vm->width, vm->height);
            m_borderless = true;
        } else {
            m_borderless = false;
            glfwSetWindowAttrib(m_window, GLFW_DECORATED, GLFW_TRUE);
            glfwSetWindowMonitor(m_window, mon, 0, 0, width > 0 ? width : vm->width, height > 0 ? height : vm->height, vm->refreshRate);
        }
    }
    void setVsync(bool on) override {
        if (m_gl) glfwSwapInterval(on ? 1 : 0);
    }

private:
    int m_savedX = 100, m_savedY = 100, m_savedW = 1280, m_savedH = 720;
    bool m_borderless = false;
    GLFWwindow* m_window;
    bool m_gl = true;
    InputState m_input;
    float m_scrollAccum = 0;
    bool m_firstPoll = true, m_cursorJump = false, m_captured = false;
};

} // namespace

std::unique_ptr<Window> Window::createNull(const WindowDesc& desc) { return std::make_unique<NullWindow>(desc); }

std::unique_ptr<Window> Window::createGlfw(const WindowDesc& desc) {
    if (g_glfwRefs == 0) {
        glfwSetErrorCallback([](int code, const char* msg) { FLEDGE_LOG_ERROR("glfw", "GLFW error {}: {}", code, msg); });
        if (!glfwInit()) {
            FLEDGE_LOG_ERROR("platform", "glfwInit failed");
            return nullptr;
        }
    }
    glfwDefaultWindowHints();
    if (desc.openGL) {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    } else {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    }
    glfwWindowHint(GLFW_VISIBLE, desc.visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_MAXIMIZED, desc.maximized ? GLFW_TRUE : GLFW_FALSE);
    GLFWwindow* w = glfwCreateWindow(desc.width, desc.height, desc.title.c_str(), nullptr, nullptr);
    if (!w) {
        FLEDGE_LOG_ERROR("platform", "Failed to create GLFW window{}", desc.openGL ? " with an OpenGL 3.3 core context" : "");
        if (g_glfwRefs == 0) glfwTerminate();
        return nullptr;
    }
    ++g_glfwRefs;
#ifdef _WIN32
    // Title bar / taskbar icon: the exe's own icon resource #1 (set by the .rc file, or by
    // fledgepack from the project icon), so packaged games show their own icon.
    if (HWND hwnd = glfwGetWin32Window(w)) {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        auto big = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                                 GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
        auto smallIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        if (big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
        if (smallIcon) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
    }
#endif
    if (desc.openGL) {
        glfwMakeContextCurrent(w);
        glfwSwapInterval(desc.vsync ? 1 : 0);
    }
    FLEDGE_LOG_INFO("platform", "Created window '{}' {}x{}", desc.title, desc.width, desc.height);
    return std::make_unique<GlfwWindow>(w, desc.openGL);
}

} // namespace fledge
