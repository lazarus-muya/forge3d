# SPDX-License-Identifier: Apache-2.0
"""Download optional test assets that are not committed to the repository.

DamagedHelmet (Khronos glTF-Sample-Assets) is derived from a CC-BY-NC-4.0 model,
so it is not redistributed with forge3d. MetalRoughSpheres (CC-BY-4.0) is
redistributable but large (11 MB). Both are used locally for visual checks
(P1.4, P4.2):

    python tools/fetch_test_assets.py
    forge3d-editor --open tests/data/gltf/DamagedHelmet.glb --capture helmet.png
"""
import hashlib
import pathlib
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASE = "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models"
ASSETS = [
    # (url, destination, notice)
    (f"{BASE}/DamagedHelmet/glTF-Binary/DamagedHelmet.glb", "tests/data/gltf/DamagedHelmet.glb",
     "Damaged Helmet: CC-BY-4.0 (ctxwing, 2018) and CC-BY-NC-4.0 (theblueturtle_, 2016). Non-commercial use only."),
    (f"{BASE}/MetalRoughSpheres/glTF-Binary/MetalRoughSpheres.glb", "tests/data/gltf/MetalRoughSpheres.glb",
     "Metal-Rough Spheres: CC-BY-4.0, (c) 2017 Analytical Graphics, Inc. (Ed Mackey)."),
]


def main():
    for url, dest, notice in ASSETS:
        path = ROOT / dest
        if path.exists():
            print(f"present: {dest}")
            continue
        print(f"downloading {url}")
        path.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(url, timeout=120) as response:
            data = response.read()
        path.write_bytes(data)
        print(f"  -> {dest} ({len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()[:16]}...)")
        print(f"  licence: {notice}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
