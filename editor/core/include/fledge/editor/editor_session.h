#pragma once
// EditorSession: editor state without any UI. Shared by the ImGui editor and
// the headless MCP server (fledge_mcp_server), so every MCP tool works the same
// in both.
//
// ALL scene mutations from the editor UI, the MCP tools and the AI assistant go
// through the "ops" below, which record undoable commands in history().
// Direct Scene access is fine for reads.
#include <chrono>
#include <functional>
#include <memory>
#include <optional>

#include "fledge/editor/autosave.h"
#include "fledge/editor/time_machine.h"
#include "fledge/assets/asset_registry.h"
#include "fledge/assets/material.h"
#include "fledge/editor/file_watcher.h"
#include "fledge/importers/importers.h"
#include "fledge/packager/cooker.h"
#include "fledge/editor/ai_control.h"
#include "fledge/editor/command_stack.h"
#include "fledge/editor/import_queue.h"
#include "fledge/importers/store_import.h"
#include "fledge/framework/game_world.h"
#include "fledge/net/net.h"
#include "fledge/framework/edition_check.h"
#include "fledge/framework/project.h"
#include "fledge/packager/packager.h"
#include "fledge/packager/script_build.h"
#include <set>

namespace fledge {

namespace editor {
class EditorControlHost;
class McpAuditLog;
class RateLimiter;
} // namespace editor

// The error for operations that need a project on disk (import, cook, package, scripts ...): says what to do next.
inline constexpr const char* kNoProjectHelp =
    "no project is open: call create_project {directory, name} first (the current scene and any materials made so far move "
    "into it), or open a project in the editor";

class EditorSession {
public:
    EditorSession();
    ~EditorSession();

    // Declines (returns a failure, changes nothing) when the project needs features this edition lacks; lastDecline()
    // then describes why (docs/EDITIONS.md). Cleared by the next openProject().
    Status openProject(const fs::path& file);
    // Called on the thread that is opening the project, between the long steps (asset scan, scene read).
    void setLoadProgress(std::function<void(const std::string& stage, float fraction)> fn) { m_loadProgress = std::move(fn); }
    const EditionCheck* lastDecline() const { return m_decline ? &*m_decline : nullptr; }
    // Records that the open project uses an edition feature (manifest "requiredFeatures"; saved at once). No-op when it
    // is already listed. Called by saveScene() for the components of Advanced modules, and by modules for their assets.
    Status recordFeatureUse(const FeatureRequirement& r);
    // The Standard game player for this project: fledge_runtime.exe.
    fs::path runtimeExe() const;
    Status newProject(const fs::path& directory, const std::string& name);
    // MCP create_project: like newProject, but work done without a project (the current scene, scratch materials) moves in.
    Status createProject(const fs::path& directory, const std::string& name);
    bool hasProject() const { return m_project.has_value(); }
    const Project& project() const { return *m_project; }

    GameWorld& world() { return *m_world; }
    Scene& scene() { return m_world->scene(); }
    Vfs& vfs() { return m_vfs; }
    AssetRegistry& registry() { return m_registry; }
    CommandStack& history() { return *m_history; }

    Status loadScene(const std::string& vpath);       // clears history
    Status saveScene(const std::string& vpath = {});  // empty = current scene path
    Status newScene(const std::string& name);         // undoable

    // ---- auto-save and crash recovery (editor/autosave.h, docs/UI.md "Auto-save and recovery")
    editor::AutosaveSettings autosave;  // per user (editor_settings.json "autosave")
    struct AutosaveEnv {
        bool busyUi = false;         // a modal, a drag / gizmo operation, an import, cook or package is running
        bool focused = true;         // the editor window has focus
        bool allowRealSave = false;  // true only for the editor GUI: headless / MCP sessions never write the real scene
    };
    struct AutosaveResult {
        bool snapshot = false;   // a recovery snapshot was written
        bool autoSaved = false;  // the scene file was auto-saved
        std::string error;       // a write that failed (the previous state is intact)
    };
    // Call once per frame (editor) or after each request (headless MCP). Does the timing of editor/autosave.h and the writes.
    AutosaveResult pollAutosave(const AutosaveEnv& env);
    AutosaveResult pollAutosave() { return pollAutosave(AutosaveEnv{}); }  // headless: snapshots only
    // Writes a recovery snapshot of the unsaved scene now when the rules allow it (project, dirty, not playing, not in a
    // transaction, no undecided recovery offer); false = skipped or failed (error in *error).
    bool writeRecoverySnapshot(std::string* error = nullptr);
    // Saves the scene to its real file like saveScene(), first keeping the last manually saved file as a backup
    // (Intermediate/Recovery/AutoSaveBackup/, once until the next manual save). Not during Play. Does not touch the backup of a
    // manual save: saveScene() removes it.
    Status autoSaveScene();
    bool hasAutoSaveBackup() const;
    // Loads the backup into the scene as unsaved changes (the user decides whether to save): undoes an unwanted auto-save.
    Status restoreAutoSaveBackup();
    // A recovery to offer for the open project (set by openProject; empty after the user decided).
    const std::optional<editor::RecoveryOffer>& recoveryOffer() const { return m_recoveryOffer; }
    Status restoreRecovery();       // open the offered snapshot as the unsaved scene (dirty); snapshots stay until the next save
    void keepSavedVersion();        // "Open saved version": keep the snapshots until the next save
    void discardRecovery();         // delete the snapshots
    // Clean close: call before the project is closed / replaced or the host exits. A clean scene deletes its snapshots, an
    // unsaved one gets a final snapshot and keeps them (the next open offers the recovery).
    void closeProjectRecovery();
    fs::path recoveryDir() const;   // <project>/Intermediate/Recovery, empty without a project
    uint64_t changeSerial() const { return m_changeSerial; }
    Json autosaveState() const;     // read-only report (MCP get_project_info)
    // Clocks, replaceable by tests: monotonic seconds for the timers, wall-clock ms for snapshot / file times.
    std::function<double()> autosaveClock;
    std::function<int64_t()> wallClock;

    Status play();
    // Listen server: Play plus a UDP host on this scene (WASD/mouse look the Host sphere). Join connects as a client.
    Status playHost(uint16_t port = 7777);
    Status playJoin(const std::string& hostPort);
    Status startListenHost(std::unique_ptr<net::Transport> transport);  // tests: SimulatedNetwork
    Status startJoin(std::unique_ptr<net::Transport> transport, net::NetAddress server);
    bool netHosting() const { return m_netServer != nullptr; }
    bool netJoining() const { return m_netClient != nullptr; }
    EntityId netLocalPlayer() const;
    std::string netStatus() const { return m_netStatus; }
    std::optional<net::NetAddress> netAddress() const;
    std::optional<ViewItem> netView(float aspect);
    Status stop();
    bool playing() const { return m_world->playing(); }
    // Pause keeps play mode (and its state) but stops gameplay ticking; step() runs one fixed frame while paused.
    void setPaused(bool paused) { m_paused = paused && playing(); }
    bool paused() const { return m_paused && playing(); }
    int64_t playFrames() const { return m_playFrames; }        // fixed 1/60 s frames run since Play started
    bool playSnapshotTaken() const { return m_snapshot.valid(); }  // the pre-play scene is held (restored on stop)
    void step(float dt = 1.0f / 60.0f) { if (paused()) m_stepQueued += dt; }
    void tick(float dt);  // gameplay runs in fixed 1/60 s steps (deterministic, recorded by the Time Machine)
    TimeMachine timeMachine;
    // Edit-in-Play with Keep: property tweaks made while playing (latest value per entity/component/property).
    // Stop restores the scene as usual; lastPlayTweaks() then lists them so the user can keep a selection.
    struct PlayTweak {
        EntityId entity = kNullEntity;
        std::string entityName, component, property;
        Json before, after;  // before = value when first tweaked during play
    };
    const std::vector<PlayTweak>& lastPlayTweaks() const { return m_lastTweaks; }
    void clearPlayTweaks() { m_lastTweaks.clear(); }
    Status keepPlayTweaks(const std::vector<size_t>& indices);  // one undoable step
    script::InputFrame liveInput;  // the editor's current input for the game (keyboard / mouse over the viewport)
    // MCP play {exclusiveInput: true} (1.3.0): the editor stops feeding its live keyboard / mouse into the game until Stop, so
    // only scripted input (simulateFrames) drives a scripted run. Cleared by stop().
    bool scriptedInputOnly = false;
    // 1.3.2 MCP scene edits during Play (docs/MCP.md "Edits while playing"): a tool call with target "edit" changes the running
    // play world now and is queued here; stop() replays the queue on the restored edit scene (one AI undo step per call) through
    // replayDeferredEdits (set by registerEngineTools) and keeps the report ({"applied", "failed": [{tool, error}]}) for the
    // stop tool. Cleared by play().
    struct DeferredEdit {
        std::string tool;
        Json args;
    };
    std::vector<DeferredEdit> deferredEdits;
    std::function<Json(const std::vector<DeferredEdit>&)> replayDeferredEdits;
    Json lastDeferredReport;  // null when the last stop() had nothing to replay
    // The project's "input" block (MCP set_input_map, 1.3.0); save = also write the .fledge. Does not touch the live map.
    Status setProjectInput(const Json& input, bool save);

    // ---- undoable scene ops (each is one command, or part of an open transaction)
    EntityId createEntity(const std::string& name, EntityId parent = kNullEntity, const Json& components = Json::object());
    Status destroyEntities(const std::vector<EntityId>& entities);  // with descendants
    Status rename(EntityId e, const std::string& name);
    Status setProperty(EntityId e, const std::string& component, const std::string& property, const Json& value,
                       const std::string& mergeKey = {});
    Status applyComponents(EntityId e, const Json& components, const std::string& label = "Edit components");  // atomic
    Status addComponent(EntityId e, const std::string& type);
    Status removeComponent(EntityId e, const std::string& type);
    Status setParent(EntityId child, EntityId parent, int index = -1, bool keepWorld = true);
    Status setWorldTransform(EntityId e, const Transform& world, const std::string& mergeKey = {});
    std::vector<EntityId> duplicate(const std::vector<EntityId>& entities);
    // Groups several ops into one undo step. Nestable; the outermost label/source win.
    void beginEdit(const std::string& label, EditSource source = EditSource::User) { m_history->begin(label, source); }
    void endEdit() { m_history->commit(); }
    void cancelEdit() { m_history->cancel(); }
    bool undo();
    bool redo();

    // ---- selection (UI state, not part of history)
    std::vector<EntityId> selection;
    EntityId primarySelection() const { return selection.empty() ? kNullEntity : selection.back(); }
    void select(EntityId e, bool additive = false);
    bool isSelected(EntityId e) const;
    void pruneSelection();

    // ---- assets (editor loads SOURCE assets directly; the runtime loads cooked data)
    Result<ImportResult> importAsset(const fs::path& source, const ModelImportOptions& options = {});  // OBJ, images, glTF, FBX; not undoable (writes files)
    Result<ImportResult> reimportAsset(const std::string& ref, std::optional<bool> fixZeroWeights = std::nullopt,
                                       std::optional<bool> fixZeroNormals = std::nullopt);
    // Creates an entity (Mesh) or an entity tree (Model) from an asset; undoable.
    Result<EntityId> instantiateAsset(const std::string& ref, EntityId parent = kNullEntity, Vec3 position = {0, 0, 0});
    // Asset store downloads (.unitypackage, Fab .zip, FBX / glTF / OBJ ...) into <storeImport.destination>/<title>/,
    // with provenance in every .meta and an <title>.fimport report (fledge/importers/store_import.h). Not undoable
    // through the history; imports().undo() removes what an import wrote.
    Result<store::ImportReport> importStoreFile(const fs::path& source, store::ImportOptions options = {});
    editor::StoreImportSettings storeImport;  // per user (editor_settings.json "storeImport"), all off by default
    editor::ImportQueue& imports() { return *m_imports; }
    // GUI host can start a queued store import without blocking the editor thread.
    std::function<void(int)> startBackgroundImport;
    std::function<Result<int>(const fs::path&, const ModelImportOptions&, bool)> startBackgroundAssetImport;
    std::function<Json(int)> backgroundAssetImportStatus;
    // Materials: files are part of the undo history (file snapshots).
    Result<AssetRecord> createMaterial(const std::string& name, const Json& properties = Json::object());
    Status setMaterialProperty(const std::string& ref, const std::string& property, const Json& value, const std::string& mergeKey = {});
    Status assignMaterial(EntityId e, const std::string& materialRef);
    std::optional<Material> loadMaterial(const std::string& ref);
    Result<CookReport> cook(bool force = false);
    // Writes world().renderSettings into the project's "render" block (per-project defaults).
    Status saveRenderSettingsToProject();
    // Undoable project graphics change: "graphics" (+ optionally "render") blocks of the .fledge
    // are snapshotted as a file, applied to the live world and saved. Used by the Graphics panel,
    // the palette commands and the MCP graphics tools (source = AI -> tagged [AI] in the history).
    Status setGraphicsSettings(const GraphicsSettings& graphics, const RenderSettings* render, const std::string& label,
                               EditSource source = EditSource::User);
    Result<PackageResult> package(bool sidecar = false);
    Status setBootSplash(bool enabled);
    // Writes the project manifest. Keeps the current file path. gameName must be a safe file name.
    Status commitProject(const Project& next, const std::string& label);

    // Live reload: start watching Content/ (native watcher, polling fallback) and
    // process debounced changes; returns the content paths that were reloaded.
    void startWatching(bool preferNative = true);
    const char* watcherBackend() const { return m_watcher ? m_watcher->backend() : "off"; }
    std::vector<std::string> pollAssetChanges();
    // Re-scan the registry and drop cached data for the given content-relative paths (empty = all).
    void refreshAssets(const std::vector<std::string>& changedRelPaths);
    void invalidateChangedAssets(const std::vector<std::string>& changedRelPaths);
    uint64_t assetGeneration() const { return m_assetGeneration; }  // bumps on every refresh (UI caches)

    // ---- Fledge Script files (Scripts/ is mounted as "scripts:/" so script edits can be undone as file snapshots)
    struct ScriptSettings {
        fs::path compiler;           // empty = auto (FLEDGE_CXX, PATH, common MinGW folders)
        bool autoBuild = true;       // editor: rebuild + hot reload when Scripts/ changes
    } scriptSettings;
    ScriptBuildOptions scriptBuildOptions() const;
    // Synchronous build of the open project's scripts; on success the module is (hot) loaded.
    ScriptBuildResult buildScripts(bool force = false);
    // Applies a finished build (from a background thread): records it and loads the module on success.
    void applyScriptBuild(const ScriptBuildResult& result);
    const ScriptBuildResult& lastScriptBuild() const { return m_lastBuild; }
    bool hasScriptBuild() const { return m_hasBuild; }
    Status loadScriptModule(const fs::path& dll);
    const script::ModuleInfo& scriptModule() { return m_world->scripts().module(); }
    // Script files written by AI tools since the last user-started build (review before building when
    // AI builds are not allowed; the editor then skips automatic rebuilds).
    std::set<std::string> aiTouchedScripts;
    Status writeScriptFile(const std::string& relPath, const std::string& content, bool fromAi);  // sandboxed, undoable

    // Behaviours ("Behaviours" component entries). Edit mode: undoable component edits.
    // Play mode: also applied to the live script object.
    Status attachBehaviour(EntityId e, const std::string& cls, const Json& properties = Json::object());
    Status detachBehaviour(EntityId e, const std::string& cls);
    Status setBehaviourProperty(EntityId e, const std::string& cls, const std::string& prop, const Json& value,
                                const std::string& mergeKey = {});
    Status setBehaviourEnabled(EntityId e, const std::string& cls, bool enabled);
    // Class defaults overlaid with the entity's overrides (live values while playing).
    Result<Json> behaviourProperties(EntityId e, const std::string& cls);
    // Ticks play mode N fixed frames (headless MCP: play + simulate).
    // Exactly "frames" fixed 1/60 s steps through the same path as real-time Play (stepOnce: scripts, services, timers,
    // coroutines, deferred spawns, physics, gameplay ...), also while paused. feed(i, input) supplies the input of frame i;
    // without it the current liveInput is used. liveInput is cleared afterwards when fed.
    using InputFeed = std::function<void(int frame, script::InputFrame& input)>;
    void simulateFrames(int frames, const InputFeed& feed = {});

    // AI master switch + control level (per-user; see ai_control.h). Every AI tool call goes through it.
    AiController ai;
    // Upscaler downloads requested by AI tools: the editor turns each into a licence dialog for the
    // human user (the AI can never accept a vendor licence). Headless servers have no licence UI.
    std::vector<std::string> upscalerInstallRequests;
    bool hasLicenseUi = false;

    // Settings migration (settings_migration.h, MCP migrate_settings): the host's --settings-dir when one was given (empty = the
    // resolved user data folder), and a hook the editor uses to reload its in-memory settings after a migration was applied.
    fs::path customSettingsDir;
    std::function<void()> onUserSettingsMigrated;

    // Full MCP Control (editor_control.h): the editor GUI's host for the editorControl tools (null in headless servers), the audit
    // log every AI tool call is written to (null = none), and who is calling right now ("mcp-http:<session>", "assistant", ...).
    editor::EditorControlHost* controlHost = nullptr;
    std::shared_ptr<editor::McpAuditLog> auditLog;
    std::string aiCaller = "mcp";
    std::shared_ptr<editor::RateLimiter> editorControlLimiter;  // created on first use (kRateLimitCalls per kRateLimitSeconds)

    // Tools directory containing fledge_runtime.exe and gameplay module DLLs
    // (defaults to the directory of the running executable).
    fs::path toolsDir;
    bool dirty = false;

    // Viewport capture hook provided by a GPU-backed host (editor). Headless
    // hosts leave it empty and take_screenshot reports that it is unavailable.
    std::function<Result<ImageData>(int width, int height)> screenshotProvider;

private:
    std::function<void(const std::string& stage, float fraction)> m_loadProgress;
    void reportLoad(const std::string& stage, float fraction) { if (m_loadProgress) m_loadProgress(stage, fraction); }
    std::optional<Project> m_project;
    std::optional<EditionCheck> m_decline;
    Vfs m_vfs;
    AssetRegistry m_registry;
    std::unique_ptr<GameWorld> m_world;
    std::unique_ptr<CommandStack> m_history;
    SceneSnapshot m_snapshot;
    std::shared_ptr<MemoryProvider> m_scratch;  // content:/ while no project is open
    std::unique_ptr<FileWatcher> m_watcher;
    std::unique_ptr<editor::ImportQueue> m_imports;
    int m_importSerial = 0;
    std::vector<std::string> m_pendingChanges;
    std::chrono::steady_clock::time_point m_lastChange;
    uint64_t m_assetGeneration = 0;
    ScriptBuildResult m_lastBuild;
    bool m_hasBuild = false;
    editor::AutosaveScheduler m_autosaveScheduler;
    std::optional<editor::RecoveryOffer> m_recoveryOffer;
    uint64_t m_changeSerial = 0;
    bool m_keepRecoveryOnClose = false;  // "Open saved version": the snapshots survive a clean close until the next save
    bool m_realSaveSeen = false;         // the host passed allowRealSave (editor GUI)
    int64_t m_lastSnapshotMs = 0;
    std::optional<uint64_t> m_lastSnapshotSerial;
    std::optional<editor::RecoveryStore> recoveryStore() const;
    void markDirty();
    Status saveSceneImpl(const std::string& vpath, bool manual);
    Status adoptSceneJson(const Json& scene, const std::string& scenePath);
    void computeRecoveryOffer();
    std::string activeScenePath() const;
    fs::path autoSaveBackupFile(const std::string& scenePath) const;
    void stopNet();
    void tickNet();
    net::NetInput liveNetInput();
    std::unique_ptr<net::NetServer> m_netServer;
    std::unique_ptr<net::NetClient> m_netClient;
    float m_netYaw = 0;
    double m_netNow = 0;
    std::string m_netStatus;
    bool m_paused = false;
    int64_t m_playFrames = 0;
    float m_stepQueued = 0;
    float m_accum = 0;
    std::vector<PlayTweak> m_tweaks, m_lastTweaks;
    void stepOnce();
    void installAssetHooks();
};

namespace mcp { class ToolRegistry; class McpServer; struct Tool; }

// Registers the engine tool catalog (see docs/MCP.md) against a session. Every
// mutating tool call runs inside one history transaction tagged EditSource::AI.
void registerEngineTools(mcp::ToolRegistry& registry, EditorSession& session);
// How the AI control gate sees a call: the tool itself, or a read-only / destructive view of it when the permission depends on
// the arguments (convert_blueprint and migrate_settings dry runs are reads, migrate_settings apply is destructive). The editor's
// approval prompt uses the same view.
mcp::Tool aiAuthorizationView(const mcp::Tool& tool, const Json& args);
// Registers fledge:// resources (project, scene, assets, log, history).
void registerEngineResources(mcp::McpServer& server, EditorSession& session);

} // namespace fledge
