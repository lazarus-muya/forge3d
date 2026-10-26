#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""A scripted MCP client against forge3d-mcp over stdio (phases.md P5.3).

forge3d-mcp starts its own headless editor (--spawn-editor). The client initializes,
lists the tools (generated from the editor's operations), runs entity.create through
its tool, reads the scene resource, takes a screenshot (an image), undoes, and closes
stdin; the bridge must then exit and stop its editor.

    python tests/integration/mcp/test_mcp.py --mcp build/win-msvc/bin/Debug/forge3d-mcp.exe
"""
import argparse
import base64
import json
import queue
import struct
import subprocess
import sys
import threading

ROCK = "00000000-0000-4000-8000-0000000006a1"


class Failure(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


class McpClient:
    def __init__(self, command):
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=sys.stderr,
                                     text=True, encoding="utf-8", bufsize=1)
        self.lines = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()
        self.next_id = 1

    def _read(self):
        for line in self.proc.stdout:
            self.lines.put(line)
        self.lines.put(None)

    def send(self, message):
        self.proc.stdin.write(json.dumps(message) + "\n")
        self.proc.stdin.flush()

    def request(self, method, params=None, timeout=180):
        request_id = self.next_id
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            message["params"] = params
        self.send(message)
        while True:
            try:
                line = self.lines.get(timeout=timeout)
            except queue.Empty:
                raise Failure(f"no reply to {method}")
            if line is None:
                raise Failure(f"forge3d-mcp exited while waiting for {method} ({self.proc.poll()})")
            try:
                reply = json.loads(line)
            except json.JSONDecodeError:
                raise Failure(f"stdout is not MCP JSON: {line!r}")
            if reply.get("id") == request_id:
                return reply

    def result(self, method, params=None):
        reply = self.request(method, params)
        check("result" in reply, f"{method} failed: {reply.get('error')}")
        return reply["result"]

    def tool(self, name, arguments=None):
        result = self.result("tools/call", {"name": name, "arguments": arguments or {}})
        check(not result.get("isError"), f"tool {name} failed: {result}")
        return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mcp", required=True)
    args = parser.parse_args()
    client = McpClient([args.mcp, "--spawn-editor"])
    try:
        init = client.result("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                            "clientInfo": {"name": "forge3d-test", "version": "1"}})
        check(init["protocolVersion"] == "2025-06-18", f"protocol: {init}")
        check(init["serverInfo"]["name"] == "forge3d", "server name")
        check("tools" in init["capabilities"] and "resources" in init["capabilities"], "capabilities")
        client.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        check(client.result("ping") == {}, "ping")

        tools = {t["name"]: t for t in client.result("tools/list")["tools"]}
        for name in ("entity_create", "entity_set_property", "screenshot", "undo", "scene_query", "entity_get"):
            check(name in tools, f"tools/list misses {name}")
        check(all(c.isalnum() or c in "_-" for t in tools for c in t), "tool names are [A-Za-z0-9_-]")
        create = tools["entity_create"]["inputSchema"]
        check(create["type"] == "object" and "name" in create["properties"], f"entity_create schema: {create}")
        print(f"{len(tools)} tools")

        created = client.tool("entity_create", {"name": "Rock", "guid": ROCK})
        check(created["structuredContent"]["undo_steps"] == 1, f"entity_create: {created}")
        client.tool("entity_set_property", {"entity": ROCK, "path": "Transform.translation", "value": [1, 2, 3]})

        resources = {r["uri"] for r in client.result("resources/list")["resources"]}
        check({"forge3d://scene", "forge3d://selection", "forge3d://assets"} <= resources, f"resources: {resources}")
        scene = client.result("resources/read", {"uri": "forge3d://scene"})["contents"][0]
        check(scene["mimeType"] == "application/json", "scene resource type")
        entities = json.loads(scene["text"])
        check([e["name"] for e in entities] == ["Rock"], f"scene resource: {entities}")
        rock = json.loads(client.tool("entity_get", {"entity": ROCK})["content"][0]["text"])
        check(rock["components"]["Transform"]["translation"] == [1, 2, 3], f"entity_get: {rock}")
        selection = json.loads(client.result("resources/read", {"uri": "forge3d://selection"})["contents"][0]["text"])
        check(selection["entities"] == [], f"selection: {selection}")

        shot = client.tool("screenshot", {"width": 200, "height": 120})
        image = shot["content"][0]
        check(image["type"] == "image" and image["mimeType"] == "image/png", f"screenshot content: {image.keys()}")
        png = base64.b64decode(image["data"])
        check(png[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG")
        check(struct.unpack(">II", png[16:24]) == (200, 120), "screenshot size")

        failed = client.result("tools/call", {"name": "entity_delete", "arguments": {"entity": "nope"}})
        check(failed.get("isError") is True, f"a failing operation is a tool error: {failed}")
        unknown = client.result("tools/call", {"name": "no_such_tool", "arguments": {}})
        check(unknown.get("isError") is True, "unknown tool")
        check("error" in client.request("no/such/method"), "unknown MCP method")

        client.tool("undo")
        client.tool("undo")
        check(json.loads(client.tool("scene_query")["content"][0]["text"]) == [], "undo emptied the scene")

        client.proc.stdin.close()
        code = client.proc.wait(timeout=60)
        check(code == 0, f"forge3d-mcp exit code {code}")
        print("PASS")
        return 0
    except Failure as e:
        print(f"FAIL: {e}")
        return 1
    finally:
        if client.proc.poll() is None:
            client.proc.kill()


if __name__ == "__main__":
    sys.exit(main())
