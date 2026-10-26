# SPDX-License-Identifier: Apache-2.0
# The forge3d Python module and embedded interpreter (phases.md P5.4), built only with
# FORGE3D_WITH_PYTHON=ON (the -full presets, which enable the vcpkg feature "python").
find_package(Python 3.12 COMPONENTS Interpreter Development.Module Development.Embed REQUIRED)
find_package(nanobind CONFIG REQUIRED)
nanobind_build_library(nanobind-static)
# Debug builds link vcpkg's debug CPython (python3XX_d). nanobind hides _DEBUG from
# Python.h unless Py_DEBUG is set, which would compile against the release layout
# (PyConfig and friends differ): say Py_DEBUG so headers and library agree.
target_compile_definitions(nanobind-static PUBLIC $<$<CONFIG:Debug>:Py_DEBUG>)

f3d_add_module(f3d_python
    SOURCES src/python.cpp
    DEPS f3d_api)
target_link_libraries(f3d_python PRIVATE nanobind-static Python::Python)
target_compile_definitions(f3d_python PUBLIC F3D_WITH_PYTHON=1)
# Development builds find the standard library of vcpkg's Python here (packages ship
# their own copy next to the executable, P11.2).
get_filename_component(_f3d_python_home "${Python_EXECUTABLE}" DIRECTORY)
target_compile_definitions(f3d_python PRIVATE F3D_PYTHON_HOME="${_f3d_python_home}")
