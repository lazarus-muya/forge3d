# SPDX-License-Identifier: Apache-2.0
"""Terrain through the Python/RPC surface (phases.md P6.1 - P6.3, Gate C rule).

    forge3d-cli run tests/integration/python/terrain.py

Creates a 1k noise terrain with terrain.create, checks its component over the scene API,
sculpts it (a flatten pad, a raised ridge, a terrace pass), erodes it (twice, the same
hash), checks heights with terrain_sample and terrain_info's hash, captures it, and checks that undo restores the
heights exactly and that redo removes and restores the terrain. The capture goes to
$F3D_TERRAIN_OUT (default terrain_python.png).
"""
import os

import forge3d

TERRAIN = "00000000-0000-4000-8000-0000000009a1"
SKY = "00000000-0000-4000-8000-0000000009a2"

ops = forge3d.ops
ops.terrain.create(name="Hills", guid=TERRAIN, tiles=4, size=1024.0, max_height=180.0, seed=4, frequency=3.0)
ops.entity.create(name="Sky", guid=SKY)
ops.entity.add_component(entity=SKY, component="Sky")
ops.bookmark.save(name="Overview", target=[0, 60, 0], distance=900.0, yaw=0.6, pitch=-0.45)
created = forge3d.terrain_info(TERRAIN)
assert created["samples"] == 1025 and created["size"] == 1024.0, created

# Sculpt: a flat pad at 120 m, a ridge raised along x, a terrace pass over it.
ops.terrain.sculpt_stroke(terrain=TERRAIN, tool="flatten", use_target=True, target_height=120.0, strength=1.0,
                          radius=60.0, falloff="constant", points=[[-200, 200, 1.0], [-150, 200, 1.0]])
pad = forge3d.terrain_sample(TERRAIN, [[-200, 200], [-175, 200]])
assert all(abs(h - 120.0) < 1e-3 for h in pad), pad
ridge = [[x, -150.0, 0.5 + x / 800.0] for x in range(-300, 301, 20)]
before = forge3d.terrain_sample(TERRAIN, [[0, -150]])[0]
ops.terrain.sculpt_stroke(terrain=TERRAIN, tool="raise", strength=1.0, radius=40.0, points=ridge)
assert forge3d.terrain_sample(TERRAIN, [[0, -150]])[0] > before + 2.0  # ~4 m from overlapping dabs
ops.terrain.sculpt_stroke(terrain=TERRAIN, tool="terrace", terrace_step=10.0, strength=1.0, radius=80.0,
                          falloff="constant", points=[[0, -150, 1.0]])
terraced = forge3d.terrain_sample(TERRAIN, [[0, -150]])[0] % 10.0  # (0, -150) is a sample: no interpolation
assert min(terraced, 10.0 - terraced) < 1e-3, terraced
sculpted = forge3d.terrain_info(TERRAIN)["hash"]
assert sculpted != created["hash"]

# Erosion (P6.3): deterministic for the same seed and parameters.
ops.terrain.erode(terrain=TERRAIN, mode="both", iterations=30, seed=3)
eroded = forge3d.terrain_info(TERRAIN)["hash"]
assert eroded != sculpted
assert forge3d.undo()
assert forge3d.terrain_info(TERRAIN)["hash"] == sculpted
ops.terrain.erode(terrain=TERRAIN, mode="both", iterations=30, seed=3)
assert forge3d.terrain_info(TERRAIN)["hash"] == eroded
STEPS = 8

terrains = forge3d.scene_query(component="Terrain")
assert [t["guid"] for t in terrains] == [TERRAIN], terrains
terrain = forge3d.entity_get(TERRAIN)["components"]["Terrain"]
assert terrain["tiles"] == 4 and terrain["size"] == 1024.0, terrain
assert (terrain["min_height"], terrain["max_height"]) == (0.0, 180.0), terrain

# Invalid parameters are rejected and leave nothing behind.
try:
    ops.terrain.create(name="Bad", format="r8")
    raise AssertionError("terrain.create with format r8 must fail")
except forge3d.Error:
    pass
assert len(forge3d.scene_query(component="Terrain")) == 1

out = os.environ.get("F3D_TERRAIN_OUT", "terrain_python.png")
shot = forge3d.capture(out, width=480, height=270, bookmark="Overview")
assert (shot["width"], shot["height"]) == (480, 270)
assert os.path.getsize(out) > 1000

# Undoing the erosion and the three strokes gives back the generated heights, bit for bit.
for _ in range(4):
    assert forge3d.undo()
assert forge3d.terrain_info(TERRAIN)["hash"] == created["hash"]
for _ in range(4):
    assert forge3d.redo()
assert forge3d.terrain_info(TERRAIN)["hash"] == eroded

undone = 0
while forge3d.undo():
    undone += 1
assert undone == STEPS, undone
assert forge3d.scene_query() == []

# Redo recreates the terrain with the same GUID (the op keeps its heights).
assert forge3d.redo()
assert [t["guid"] for t in forge3d.scene_query(component="Terrain")] == [TERRAIN]
assert forge3d.terrain_info(TERRAIN)["hash"] == created["hash"]
assert forge3d.undo()
print("terrain: created, captured to %s, undid %d steps, redo ok" % (out, undone))
