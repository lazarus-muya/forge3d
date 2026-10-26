# SPDX-License-Identifier: Apache-2.0
add_executable(forge3d-cli main.cpp)
target_link_libraries(forge3d-cli PRIVATE f3d_options f3d_core f3d_scene f3d_ops f3d_assets f3d_terrain f3d_modeling f3d_io
                                          f3d_render_rhi f3d_render_raster f3d_api)
if(TARGET f3d_python)
    target_link_libraries(forge3d-cli PRIVATE f3d_python) # `run script.py` (P5.4)
endif()
# viewport.capture renders with the compiled shaders (phases.md P4.4).
add_dependencies(forge3d-cli f3d_shaders)
f3d_deploy_dxc_runtime(forge3d-cli)

if(F3D_BUILD_TESTS)
    # M2 exit (phases.md P2.6): create/transform/delete/reparent -> undo/redo -> save ->
    # reload in a new process -> the serialised scenes are identical.
    set(_out ${CMAKE_BINARY_DIR}/test-output/cli)
    file(MAKE_DIRECTORY ${_out})
    set(_a 00000000-0000-4000-8000-0000000000c1)
    set(_b 00000000-0000-4000-8000-0000000000c2)
    set(_c 00000000-0000-4000-8000-0000000000c3)
    set(_d 00000000-0000-4000-8000-0000000000c4)
    add_test(NAME integration.cli.edit_save
             COMMAND forge3d-cli --quiet new
                     run-op entity.create --params "{\"name\":\"A\",\"guid\":\"${_a}\"}"
                     run-op entity.create --params "{\"name\":\"B\",\"guid\":\"${_b}\",\"parent\":\"${_a}\"}"
                     run-op entity.create --params "{\"name\":\"C\",\"guid\":\"${_c}\"}"
                     run-op entity.create --params "{\"name\":\"D\",\"guid\":\"${_d}\"}"
                     run-op entity.set_property --params "{\"entity\":\"${_b}\",\"path\":\"Transform.translation\",\"value\":[1.5,2,-3]}"
                     run-op entity.set_property --params "{\"entity\":\"${_a}\",\"path\":\"Transform.rotation\",\"value\":[0.7071068,0,0.7071068,0]}"
                     run-op entity.reparent --params "{\"entity\":\"${_c}\",\"parent\":\"${_b}\"}"
                     run-op entity.delete --params "{\"entity\":\"${_d}\"}"
                     run-op layer.create --params "{\"name\":\"Props\"}"
                     undo undo undo redo redo
                     print --out ${_out}/expected.json
                     save ${_out}/edited.forge)
    set_tests_properties(integration.cli.edit_save PROPERTIES LABELS integration FIXTURES_SETUP cli_edit)
    add_test(NAME integration.cli.reload
             COMMAND forge3d-cli --quiet open ${_out}/edited.forge print --out ${_out}/reloaded.json)
    set_tests_properties(integration.cli.reload PROPERTIES
        LABELS integration FIXTURES_REQUIRED cli_edit FIXTURES_SETUP cli_reload)
    add_test(NAME integration.cli.reload.check
             COMMAND ${CMAKE_COMMAND} -E compare_files ${_out}/expected.json ${_out}/reloaded.json)
    set_tests_properties(integration.cli.reload.check PROPERTIES LABELS integration FIXTURES_REQUIRED cli_reload)

    # The format-v1 fixture opens, and invalid input fails with a non-zero exit code.
    add_test(NAME integration.cli.open_fixture
             COMMAND forge3d-cli open ${PROJECT_SOURCE_DIR}/tests/data/forge/v1_basic.forge info)
    set_tests_properties(integration.cli.open_fixture PROPERTIES LABELS integration PASS_REGULAR_EXPRESSION "entities: 7")
    # An independent ZIP reader (Python's zipfile) accepts what we write.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME integration.cli.zip_interop
                 COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/check_forge_zip.py ${_out}/edited.forge)
        set_tests_properties(integration.cli.zip_interop PROPERTIES LABELS integration FIXTURES_REQUIRED cli_edit)
        add_test(NAME unit.forge_fixture_zip
                 COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/check_forge_zip.py
                         ${PROJECT_SOURCE_DIR}/tests/data/forge/v1_basic.forge)
        set_tests_properties(unit.forge_fixture_zip PROPERTIES LABELS unit)
    endif()
    # P3.2: the import corpus (plus the glTF samples) cooks from the command line.
    set(_corpus ${_out}/import_corpus)
    add_test(NAME integration.cli.import.setup
             COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/tests/data/import ${_corpus})
    set_tests_properties(integration.cli.import.setup PROPERTIES LABELS integration FIXTURES_SETUP cli_import)
    add_test(NAME integration.cli.import
             COMMAND forge3d-cli import ${_corpus} --force)
    set_tests_properties(integration.cli.import PROPERTIES
        LABELS integration FIXTURES_REQUIRED cli_import PASS_REGULAR_EXPRESSION "8 assets, 8 imported, 0 failed")
    # P4.4: viewport.capture writes a PNG headlessly, in every view mode.
    set(_captures ${_out}/captures)
    file(MAKE_DIRECTORY ${_captures})
    set(_capture_ops)
    foreach(mode IN ITEMS lit unlit wireframe normals cascades)
        list(APPEND _capture_ops run-op viewport.capture --params
             "{\"path\":\"${_captures}/sky_${mode}.png\",\"width\":640,\"height\":360,\"mode\":\"${mode}\",\"bookmark\":\"View\"}")
    endforeach()
    add_test(NAME integration.cli.capture
             COMMAND forge3d-cli run tests/data/scenes/sky_1200.json ${_capture_ops}
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(integration.cli.capture PROPERTIES
        LABELS integration TIMEOUT 300 FIXTURES_SETUP cli_capture)
    add_test(NAME integration.cli.capture.check
             COMMAND forge3d-tests --source-file=*integration?editor?cli_capture* --no-version)
    set_tests_properties(integration.cli.capture.check PROPERTIES
        LABELS integration FIXTURES_REQUIRED cli_capture ENVIRONMENT "F3D_CLI_CAPTURE_DIR=${_captures}")
    add_test(NAME integration.cli.capture.bad_mode
             COMMAND forge3d-cli --quiet run-op viewport.capture --params "{\"path\":\"${_captures}/x.png\",\"mode\":\"sepia\"}")
    set_tests_properties(integration.cli.capture.bad_mode PROPERTIES LABELS integration WILL_FAIL TRUE)
    # P4.5: golden-image tests (label golden), rendered by a software rasteriser so
    # results don't depend on the GPU: WARP on Windows, lavapipe on Linux.
    if(WIN32)
        set(_golden_rhi d3d12)
    else()
        set(_golden_rhi vulkan)
    endif()
    file(GLOB _golden_specs CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/golden/*.json)
    foreach(spec IN LISTS _golden_specs)
        get_filename_component(_name ${spec} NAME_WE)
        add_test(NAME golden.${_name}
                 COMMAND forge3d-cli --rhi ${_golden_rhi} --adapter software golden ${spec}
                         --out ${CMAKE_BINARY_DIR}/test-output/golden
                 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(golden.${_name} PROPERTIES LABELS golden TIMEOUT 600)
    endforeach()
    # The runner catches a changed image: the morning scene against the noon reference fails.
    add_test(NAME golden.self_check
             COMMAND forge3d-cli --quiet --rhi ${_golden_rhi} --adapter software golden
                     ${PROJECT_SOURCE_DIR}/tests/data/golden/mismatch.json --out ${CMAKE_BINARY_DIR}/test-output/golden
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    # It must fail on the comparison (not for another reason, e.g. no device).
    set_tests_properties(golden.self_check PROPERTIES LABELS golden TIMEOUT 600
                         PASS_REGULAR_EXPRESSION "golden mismatch: SSIM 0\.[0-9]+")
    # P5.1: the sample plugin (plugins/ next to the CLI) adds an operation that runs and undoes.
    add_dependencies(forge3d-cli hello_op)
    add_test(NAME integration.cli.plugin_list COMMAND forge3d-cli list-ops)
    set_tests_properties(integration.cli.plugin_list PROPERTIES
        LABELS integration PASS_REGULAR_EXPRESSION "sample\.scatter_cubes")
    add_test(NAME integration.cli.plugin_run
             COMMAND forge3d-cli --quiet run-op sample.scatter_cubes --params "{\"count\":10,\"seed\":7}" info
                     undo info)
    set_tests_properties(integration.cli.plugin_run PROPERTIES
        LABELS integration PASS_REGULAR_EXPRESSION "entities: 11.*undo steps: 1.*entities: 0")
    # P6.1: terrain.create from the CLI (the Python test below covers the RPC surface).
    add_test(NAME integration.cli.terrain_run
             COMMAND forge3d-cli --quiet run-op terrain.create --params "{\"tiles\":2,\"size\":512}" info
                     undo info)
    set_tests_properties(integration.cli.terrain_run PROPERTIES
        LABELS integration PASS_REGULAR_EXPRESSION "entities: 1.*undo steps: 1.*entities: 0")
    add_test(NAME integration.cli.no_plugins COMMAND forge3d-cli --no-plugins --quiet run-op sample.scatter_cubes)
    set_tests_properties(integration.cli.no_plugins PROPERTIES LABELS integration WILL_FAIL TRUE)
    # P5.4 (M5 exit): the scene built by a Python script matches the one built over RPC.
    if(TARGET f3d_python)
        set(_m5 ${_out}/m5)
        file(MAKE_DIRECTORY ${_m5})
        add_test(NAME integration.python.m5_scene
                 COMMAND ${CMAKE_COMMAND} -E env F3D_M5_OUT=${_m5}/m5_python.png
                         $<TARGET_FILE:forge3d-cli> run tests/integration/python/m5_scene.py
                 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(integration.python.m5_scene PROPERTIES
            LABELS integration TIMEOUT 300 FIXTURES_SETUP m5_python)
        add_test(NAME integration.python.terrain
                 COMMAND ${CMAKE_COMMAND} -E env F3D_TERRAIN_OUT=${_m5}/terrain_python.png
                         $<TARGET_FILE:forge3d-cli> run tests/integration/python/terrain.py
                 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(integration.python.terrain PROPERTIES LABELS integration TIMEOUT 300)
        add_test(NAME integration.python.landscape
                 COMMAND ${CMAKE_COMMAND} -E env F3D_LANDSCAPE_OUT=${_m5}/landscape_python.png
                         $<TARGET_FILE:forge3d-cli> run tests/integration/python/landscape.py
                 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(integration.python.landscape PROPERTIES LABELS integration TIMEOUT 300)
        if(Python3_Interpreter_FOUND)
            add_test(NAME integration.python.m5_rpc
                     COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tests/integration/python/m5_rpc.py
                             --editor $<TARGET_FILE:forge3d-editor> --out ${_m5}/m5_rpc.png
                     WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
            set_tests_properties(integration.python.m5_rpc PROPERTIES
                LABELS integration TIMEOUT 300 SKIP_RETURN_CODE 77 FIXTURES_SETUP m5_rpc)
            add_test(NAME integration.python.m5_match
                     COMMAND forge3d-cli compare ${_m5}/m5_python.png ${_m5}/m5_rpc.png --ssim 0.99)
            set_tests_properties(integration.python.m5_match PROPERTIES
                LABELS integration FIXTURES_REQUIRED "m5_python;m5_rpc")
        endif()
    else()
        # Without Python, `run script.py` explains how to get it.
        add_test(NAME integration.cli.no_python
                 COMMAND forge3d-cli run ${PROJECT_SOURCE_DIR}/tests/integration/python/m5_scene.py)
        set_tests_properties(integration.cli.no_python PROPERTIES
            LABELS integration PASS_REGULAR_EXPRESSION "FORGE3D_WITH_PYTHON=ON")
    endif()
    add_test(NAME integration.cli.bad_op COMMAND forge3d-cli --quiet run-op does.not_exist)
    set_tests_properties(integration.cli.bad_op PROPERTIES LABELS integration WILL_FAIL TRUE)
    add_test(NAME integration.cli.list_ops COMMAND forge3d-cli list-ops --json)
    set_tests_properties(integration.cli.list_ops PROPERTIES
        LABELS integration PASS_REGULAR_EXPRESSION "\"id\": \"entity.set_property\"")
endif()
