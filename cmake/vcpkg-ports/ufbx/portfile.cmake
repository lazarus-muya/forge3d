# SPDX-License-Identifier: Apache-2.0
# Overlay port: ufbx v0.23.1, pinned by commit and SHA512.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ufbx/ufbx
    REF 26a482ae66871d7de36eb722aa060bce95bce274 # v0.23.1
    SHA512 af896251f678abb2d721316a3f3f9facb59f35d67a8874fec561ce25396f25a10de39876bc2ad5542a00ece5a8427d020772434acfa663fb82bd81d46c34a7be
    HEAD_REF master
)
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME ufbx CONFIG_PATH share/ufbx)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
