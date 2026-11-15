/* Fledge gameplay module C ABI (version 1).
 *
 * A gameplay module is a DLL that exports:
 *     const FledgeModuleApi* fledge_get_module_api(void);
 * The engine passes a FledgeHostApi with callbacks into the engine. Only plain C
 * types cross the boundary, so modules can be built by any compiler (MSVC,
 * MinGW, Clang) independently of the engine. Bump FLEDGE_MODULE_API_VERSION on
 * any layout change; the engine rejects mismatching modules.
 */
#ifndef FLEDGE_MODULE_API_H
#define FLEDGE_MODULE_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLEDGE_MODULE_API_VERSION 1u

#if defined(_WIN32)
#define FLEDGE_MODULE_EXPORT __declspec(dllexport)
#else
#define FLEDGE_MODULE_EXPORT __attribute__((visibility("default")))
#endif

typedef uint32_t FledgeEntity; /* 0 = null */
typedef struct FledgeVec3 { float x, y, z; } FledgeVec3;
typedef struct FledgeQuat { float x, y, z, w; } FledgeQuat;

enum { FLEDGE_LOG_INFO = 2, FLEDGE_LOG_WARN = 3, FLEDGE_LOG_ERROR = 4 };

typedef struct FledgeHostApi {
    uint32_t version;
    void* host; /* opaque, pass back to every callback */
    void (*log)(void* host, int level, const char* message);
    FledgeEntity (*find_entity)(void* host, const char* name);
    int (*get_position)(void* host, FledgeEntity e, FledgeVec3* out);
    int (*set_position)(void* host, FledgeEntity e, FledgeVec3 value);
    int (*get_rotation)(void* host, FledgeEntity e, FledgeQuat* out);
    int (*set_rotation)(void* host, FledgeEntity e, FledgeQuat value);
    int (*get_scale)(void* host, FledgeEntity e, FledgeVec3* out);
    int (*set_scale)(void* host, FledgeEntity e, FledgeVec3 value);
    double (*time_seconds)(void* host); /* play time */
} FledgeHostApi;

typedef struct FledgeModuleApi {
    uint32_t version; /* must equal FLEDGE_MODULE_API_VERSION */
    const char* name;
    int (*on_load)(const FledgeHostApi* host); /* return 0 on success */
    void (*on_begin_play)(void);
    /* called for every entity with a Script component bound to this module */
    void (*on_entity_tick)(FledgeEntity entity, const char* behaviour, float param, float dt);
    void (*on_tick)(float dt);
    void (*on_end_play)(void);
    void (*on_unload)(void);
} FledgeModuleApi;

typedef const FledgeModuleApi* (*FledgeGetModuleApiFn)(void);

#ifdef __cplusplus
}
#endif

#endif /* FLEDGE_MODULE_API_H */
