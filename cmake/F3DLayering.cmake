# SPDX-License-Identifier: Apache-2.0
#
# Module layering enforcement (phases.md §0.5):
#
#   core <- scene <- ops <- {assets, terrain, world} <- {render/*, io, api} <- {editor, tools}
#
# Every engine module is created with f3d_add_module(), which checks each f3d
# dependency against the allowed-edge table below and stops configuration with
# FATAL_ERROR on a forbidden edge. f3d_verify_layering() re-checks the final
# link graph at the end of configuration, so edges added later with a plain
# target_link_libraries() call are caught too. Diligent may only be linked,
# privately, by f3d_render_rhi.
#
# This file also works in script mode (cmake -P) so the check can be tested.

include_guard(GLOBAL)

# --- Allowed-edge table -------------------------------------------------------
# F3D_LAYER_ALLOWED_<target> lists every f3d target <target> may depend on.
set(_f3d_l0 f3d_core)
set(_f3d_l1 ${_f3d_l0} f3d_scene)
set(_f3d_l2 ${_f3d_l1} f3d_ops)
set(_f3d_domain ${_f3d_l2} f3d_assets f3d_terrain f3d_world f3d_modeling)
set(_f3d_render f3d_render_rhi f3d_render_graph f3d_render_upscale f3d_render_raster)
set(_f3d_l4 ${_f3d_domain} ${_f3d_render} f3d_io f3d_api f3d_python)

set(F3D_LAYER_ALLOWED_f3d_core "")
set(F3D_LAYER_ALLOWED_f3d_scene ${_f3d_l0})
set(F3D_LAYER_ALLOWED_f3d_ops ${_f3d_l1})
# Domain modules: assets is the base, terrain builds on assets, world on terrain.
set(F3D_LAYER_ALLOWED_f3d_assets ${_f3d_l2})
set(F3D_LAYER_ALLOWED_f3d_terrain ${_f3d_l2} f3d_assets)
set(F3D_LAYER_ALLOWED_f3d_world ${_f3d_l2} f3d_assets f3d_terrain)
# Polygon modeling (editable meshes) builds on assets.
set(F3D_LAYER_ALLOWED_f3d_modeling ${_f3d_l2} f3d_assets)
# Render stack: rhi is the bottom and sees only core; the rest stack in order.
set(F3D_LAYER_ALLOWED_f3d_render_rhi ${_f3d_l0})
set(F3D_LAYER_ALLOWED_f3d_render_graph ${_f3d_l0} f3d_render_rhi)
set(F3D_LAYER_ALLOWED_f3d_render_upscale ${_f3d_l0} f3d_render_rhi f3d_render_graph)
set(F3D_LAYER_ALLOWED_f3d_render_raster ${_f3d_domain} f3d_render_rhi f3d_render_graph f3d_render_upscale)
set(F3D_LAYER_ALLOWED_f3d_io ${_f3d_domain})
set(F3D_LAYER_ALLOWED_f3d_api ${_f3d_domain} ${_f3d_render} f3d_io)
# The optional Python module (modules/python) sits beside api and uses it.
set(F3D_LAYER_ALLOWED_f3d_python ${_f3d_domain} ${_f3d_render} f3d_io f3d_api)
# Top tier: the editor library and the tools may use everything below them.
set(F3D_LAYER_ALLOWED_f3d_editor ${_f3d_l4})

set(F3D_LAYER_MODULES
    f3d_core f3d_scene f3d_ops f3d_assets f3d_terrain f3d_world f3d_modeling
    f3d_render_rhi f3d_render_graph f3d_render_upscale f3d_render_raster
    f3d_io f3d_api f3d_python f3d_editor)

# --- Checks -----------------------------------------------------------------

# Fails configuration if `from` may not depend on the f3d target `to`.
# Non-f3d names (third-party targets) are ignored here.
function(f3d_check_layer_edge from to)
    if(NOT to MATCHES "^f3d_" OR to STREQUAL "f3d_options")
        return()
    endif()
    if(NOT DEFINED F3D_LAYER_ALLOWED_${from})
        message(FATAL_ERROR "forge3d layering: '${from}' is not in the layer table (cmake/F3DLayering.cmake)")
    endif()
    if(NOT to IN_LIST F3D_LAYER_ALLOWED_${from})
        string(REPLACE ";" ", " _allowed "${F3D_LAYER_ALLOWED_${from}}")
        if(_allowed STREQUAL "")
            set(_allowed "(nothing - it is the bottom layer)")
        endif()
        message(FATAL_ERROR
            "forge3d layering violation: '${from}' must not depend on '${to}'.\n"
            "  Allowed dependencies of ${from}: ${_allowed}\n"
            "  Layer order: core <- scene <- ops <- {assets, terrain, world} <- {render/*, io, api} <- {editor, tools}\n"
            "  See phases.md §0.5 and cmake/F3DLayering.cmake.")
    endif()
endfunction()

# Fails configuration if a third-party library may not be linked by `from`.
function(f3d_check_layer_lib from lib visibility)
    if(lib MATCHES "Diligent")
        if(NOT from STREQUAL "f3d_render_rhi")
            message(FATAL_ERROR
                "forge3d layering violation: '${from}' links '${lib}'. Only f3d_render_rhi may use Diligent.")
        endif()
        if(visibility STREQUAL "PUBLIC")
            message(FATAL_ERROR
                "forge3d layering violation: f3d_render_rhi must link '${lib}' PRIVATE so Diligent headers stay inside render/rhi.")
        endif()
    endif()
endfunction()

# --- Module creation --------------------------------------------------------

# f3d_add_module(<name>
#     [SOURCES <src>...]          # private sources (default: none)
#     [DEPS <f3d target>...]      # public f3d dependencies, checked against the table
#     [PRIVATE_DEPS <f3d target>...]
#     [PUBLIC_LIBS <target>...]   # public third-party libraries
#     [PRIVATE_LIBS <target>...]) # private third-party libraries
#
# Creates a static library with `include/` as its public include directory.
function(f3d_add_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;PRIVATE_DEPS;PUBLIC_LIBS;PRIVATE_LIBS" ${ARGN})
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "f3d_add_module(${name}): unknown arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    foreach(dep IN LISTS ARG_DEPS ARG_PRIVATE_DEPS)
        f3d_check_layer_edge(${name} ${dep})
    endforeach()
    foreach(lib IN LISTS ARG_PUBLIC_LIBS)
        f3d_check_layer_lib(${name} ${lib} PUBLIC)
    endforeach()
    foreach(lib IN LISTS ARG_PRIVATE_LIBS)
        f3d_check_layer_lib(${name} ${lib} PRIVATE)
    endforeach()

    add_library(${name} STATIC ${ARG_SOURCES})
    target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(${name} PUBLIC cxx_std_20)
    target_link_libraries(${name}
        PUBLIC ${ARG_DEPS} ${ARG_PUBLIC_LIBS}
        PRIVATE f3d_options ${ARG_PRIVATE_DEPS} ${ARG_PRIVATE_LIBS})
endfunction()

# Re-checks the final link graph of every module (catches edges added outside
# f3d_add_module). Call once at the end of the top-level CMakeLists.txt.
function(f3d_verify_layering)
    foreach(module IN LISTS F3D_LAYER_MODULES)
        if(NOT TARGET ${module})
            continue()
        endif()
        get_target_property(_libs ${module} LINK_LIBRARIES)
        get_target_property(_ilibs ${module} INTERFACE_LINK_LIBRARIES)
        foreach(lib IN LISTS _libs _ilibs)
            if(lib MATCHES "^\\$<LINK_ONLY:(.*)>$")
                set(lib "${CMAKE_MATCH_1}")
            endif()
            if(lib AND NOT lib MATCHES "-NOTFOUND$")
                f3d_check_layer_edge(${module} ${lib})
                f3d_check_layer_lib(${module} "${lib}" PRIVATE)
            endif()
        endforeach()
        if(_ilibs)
            foreach(lib IN LISTS _ilibs)
                if(NOT lib MATCHES "^\\$<LINK_ONLY:")
                    f3d_check_layer_lib(${module} "${lib}" PUBLIC)
                endif()
            endforeach()
        endif()
    endforeach()
endfunction()

# Script mode: `cmake -DF3D_CHECK_FROM=a -DF3D_CHECK_TO=b -P F3DLayering.cmake`
if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE AND DEFINED F3D_CHECK_FROM)
    f3d_check_layer_edge(${F3D_CHECK_FROM} ${F3D_CHECK_TO})
    message(STATUS "forge3d layering: ${F3D_CHECK_FROM} -> ${F3D_CHECK_TO} is allowed")
endif()
