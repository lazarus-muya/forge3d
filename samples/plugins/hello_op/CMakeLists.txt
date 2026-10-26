# SPDX-License-Identifier: Apache-2.0
# A sample plugin (phases.md P5.1), in plain C to show the C ABI needs nothing else.
# Built into bin/<config>/plugins/, where the editor and the CLI load it from.
add_library(hello_op MODULE hello_op.c)
target_include_directories(hello_op PRIVATE ${PROJECT_SOURCE_DIR}/engine/api/include)
set_target_properties(hello_op PROPERTIES
    PREFIX ""
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/plugins)
if(MSVC)
    target_compile_options(hello_op PRIVATE /W4 /WX /utf-8)
    target_compile_definitions(hello_op PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(hello_op PRIVATE -Wall -Wextra -Werror)
endif()
