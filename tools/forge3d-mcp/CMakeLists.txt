# SPDX-License-Identifier: Apache-2.0
# forge3d-mcp (phases.md P5.3): an MCP server on stdio, bridging to the editor's JSON-RPC.
find_package(ixwebsocket CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(forge3d-mcp main.cpp)
target_link_libraries(forge3d-mcp PRIVATE f3d_options f3d_core ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json)
# It starts the headless editor next to it when no editor is running.
add_dependencies(forge3d-mcp forge3d-editor)

if(F3D_BUILD_TESTS)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        # A scripted MCP client over stdio: initialize, list tools, run entity.create, screenshot.
        add_test(NAME integration.mcp
                 COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tests/integration/mcp/test_mcp.py
                         --mcp $<TARGET_FILE:forge3d-mcp>
                 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(integration.mcp PROPERTIES LABELS integration TIMEOUT 300)
    endif()
endif()
