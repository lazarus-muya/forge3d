# SPDX-License-Identifier: Apache-2.0
# Overlay port: bc7enc_rdo, pinned by commit and SHA512. Only the encoders
# (bc7enc.cpp: BC7; rgbcx.cpp: BC1/BC3/BC4/BC5) and the BC7 decoder are built;
# the RDO tool, ISPC encoder and bundled lodepng/miniz are not.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO richgel999/bc7enc_rdo
    REF b9438627eef73a1157e84201b6fa6eb2ffd6d9f0
    SHA512 ddb91ebcc2899fc81efdb53883c117653ffb4055049e4e7c41ee2c41635f70c5aa95e5c36e2dede7d8a541f5d9cbb06dc83fa116a5f04b0cf14b3beebfcb3f70
    HEAD_REF master
)
# Replaces upstream's CMakeLists.txt (which builds the command-line tool).
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME bc7enc_rdo CONFIG_PATH share/bc7enc_rdo)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
