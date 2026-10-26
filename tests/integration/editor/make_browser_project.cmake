# SPDX-License-Identifier: Apache-2.0
# Builds the asset-browser test project: the import corpus plus the glTF samples,
# in a fresh directory (cmake -DSOURCE=<tests/data> -DDEST=<dir> -P this file).
file(REMOVE_RECURSE "${DEST}")
file(COPY "${SOURCE}/import/" DESTINATION "${DEST}")
file(COPY "${SOURCE}/gltf/Box.glb" "${SOURCE}/gltf/Fox.glb" DESTINATION "${DEST}/models")
