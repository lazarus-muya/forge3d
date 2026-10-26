#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The M5 scene over JSON-RPC (phases.md P5.4): the same steps as m5_scene.py, against a
headless editor, capturing to the given path for comparison with the Python capture.

    python tests/integration/python/m5_rpc.py --editor <forge3d-editor> --out m5_rpc.png

Exit code 0: done, 1: failed, 77: skipped (no `websockets` package).
"""
import argparse
import asyncio
import json
import os
import subprocess
import sys
import tempfile
import time

try:
    import websockets
except ImportError:
    print("SKIP: the Python package 'websockets' is not installed")
    sys.exit(77)

GROUND = "00000000-0000-4000-8000-0000000007a1"
CUBE = "00000000-0000-4000-8000-0000000007a2"
SKY = "00000000-0000-4000-8000-0000000007a3"
BOX = {"path": "tests/data/gltf/Box.glb#mesh/0"}

STEPS = [
    ("entity.create", {"name": "Ground", "guid": GROUND}),
    ("entity.add_component", {"entity": GROUND, "component": "MeshRenderer"}),
    ("entity.set_property", {"entity": GROUND, "path": "MeshRenderer.mesh", "value": BOX}),
    ("entity.set_property", {"entity": GROUND, "path": "Transform.scale", "value": [20, 0.1, 20]}),
    ("entity.set_property", {"entity": GROUND, "path": "Transform.translation", "value": [0, -0.06, 0]}),
    ("entity.create", {"name": "Cube", "guid": CUBE}),
    ("entity.add_component", {"entity": CUBE, "component": "MeshRenderer"}),
    ("entity.set_property", {"entity": CUBE, "path": "MeshRenderer.mesh", "value": BOX}),
    ("entity.set_property", {"entity": CUBE, "path": "Transform.translation", "value": [0, 0.5, 0]}),
    ("entity.create", {"name": "Sky", "guid": SKY}),
    ("entity.add_component", {"entity": SKY, "component": "Sky"}),
    ("entity.set_property", {"entity": SKY, "path": "Sky.time_of_day", "value": 15.0}),
    ("bookmark.save", {"name": "View", "target": [0, 0.5, 0], "distance": 8.0, "yaw": 0.6, "pitch": -0.3}),
]


async def build(uri, token, out):
    async with websockets.connect(uri, max_size=64 << 20) as ws:
        next_id = 0

        async def call(method, params=None):
            nonlocal next_id
            next_id += 1
            await ws.send(json.dumps({"jsonrpc": "2.0", "id": next_id, "method": method, "params": params or {}}))
            while True:
                reply = json.loads(await asyncio.wait_for(ws.recv(), timeout=120))
                if reply.get("id") == next_id:
                    if "error" in reply:
                        raise RuntimeError(f"{method}: {reply['error']}")
                    return reply["result"]

        await call("session.authenticate", {"token": token})
        for op_id, params in STEPS:
            await call("ops.run", {"id": op_id, "params": params})
        await call("viewport.capture", {"path": out, "width": 480, "height": 270, "bookmark": "View"})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    workdir = tempfile.mkdtemp(prefix="forge3d-m5-")
    token_file = os.path.join(workdir, "rpc.json")
    editor = subprocess.Popen([args.editor, "--hidden", "--rpc=0", "--rpc-token-file", token_file],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + 90
        while not os.path.exists(token_file):
            if editor.poll() is not None or time.time() > deadline:
                print("FAIL: the editor didn't start its RPC server")
                return 1
            time.sleep(0.2)
        time.sleep(0.2)
        with open(token_file) as f:
            info = json.load(f)
        asyncio.run(build(f"ws://127.0.0.1:{info['port']}", info["token"], os.path.abspath(args.out)))
        print(f"m5_rpc: captured to {args.out}")
        return 0
    except Exception as e:  # noqa: BLE001 - report anything as a failure
        print(f"FAIL: {e}")
        return 1
    finally:
        editor.terminate()
        try:
            editor.wait(timeout=20)
        except subprocess.TimeoutExpired:
            editor.kill()


if __name__ == "__main__":
    sys.exit(main())
