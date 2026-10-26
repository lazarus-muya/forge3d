#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Streaming stress run (phases.md P3.5, M3 exit scaled down).

Generates a project with 200 uncompressed 1024^2 textures (~5.6 MB each on the
GPU with mips, ~1.1 GB if all were resident at full resolution) on 200 quads,
runs the editor with --vram-cap and --bench, and checks the bench report: the
streamed assets must stay under the cap.

    python tools/stress_streaming.py --editor build/win-msvc/bin/Release/forge3d-editor.exe \
        --cli build/win-msvc/bin/Release/forge3d-cli.exe --out build/stress_streaming
"""

import argparse
import json
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

SIZE = 1024
COUNT = 200
COLUMNS = 20


def png(path: Path, seed: int) -> None:
    """A distinct RGBA pattern per texture (bands and a grid), small to compress."""
    r, g, b = (seed * 37) % 256, (seed * 91) % 256, (seed * 53) % 256
    rows = []
    for y in range(SIZE):
        band = (y // 64) % 2
        line = bytearray()
        line.append(0)  # filter: none
        a = bytes((r ^ (band * 255), g, b, 255))
        c = bytes((r, g ^ (band * 255), b ^ 128, 255))
        segment = (a * 64) + (c * 64)
        line += segment * (SIZE // 128)
        rows.append(bytes(line))
    raw = b"".join(rows)

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 6)) +
                     chunk(b"IEND", b""))


def meta(path: Path, guid: str, importer: str, settings: dict) -> None:
    doc = {"format": "forge3d.meta", "version": 1, "guid": guid, "importer": importer, "settings": settings,
           "source_hash": ""}
    Path(str(path) + ".meta").write_text(json.dumps(doc, indent=2) + "\n")


def guid(n: int) -> str:
    return f"00000000-0000-4000-8000-{n:012x}"


def generate(project: Path) -> Path:
    if project.exists():
        shutil.rmtree(project)
    (project / "textures").mkdir(parents=True)
    quad = project / "quad.obj"
    quad.write_text("v -1 -1 0\nv 1 -1 0\nv 1 1 0\nv -1 1 0\n"
                    "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\n"
                    "f 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n")
    meta(quad, guid(0xA000), "obj", {})
    ops = []
    for i in range(COUNT):
        texture = project / "textures" / f"t{i:03d}.png"
        png(texture, i)
        meta(texture, guid(0xB000 + i), "texture", {"compression": "none"})
        entity = guid(0xC000 + i)
        x = (i % COLUMNS) * 2.2 - COLUMNS * 1.1
        y = (i // COLUMNS) * 2.2 + 1.0
        ops += [
            {"op": "entity.create", "params": {"name": f"Quad {i}", "guid": entity}},
            {"op": "entity.set_property", "params": {"entity": entity, "path": "Transform.translation",
                                                     "value": [x, y, 0.0]}},
            {"op": "entity.add_component", "params": {"entity": entity, "component": "MeshRenderer"}},
            {"op": "entity.set_property", "params": {"entity": entity, "path": "MeshRenderer.mesh",
                                                     "value": {"guid": guid(0xA000),
                                                               "path": "project://quad.obj#mesh/0"}}},
            {"op": "entity.set_property", "params": {"entity": entity, "path": "MeshRenderer.texture",
                                                     "value": {"guid": guid(0xB000 + i),
                                                               "path": f"project://textures/t{i:03d}.png"}}},
        ]
    # Close to the wall: the nearest quads want mip 0, the far ones coarse mips.
    ops += [
        {"op": "bookmark.save", "params": {"name": "Stress", "target": [-10.0, 6.0, 0.0], "distance": 5.0,
                                           "yaw": 0.6, "pitch": 0.1}},
        {"op": "bookmark.recall", "params": {"name": "Stress"}},
    ]
    script = project / "stress_ops.json"
    script.write_text(json.dumps({"format": "forge3d.ops", "version": 1, "ops": ops}, indent=1) + "\n")
    return script


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--editor", required=True, type=Path)
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--cap-mb", type=int, default=512)
    parser.add_argument("--frames", type=int, default=240)
    args = parser.parse_args()

    project = args.out / "project"
    script = generate(project)
    scene = project / "stress.forge"
    subprocess.run([str(args.cli), "new", "save", str(scene)], check=True)
    report = args.out / "bench.json"
    command = [str(args.editor), "--hidden", "--frames", str(args.frames), "--open", str(scene), "--scene",
               str(script), "--scan-assets", "--vram-cap", str(args.cap_mb), "--bench", str(report),
               "--capture", str(args.out / "stress.png")]
    print(" ".join(command))
    subprocess.run(command, check=True)
    bench = json.loads(report.read_text())
    residency = bench["residency"]
    print(json.dumps(residency))
    ok = residency["peak_mb"] <= args.cap_mb and residency["used_mb"] <= args.cap_mb and residency["textures"] > 0
    print("PASS" if ok else "FAIL", f"peak {residency['peak_mb']} MB of {args.cap_mb} MB cap,",
          f"{residency['textures']} textures resident, RAM {bench['ram_mb']['working_set']} MB")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
