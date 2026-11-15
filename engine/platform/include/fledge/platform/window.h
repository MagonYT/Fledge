#pragma once
// Window / input / timing abstraction. Backends: GLFW (desktop, creates an
// OpenGL 3.3 core context) and Null (headless; no OS window at all).
#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "fledge/core/math.h"

struct GLFWwindow;

namespace fledge { using GlProcLoader = void* (*)(const char*); }

namespace fledge {

enum class Key : int {
    Unknown = 0, W, A, S, D, Q, E, Space, Escape, LeftShift, Up, Down, Left, Right, F, Delete, Count
};
enum class MouseButton : int { Left = 0, Right = 1, Middle = 2 };

struct InputState {
    std::array<bool, static_cast<size_t>(Key::Count)> keys{};
    std::array<bool, 3> mouse{};
    Vec2 mousePos{};
    Vec2 mouseDelta{};
    float scroll = 0;
    std::string asciiText;  // printable characters received during the last event poll
    // Every key by GLFW key code (letters = ASCII); 500..502 = mouse left/right/middle (Fledge Script input map);
    // 400..414 = gamepad buttons (A B X Y LB RB Back Start Guide LS RS Up Right Down Left), 416 / 417 = RT / LT pulled.
    std::array<bool, 512> raw{};
    std::array<float, 6> padAxes{};  // first gamepad: left x, left y, right x, right y, LT, RT (GLFW ranges)
    bool padConnected = false;
    bool keyDown(Key k) const { return keys[static_cast<size_t>(k)]; }
    bool mouseDown(MouseButton b) const { return mouse[static_cast<size_t>(b)]; }
};

struct WindowDesc {
    std::string title = "Fledge";
    int width = 1280, height = 720;
    bool vsync = true;
    bool visible = true;
    bool maximized = false;
    bool openGL = true;  // false: no GL context (D3D12 owns the swapchain)
};

class Window {
public:
    virtual ~Window() = default;
    virtual bool shouldClose() const = 0;
    virtual void requestClose() = 0;
    virtual void pollEvents() = 0;
    virtual void swapBuffers() = 0;
    virtual int width() const = 0;   // framebuffer size in pixels
    virtual int height() const = 0;
    virtual const InputState& input() const = 0;
    virtual void setTitle(const std::string& title) = 0;
    virtual bool isHeadless() const = 0;
    // GL proc loader for the render backend (nullptr for headless).
    virtual GlProcLoader glLoader() const = 0;
    // Native handle for integrations (GLFWwindow* for GLFW backend).
    virtual GLFWwindow* glfwHandle() const { return nullptr; }
    virtual void* nativeHandle() const { return nullptr; }  // HWND on Windows
    // DPI scale of the monitor the window is on (1.0 = 96 dpi).
    virtual float contentScale() const { return 1.0f; }
    // Display mode: 0 windowed, 1 borderless fullscreen (desktop resolution), 2 exclusive fullscreen.
    // width/height 0 = the monitor's current mode (fullscreen) / keep the size (windowed).
    virtual void setDisplayMode(int mode, int width, int height) { (void)mode; (void)width; (void)height; }
    virtual void setVsync(bool on) { (void)on; }
    // Hides and locks the cursor (mouse-look games); deltas keep coming. The frame it changes reports no delta.
    virtual void setCursorCaptured(bool captured) { (void)captured; }
    virtual bool cursorCaptured() const { return false; }
    virtual bool focused() const { return true; }
    virtual void requestFocus() {}

    static std::unique_ptr<Window> createGlfw(const WindowDesc& desc);   // nullptr on failure
    static std::unique_ptr<Window> createNull(const WindowDesc& desc);
};

// High resolution clock.
class Clock {
public:
    Clock();
    double now() const;          // seconds since construction
    float tick();                // seconds since last tick (clamped to 0.25s)
private:
    int64_t m_start;
    int64_t m_last;
};

} // namespace fledge
