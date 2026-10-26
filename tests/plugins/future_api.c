// SPDX-License-Identifier: Apache-2.0
/*
 * A test plugin built for a forge3d API this host doesn't provide (2.0): the
 * loader must refuse it without calling f3d_plugin_entry (phases.md P5.1).
 */
#include "forge3d_api.h"

F3D_PLUGIN_EXPORT uint32_t f3d_plugin_api_version(void) {
    return (uint32_t)(2u << 16);
}

F3D_PLUGIN_EXPORT F3dResult f3d_plugin_entry(F3dGetApiFn get_api, F3dHost* host, F3dPluginInfo* info) {
    (void)get_api;
    (void)host;
    info->name = "future_api: entry was called (must not happen)";
    return F3D_ERROR_FAILED;
}
