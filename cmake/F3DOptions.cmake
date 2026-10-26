# SPDX-License-Identifier: Apache-2.0
#
# Project-wide build options and the `f3d_options` interface target that every
# forge3d target links privately (C++20, warnings as errors, sanitizers).

include_guard(GLOBAL)

option(F3D_BUILD_TESTS "Build forge3d-tests and register ctest tests" ON)
option(F3D_BUILD_SPIKES "Build the throw-away technology spikes in spikes/" OFF)
option(F3D_TRACY "Compile Tracy profiler zones into F3D_ZONE / F3D_FRAME_MARK" OFF)
option(F3D_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
option(FORGE3D_WITH_PYTHON "Build the Python module and embedded interpreter (modules/python; needs the vcpkg feature python)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No C++20 modules are used; skip the per-file dependency scan.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

add_library(f3d_options INTERFACE)
target_compile_features(f3d_options INTERFACE cxx_std_20)

if(MSVC)
    # MSVC and clang-cl share the cl-style command line.
    target_compile_options(f3d_options INTERFACE
        /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /external:W0
        $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor /MP>)
    target_compile_definitions(f3d_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_link_options(f3d_options INTERFACE /WX)
else()
    target_compile_options(f3d_options INTERFACE -Wall -Wextra -Werror)
endif()

if(F3D_SANITIZE)
    if(MSVC)
        message(WARNING "F3D_SANITIZE: only ASan is available with MSVC; enabling /fsanitize=address")
        target_compile_options(f3d_options INTERFACE /fsanitize=address)
    else()
        target_compile_options(f3d_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer
                                                     -fno-sanitize-recover=undefined)
        target_link_options(f3d_options INTERFACE -fsanitize=address,undefined)
    endif()
endif()
