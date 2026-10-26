# SPDX-License-Identifier: Apache-2.0
# f3d_editor holds all editor code so it can be unit tested; forge3d-editor is
# only the entry point.
find_package(SDL3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(imguizmo CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

f3d_add_module(f3d_editor
    SOURCES
        src/app.cpp
        src/app_assets.cpp
        src/app_document.cpp
        src/app_landscape.cpp
        src/app_modeling.cpp
        src/bench.cpp
        src/camera.cpp
        src/camera_actors.cpp
        src/command_registry.cpp
        src/console_model.cpp
        src/document.cpp
        src/editor.cpp
        src/foliage_tool.cpp
        src/fuzzy.cpp
        src/import_queue.cpp
        src/input.cpp
        src/launch_options.cpp
        src/measure_tool.cpp
        src/modeling_tool.cpp
        src/navigation.cpp
        src/paint_tool.cpp
        src/platform.cpp
        src/preferences.cpp
        src/project_import.cpp
        src/scene_editor.cpp
        src/sculpt_tool.cpp
        src/spline_tool.cpp
        src/water_tool.cpp
        src/selection_tools.cpp
        src/thumbnail_cache.cpp
        src/transform_tool.cpp
        src/viewport_stats.cpp
        src/viewport_tool.cpp
        src/ui/asset_browser.cpp
        src/ui/editor_ui.cpp
        src/ui/imgui_layer.cpp
        src/ui/material_panel.cpp
        src/ui/modeling_panel.cpp
        src/ui/panel.cpp
        src/ui/panels.cpp
        src/ui/python_panel.cpp
        src/ui/reflect_widgets.cpp
        src/ui/scatter_panel.cpp
        src/ui/scene_panels.cpp
        src/ui/terrain_panel.cpp
        src/ui/tool_panels.cpp
    DEPS f3d_core f3d_scene f3d_ops f3d_assets f3d_terrain f3d_world f3d_modeling
         f3d_render_rhi f3d_render_graph f3d_render_upscale f3d_render_raster f3d_io f3d_api
    PRIVATE_LIBS SDL3::SDL3 imgui::imgui imguizmo::imguizmo nlohmann_json::nlohmann_json)

if(TARGET f3d_python)
    target_link_libraries(f3d_editor PRIVATE f3d_python) # the Python panel (P5.4)
endif()

set(F3D_EDITOR_RESOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/resources)
configure_file(src/editor_config.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/f3d_editor_config.hpp @ONLY)
target_include_directories(f3d_editor PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

add_executable(forge3d-editor main.cpp)
target_link_libraries(forge3d-editor PRIVATE f3d_editor f3d_options)
add_dependencies(forge3d-editor f3d_shaders hello_op)
f3d_deploy_dxc_runtime(forge3d-editor)
# Runtime resources (navigation presets, ...) next to the executable.
add_custom_command(TARGET forge3d-editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${F3D_EDITOR_RESOURCE_DIR} $<TARGET_FILE_DIR:forge3d-editor>/resources
    VERBATIM)

if(F3D_BUILD_TESTS)
    set(_backends vulkan)
    if(WIN32)
        list(PREPEND _backends d3d12)
    endif()
    foreach(rhi IN LISTS _backends)
        # --frames smoke test (phases.md P1.1).
        add_test(NAME integration.editor.smoke.${rhi}
                 COMMAND forge3d-editor --rhi=${rhi} --frames 120 --hidden)
        set_tests_properties(integration.editor.smoke.${rhi} PROPERTIES LABELS integration TIMEOUT 120)

        # --capture writes the clear colour; checked by forge3d-tests (tests/integration/editor).
        set(_png ${CMAKE_BINARY_DIR}/test-output/editor-capture-${rhi}.png)
        add_test(NAME integration.editor.capture.${rhi}
                 COMMAND forge3d-editor --rhi=${rhi} --frames 5 --hidden --capture ${_png})
        set_tests_properties(integration.editor.capture.${rhi} PROPERTIES
            LABELS integration TIMEOUT 120 FIXTURES_SETUP editor_capture_${rhi})
        add_test(NAME integration.editor.capture.${rhi}.check
                 COMMAND forge3d-tests --source-file=*integration?editor?capture* --no-version)
        set_tests_properties(integration.editor.capture.${rhi}.check PROPERTIES
            LABELS integration FIXTURES_REQUIRED editor_capture_${rhi} ENVIRONMENT "F3D_CAPTURE_PNG=${_png}")
    endforeach()
    # P5.2: the JSON-RPC server end to end (needs Python's `websockets`; skipped without it).
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME integration.rpc
                 COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tests/integration/rpc/test_rpc.py
                         --editor $<TARGET_FILE:forge3d-editor>
                 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(integration.rpc PROPERTIES LABELS integration TIMEOUT 300 SKIP_RETURN_CODE 77)
    endif()
    # --capture-ui: the whole window with the default dock layout (inspected visually).
    add_test(NAME integration.editor.capture_ui
             COMMAND forge3d-editor --frames 5 --hidden --capture ${CMAKE_BINARY_DIR}/test-output/editor-ui.png
                     --capture-ui)
    set_tests_properties(integration.editor.capture_ui PROPERTIES LABELS integration TIMEOUT 120)
    # Navigation replay (phases.md P1.3): the same orbit/pan/zoom gestures in every preset's
    # chords must leave the camera at the same expected transform.
    foreach(preset IN ITEMS blender maya unreal)
        set(_json ${CMAKE_BINARY_DIR}/test-output/replay-${preset}.json)
        add_test(NAME integration.editor.replay.${preset}
                 COMMAND forge3d-editor --hidden --frames 14 --replay ${PROJECT_SOURCE_DIR}/tests/data/input/nav_${preset}.json
                         --bench ${_json})
        set_tests_properties(integration.editor.replay.${preset} PROPERTIES
            LABELS integration TIMEOUT 120 FIXTURES_SETUP editor_replay_${preset})
        add_test(NAME integration.editor.replay.${preset}.check
                 COMMAND forge3d-tests --source-file=*integration?editor?replay* --no-version)
        set_tests_properties(integration.editor.replay.${preset}.check PROPERTIES
            LABELS integration FIXTURES_REQUIRED editor_replay_${preset} ENVIRONMENT "F3D_REPLAY_BENCH=${_json}")
    endforeach()
    # First mesh (phases.md P1.4): a committed CC-BY model, lit by the Lambert pass.
    set(_png ${CMAKE_BINARY_DIR}/test-output/editor-open-fox.png)
    add_test(NAME integration.editor.open_gltf
             COMMAND forge3d-editor --hidden --frames 5 --open ${PROJECT_SOURCE_DIR}/tests/data/gltf/Fox.glb
                     --capture ${_png})
    set_tests_properties(integration.editor.open_gltf PROPERTIES
        LABELS integration TIMEOUT 120 FIXTURES_SETUP editor_open_gltf)
    add_test(NAME integration.editor.open_gltf.check
             COMMAND forge3d-tests --source-file=*integration?editor?mesh_capture* --no-version)
    set_tests_properties(integration.editor.open_gltf.check PROPERTIES
        LABELS integration FIXTURES_REQUIRED editor_open_gltf ENVIRONMENT "F3D_MESH_PNG=${_png}")
    # Outliner & layers (phases.md P2.5): an operation script builds a hierarchy with a locked
    # and a hidden layer on top of a model; the capture is inspected visually.
    add_test(NAME integration.editor.outliner_layers
             COMMAND forge3d-editor --hidden --frames 5 --open ${PROJECT_SOURCE_DIR}/tests/data/gltf/Fox.glb
                     --scene ${PROJECT_SOURCE_DIR}/tests/data/scenes/outliner_layers.json
                     --capture ${CMAKE_BINARY_DIR}/test-output/editor-outliner-layers.png --capture-ui)
    set_tests_properties(integration.editor.outliner_layers PROPERTIES LABELS integration TIMEOUT 120)
    # Asset browser (phases.md P3.3): a project holding the import corpus and the glTF samples
    # is scanned, imported and thumbnailed at start-up; the capture shows the browser.
    set(_project ${CMAKE_BINARY_DIR}/test-output/browser_project)
    add_test(NAME integration.editor.asset_browser.setup
             COMMAND ${CMAKE_COMMAND} -DSOURCE=${PROJECT_SOURCE_DIR}/tests/data -DDEST=${_project}
                     -P ${PROJECT_SOURCE_DIR}/tests/integration/editor/make_browser_project.cmake)
    set_tests_properties(integration.editor.asset_browser.setup PROPERTIES
        LABELS integration FIXTURES_SETUP editor_browser_files)
    add_test(NAME integration.editor.asset_browser.scene COMMAND forge3d-cli --quiet new save ${_project}/scene.forge)
    set_tests_properties(integration.editor.asset_browser.scene PROPERTIES
        LABELS integration FIXTURES_REQUIRED editor_browser_files FIXTURES_SETUP editor_browser_project)
    add_test(NAME integration.editor.asset_browser
             COMMAND forge3d-editor --hidden --frames 5 --open ${_project}/scene.forge --scan-assets
                     --capture ${CMAKE_BINARY_DIR}/test-output/editor-asset-browser.png --capture-ui)
    set_tests_properties(integration.editor.asset_browser PROPERTIES
        LABELS integration TIMEOUT 300 FIXTURES_REQUIRED editor_browser_project
        PASS_REGULAR_EXPRESSION "Assets: 10 found, 10 imported, 10 thumbnails")
    add_test(NAME integration.editor.bench
             COMMAND forge3d-editor --frames 60 --hidden --bench ${CMAKE_BINARY_DIR}/test-output/editor-bench.json)
    set_tests_properties(integration.editor.bench PROPERTIES LABELS integration TIMEOUT 120)
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test-output)
endif()
