# SPDX-License-Identifier: Apache-2.0
find_package(doctest CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# One doctest runner for all unit tests. Each tests/unit/<module>/ directory is
# registered as its own ctest entry `unit.<module>` (label `unit`), selected by
# source-file filter.
file(GLOB_RECURSE F3D_UNIT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/*_test.cpp)
# Integration tests (GPU, files) live in the same runner and are registered per
# tests/integration/<area>/ directory with the `integration` label.
file(GLOB_RECURSE F3D_INTEGRATION_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/integration/*_test.cpp)
# Performance tests (label perf): budgets measured on the build machine, run on demand.
file(GLOB_RECURSE F3D_PERF_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/perf/*_test.cpp)

add_executable(forge3d-tests main.cpp ${F3D_UNIT_TEST_SOURCES} ${F3D_INTEGRATION_TEST_SOURCES} ${F3D_PERF_TEST_SOURCES})
target_link_libraries(forge3d-tests PRIVATE
    f3d_options doctest::doctest nlohmann_json::nlohmann_json
    f3d_core f3d_scene f3d_ops f3d_assets f3d_terrain f3d_world f3d_modeling
    f3d_render_rhi f3d_render_graph f3d_render_upscale f3d_render_raster f3d_io f3d_api f3d_editor)
# doctest's own std forward declarations clash with recent MSVC STL headers.
target_compile_definitions(forge3d-tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS
    F3D_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data" F3D_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test-output"
    F3D_EDITOR_PRESET_DIR="${PROJECT_SOURCE_DIR}/editor/resources/input")
add_dependencies(forge3d-tests f3d_shaders)

# Plugin tests (phases.md P5.1): the sample plugin, and one built for a future API.
add_library(f3d_test_future_api MODULE plugins/future_api.c)
target_include_directories(f3d_test_future_api PRIVATE ${PROJECT_SOURCE_DIR}/engine/api/include)
set_target_properties(f3d_test_future_api PROPERTIES PREFIX "" C_STANDARD 11
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test-output/plugins/$<CONFIG>)
add_dependencies(forge3d-tests hello_op f3d_test_future_api)
target_compile_definitions(forge3d-tests PRIVATE
    F3D_HELLO_PLUGIN="$<TARGET_FILE:hello_op>" F3D_FUTURE_PLUGIN="$<TARGET_FILE:f3d_test_future_api>")
target_include_directories(forge3d-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
f3d_deploy_dxc_runtime(forge3d-tests)

file(GLOB F3D_UNIT_TEST_DIRS LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/unit/*)
foreach(dir IN LISTS F3D_UNIT_TEST_DIRS)
    if(IS_DIRECTORY ${dir})
        get_filename_component(module ${dir} NAME)
        # `?` matches either path separator in doctest's wildcard filter.
        add_test(NAME unit.${module}
                 COMMAND forge3d-tests --source-file=*unit?${module}?* --no-version)
        set_tests_properties(unit.${module} PROPERTIES LABELS unit)
    endif()
endforeach()

foreach(area IN ITEMS api rhi raster streaming)
    add_test(NAME integration.${area}
             COMMAND forge3d-tests --source-file=*integration?${area}?* --no-version)
    set_tests_properties(integration.${area} PROPERTIES LABELS integration TIMEOUT 300)
endforeach()

foreach(source IN LISTS F3D_PERF_TEST_SOURCES)
    get_filename_component(name ${source} NAME_WE)
    string(REGEX REPLACE "_perf_test$" "" name ${name})
    add_test(NAME perf.${name} COMMAND forge3d-tests --source-file=*perf?${name}_perf_test* --no-version --success=false)
    set_tests_properties(perf.${name} PROPERTIES LABELS perf TIMEOUT 600)
endforeach()

# Diligent headers only inside engine/render/rhi/src (phases.md P1.1).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME unit.diligent_includes
             COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/check_diligent_includes.py
                     --diligent-dir ${FETCHCONTENT_BASE_DIR})
    set_tests_properties(unit.diligent_includes PROPERTIES LABELS unit)
endif()

# Layering rule (cmake/F3DLayering.cmake): a forbidden edge must stop configuration
# with a clear message, an allowed edge must pass.
add_test(NAME unit.layering.forbidden_edge
         COMMAND ${CMAKE_COMMAND} -DF3D_CHECK_FROM=f3d_core -DF3D_CHECK_TO=f3d_scene
                 -P ${PROJECT_SOURCE_DIR}/cmake/F3DLayering.cmake)
set_tests_properties(unit.layering.forbidden_edge PROPERTIES
    LABELS unit PASS_REGULAR_EXPRESSION "layering violation: 'f3d_core' must not depend on 'f3d_scene'")
add_test(NAME unit.layering.allowed_edge
         COMMAND ${CMAKE_COMMAND} -DF3D_CHECK_FROM=f3d_render_raster -DF3D_CHECK_TO=f3d_world
                 -P ${PROJECT_SOURCE_DIR}/cmake/F3DLayering.cmake)
set_tests_properties(unit.layering.allowed_edge PROPERTIES LABELS unit)
