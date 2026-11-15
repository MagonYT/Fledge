// Command registry, keyboard shortcuts, command palette and the Keyboard Shortcuts dialog.
//
// Every menu item, toolbar button and palette entry runs a registered Command, so
// shortcuts, menus and the palette always agree. Later milestones (scripting,
// animation, AI authoring tools) add their own commands with registerCommands-style calls.
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <format>

#include "fledge/editor/fuzzy.h"
#include "editor_app.h"
#include "editor_code.h"
#include "editor_icons.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace fledge::editor {

// ------------------------------------------------------------------ registry --
void CommandRegistry::add(Command c) {
    for (auto& existing : m_commands)
        if (existing.id == c.id) {
            existing = std::move(c);
            return;
        }
    m_commands.push_back(std::move(c));
}

const Command* CommandRegistry::find(const std::string& id) const {
    for (auto& c : m_commands)
        if (c.id == id) return &c;
    return nullptr;
}

bool CommandRegistry::execute(const std::string& id) {
    const Command* c = find(id);
    if (!c || !c->run || !isEnabled(*c) || isHidden(*c)) return false;
    auto run = c->run;  // the command may re-register commands
    run();
    return true;
}

void CommandRegistry::dispatchShortcuts(bool textInputActive, bool viewportFlying, bool gameOwnsInput) {
    bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    for (size_t i = 0; i < m_commands.size(); ++i) {
        const Command& c = m_commands[i];
        if (!c.shortcut || !c.run || isHidden(c)) continue;
        if (gameOwnsInput) continue;
        bool plainKey = (c.shortcut & ImGuiMod_Mask_) == 0;
        if ((textInputActive || popupOpen) && !c.global) continue;
        if (viewportFlying && plainKey) continue;  // WASD/QE fly the camera while RMB is held
        if (ImGui::IsKeyChordPressed(c.shortcut) && isEnabled(c)) {
            execute(c.id);
            return;  // one command per frame
        }
    }
}

std::string CommandRegistry::chordName(ImGuiKeyChord chord) {
    if (!chord) return {};
    std::string s;
    if (chord & ImGuiMod_Ctrl) s += "Ctrl+";
    if (chord & ImGuiMod_Shift) s += "Shift+";
    if (chord & ImGuiMod_Alt) s += "Alt+";
    ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
    switch (key) {
        case ImGuiKey_Comma: return s + ",";
        case ImGuiKey_Slash: return s + "/";
        case ImGuiKey_Delete: return s + "Del";
        case ImGuiKey_Escape: return s + "Esc";
        case ImGuiKey_Enter: return s + "Enter";
        default: break;
    }
    const char* n = ImGui::GetKeyName(key);
    return s + (n ? n : "?");
}

// ------------------------------------------------------------ registration --
void EditorApp::registerCommands() {
    auto hasProject = [this] { return m_session.hasProject(); };
    auto editable = [this] { return m_session.hasProject() && !m_session.playing(); };
    auto hasSel = [this] { return !m_session.selection.empty() && !m_session.playing(); };
    CommandRegistry& R = m_commands;

    // File
    R.add({"file.new_project", "New Project...", "File", ImGuiMod_Ctrl | ImGuiKey_N, ICON_PLUS, [this] {
               m_showWelcome = true;
               m_welcomeTab = 1;
           }});
    R.add({"file.open_project", "Open Project...", "File", ImGuiMod_Ctrl | ImGuiKey_O, ICON_FOLDER_OPEN, [this] {
               m_showWelcome = true;
               m_welcomeTab = 0;
           }});
    R.add({"file.welcome", "Welcome Screen", "File", 0, ICON_HOUSE, [this] {
               m_showWelcome = true;
               m_welcomeTab = 0;
           }});
    R.add({"file.save", "Save Scene (or the script being edited)", "File", ImGuiMod_Ctrl | ImGuiKey_S, ICON_SAVE,
           [this] {
               if (m_code && m_code->focused()) {  // Ctrl+S in the Code workspace saves the active file
                   m_code->saveActive();
                   return;
               }
               if (m_scriptEditorFocused && !m_scriptFile.empty()) {  // Ctrl+S in the Script Editor saves the script
                   if (saveScriptFile()) toast(ToastLevel::Success, "Saved Scripts/" + m_scriptFile);
                   return;
               }
               auto st = m_session.saveScene();
               toast(st ? ToastLevel::Success : ToastLevel::Error, st ? "Scene saved" : st.error);
           },
           [this] { return (m_session.hasProject() && !m_session.playing()) || m_scriptEditorFocused || (m_code && m_code->focused()); }, nullptr, true});
    R.add({"file.revert_autosave", "Revert Last Auto-save", "File", 0, ICON_HISTORY,
           [this] {
               auto st = m_session.restoreAutoSaveBackup();
               toast(st ? ToastLevel::Success : ToastLevel::Error,
                     st ? "Loaded the scene as it was before the first auto-save. It is unsaved: save it to keep it." : st.error);
           },
           [this] { return m_session.hasProject() && !m_session.playing() && m_session.hasAutoSaveBackup(); }});
    R.add({"file.cook", "Cook Content", "File", 0, ICON_FIRE, [this] { startCook(false); },
           [this] { return m_session.hasProject() && !m_cookRunning; }});
    R.add({"file.cook_force", "Cook Content (Force Rebuild)", "File", 0, ICON_FIRE, [this] { startCook(true); },
           [this] { return m_session.hasProject() && !m_cookRunning; }});
    R.add({"file.package", "Package Game", "File", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_B, ICON_BOX, [this] { startPackage(); },
           [this, editable] { return editable() && !m_packageRunning; }});
    R.add({"file.exit", "Exit", "File", 0, ICON_XMARK, [this] { m_quit = true; }});
    // Asset Store Import (editor_store.cpp)
    R.add({"store.panel", "Asset Store Import", "Assets", 0, ICON_DOWNLOAD, [this] {
               if (m_prefs.simpleMode) m_showSimpleStore = true;
               else if (PanelDef* d = panel("Asset Store Import")) {
                   d->open = true;
                   m_focusQueue.push_back("Asset Store Import");
               }
           }});
    R.add({"store.import_file", "Import Asset Store Download (.unitypackage, .zip, .fbx ...)", "Assets", 0, ICON_DOWNLOAD, [this] { chooseStoreFile(); }});
    R.add({"store.third_party", "Export Third-party Assets List (attribution / licences)", "Assets", 0, ICON_FILE, [this] { exportThirdPartyList(); },
           [this] { return m_session.hasProject(); }});
    R.add({"store.restore_handlers", "Restore Original Link Handlers (Unity / Epic)", "Assets", 0, ICON_UNDO, [this] {
               auto r = linkHandlers().restoreAll(executablePath());
               toast(r ? ToastLevel::Success : ToastLevel::Error, r ? "Original link handlers restored" : r.error);
           }});

    // Graphics & Platform (editor_graphics.cpp) - each preset is one undoable step
    auto gfxProject = [this] { return m_session.hasProject(); };
    R.add({"project.settings", "Project Settings...", "Project", 0, ICON_SLIDERS, [this] { m_showProjectSettings = true; }, gfxProject});
    R.add({"graphics.panel", "Graphics & Platform", "Graphics", 0, ICON_GAUGE,
           [this] {
               if (PanelDef* d = panel("Graphics & Platform")) d->open = true;
               ImGui::SetWindowFocus("Graphics & Platform");
           }});
    for (const auto& info : graphicsPresets()) {
        GraphicsPreset preset = info.preset;
        R.add({std::string("graphics.preset.") + toString(preset), std::string("Graphics Preset: ") + info.name, "Graphics", 0, ICON_WAND,
               [this, preset] {
                   GraphicsSettings g = m_session.project().graphics;
                   RenderSettings r = m_session.project().render;
                   applyGraphicsPreset(preset, r, g, gameBackend(g));
                   applyGraphics(g, &r, std::string("Graphics preset: ") + toString(preset));
               },
               gfxProject, [this, preset] { return m_session.hasProject() && m_session.project().graphics.preset == preset; }});
    }
    R.add({"graphics.auto_tune", "Auto-tune Graphics (benchmark)", "Graphics", 0, ICON_GAUGE, [this] { startAutoTune(); },
           [this] { return m_session.hasProject() && !m_autoTuneRunning; }});
    R.add({"graphics.preview", "Preview Game in Selected API", "Graphics", 0, ICON_PLAY,
           [this] { launchPreview(rhi::toString(gameBackend(m_session.project().graphics)), {}); }, gfxProject});
    R.add({"graphics.path_trace", "Path-traced Reference (D3D12 + DXR)", "Graphics", 0, ICON_CAMERA,
           [this] { launchPreview("d3d12", {"--path-trace", "--freeze-time"}); },
           [this] { return m_session.hasProject() && apiProbe(rhi::Backend::D3D12).rayTracing; }});

    // Scripts (Fledge Script)
    R.add({"scripts.build", "Build Scripts", "Scripts", ImGuiMod_Ctrl | ImGuiKey_B, ICON_BOLT,
           [this] {
               if (m_scriptText != m_scriptSaved) saveScriptFile();
               if (m_code) m_code->saveAll();
               startScriptBuild(false, true);
           },
           hasProject, nullptr, true});
    R.add({"scripts.rebuild", "Rebuild All Scripts", "Scripts", 0, ICON_REDO, [this] { startScriptBuild(true, true); }, hasProject});
    R.add({"scripts.new", "New Script...", "Scripts", 0, ICON_PLUS, [this] { m_showNewScript = true; }, hasProject});
    R.add({"scripts.convert_blueprint", "Convert Unreal Blueprint...", "Scripts", 0, ICON_CODE, [this] {
               if (m_prefs.simpleMode) m_showBlueprintSimple = true;
               else if (PanelDef* d = panel("Convert Blueprint")) {
                   d->open = true;
                   m_focusQueue.push_back("Convert Blueprint");
               }
           }});
    if (currentEdition() == Edition::Advanced)  // Fledge only shows a hint on Settings > Migrate Settings
        R.add({"file.migrate_settings", "Migrate all settings to Fledge Advanced...", "File", 0, ICON_FILES, [this] { openMigrationWindow(false); }});
    R.add({"scripts.reload", "Reload Script Module", "Scripts", 0, ICON_ROTATE,
           [this] {
               fs::path dll = m_session.project().currentScriptModule();
               if (dll.empty()) toast(ToastLevel::Warning, "No built script module yet");
               else if (auto st = m_session.loadScriptModule(dll); !st) toast(ToastLevel::Error, st.error);
               else toast(ToastLevel::Success, "Reloaded " + narrow(dll.filename().wstring()));
           },
           hasProject});
    R.add({"code.open", "Open Code Workspace", "Code", 0, ICON_CODE, [this] { switchWorkspace("Code"); }, hasProject});
    R.add({"code.save_all", "Save All Code Files", "Code", 0, ICON_SAVE, [this] { m_code->saveAll(); }, [this] { return m_code && m_code->anyDirty(); }});
    R.add({"code.new_terminal", "New Terminal (Code workspace)", "Code", 0, ICON_TERMINAL,
           [this] {
               switchWorkspace("Code");
               m_code->newTerminal();
           },
           hasProject});
    R.add({"code.quick_open", "Go to File...", "Code", 0, ICON_FILE, [this] { openPalette(false, "/"); }, hasProject});
    R.add({"scripts.open_ide", "Open Scripts in External Editor", "Scripts", 0, ICON_EXTERNAL,
           [this] { openInExternalIde(m_scriptFile, m_scriptCursorLine); }, hasProject});

    // Edit
    R.add({"edit.undo", "Undo", "Edit", ImGuiMod_Ctrl | ImGuiKey_Z, ICON_UNDO, [this] { m_session.undo(); },
           [this] { return m_session.history().canUndo() && !m_session.playing(); }});
    R.add({"edit.redo", "Redo", "Edit", ImGuiMod_Ctrl | ImGuiKey_Y, ICON_REDO, [this] { m_session.redo(); },
           [this] { return m_session.history().canRedo() && !m_session.playing(); }});
    R.add({"edit.redo_alt", "Redo (alternate shortcut)", "Edit", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ICON_REDO,
           [this] { m_session.redo(); }, [this] { return m_session.history().canRedo() && !m_session.playing(); }});
    R.add({"edit.duplicate", "Duplicate", "Edit", ImGuiMod_Ctrl | ImGuiKey_D, ICON_COPY,
           [this] { m_session.selection = m_session.duplicate(m_session.selection); }, hasSel});
    R.add({"edit.delete", "Delete", "Edit", ImGuiKey_Delete, ICON_TRASH,
           [this] {
               if (m_renaming == kNullEntity) (void)m_session.destroyEntities(m_session.selection);
           },
           hasSel});
    R.add({"edit.select_none", "Select None", "Edit", ImGuiKey_Escape, ICON_XMARK, [this] { m_session.selection.clear(); }});
    R.add({"edit.focus", "Focus Selection", "Edit", ImGuiKey_F, ICON_CROSSHAIRS, [this] { focusSelection(); },
           [this] { return !m_session.selection.empty(); }});
    R.add({"edit.create_empty", "Create Empty Entity", "Edit", 0, ICON_PLUS,
           [this] { m_session.select(m_session.createEntity("Entity")); }, editable});
    R.add({"edit.create_cube", "Create Cube", "Edit", 0, ICON_CUBE,
           [this] { m_session.select(m_session.createEntity("Cube", kNullEntity, {{"MeshRenderer", {{"mesh", "builtin:cube"}}}})); },
           editable});
    R.add({"edit.create_light", "Create Point Light", "Edit", 0, ICON_LIGHTBULB,
           [this] { m_session.select(m_session.createEntity("Point Light", kNullEntity, {{"Light", {{"type", "Point"}}}})); },
           editable});
    R.add({"scene.why", "Ask the Scene: Why? (selected entity)", "Edit", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Y, ICON_LIGHTBULB,
           [this] {
               m_whyEntity = m_session.selection.front();
               m_whyOpen = true;
           },
           hasSel});
    R.add({"file.ship", "Ship / Capsule...", "File", 0, ICON_SHARE, [this] { m_showShip = true; }});
    R.add({"perf.make_it_run_well", "Make it run well...", "Graphics", 0, ICON_GAUGE, [this] { m_showPerf = true; }, [this] { return m_session.hasProject(); }});
    R.add({"play.bot", "Playtest Bot...", "Play", 0, ICON_ROBOT, [this] { m_showBot = true; }, [this] { return m_session.hasProject(); }});
    R.add({"play.rewind", "Time Machine: Rewind 1 s", "Play", 0, ICON_HISTORY,
           [this] { m_session.timeMachine.seek(m_session, std::max(m_session.timeMachine.firstStep(), m_session.timeMachine.currentStep() - 60)); },
           [this] { return m_session.playing(); }});
    R.add({"edit.settings", "Settings...", "Edit", ImGuiMod_Ctrl | ImGuiKey_Comma, ICON_GEAR, [this] { m_showSettings = true; }});

    // Tools (transform)
    R.add({"tool.translate", "Move Tool", "Tools", ImGuiKey_W, ICON_MOVE, [this] { m_gizmo.mode = GizmoMode::Translate; }, nullptr,
           [this] { return m_gizmo.mode == GizmoMode::Translate; }});
    R.add({"tool.rotate", "Rotate Tool", "Tools", ImGuiKey_E, ICON_ROTATE, [this] { m_gizmo.mode = GizmoMode::Rotate; }, nullptr,
           [this] { return m_gizmo.mode == GizmoMode::Rotate; }});
    R.add({"tool.scale", "Scale Tool", "Tools", ImGuiKey_R, ICON_SCALE, [this] { m_gizmo.mode = GizmoMode::Scale; }, nullptr,
           [this] { return m_gizmo.mode == GizmoMode::Scale; }});
    R.add({"tool.space", "Toggle Local / World Space", "Tools", ImGuiKey_L, ICON_GLOBE,
           [this] { m_gizmo.space = m_gizmo.space == GizmoSpace::Local ? GizmoSpace::World : GizmoSpace::Local; }, nullptr,
           [this] { return m_gizmo.space == GizmoSpace::Local; }});
    R.add({"tool.snap", "Toggle Snapping", "Tools", ImGuiMod_Ctrl | ImGuiKey_G, ICON_MAGNET, [this] { m_gizmo.snap = !m_gizmo.snap; },
           nullptr, [this] { return m_gizmo.snap; }});

    // Play
    R.add({"play.play", "Play", "Play", ImGuiKey_F5, ICON_PLAY,
           [this] {
               if (m_session.playing()) m_session.setPaused(false);
               else {
                   m_userPlayPending = true;  // Play button / F5 / menu / palette: a user action (the mouse lock may engage)
                   stopSimulation(false);  // Play starts from the edit state
                   if (auto st = m_session.play(); !st) toast(ToastLevel::Error, st.error);
               }
           },
           [this] { return m_session.hasProject() && (!m_session.playing() || m_session.paused()); }});
    R.add({"play.pause", "Pause / Resume", "Play", ImGuiKey_F6, ICON_PAUSE, [this] { m_session.setPaused(!m_session.paused()); },
           [this] { return m_session.playing(); }, [this] { return m_session.paused(); }});
    R.add({"play.step", "Step One Frame", "Play", ImGuiKey_F10, ICON_STEP, [this] { m_session.step(); },
           [this] { return m_session.paused(); }});
    R.add({"play.stop", "Stop", "Play", ImGuiMod_Shift | ImGuiKey_F5, ICON_STOP, [this] { (void)m_session.stop(); },
           [this] { return m_session.playing(); }});
    R.add({"play.host", "Host Multiplayer", "Play", 0, ICON_SERVER,
           [this] {
               m_userPlayPending = true;
               stopSimulation(false);
               if (auto st = m_session.playHost(static_cast<uint16_t>(std::clamp(m_hostPort, 1, 65535))); !st)
                   toast(ToastLevel::Error, st.error);
           },
           [this] { return m_session.hasProject() && !m_session.playing(); }});
    R.add({"play.join", "Join Multiplayer...", "Play", 0, ICON_LINK, [this] { m_showJoin = true; },
           [this] { return m_session.hasProject() && !m_session.playing(); }});

    registerPhysicsCommands();

    // View
    R.add({"view.palette", "Command Palette", "View", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P, ICON_SEARCH,
           [this] { openPalette(true); }, nullptr, nullptr, true});
    R.add({"view.quick_open", "Go to Anything (commands, files, entities, assets)", "View", ImGuiMod_Ctrl | ImGuiKey_P, ICON_SEARCH,
           [this] { openPalette(false); }, nullptr, nullptr, true});
    const char* workspaces[] = {"Level", "Scripting", "Code", "Animation", "AI"};
    const char* wsIcons[] = {ICON_CUBES, ICON_BOLT, ICON_CODE, ICON_FILM, ICON_ROBOT};
    for (int i = 0; i < 5; ++i) {
        std::string ws = workspaces[i];
        R.add({"view.workspace." + toLower(ws), "Workspace: " + ws, "View", ImGuiMod_Ctrl | static_cast<ImGuiKey>(ImGuiKey_1 + i),
               wsIcons[i], [this, ws] { switchWorkspace(ws); }, nullptr, [this, ws] { return m_prefs.workspace == ws; }});
    }
    R.add({"view.simple_mode", "Simple Mode (Describe, Play, Share)", "View", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_M, ICON_FEATHER,
           [this] { setSimpleMode(!m_prefs.simpleMode); }, nullptr, [this] { return m_prefs.simpleMode; }, true});
    R.add({"help.tour", "Show the 60-second Tour", "Help", 0, ICON_INFO,
           [this] {
               setSimpleMode(true);
               startTour();
           }});
    R.add({"view.layout.reset", "Reset Layout", "View", 0, ICON_TABLE, [this] { resetLayout(); }});
    R.add({"view.layout.save", "Save Layout", "View", 0, ICON_SAVE, [this] { saveLayout(); }});
    R.add({"view.theme.dark", "Theme: Dark", "View", 0, ICON_MOON,
           [this] {
               m_prefs.theme = Theme::Dark;
               applyTheme();
           },
           nullptr, [this] { return m_prefs.theme == Theme::Dark; }});
    R.add({"view.theme.light", "Theme: Light", "View", 0, ICON_SUN,
           [this] {
               m_prefs.theme = Theme::Light;
               applyTheme();
           },
           nullptr, [this] { return m_prefs.theme == Theme::Light; }});
    R.add({"view.grid", "Toggle Grid", "View", ImGuiKey_G, ICON_GRID, [this] { m_prefs.showGrid = !m_prefs.showGrid; }, nullptr,
           [this] { return m_prefs.showGrid; }});
    R.add({"view.stats", "Toggle Viewport Stats", "View", 0, ICON_GAUGE, [this] { m_prefs.showStats = !m_prefs.showStats; }, nullptr,
           [this] { return m_prefs.showStats; }});
    R.add({"view.orientation", "Toggle Orientation Cube", "View", 0, ICON_CUBE,
           [this] { m_prefs.showOrientation = !m_prefs.showOrientation; }, nullptr, [this] { return m_prefs.showOrientation; }});
    struct Mode {
        DebugView v;
        const char* name;
    };
    for (Mode m : {Mode{DebugView::None, "Lit"}, Mode{DebugView::Albedo, "Albedo"}, Mode{DebugView::Normal, "Normals"},
                   Mode{DebugView::Roughness, "Roughness"}, Mode{DebugView::Metallic, "Metallic"}, Mode{DebugView::Depth, "Depth"},
                   Mode{DebugView::ShadowCascades, "Shadow Cascades"}, Mode{DebugView::Wireframe, "Wireframe"}}) {
        R.add({std::format("view.mode.{}", static_cast<int>(m.v)), std::string("View Mode: ") + m.name, "View", 0, ICON_EYE,
               [this, v = m.v] { m_viewMode = v; }, nullptr, [this, v = m.v] { return m_viewMode == v; }});
    }
    struct Cam {
        const char* id;
        const char* name;
        Vec3 fwd;
    };
    for (Cam c : {Cam{"top", "Top", {0, -1, 0}}, Cam{"bottom", "Bottom", {0, 1, 0}}, Cam{"front", "Front", {0, 0, -1}},
                  Cam{"back", "Back", {0, 0, 1}}, Cam{"right", "Right", {-1, 0, 0}}, Cam{"left", "Left", {1, 0, 0}}}) {
        R.add({std::string("view.camera.") + c.id, std::string("Camera: ") + c.name + " View", "View", 0, ICON_VIDEO,
               [this, f = c.fwd] { setCameraView(f); }});
    }

    // AI / MCP
    R.add({"ai.assistant", "Open AI Assistant", "AI", ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_A, ICON_ROBOT, [this] {
               if (PanelDef* p = panel("AI Assistant")) p->open = true;
               ImGui::SetWindowFocus("AI Assistant");
           }});
    R.add({"ai.new_chat", "AI: New Chat", "AI", 0, ICON_COMMENT,
           [this] {
               if (m_assistant) m_assistant->reset();
               std::lock_guard l(m_chatMutex);
               m_chat.clear();
           },
           [this] { return !m_aiBusy; }});
    R.add({"ai.settings", "AI Provider Settings...", "AI", 0, ICON_KEY, [this] {
               m_showSettings = true;
               m_settingsPage = "AI Providers";
           }});
    R.add({"mcp.toggle", "Start / Stop MCP HTTP Server", "AI", 0, ICON_SERVER,
           [this] {
               if (m_mcpHttp.running()) {
                   m_mcpHttp.stop();
                   toast(ToastLevel::Info, "MCP HTTP server stopped");
               } else {
                   startMcpHttp(m_mcpPortSetting);
               }
           },
           nullptr, [this] { return m_mcpHttp.running(); }});

    // AI master switch + control level (always visible: this is how the user turns AI back on)
    R.add({"ai.toggle", "Enable AI Features (master switch)", "AI", 0, ICON_ROBOT, [this] { setAiEnabled(!aiOn()); }, nullptr,
           [this] { return aiOn(); }});
    const char* levels[] = {"Off (AI may only read)", "Ask First (approve every change)", "Auto-approve Safe Actions", "Full Control"};
    for (int i = 0; i < 4; ++i) {
        AiLevel lv = static_cast<AiLevel>(i);
        R.add({std::string("ai.level.") + toString(lv), std::string("AI Control Level: ") + levels[i], "AI", 0, ICON_SHIELD,
               [this, lv] {
                   m_session.ai.setLevel(lv);
                   m_session.ai.applyTo(m_mcp->policy());
                   saveSettings();
                   toast(ToastLevel::Info, std::string("AI control level: ") + toString(lv));
               },
               [this] { return aiOn(); }, [this, lv] { return m_session.ai.level() == lv; }});
    }

    // Help
    R.add({"help.script", "Fledge Script Guide (docs/FLEDGE_SCRIPT.md)", "Help", 0, ICON_BOOK, [this] {
#ifdef _WIN32
               ShellExecuteW(nullptr, L"open", (m_paths.docs / "FLEDGE_SCRIPT.md").wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
           }});
    R.add({"help.shortcuts", "Keyboard Shortcuts", "Help", ImGuiKey_F1, ICON_KEYBOARD, [this] { m_showShortcuts = true; }});
    R.add({"help.about", "About Fledge", "Help", 0, ICON_INFO, [this] { m_showAbout = true; }});
#ifdef _WIN32
    auto openFolder = [this](const fs::path& p, const char* what) {
        std::error_code ec;
        fs::create_directories(p, ec);
        auto r = reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (r <= 32) toast(ToastLevel::Error, std::string("Cannot open the ") + what + " folder");
    };
    R.add({"help.docs", "Open Documentation Folder", "Help", 0, ICON_BOOK, [this, openFolder] { openFolder(m_paths.docs, "docs"); }});
    R.add({"help.licenses", "Open Licenses Folder", "Help", 0, ICON_SHIELD,
           [this, openFolder] { openFolder(m_paths.licenses, "licenses"); }});
    R.add({"help.userdata", "Open User Data Folder", "Help", 0, ICON_FOLDER,
           [this, openFolder] { openFolder(m_opt.settingsDir, "user data"); }});
    R.add({"file.reveal_project", "Show Project in Explorer", "File", 0, ICON_FOLDER,
           [this, openFolder] { openFolder(m_session.project().rootDir(), "project"); }, hasProject});
#endif
}

// ----------------------------------------------------------------- palette --
namespace {
struct PaletteItem {
    enum class Kind { Command, Entity, Asset, Recent, File } kind;
    std::string label, detail;
    const char* icon = "";
    int score = 0;
    std::string id;          // command id / asset path / recent project path
    EntityId entity = kNullEntity;
    bool enabled = true;
};
} // namespace

void EditorApp::openPalette(bool commandsOnly, const std::string& query) {
    m_showPalette = true;
    m_paletteCommandsOnly = commandsOnly;
    m_paletteQuery = query;
    m_paletteSel = 0;
    m_paletteFocus = true;
}

void EditorApp::drawCommandPalette() {
    if (m_showPalette) {
        ImGui::OpenPopup("##palette");
        m_showPalette = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float w = std::min(640.0f * effectiveScale(), vp->WorkSize.x - 40);
    ImGui::SetNextWindowPos({vp->WorkPos.x + (vp->WorkSize.x - w) * 0.5f, vp->WorkPos.y + 40 * effectiveScale()});
    ImGui::SetNextWindowSize({w, 0});
    m_paletteVisible = ImGui::BeginPopup("##palette", ImGuiWindowFlags_NoMove);
    if (!m_paletteVisible) return;

    // ---- query
    if (m_paletteFocus) {
        ImGui::SetKeyboardFocusHere();
        m_paletteFocus = false;
    }
    ImGui::SetNextItemWidth(-1);
    const char* hint = m_paletteCommandsOnly ? "Type a command..." : "Search commands, files, entities, assets   (> commands  / files  @ entities  # assets)";
    std::string before = m_paletteQuery;
    bool enterInInput = ImGui::InputTextWithHint("##q", hint, &m_paletteQuery, ImGuiInputTextFlags_EnterReturnsTrue);
    if (m_paletteQuery != before) m_paletteSel = 0;
    if (enterInInput) ImGui::SetKeyboardFocusHere(-1);  // keep typing after Enter if the popup stays open
    trackRect("palette.input");

    std::string q = m_paletteQuery;
    bool cmds = true, ents = !m_paletteCommandsOnly, assets = !m_paletteCommandsOnly, files = !m_paletteCommandsOnly;
    if (!q.empty() && (q[0] == '>' || q[0] == '@' || q[0] == '#' || q[0] == '/')) {
        cmds = q[0] == '>';
        ents = q[0] == '@';
        assets = q[0] == '#';
        files = q[0] == '/';
        q = q.substr(1);
    }

    std::vector<PaletteItem> items;
    if (cmds) {
        for (const Command& c : m_commands.all()) {
            if (m_commands.isHidden(c)) continue;
            std::string label = c.category + ": " + c.title;
            int s = fuzzyScore(q, label);
            if (s < 0) continue;
            PaletteItem it{PaletteItem::Kind::Command, c.title, CommandRegistry::chordName(c.shortcut), c.icon, s + 2, c.id};
            it.enabled = m_commands.isEnabled(c);
            if (c.checked && c.checked()) it.detail = (it.detail.empty() ? "" : it.detail + "  ") + "(on)";
            it.label = c.category + ": " + c.title;
            items.push_back(std::move(it));
        }
    }
    if (ents && m_session.hasProject()) {
        for (EntityId e : m_session.scene().entities()) {
            std::string name = m_session.scene().displayName(e);
            int s = fuzzyScore(q, name);
            if (s < 0 || (q.empty() && !m_paletteCommandsOnly && items.size() > 60)) continue;
            PaletteItem it{PaletteItem::Kind::Entity, name, std::format("entity #{}", e), ICON_CUBE, s + (q.empty() ? -5 : 1)};
            it.entity = e;
            items.push_back(std::move(it));
        }
    }
    if (assets && m_session.hasProject()) {
        for (const auto& rec : m_session.registry().all()) {
            int s = fuzzyScore(q, rec.path);
            if (s < 0) continue;
            const char* icon = rec.type == AssetType::Material ? ICON_PALETTE
                               : rec.type == AssetType::Texture ? ICON_IMAGE
                               : rec.type == AssetType::Scene   ? ICON_MAP
                                                                : ICON_CUBES;
            items.push_back({PaletteItem::Kind::Asset, rec.path, toString(rec.type), icon, s + (q.empty() ? -6 : 0), rec.path});
        }
    }
    if (files && m_code && m_session.hasProject()) {  // file mode: project text files, opened in the Code workspace
        bool only = !cmds;
        for (const auto& f : m_code->projectFiles()) {
            int s = fuzzyScore(q, f);
            if (s < 0) continue;
            std::string name = narrow(pathFromUtf8(f).filename().wstring());
            items.push_back({PaletteItem::Kind::File, name, f, ICON_FILE, s + (only ? 3 : (q.empty() ? -8 : 1)), f});
            if (items.size() > 4000) break;
        }
    }
    if (!m_paletteCommandsOnly && cmds && ents && assets) {  // unprefixed quick-open also lists recent projects
        for (const auto& p : m_recent.items()) {
            std::string label = "Open Recent: " + narrow(p.stem().wstring());
            int s = fuzzyScore(q, label);
            if (s >= 0) items.push_back({PaletteItem::Kind::Recent, label, narrow(p.wstring()), ICON_CLOCK, s, narrow(p.wstring())});
        }
    }
    std::stable_sort(items.begin(), items.end(), [](const PaletteItem& a, const PaletteItem& b) { return a.score > b.score; });
    if (items.size() > 200) items.resize(200);
    int n = static_cast<int>(items.size());
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) m_paletteSel = std::min(n - 1, m_paletteSel + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) m_paletteSel = std::max(0, m_paletteSel - 1);
    m_paletteSel = std::clamp(m_paletteSel, 0, std::max(0, n - 1));
    bool enter = enterInInput || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();

    int chosen = -1;
    float rowH = ImGui::GetTextLineHeightWithSpacing() + 4;
    ImGui::BeginChild("##results", {0, std::min(12, std::max(1, n)) * rowH + 6}, ImGuiChildFlags_None);
    for (int i = 0; i < n; ++i) {
        const PaletteItem& it = items[i];
        ImGui::PushID(i);
        bool sel = i == m_paletteSel;
        ImGui::BeginDisabled(!it.enabled);
        if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_AllowOverlap, {0, rowH - 4})) chosen = i;
        ImGui::EndDisabled();
        if (sel && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow))) ImGui::SetScrollHereY();
        ImGui::SameLine(8);
        ImGui::TextDisabled("%s", it.icon);
        ImGui::SameLine(34 * effectiveScale());
        if (!it.enabled) ImGui::TextDisabled("%s", it.label.c_str());
        else ImGui::TextUnformatted(it.label.c_str());
        if (!it.detail.empty()) {
            float dw = ImGui::CalcTextSize(it.detail.c_str()).x;
            ImGui::SameLine(ImGui::GetContentRegionMax().x - dw - 8);
            ImGui::TextDisabled("%s", it.detail.c_str());
        }
        ImGui::PopID();
    }
    if (n == 0) ImGui::TextDisabled("  No matches");
    ImGui::EndChild();
    ImGui::TextDisabled("%s  Enter run   Up/Down select   Esc close", ICON_KEYBOARD);
    if (enter && n > 0) chosen = m_paletteSel;
    if (chosen >= 0 && items[chosen].enabled) {
        PaletteItem it = items[chosen];
        ImGui::CloseCurrentPopup();
        switch (it.kind) {
            case PaletteItem::Kind::Command: m_commands.execute(it.id); break;
            case PaletteItem::Kind::Entity:
                m_session.select(it.entity);
                focusSelection();
                break;
            case PaletteItem::Kind::Asset: {
                const AssetRecord* rec = m_session.registry().findByPath(it.id);
                if (rec && rec->type == AssetType::Material) openMaterialEditor(rec->guid.toString());
                else if (rec && rec->type == AssetType::Scene) dropAsset(it.id, kNullEntity, std::nullopt);
                else {
                    m_assetFilter = narrow(pathFromUtf8(it.id).filename().wstring());
                    ImGui::SetWindowFocus("Content Browser");
                    toast(ToastLevel::Info, "Showing " + it.id + " in the Content Browser");
                }
                break;
            }
            case PaletteItem::Kind::Recent: openProject(pathFromUtf8(it.id)); break;
            case PaletteItem::Kind::File:
                switchWorkspace("Code");
                m_code->openFile(m_code->root() / pathFromUtf8(it.id));
                break;
        }
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------- shortcuts list --
void EditorApp::drawShortcutsDialog() {
    if (!m_showShortcuts) return;
    ImGui::SetNextWindowSize({620 * effectiveScale(), 560 * effectiveScale()}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (!ImGui::Begin(ICON_KEYBOARD "  Keyboard Shortcuts###shortcuts", &m_showShortcuts, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##f", ICON_SEARCH "  filter", &m_shortcutFilter);
    if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                          {0, ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() * 8.5f})) {
        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthFixed, 150 * effectiveScale());
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 90 * effectiveScale());
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const Command& c : m_commands.all()) {
            if (!c.shortcut || m_commands.isHidden(c)) continue;
            std::string key = CommandRegistry::chordName(c.shortcut);
            if (fuzzyScore(m_shortcutFilter, c.title + " " + key + " " + c.category) < 0) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s  %s", c.icon, c.title.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "%s", key.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", c.category.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Viewport camera and selection");
    ImGui::TextDisabled("RMB + WASD / Q E   fly (hold Shift for speed)      Alt + LMB   orbit      MMB   pan      Wheel   dolly");
    ImGui::TextDisabled("LMB   pick      Ctrl + LMB   add to selection      drag a gizmo handle   move / rotate / scale");
    ImGui::TextDisabled("Hierarchy: drag rows to re-parent, double-click to rename. Content Browser: drag assets into the viewport.");
    ImGui::Spacing();
    ImGui::TextDisabled("%s Shortcuts are fixed in this version; rebinding is on the roadmap.", ICON_INFO);
    ImGui::End();
}

} // namespace fledge::editor
