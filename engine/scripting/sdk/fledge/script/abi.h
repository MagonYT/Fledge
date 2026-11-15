/* Fledge Script - Fledge native C++ scripting. C ABI between the engine (host) and a
 * compiled script module (DLL).
 *
 * Only plain C types cross this boundary, so script modules can be built by a
 * different compiler than the engine (MinGW g++ today; MSVC / clang later).
 * Script authors never use this header directly: <fledge/script/script.h> wraps it.
 *
 * A script module exports:   const FScriptModuleApi* fledge_script_module_entry(void);
 * The engine rejects modules whose abiVersion differs from FSCRIPT_ABI_VERSION or
 * whose structSize is smaller than expected.
 *
 * Strings returned by host callbacks are owned by the host and stay valid until
 * the next host call on the same thread; copy them if you need to keep them.
 * Strings returned by the module (JSON) stay valid until the next module call.
 */
#ifndef FLEDGE_SCRIPT_ABI_H
#define FLEDGE_SCRIPT_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FSCRIPT_ABI_VERSION 1u
#define FSCRIPT_SDK_VERSION "0.9.0"  /* 0.9.0 (Fledge 1.3.2): Animator layers, filtered Physics::Raycast / SphereCast (2 host functions
                                        appended; ABI 1 unchanged: 0.8.x / 0.7.x modules still load, 0.9.0 modules on an older host see
                                        the new calls fail softly) */

#if defined(_WIN32)
#define FSCRIPT_EXPORT __declspec(dllexport)
#else
#define FSCRIPT_EXPORT __attribute__((visibility("default")))
#endif

typedef uint32_t FScriptEntity; /* 0 = none */
typedef uint64_t FScriptInstance; /* opaque handle to a script object, 0 = none */
typedef struct FScriptVec3 { float x, y, z; } FScriptVec3;
typedef struct FScriptQuat { float x, y, z, w; } FScriptQuat;

typedef struct FScriptFrameInfo {
    float delta;        /* scaled seconds since the last frame */
    float unscaledDelta;
    float fixedDelta;   /* fixed-update step (seconds) */
    float timeScale;
    double elapsed;     /* scaled play time */
    uint64_t frame;     /* frame counter since play started */
} FScriptFrameInfo;

typedef struct FScriptRayHit {
    FScriptEntity entity;
    FScriptVec3 point;
    FScriptVec3 normal;
    float distance;
} FScriptRayHit;

enum { FSCRIPT_LOG_DEBUG = 1, FSCRIPT_LOG_INFO = 2, FSCRIPT_LOG_WARN = 3, FSCRIPT_LOG_ERROR = 4 };
enum { FSCRIPT_INPUT_DOWN = 0, FSCRIPT_INPUT_PRESSED = 1, FSCRIPT_INPUT_RELEASED = 2 };
enum { FSCRIPT_SPACE_WORLD = 0, FSCRIPT_SPACE_LOCAL = 1 };

/* Engine -> module. Functions may be NULL when a host does not support them (check before calling). */
typedef struct FScriptHostApi {
    uint32_t abiVersion;
    uint32_t structSize;
    void* host;
    const FScriptFrameInfo* frameInfo;

    void (*log)(void* host, int level, const char* message);

    /* entities */
    int (*entity_valid)(void* host, FScriptEntity e);
    const char* (*entity_name)(void* host, FScriptEntity e);
    void (*set_entity_name)(void* host, FScriptEntity e, const char* name);
    int (*get_transform)(void* host, FScriptEntity e, int space, FScriptVec3* pos, FScriptQuat* rot, FScriptVec3* scale);
    int (*set_position)(void* host, FScriptEntity e, int space, FScriptVec3 value);
    int (*set_rotation)(void* host, FScriptEntity e, int space, FScriptQuat value);
    int (*set_scale)(void* host, FScriptEntity e, FScriptVec3 value);
    FScriptEntity (*parent)(void* host, FScriptEntity e);
    int (*set_parent)(void* host, FScriptEntity child, FScriptEntity parent, int keepWorld);
    int (*children)(void* host, FScriptEntity e, FScriptEntity* out, int capacity); /* returns total count */
    const char* (*tags)(void* host, FScriptEntity e);                             /* space-separated */
    void (*set_tags)(void* host, FScriptEntity e, const char* tags);

    /* components (reflected; values as JSON text) */
    int (*has_component)(void* host, FScriptEntity e, const char* type);
    int (*add_component)(void* host, FScriptEntity e, const char* type);
    int (*remove_component)(void* host, FScriptEntity e, const char* type);
    const char* (*get_component)(void* host, FScriptEntity e, const char* type); /* JSON object or NULL */
    int (*set_component)(void* host, FScriptEntity e, const char* type, const char* json); /* partial JSON object merge */

    /* scene queries */
    FScriptEntity (*find_by_name)(void* host, const char* name);
    int (*find_with_tag)(void* host, const char* tag, FScriptEntity* out, int capacity);
    int (*find_with_component)(void* host, const char* type, FScriptEntity* out, int capacity);
    int (*raycast)(void* host, FScriptVec3 origin, FScriptVec3 direction, float maxDistance, FScriptRayHit* hit);

    /* structural changes: entities appear immediately; behaviour creation / destruction is deferred to the end of the frame */
    FScriptEntity (*create_entity)(void* host, const char* name, FScriptEntity parent);
    void (*destroy_entity)(void* host, FScriptEntity e);
    FScriptEntity (*spawn_scene)(void* host, const char* scenePath, FScriptVec3 position, FScriptQuat rotation);
    int (*add_behaviour)(void* host, FScriptEntity e, const char* className, const char* propertiesJson);

    /* input (action / axis names come from the project's input map) */
    int (*input_action)(void* host, const char* action, int query);
    float (*input_axis)(void* host, const char* axis);
    void (*mouse_delta)(void* host, float* dx, float* dy);
    int (*input_rebind)(void* host, const char* action, const char* key); /* replaces the action's keys */

    void (*set_time_scale)(void* host, float scale);

    /* animation bones (SDK 0.7.0 / Fledge 1.2.1; appended, so modules built against older SDKs keep working).
     * World-space transform of a bone of the entity's Animator, valid after the animation update of the current frame
     * (OnLateUpdate). Returns 1 when the bone exists; 0 (outputs untouched) for an unknown bone or no Animator. */
    int (*bone_transform)(void* host, FScriptEntity e, const char* bone, FScriptVec3* pos, FScriptQuat* rot, FScriptVec3* scale);
    const char* (*bone_names)(void* host, FScriptEntity e); /* newline-separated, "" if none */

    /* filtered physics queries (SDK 0.9.0 / Fledge 1.3.2; appended). `ignore` (0 = none) is never hit, with
     * FSCRIPT_QUERY_IGNORE_HIERARCHY its children neither; layerMask: bit n = Collider.layer n (0xFFFFFFFF = every layer;
     * meshes / terrains without a collider are layer 0); triggers are never hit. Returns 1 and fills *hit on a hit. */
    int (*raycast_filtered)(void* host, FScriptVec3 origin, FScriptVec3 direction, float maxDistance, FScriptEntity ignore,
                            uint32_t layerMask, int flags, FScriptRayHit* hit);
    int (*sphere_cast)(void* host, FScriptVec3 origin, float radius, FScriptVec3 direction, float maxDistance, FScriptEntity ignore,
                       uint32_t layerMask, int flags, FScriptRayHit* hit);
} FScriptHostApi;

enum { FSCRIPT_QUERY_IGNORE_HIERARCHY = 1 };

enum FScriptHook {
    FSCRIPT_HOOK_CREATE = 0,
    FSCRIPT_HOOK_ENABLE = 1,
    FSCRIPT_HOOK_DISABLE = 2,
    FSCRIPT_HOOK_DESTROY = 3, /* OnDisable (if enabled) + OnDestroy, then the object is deleted */
    FSCRIPT_HOOK_HOT_RELOAD = 4
};
enum FScriptPhase { FSCRIPT_PHASE_UPDATE = 0, FSCRIPT_PHASE_FIXED = 1, FSCRIPT_PHASE_LATE = 2, FSCRIPT_PHASE_END_FRAME = 3 };
enum FScriptCollision { FSCRIPT_COLLISION_ENTER = 0, FSCRIPT_COLLISION_EXIT = 1, FSCRIPT_TRIGGER_ENTER = 2, FSCRIPT_TRIGGER_EXIT = 3 };

/* Module -> engine. */
typedef struct FScriptModuleApi {
    uint32_t abiVersion;
    uint32_t structSize;
    const char* sdkVersion;
    const char* compiler; /* e.g. "gcc 13.2.0" */

    int (*init)(const FScriptHostApi* host); /* 0 = ok */
    void (*shutdown)(void);                  /* deletes every object WITHOUT lifecycle calls (hot reload / unload) */

    const char* (*describe)(void); /* JSON: {"classes":[{"name","kind":"behaviour|service","properties":[...]}]} */

    FScriptInstance (*create)(const char* className, FScriptEntity owner); /* constructs only */
    int (*set_properties)(FScriptInstance inst, const char* json);        /* JSON object, unknown keys ignored */
    const char* (*get_properties)(FScriptInstance inst);                  /* JSON object */
    void (*hook)(FScriptInstance inst, int hook);                          /* FScriptHook */
    void (*set_reloading)(int reloading);                                /* IsHotReloading() during CREATE/ENABLE after a reload */
    void (*frame)(int phase, float dt);                                  /* drives every live object + scheduler */
    void (*collision)(FScriptInstance inst, int kind, FScriptEntity other);  /* called by the physics step (docs/PHYSICS.md) */
    int (*instance_count)(void);
    const char* (*last_error)(void); /* last exception / registration error, "" if none */
} FScriptModuleApi;

typedef const FScriptModuleApi* (*FScriptModuleEntryFn)(void);

#ifdef __cplusplus
}
#endif

#endif /* FLEDGE_SCRIPT_ABI_H */
