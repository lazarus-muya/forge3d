# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.28)

project(forge3d
    VERSION 0.1.0
    DESCRIPTION "Lightweight open-source 3D landscape and scene builder"
    LANGUAGES C CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use a preset: cmake --preset <name>")
endif()

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(F3DOptions)
include(F3DLayering)
include(F3DShaders)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)

enable_testing()

add_subdirectory(engine)
if(FORGE3D_WITH_PYTHON)
    add_subdirectory(modules/python)
endif()
add_subdirectory(shaders)
add_subdirectory(editor)
add_subdirectory(tools)
add_subdirectory(samples)
if(F3D_BUILD_TESTS)
    add_subdirectory(tests)
endif()
if(F3D_BUILD_SPIKES)
    add_subdirectory(spikes)
endif()

f3d_verify_layering()
