#pragma once
// GameWorld: the piece shared by the editor and the shipping runtime.
// Owns the Scene, runs built-in systems + gameplay modules, and turns the
// scene into renderer draw lists (with a GPU resource cache keyed by asset ref).
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

#include "fledge/assets/asset_registry.h"
#include "fledge/assets/material.h"
#include "fledge/framework/asset_loader.h"
#include "fledge/core/vfs.h"
#include "fledge/render/renderer.h"
#include "fledge/scene/components.h"
#include "fledge/scene/scene.h"
#include "fledge/script/script_host.h"
#include "fledge/script/module_host.h"
#include "fledge/framework/anim_system.h"
#include "fledge/physics/physics.h"
#include "fledge/audio/audio.h"
#include "fledge/world/streaming.h"
#include "fledge/world/terrain.h"
#include "fledge/world/roads.h"

#include "fledge/framework/project.h"
#include "fledge/framework/world_system.h"

namespace fledge::gameplay {
class TrafficSim;
class NavMesh;
class Localization;
class Font;
} // namespace fledge::gameplay

namespace fledge {

class GameWorld {
public:
    GameWorld(Vfs& vfs, AssetRegistry& registry);
    ~GameWorld();

    Scene& scene() { return m_scene; }
    ModuleHost& modules() { return m_modules; }  // legacy C-ABI gameplay modules
    script::ScriptHost& scripts() { return m_script; }  // Fledge Script C++ scripts
    Vfs& vfs() { return m_vfs; }
    AssetRegistry& registry() { return m_registry; }

    Status loadScene(const std::string& vpath);
    Status saveScene(const std::string& vpath);
    const std::string& currentScenePath() const { return m_scenePath; }

    void beginPlay();
    void endPlay();
    bool playing() const { return m_playing; }
    // Runs systems. Rotator, modules and Fledge Script files only tick while playing.
    void tick(float dt);
    // Ray query (Fledge Script Scene::Raycast): exact physics colliders while the simulation runs, plus mesh bounds and
    // terrains for everything without a collider.
    std::optional<script::RaycastHit> raycast(Vec3 origin, Vec3 dir, float maxDistance);
    // 1.3.2 filtered queries (Fledge Script Physics::Raycast / SphereCast, MCP raycast; docs/PHYSICS.md "Collision layers and
    // query filters"): `ignore` (and with ignoreHierarchy its children, e.g. the player and its held weapon) is never hit,
    // layerMask selects Collider.layer bits (meshes without a collider count as layer 0), triggers are never hit. Physics
    // colliders while the simulation runs, plus mesh bounds of entities without a collider (rays only).
    struct QueryFilter {
        EntityId ignore = kNullEntity;
        bool ignoreHierarchy = true;
        uint32_t layerMask = 0xFFFFFFFFu;
    };
    std::optional<script::RaycastHit> raycast(Vec3 origin, Vec3 dir, float maxDistance, const QueryFilter& filter);
    std::optional<script::RaycastHit> sphereCast(Vec3 origin, float radius, Vec3 dir, float maxDistance, const QueryFilter& filter);

    void setRenderer(ForwardRenderer* renderer);  // nullptr releases GPU resources
    ForwardRenderer* renderer() const { return m_renderer; }
    // 1.3.1: where the Animator on e is skinned and why: {"path": "gpu"|"cpu"|"none", "reason", "bones", "gpuLimit",
    // "editionLimit", "edition", "backend", "note"} (get_animation_info, describe_edition, the Inspector).
    Json skinningInfo(EntityId e);
    // Camera from the primary Camera entity; nullopt if none.
    std::optional<ViewItem> primaryView(float aspect);
    // selection entities are tinted and get a bounds outline (editor).
    void render(rhi::RenderTargetHandle target, int width, int height, const ViewItem& view,
                const std::vector<EntityId>& selection = {}, bool drawGrid = false);
    // CPU mesh for a mesh ref (builtin or asset); cached. nullptr if unavailable.
    const MeshData* cpuMesh(const std::string& ref);
    // Thumbnail: renders one mesh (+ optional material) framed by its bounds (editor content browser).
    void renderAssetPreview(rhi::RenderTargetHandle target, int size, const std::string& meshRef, const std::string& materialRef);
    // Material for a ref (GUID or content path); cached. nullptr if unavailable.
    const Material* material(const std::string& ref);
    // Uploads the meshes and material textures the open scene references, so the first viewport frame is not the load.
    void preloadSceneAssets(const std::function<void(int done, int total, const std::string& label)>& progress = {});

    // ---- open world (docs/WORLD_STREAMING.md)
    // Streaming runs while playing when the scene has a WorldPartition component (in-memory chunks in the
    // editor, cooked .world chunks from the pak in packaged games).
    world::StreamingWorld* streaming() { return m_streaming.get(); }
    // Viewer used for streaming / terrain LOD when the scene has no primary camera (editor fly camera).
    std::optional<Vec3> viewerOverride;
    // Floating-origin shift applied during the last tick (callers move their own cameras by -delta).
    Vec3 lastRebase() const { return m_lastRebase; }
    // Terrain under a world-space x/z (top-most terrain), if any.
    std::optional<float> terrainHeight(float x, float z);
    const world::Heightfield* heightfield(EntityId terrainEntity);
    // Drops cached procedural meshes/textures of an entity (terrain edits, spline edits).
    void invalidateProcedural(EntityId e);
    struct OpenWorldStats {
        int terrainTiles = 0, roadMeshes = 0, foliageInstances = 0, culled = 0;
    };
    const OpenWorldStats& openWorldStats() const { return m_owStats; }

    // ---- animation (docs/ANIMATION.md)
    AnimationSystem& animation() { return m_anim; }
    // Advances Animator components (tick() does this while playing; the editor calls it for previews).
    void animate(float dt);
    // While playing: applies the pending time of Animators that animation LOD skipped (AnimationSystem::catchUp), so reads
    // right after a batch of simulated frames (MCP play + simulateFrames) see the current state / pose.
    void catchUpAnimation();

    // ---- physics (docs/PHYSICS.md)
    // Exists while playing (or simulating in the editor) when the scene has any physics component, a terrain or a road.
    physics::PhysicsWorld* physics() { return m_physics.get(); }
    float physicsStep = 1.0f / 60.0f;  // fixed step (s); tick() runs as many steps as the frame needs (max 4)
    bool physicsDebug = false;         // draw collider bounds (editor / F-key in runtime)
    // Editor "Simulate": physics only (no scripts), start / stop restore nothing by themselves (the editor snapshots).
    void startPhysics();
    void stopPhysics();
    // Advances physics by dt in fixed steps (tick() calls it while playing; the editor's Simulate mode calls it).
    void stepPhysics(float dt);
    // Time Machine: exact simulation state (empty when no physics is running).
    Bytes savePhysicsState() const;
    bool restorePhysicsState(const Bytes& state);
    static bool sceneNeedsPhysics(Scene& scene);

    // ---- audio (docs/AUDIO.md)
    // The app opens the real output device (editor / runtime); everything else (tests, headless tools) gets the
    // null device on first use, which renders into a scratch buffer so playback time still passes.
    void openAudio(bool nullDevice);
    audio::Mixer& mixer() { return m_mixer; }
    audio::AudioDevice* audioDevice() { return m_audioDevice.get(); }
    // Starts / stops / moves AudioSource voices, vehicle engine sounds, listener, mixer settings (tick() calls it
    // while playing; the editor calls it every frame for previews).
    void updateAudio(float dt);
    // Editor preview of a clip on the UI bus (non-spatial); returns false if the clip cannot be loaded.
    bool previewClip(const std::string& ref);
    void stopPreview();
    std::shared_ptr<const audio::Clip> audioClip(const std::string& ref);
    struct AudioStats {
        int sources = 0, voices = 0, streams = 0, engines = 0;
        int occluded = 0;        // AudioSources muffled by walls
        float reverbZone = 0;    // 0..1 how deep the listener is inside a ReverbZone (0 = no reverb)
    };
    const AudioStats& audioStats() const { return m_audioStats; }

    // ---- gameplay (docs/GAMEPLAY.md): player controller, traffic, police / wanted, missions, pickups, time of
    // day, weather, cameras, sequences, HUD, save / load. Runs while playing.
    struct GameplayStats {
        int trafficCars = 0, pedestrians = 0, police = 0, missionsRunning = 0, wanted = 0;
        int policeCars = 0, shotsAtPlayer = 0, hitsOnPlayer = 0;
        int particles = 0, rainDrops = 0;
        float fogDensity = 0;  // height fog in use (weather + project), 0 = none
        float hour = -1, speedKmh = 0;
        std::string weather, playerVehicle, objective;
        bool navMesh = false;
        double ms = 0;
    };
    GameplayStats gameplayStats() const;
    EntityId player();  // first entity with a PlayerController
    int missionNode(EntityId missionEntity) const;  // current node id of a running mission (0 = none)
    bool navDebug = false;                          // draw the navmesh (when built) while playing
    void commitCrime(int crime);  // gameplay::Crime
    const world::RoadGraph* roadGraph();
    gameplay::TrafficSim* traffic();
    gameplay::NavMesh* navMesh();  // built on first use from static colliders, terrains and roads
    void showMessage(const std::string& text, float seconds = 3, Vec3 color = {1, 1, 1});
    // Save / load the play state (player, inventory, wanted, missions, time of day, vehicles) as JSON.
    Status saveGame(const fs::path& file);
    Status loadGame(const fs::path& file);
    fs::path saveDirectory;  // quick save (QuickSave, F8) / quick load (QuickLoad, F9) use <saveDirectory>/quicksave.json
    // The HUD triangles for a frame (render() draws them; tests inspect them).
    std::vector<ForwardRenderer::OverlayVertex> hudOverlay(int width, int height);
    // The particles of a frame while playing: ParticleEmitter components + weather rain (render() draws them).
    std::vector<ParticleItem> particleItems();

    // ---- game UI (docs/GAMEPLAY.md "UI"): TrueType font, localisation, retained UiScreen documents
    Project::UiSettings uiSettings;  // from the project ("ui" block)
    struct UiPointer {
        Vec2 pos{-1, -1};  // pixels in the game view; negative = no pointer
        bool down = false; // primary button
    } uiPointer;           // set by the app every frame (runtime: the window cursor)
    // True while a visible, interactive UiScreen is up (a menu): apps free the mouse cursor instead of locking it.
    bool wantsCursor();
    // String tables (Content/Localization/<lang>.json), loaded at beginPlay; language from uiSettings.
    gameplay::Localization& localization();
    void reloadLocalization();
    // The UI font. Empty uiSettings.font loads bundled Roboto from resources/fonts; nullptr = the pixel font fallback.
    const gameplay::Font* uiFont();
    // Live HUD / weapon snapshot for FHTML (`fledge.state`) and native bindings. Compact JSON.
    std::string uiStateJson();

    // ---- LOD & culling (docs/WORLD_STREAMING.md "LOD & culling")
    bool frustumCulling = true, occlusionCulling = true;
    float drawDistance = 0;  // global distance culling (m), 0 = off
    float lodBias = 0;       // global LOD bias (Make it run well raises it)
    struct RenderStats {
        int candidates = 0, drawn = 0, frustumCulled = 0, distanceCulled = 0, occlusionCulled = 0, occluders = 0, hlodDrawn = 0;
        int shadowProxies = 0;  // off-camera characters drawn as a box into the shadow map instead of the skinned mesh
        int lodHistogram[5] = {};
        double cullMs = 0;
    };
    const RenderStats& renderStats() const { return m_rstats; }
    // CPU time per system, summed since the last reset (ms). tick() and render() fill it (fledge_runtime --profile).
    struct SystemTimes {
        double modules = 0, scripts = 0, gameplay = 0, physics = 0, animation = 0, lateGameplay = 0, streaming = 0, audio = 0;
        double worldMatrices = 0, drawItems = 0, renderer = 0;  // drawItems = all of the following + selection + lights
        double openWorldItems = 0, meshItems = 0, animatedItems = 0, hud = 0;
        int ticks = 0, renders = 0;
    };
    const SystemTimes& systemTimes() const { return m_times; }
    void resetSystemTimes() { m_times = {}; }

    // Rendering settings (project defaults; the editor's Render Settings panel edits them).
    RenderSettings renderSettings;
    GraphicsSettings graphicsSettings;  // upscaler, ray tracing, HDR ... (project "graphics")
    bool pathTraceReference = false;    // editor: progressive path-traced reference view

    // ---- 1.3.2 projected decals (Fledge Advanced; docs/GRAPHICS.md "Decals"). Decal components are rebuilt when they move;
    // spawnDecal (ballistics impacts) uses a pool of maxDecals runtime decals: the oldest is recycled when it is full. Each
    // decal is clipped once onto the opaque MeshRenderer geometry in its box (pieces in the receivers' own space, so a decal on
    // a moving crate moves with it); drawing allocates nothing per frame.
    struct DecalSpawn {
        Vec3 position, normal{0, 1, 0};  // surface point and its normal: the decal projects along -normal
        float rotation = 0;              // radians about the normal
        Vec3 size{0.1f, 0.1f, 0.1f};     // width, height, projection depth (m)
        std::string material, texture = "builtin:decal_bullet";
        Vec3 color{1, 1, 1};
        float opacity = 1, angleFade = 70, lifetime = 30, fadeOut = 2;  // lifetime 0 = until recycled
        int sortOrder = 0;
        uint32_t layerMask = 0xFFFFFFFFu;
        EntityId only = kNullEntity;     // project onto this entity only (the hit surface); kNullEntity = every receiver in the box
    };
    bool decalsAvailable() const;  // Fledge Advanced at run time + a renderer with the decal shader (full pipeline)
    int spawnDecal(const DecalSpawn& d);  // pool slot, or -1 (not available here / nothing to project on)
    void clearDecals();
    int maxDecals = 256;  // runtime pool size (Ballistics.decalPool sets it for impacts)
    struct DecalStats {
        int active = 0, components = 0, pieces = 0, triangles = 0, spawned = 0, recycled = 0, rebuilt = 0;
    };
    const DecalStats& decalStats() const { return m_decalStats; }
    // The clipping step (unit-tested): receiver triangles moved into decal space by toDecal, clipped to the box of half size
    // h, faded by angleFade; out = receiver-local vertices (tangent.w = fade, uv over the box) + indices.
    static void clipDecalMesh(const MeshData& receiver, const Mat4& toDecal, Vec3 h, float angleFade, MeshData& out);

    // ---- world systems of linked modules (world_system.h; Fledge Advanced only, none in Fledge)
    // The live instance registered under `name` while playing, nullptr otherwise.
    WorldSystem* system(std::string_view name);

    // Default: CookedAssetLoader (runtime). The editor installs a source loader.
    void setAssetLoader(std::shared_ptr<AssetLoader> loader);
    // Live reload: drop cached CPU/GPU data for one content path ("content:/..."), or everything.
    void invalidateAsset(const std::string& vpath);
    void invalidateAll();

private:
    std::optional<script::RaycastHit> raycastImpl(Vec3 origin, Vec3 dir, float maxDistance, const QueryFilter* filter);
    const std::vector<EntityId>& queryIgnoreList(const QueryFilter& filter);  // ignore + descendants (reused buffer)
    std::vector<EntityId> m_queryIgnore;
    rhi::MeshHandle meshFor(const std::string& ref);
    rhi::TextureHandle uploadTexture(const TextureData& tex);
    void applyMaterial(DrawItem& item, const std::string& materialRef);
    void updateEnvironment();  // (re)loads renderSettings.environment into the renderer when it changes
    std::optional<std::string> m_loadedEnvironment;
    rhi::TextureHandle textureFor(const std::string& ref);
    void releaseGpu();
    // open world (game_world_open.cpp)
    void addOpenWorldItems(std::vector<DrawItem>& items, const std::unordered_map<EntityId, Mat4>& worlds, Vec3 viewPos);
    void updateStreaming(float dt);
    std::optional<Vec3> streamingViewer();
    rhi::MeshHandle procMesh(const std::string& ref, uint64_t hash, const std::function<MeshData()>& build);
    void syncAnimationGpuPath();  // GPU skin flags before animate(), so edit mode does not CPU-skin 190k-vert characters
    void addMeshItems(std::vector<DrawItem>& items, std::unordered_map<EntityId, Mat4>& worlds, const ViewItem& view,
                      const std::vector<EntityId>& selection, const std::vector<Mat4>& shadowVolumes);
    rhi::MeshHandle lodMesh(const std::string& ref, int lod);
    MeshData* lodChain(const std::string& ref);  // cpu mesh with LODs generated on demand
    void buildHlods();
    void addHlodItems(std::vector<DrawItem>& items, Vec3 viewPos);
    struct Hlod {
        MeshData mesh;
        Vec3 color{0.6f, 0.6f, 0.6f};
        Vec3 centre;
        float radius = 0;
    };
    std::map<world::ChunkCoord, Hlod> m_hlods;
    std::unordered_map<EntityId, int> m_lodState;
    uint64_t m_giKey = ~0ull;
    std::vector<std::pair<const MeshData*, Mat4>> m_frameOccluders;
    RenderStats m_rstats;
    SystemTimes m_times;
    std::unique_ptr<physics::PhysicsWorld> m_physics;
    std::vector<std::pair<std::string, std::unique_ptr<WorldSystem>>> m_systems;  // world_system.h (empty in Fledge)
    std::unique_ptr<gameplay::Localization> m_loc;
    std::shared_ptr<gameplay::Font> m_uiFont;
    std::string m_uiFontKey;  // font path + size it was loaded with ("" = not tried)
    rhi::TextureHandle m_uiFontTexture;
    struct GameplayState;
    std::unique_ptr<GameplayState> m_gp;
    void beginGameplay();
    void endGameplay();
    void updateGameplay(float dt);  // before physics: player input, traffic, police, pickups
    void lateGameplay(float dt);    // after physics + animation: camera, missions, wanted, time of day, weather
    void addHud(int width, int height);
    // Police sight: nothing solid between `from` and `to` (hits on `target` or its vehicle count as clear).
    bool lineOfSight(Vec3 from, Vec3 to, EntityId ignore, EntityId target, EntityId targetVehicle);
    void updatePoliceCars(float dt, int wantedCars, EntityId player, EntityId playerCar);
    audio::Mixer m_mixer{48000};
    std::unique_ptr<audio::AudioDevice> m_audioDevice;
    struct SourceState {
        audio::VoiceId voice = 0;
        bool started = false;
        Vec3 lastPos;
        bool hasLast = false;
        std::shared_ptr<audio::EngineSynth> engine;
        float occlusion = 0, occlusionTimer = 0;  // walls between the listener and the source (re-cast every 0.1 s)
    };
    // 0 (clear), 0.6 (one obstacle) or 0.9 (two or more) between the listener and a source, from physics rays.
    float occlusionBetween(Vec3 ears, Vec3 source, EntityId sourceEntity, EntityId alsoClear);
    std::unordered_map<EntityId, SourceState> m_sources, m_engines;
    std::unordered_map<std::string, std::shared_ptr<const audio::Clip>> m_clips;
    audio::VoiceId m_preview = 0;
    std::optional<Vec3> m_lastListener;
    AudioStats m_audioStats;
    void stopSceneAudio();
    float m_physicsAccum = 0;
    void addPhysicsDebug(const std::vector<EntityId>& selection, std::unordered_map<EntityId, Mat4>& worlds);
    // Constructed from the constructor's `vfs` argument, never from m_vfs: m_vfs is declared further down, so a
    // default initialiser `{m_vfs}` bound an uninitialised reference (1.1.0 crash on any imported .fchar).
    AnimationSystem m_anim;
    std::unordered_map<std::string, rhi::TextureHandle> m_skinTextures;
    uint64_t m_skinGeneration = 0;  // AnimationSystem::characterGeneration() the bind meshes / skin textures were built for
    // 1.3.2 multi-material characters (Fledge Advanced): per material slot the vertices it uses and its remapped indices
    struct CharSlots {
        std::vector<std::vector<uint32_t>> verts, indices;
    };
    std::unordered_map<const anim::Character*, CharSlots> m_charSlots;
    const CharSlots* characterSlots(const anim::Character& ch);  // nullptr: draw the whole mesh as one item
    std::set<std::string> m_multiMaterialWarned;
    // 1.3.2 decals (game_world_decals.cpp)
    struct DecalPiece {
        EntityId receiver = kNullEntity;
        rhi::MeshHandle mesh;
        int triangles = 0;
    };
    struct DecalInst {
        bool active = false;
        uint64_t seq = 0, hash = 0;
        EntityId entity = kNullEntity;  // Decal component (kNullEntity = a pooled runtime decal)
        std::vector<DecalPiece> pieces;
        std::string material, texture;
        Vec3 color{1, 1, 1};
        float opacity = 1, age = 0, lifetime = 0, fadeOut = 0;
        int sortOrder = 0;
    };
    std::vector<DecalInst> m_decalPool;
    std::unordered_map<EntityId, DecalInst> m_decalComponents;
    uint64_t m_decalSeq = 0;
    size_t m_decalNext = 0;
    DecalStats m_decalStats;
    bool m_decalWarned = false;
    rhi::TextureHandle m_decalTex[2];  // builtin:decal_bullet, builtin:decal_blood (procedural)
    MeshData m_decalScratch;           // reused while clipping (capacity kept)
    bool buildDecal(DecalInst& d, const Mat4& decalWorld, Vec3 size, float angleFade, uint32_t layerMask, EntityId only,
                    const std::unordered_map<EntityId, Mat4>& worlds);
    void releaseDecal(DecalInst& d);
    void updateDecals(float dt);
    void addDecalItems(std::vector<DrawItem>& items, std::unordered_map<EntityId, Mat4>& worlds);
    rhi::TextureHandle decalTexture(const std::string& ref);
    void addAnimatedItems(std::vector<DrawItem>& items, std::unordered_map<EntityId, Mat4>& worlds, const ViewItem& view,
                          const std::vector<EntityId>& selection, const std::vector<Mat4>& shadowVolumes);
    // 1.3.1 per-mesh shadows: the Camera entity the current render() views from (ViewItem::camera, 0 = editor free camera),
    // and whether a mesh with shadowOnly / ownerCamera is shadow-only in that view.
    EntityId m_viewCamera = kNullEntity;
    bool m_viewViewmodel = false;  // 1.3.2: the current view has a viewmodel pass (game camera)
    bool shadowOnlyInView(bool shadowOnly, const std::string& ownerCamera) const;
    struct TerrainCache {
        uint64_t hash = 0;
        world::Heightfield field;
        rhi::TextureHandle albedo;
        std::map<std::pair<int, int>, int> lods;
    };
    std::unordered_map<EntityId, TerrainCache> m_terrains;
    std::unordered_map<std::string, uint64_t> m_procHash;
    std::unordered_map<EntityId, std::pair<uint64_t, std::vector<Mat4>>> m_foliage;
    struct RoadKey {  // per-frame change detection by value (no JSON hashing unless something changed)
        SplineComponent spline;
        uint64_t hash = 0;
        std::string refs[3];
        bool valid = false;
    };
    std::unordered_map<EntityId, RoadKey> m_roadKeys;
    const world::RoadGraph* m_minimapGraph = nullptr;
    size_t m_minimapEdges = 0, m_minimapPoints = 0;
    std::vector<std::vector<Vec3>> m_minimapRoads;
    struct FoliageKey {
        FoliageComponent foliage;
        Mat4 world;
        uint64_t terrainHash = 0, hash = 0;
        bool valid = false;
    };
    std::unordered_map<EntityId, FoliageKey> m_foliageKeys;
    std::unique_ptr<JobSystem> m_jobs;
    std::unique_ptr<world::StreamingWorld> m_streaming;
    Json m_cookedWorldIndex;  // set by loadScene for cooked open worlds
    std::string m_cookedWorldDir;
    std::optional<Vec3> m_prevViewer;
    Vec3 m_lastRebase{0, 0, 0};
    OpenWorldStats m_owStats;

    Vfs& m_vfs;
    AssetRegistry& m_registry;
    Scene m_scene;
    ModuleHost m_modules;
    ForwardRenderer* m_renderer = nullptr;
    std::unordered_map<std::string, rhi::MeshHandle> m_meshCache;
    std::vector<rhi::MeshHandle> m_clothFrameMeshes;
    std::unordered_map<std::string, rhi::TextureHandle> m_texCache;
    std::unordered_map<std::string, std::unique_ptr<MeshData>> m_cpuMeshes;
    std::unordered_map<std::string, std::unique_ptr<Material>> m_materials;
    std::shared_ptr<AssetLoader> m_loader;
    std::string m_scenePath;
    bool m_playing = false;
    script::ScriptHost m_script;  // declared after m_scene: destroyed first (scripts may touch the scene on shutdown)
};

} // namespace fledge
