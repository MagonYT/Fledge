#pragma once
// In-game graphics settings menu for players (F10). Drawn with the renderer's 2D overlay and
// stb_easy_font. Changes apply live where possible (preset, upscaler, ray tracing, sync, window
// mode); the graphics API and HDR take effect on the next start. "Apply & save" writes the
// player's choices to <user data>/Games/<game>/settings.json - never into the game folder.
#include <string>
#include <vector>

#include "fledge/framework/project.h"
#include "fledge/script/input_map.h"
#include "fledge/platform/window.h"
#include "fledge/render/renderer.h"

namespace fledge {

class PlayerSettingsMenu {
public:
    PlayerSettingsMenu(GraphicsSettings& graphics, RenderSettings& render, fs::path settingsFile, rhi::RenderDevice& device, Window& window);

    // Merges saved player overrides into the project defaults (returns base when there is no file).
    static GraphicsSettings loadOverrides(const fs::path& file, const GraphicsSettings& base);
    // The player's key bindings saved by the Controls page ("input" block; null when there are none).
    static Json loadInputOverrides(const fs::path& file);
    // Enables the Controls page (key / gamepad rebinding) for this input map.
    void setInputMap(script::InputMap* map) { m_inputMap = map; }

    void update(const InputState& in);  // F10 toggles; arrows / Enter / Esc navigate
    bool isOpen() const { return m_open; }
    void setOpen(bool o) { m_open = o; }
    void draw(ForwardRenderer& renderer, int width, int height);
    bool consumedEscape() const { return m_ateEscape; }

private:
    struct Item {
        std::string label;
        std::vector<std::string> values;
        int index = 0;
        bool restart = false;    // takes effect after a restart
        std::string disabled;    // reason when the option is unavailable
    };
    void rebuild();
    void change(int item, int delta);
    void apply(int item);
    bool save();
    bool pressed(const InputState& in, int key);

    GraphicsSettings& m_g;
    RenderSettings& m_r;
    fs::path m_file;
    rhi::RenderDevice& m_device;
    Window& m_window;
    std::vector<Item> m_items;
    int m_sel = 0;
    bool m_open = false, m_ateEscape = false;
    std::array<bool, 512> m_prev{};
    script::InputMap* m_inputMap = nullptr;
    bool m_controls = false, m_waiting = false;
    int m_csel = 0;
    std::vector<std::string> controlActions() const;
    void updateControls(const InputState& in);
    std::string m_status;
    double m_statusTime = 0;
    int m_frame = 0;
};

class DeveloperOverlay {
public:
    // Returns true on frames where the console owns keyboard and pointer input.
    bool update(const InputState& input, bool available);
    void draw(ForwardRenderer& renderer, int width, int height, float dt, size_t entities,
              uint32_t draws, uint32_t triangles);
    bool consoleOpen() const { return m_console; }

private:
    std::array<bool, 512> m_previous{};
    bool m_console = false;
    bool m_hud = false;
    std::string m_command;
    std::string m_stats;
};

} // namespace fledge
