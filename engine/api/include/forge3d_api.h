// SPDX-License-Identifier: Apache-2.0
/*
 * forge3d C API (phases.md P5.1): the stable binary interface for plugins and
 * embedding. Pure C, no forge3d headers needed.
 *
 *  - Everything is reached through a function table, F3dApi, obtained with
 *    f3d_api_get(F3D_API_VERSION). A host that can't serve that version returns
 *    NULL (version handshake): same major, minor not newer than the host's.
 *  - Handles are opaque: F3dHost (an editor or CLI session: its operation
 *    registry and history), F3dWorld (a scene), F3dOpContext (a running
 *    operation). Entities are named by value with their 128-bit GUID (F3dEntity).
 *  - Scene changes made inside an operation go through the context, so the host
 *    records them: plugin operations are undoable like built-in ones, and redo
 *    replays the recorded changes without calling the plugin again.
 *  - Strings are UTF-8; JSON is used for property values and parameters. Every
 *    function returns an F3dResult; F3dApi.last_error describes the last failure
 *    on the calling thread.
 *
 * Plugins are shared libraries in the host's plugins/ directory that export
 *
 *     uint32_t  f3d_plugin_api_version(void);   (use F3D_PLUGIN_DECLARE_VERSION)
 *     F3dResult f3d_plugin_entry(F3dGetApiFn get_api, F3dHost* host, F3dPluginInfo* info);
 *
 * The host checks f3d_plugin_api_version before running any plugin code, then
 * calls f3d_plugin_entry once; the plugin fetches the table with get_api and
 * registers its operations.
 */
#ifndef FORGE3D_API_H
#define FORGE3D_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define F3D_API_VERSION_MAJOR 1
#define F3D_API_VERSION_MINOR 0
#define F3D_API_VERSION ((uint32_t)((F3D_API_VERSION_MAJOR << 16) | F3D_API_VERSION_MINOR))
#define F3D_API_MAJOR(version) ((uint32_t)(version) >> 16)
#define F3D_API_MINOR(version) ((uint32_t)(version) & 0xFFFFu)

#if defined(_WIN32)
#define F3D_PLUGIN_EXPORT __declspec(dllexport)
#else
#define F3D_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

typedef enum F3dResult {
    F3D_OK = 0,
    F3D_ERROR_INVALID_ARGUMENT = 1, /* a null handle, malformed JSON, an unknown path */
    F3D_ERROR_NOT_FOUND = 2,        /* no such entity, operation or component */
    F3D_ERROR_VERSION_MISMATCH = 3, /* the host can't serve the requested API version */
    F3D_ERROR_FAILED = 4,           /* the operation ran and failed */
    F3D_ERROR_BUFFER_TOO_SMALL = 5, /* the output didn't fit; *written holds the size needed */
    F3D_ERROR_WRONG_CONTEXT = 6     /* e.g. a scene change outside an operation */
} F3dResult;

typedef enum F3dLogLevel { F3D_LOG_LEVEL_INFO = 0, F3D_LOG_LEVEL_WARN = 1, F3D_LOG_LEVEL_ERROR = 2 } F3dLogLevel;

/* An entity, by its GUID. The null entity is {0, 0}. */
typedef struct F3dEntity {
    uint64_t hi;
    uint64_t lo;
} F3dEntity;

typedef struct F3dHost F3dHost;
typedef struct F3dWorld F3dWorld;
typedef struct F3dOpContext F3dOpContext;

/* A plugin operation's work: change the scene through `ctx`. `params_json` is the
 * operation's parameters as a JSON object. Return F3D_OK, or an error (the host
 * then reverts whatever the call changed). */
typedef F3dResult (*F3dOpExecuteFn)(F3dOpContext* ctx, const char* params_json, void* user_data);

typedef struct F3dOpDesc {
    const char* id;           /* "domain.verb", unique; plugins should use their own domain */
    const char* display_name; /* shown in menus and the history */
    /* Parameters as a JSON array of {"name", "type", "default"?, "tooltip"?}; types
     * are "int", "float", "bool", "string", "vec3", "guid", "json". May be NULL. */
    const char* params_schema_json;
    F3dOpExecuteFn execute;
    void* user_data;
} F3dOpDesc;

typedef struct F3dApi {
    uint32_t version; /* the version this table implements (F3D_API_VERSION of the host) */
    uint32_t size;    /* sizeof(F3dApi) in the host: fields beyond it don't exist */

    /* Diagnostics. */
    const char* (*last_error)(void);
    void (*log)(F3dLogLevel level, const char* message);

    /* Operations (on a host). */
    F3dResult (*register_op)(F3dHost* host, const F3dOpDesc* desc);
    /* Runs an operation by id through the host's history (undoable). */
    F3dResult (*run_op)(F3dHost* host, const char* id, const char* params_json);
    F3dResult (*undo)(F3dHost* host);
    F3dResult (*redo)(F3dHost* host);
    F3dWorld* (*host_world)(F3dHost* host);

    /* Reading a world (any time). */
    size_t (*entity_count)(F3dWorld* world);
    /* The first entity with this name, in scene order. */
    F3dResult (*find_entity)(F3dWorld* world, const char* name, F3dEntity* out);
    /* A property ("Component.field") as JSON. With `buffer` NULL or too small,
     * *written receives the size needed (including the terminating zero). */
    F3dResult (*get_property)(F3dWorld* world, F3dEntity entity, const char* path, char* buffer, size_t capacity,
                              size_t* written);

    /* Changing the scene (inside an operation only; recorded for undo). */
    F3dWorld* (*context_world)(F3dOpContext* ctx);
    F3dResult (*create_entity)(F3dOpContext* ctx, const char* name, F3dEntity parent, F3dEntity* out);
    F3dResult (*delete_entity)(F3dOpContext* ctx, F3dEntity entity);
    F3dResult (*set_property)(F3dOpContext* ctx, F3dEntity entity, const char* path, const char* json_value);
    F3dResult (*add_component)(F3dOpContext* ctx, F3dEntity entity, const char* component);
    /* Runs another operation as part of this one (one undo step for both). */
    F3dResult (*run_child_op)(F3dOpContext* ctx, const char* id, const char* params_json);
} F3dApi;

/* The function table for `requested_version`, or NULL if this host can't serve it. */
typedef const F3dApi* (*F3dGetApiFn)(uint32_t requested_version);

/* In-process hosts (and the plugin loader) export this. */
const F3dApi* f3d_api_get(uint32_t requested_version);

typedef struct F3dPluginInfo {
    const char* name;    /* set by the plugin in f3d_plugin_entry */
    const char* version; /* the plugin's own version, free form */
} F3dPluginInfo;

typedef uint32_t (*F3dPluginApiVersionFn)(void);
typedef F3dResult (*F3dPluginEntryFn)(F3dGetApiFn get_api, F3dHost* host, F3dPluginInfo* info);

/* Defines f3d_plugin_api_version() in a plugin: the API version it was built against. */
#define F3D_PLUGIN_DECLARE_VERSION                                                                                     \
    F3D_PLUGIN_EXPORT uint32_t f3d_plugin_api_version(void) {                                                          \
        return F3D_API_VERSION;                                                                                        \
    }

#ifdef __cplusplus
}
#endif

#endif /* FORGE3D_API_H */
