#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""End-to-end test of the JSON-RPC server (phases.md P5.2).

Starts the editor headless with --rpc, then over a WebSocket: refuses a wrong token and an
unauthenticated call, builds a small scene, queries it, receives change events, captures
it, and undoes everything. Also checks the port isn't reachable on non-loopback addresses.

    python tests/integration/rpc/test_rpc.py --editor build/win-msvc/bin/Debug/forge3d-editor.exe

Exit code 0: passed, 1: failed, 77: skipped (the `websockets` package is missing).
"""
import argparse
import asyncio
import base64
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

try:
    import websockets
except ImportError:
    print("SKIP: the Python package 'websockets' is not installed (pip install websockets)")
    sys.exit(77)

GROUND = "00000000-0000-4000-8000-0000000005a1"
SKY = "00000000-0000-4000-8000-0000000005a2"
CUBE = "00000000-0000-4000-8000-0000000005a3"


class Failure(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


class Client:
    def __init__(self, ws):
        self.ws = ws
        self.next_id = 1
        self.events = []

    async def request(self, method, params=None):
        request_id = self.next_id
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            message["params"] = params
        await self.ws.send(json.dumps(message))
        while True:
            reply = json.loads(await asyncio.wait_for(self.ws.recv(), timeout=60))
            if reply.get("method") == "events.changed":
                self.events.append(reply["params"])
                continue
            check(reply.get("id") == request_id, f"reply for another request: {reply}")
            return reply

    async def result(self, method, params=None):
        reply = await self.request(method, params)
        check("result" in reply, f"{method} failed: {reply.get('error')}")
        return reply["result"]


async def expect_rejected(uri, first_message, expected_code):
    """The server answers with an error and closes the connection."""
    async with websockets.connect(uri) as ws:
        await ws.send(json.dumps(first_message))
        reply = json.loads(await asyncio.wait_for(ws.recv(), timeout=30))
        check(reply.get("error", {}).get("code") == expected_code, f"expected error {expected_code}, got {reply}")
        try:
            await asyncio.wait_for(ws.recv(), timeout=10)
            raise Failure("the connection stayed open after the rejection")
        except websockets.exceptions.ConnectionClosed:
            pass


def png_size(data):
    check(data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG")
    width, height = struct.unpack(">II", data[16:24])
    return width, height


async def session(uri, token):
    # A wrong token, and a call before authenticating, are refused and disconnected.
    await expect_rejected(uri, {"jsonrpc": "2.0", "id": 1, "method": "session.authenticate",
                                "params": {"token": "0" * len(token)}}, -32001)
    await expect_rejected(uri, {"jsonrpc": "2.0", "id": 1, "method": "ops.list"}, -32001)

    async with websockets.connect(uri, max_size=64 << 20) as ws:
        c = Client(ws)
        auth = await c.result("session.authenticate", {"token": token})
        check(auth["authenticated"] is True, "not authenticated")

        ops = {op["id"]: op for op in await c.result("ops.list")}
        check("entity.create" in ops, "ops.list misses entity.create")
        schema = ops["entity.set_property"]["schema"]
        check(schema["type"] == "object" and "entity" in schema["properties"], f"bad schema: {schema}")
        check(schema["properties"]["entity"].get("format") == "uuid", "GUID parameters are uuid strings")

        await c.result("events.subscribe", {"kinds": ["entity_created", "component_changed"]})

        steps = [
            ("entity.create", {"name": "Ground", "guid": GROUND}),
            ("entity.add_component", {"entity": GROUND, "component": "MeshRenderer"}),
            ("entity.create", {"name": "Cube", "guid": CUBE}),
            ("entity.add_component", {"entity": CUBE, "component": "MeshRenderer"}),
            ("entity.create", {"name": "Sky", "guid": SKY}),
            ("entity.add_component", {"entity": SKY, "component": "Sky"}),
        ]
        for op_id, params in steps:
            await c.result("ops.run", {"id": op_id, "params": params})
        box = {"path": "tests/data/gltf/Box.glb#mesh/0"}
        for entity in (GROUND, CUBE):
            await c.result("entity.set", {"entity": entity, "path": "MeshRenderer.mesh", "value": box})
        await c.result("entity.set", {"entity": GROUND, "path": "Transform.scale", "value": [20, 0.1, 20]})
        await c.result("entity.set", {"entity": GROUND, "path": "Transform.translation", "value": [0, -0.06, 0]})
        await c.result("entity.set", {"entity": CUBE, "path": "Transform.translation", "value": [0, 0.5, 0]})
        await c.result("entity.set", {"entity": SKY, "path": "Sky.time_of_day", "value": 15.0})
        total_steps = len(steps) + 6

        # Events arrived for what we subscribed to (and only that).
        kinds = {e["kind"] for e in c.events}
        check("entity_created" in kinds, f"no entity_created events: {c.events}")
        check("component_changed" in kinds, f"no component_changed events: {c.events}")
        check(kinds <= {"entity_created", "component_changed"}, f"unsubscribed events: {kinds}")
        check(any(e["entity"] == GROUND for e in c.events), "no event names the ground")

        everything = await c.result("scene.query")
        check(len(everything) == 3, f"expected 3 entities, got {everything}")
        skies = await c.result("scene.query", {"component": "Sky"})
        check([e["guid"] for e in skies] == [SKY], f"component filter: {skies}")
        cubes = await c.result("scene.query", {"name": "cub"})
        check([e["name"] for e in cubes] == ["Cube"], f"name filter: {cubes}")

        ground = await c.result("entity.get", {"entity": GROUND})
        check(ground["components"]["Transform"]["scale"] == [20, 0.1, 20] or
              all(abs(a - b) < 1e-5 for a, b in zip(ground["components"]["Transform"]["scale"], [20, 0.1, 20])),
              f"entity.get Transform: {ground['components'].get('Transform')}")
        check(ground["components"]["MeshRenderer"]["mesh"]["path"] == box["path"], "entity.get MeshRenderer")

        shot = await c.result("viewport.capture", {"width": 320, "height": 180})
        width, height = png_size(base64.b64decode(shot["png_base64"]))
        check((width, height) == (320, 180), f"capture size {width}x{height}")

        bad = await c.request("ops.run", {"id": "entity.delete", "params": {"entity": "not-a-guid"}})
        check("error" in bad, "a bad operation must fail")
        unknown = await c.request("no.such.method")
        check(unknown["error"]["code"] == -32601, f"unknown method: {unknown}")

        # Undo everything, then the scene is empty again.
        undone = 0
        while (await c.result("ops.undo"))["done"]:
            undone += 1
        check(undone == total_steps, f"undid {undone} steps, expected {total_steps}")
        check(await c.result("scene.query") == [], "the scene isn't empty after undoing everything")


def loopback_only(port):
    """Every non-loopback IPv4 address of this machine refuses the port."""
    addresses = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addresses.add(info[4][0])
    except socket.gaierror:
        pass
    addresses = {a for a in addresses if not a.startswith("127.")}
    if not addresses:
        print("note: no non-loopback address to try")
    for address in sorted(addresses):
        try:
            with socket.create_connection((address, port), timeout=3):
                raise Failure(f"the RPC port is reachable on {address}")
        except (ConnectionRefusedError, TimeoutError, socket.timeout, OSError) as e:
            if isinstance(e, Failure):
                raise
            print(f"ok: {address}:{port} refused ({type(e).__name__})")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True)
    args = parser.parse_args()

    workdir = tempfile.mkdtemp(prefix="forge3d-rpc-")
    token_file = os.path.join(workdir, "rpc.json")
    log = open(os.path.join(workdir, "editor.log"), "w")
    editor = subprocess.Popen([args.editor, "--hidden", "--rpc=0", "--rpc-token-file", token_file],
                              stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.time() + 90
        while not os.path.exists(token_file):
            if editor.poll() is not None:
                raise Failure(f"the editor exited ({editor.returncode}); see {log.name}")
            if time.time() > deadline:
                raise Failure("the editor didn't start the RPC server")
            time.sleep(0.2)
        time.sleep(0.2)
        with open(token_file) as f:
            info = json.load(f)
        port, token = info["port"], info["token"]
        check(len(token) == 64, "a 256-bit token")
        print(f"editor RPC on 127.0.0.1:{port}")
        asyncio.run(session(f"ws://127.0.0.1:{port}", token))
        loopback_only(port)
        print("PASS")
        return 0
    except Failure as e:
        print(f"FAIL: {e}")
        return 1
    finally:
        editor.terminate()
        try:
            editor.wait(timeout=20)
        except subprocess.TimeoutExpired:
            editor.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
