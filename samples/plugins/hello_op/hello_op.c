// SPDX-License-Identifier: Apache-2.0
/*
 * hello_op: a sample forge3d plugin (phases.md P5.1).
 *
 * Registers `sample.scatter_cubes`: a root entity with `count` children scattered
 * over a square of half-size `radius`, with random yaw and scale (seeded, so the
 * same parameters give the same scene). With `mesh` set (a mesh asset path such as
 * "models/box.glb#mesh/0"), each child also draws it. The changes go through the
 * operation context, so the whole scatter is one undo step.
 */
#include "forge3d_api.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const F3dApi* g_api = NULL;

F3D_PLUGIN_DECLARE_VERSION

/* The value after "key": in a flat JSON object, or NULL. Enough for this sample's
 * parameters (numbers and simple strings); real plugins should use a JSON parser. */
static const char* find_value(const char* json, const char* key) {
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\"", key);
    const char* at = json ? strstr(json, pattern) : NULL;
    if (!at) {
        return NULL;
    }
    at = strchr(at + strlen(pattern), ':');
    if (!at) {
        return NULL;
    }
    ++at;
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') {
        ++at;
    }
    return at;
}

static double param_number(const char* json, const char* key, double fallback) {
    const char* value = find_value(json, key);
    if (!value) {
        return fallback;
    }
    char* end = NULL;
    const double number = strtod(value, &end);
    return end == value ? fallback : number;
}

static void param_string(const char* json, const char* key, char* out, size_t capacity) {
    out[0] = '\0';
    const char* value = find_value(json, key);
    if (!value || *value != '"') {
        return;
    }
    ++value;
    size_t n = 0;
    while (value[n] && value[n] != '"' && n + 1 < capacity) {
        ++n;
    }
    memcpy(out, value, n);
    out[n] = '\0';
}

/* xorshift32: a small deterministic generator. */
static double next_unit(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return (double)(x & 0xFFFFFFu) / (double)0x1000000u;
}

#define CHECK(call)                                                                                                    \
    do {                                                                                                               \
        const F3dResult r_ = (call);                                                                                   \
        if (r_ != F3D_OK) {                                                                                            \
            return r_;                                                                                                 \
        }                                                                                                              \
    } while (0)

static F3dResult scatter_cubes(F3dOpContext* ctx, const char* params, void* user_data) {
    (void)user_data;
    int count = (int)param_number(params, "count", 10.0);
    if (count < 0 || count > 10000) {
        g_api->log(F3D_LOG_LEVEL_ERROR, "sample.scatter_cubes: count must be 0..10000");
        return F3D_ERROR_INVALID_ARGUMENT;
    }
    uint32_t state = (uint32_t)param_number(params, "seed", 1.0) * 2654435761u + 1u;
    if (state == 0) {
        state = 1;
    }
    const double radius = param_number(params, "radius", 5.0);
    char mesh[512];
    param_string(params, "mesh", mesh, sizeof mesh);

    const F3dEntity none = {0, 0};
    F3dEntity root;
    CHECK(g_api->create_entity(ctx, "Scattered cubes", none, &root));
    char name[64];
    char value[640];
    for (int i = 0; i < count; ++i) {
        F3dEntity cube;
        snprintf(name, sizeof name, "Cube %d", i + 1);
        CHECK(g_api->create_entity(ctx, name, root, &cube));
        const double x = (next_unit(&state) * 2.0 - 1.0) * radius;
        const double z = (next_unit(&state) * 2.0 - 1.0) * radius;
        const double scale = 0.5 + next_unit(&state);
        const double yaw = next_unit(&state) * 6.283185307179586;
        snprintf(value, sizeof value, "[%.6f, %.6f, %.6f]", x, scale * 0.5, z);
        CHECK(g_api->set_property(ctx, cube, "Transform.translation", value));
        snprintf(value, sizeof value, "[%.6f, %.6f, %.6f]", scale, scale, scale);
        CHECK(g_api->set_property(ctx, cube, "Transform.scale", value));
        /* Rotation about +Y as [w, x, y, z]. */
        snprintf(value, sizeof value, "[%.7f, 0.0, %.7f, 0.0]", cos(yaw * 0.5), sin(yaw * 0.5));
        CHECK(g_api->set_property(ctx, cube, "Transform.rotation", value));
        if (mesh[0]) {
            CHECK(g_api->add_component(ctx, cube, "MeshRenderer"));
            snprintf(value, sizeof value, "{\"path\": \"%s\"}", mesh);
            CHECK(g_api->set_property(ctx, cube, "MeshRenderer.mesh", value));
        }
    }
    return F3D_OK;
}

F3D_PLUGIN_EXPORT F3dResult f3d_plugin_entry(F3dGetApiFn get_api, F3dHost* host, F3dPluginInfo* info) {
    g_api = get_api(F3D_API_VERSION);
    if (!g_api) {
        return F3D_ERROR_VERSION_MISMATCH;
    }
    info->name = "hello_op";
    info->version = "1.0.0";
    const F3dOpDesc scatter = {
        "sample.scatter_cubes",
        "Scatter Cubes",
        "[{\"name\": \"count\", \"type\": \"int\", \"default\": 10, \"tooltip\": \"Number of cubes (0..10000)\"},"
        " {\"name\": \"seed\", \"type\": \"int\", \"default\": 1},"
        " {\"name\": \"radius\", \"type\": \"float\", \"default\": 5.0, \"unit\": \"m\"},"
        " {\"name\": \"mesh\", \"type\": \"string\", \"default\": \"\","
        " \"tooltip\": \"Mesh asset each cube draws (optional)\"}]",
        scatter_cubes,
        NULL,
    };
    return g_api->register_op(host, &scatter);
}
