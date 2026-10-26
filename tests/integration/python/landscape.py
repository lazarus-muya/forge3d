# SPDX-License-Identifier: Apache-2.0
"""A whole landscape through the Python/RPC surface (phases.md P6.4, P7.1, P7.3, P7.4;
Gate C rule).

    forge3d-cli run tests/integration/python/landscape.py

Builds a terrain, gives it the grassland auto material, paints a layer, adds a
paint-only scatter and paints it, lays a road and a river with spline.create and
fills a lake with water.create. Checks that painting changes the splat hash and undo
restores it, that splines shape the surface but never the sculpted heights, that the
lake is a WaterBody at its level, and that undoing everything gives back the generated
terrain and then an empty scene. The capture goes to $F3D_LANDSCAPE_OUT (default
landscape_python.png).
"""
import os

import forge3d

TERRAIN = "00000000-0000-4000-8000-000000000a01"
SKY = "00000000-0000-4000-8000-000000000a02"
TREES = "00000000-0000-4000-8000-000000000a03"
ROAD = "00000000-0000-4000-8000-000000000a04"
RIVER = "00000000-0000-4000-8000-000000000a05"
LAKE = "00000000-0000-4000-8000-000000000a06"

ops = forge3d.ops
steps = 0


def run(op, **params):
    global steps
    getattr(getattr(ops, op.split(".")[0]), op.split(".")[1])(**params)
    steps += 1


def ground(points, surface=False):
    hs = forge3d.terrain_sample(TERRAIN, [[p[0], p[1]] for p in points], surface=surface)
    return [[p[0], h, p[1]] for p, h in zip(points, hs)]


run("terrain.create", name="Valley", guid=TERRAIN, tiles=2, size=512.0, max_height=80.0, seed=7, frequency=2.0)
run("entity.create", name="Sky", guid=SKY)
run("entity.add_component", entity=SKY, component="Sky")
created = forge3d.terrain_info(TERRAIN)
assert created["surface_hash"] == created["hash"], created  # no splines yet

# Layers (P6.4): an auto material, then a hand-painted patch of layer 1.
run("terrain.auto_material", terrain=TERRAIN, preset="grassland")
material = forge3d.terrain_info(TERRAIN)
assert material["layers"] >= 3, material
assert material["hash"] == created["hash"]  # painting never moves the ground
run("terrain.paint_stroke", terrain=TERRAIN, layer=1, radius=30.0, strength=1.0, falloff="constant",
    points=[[-100, -100, 1.0], [-60, -100, 1.0]])
painted = forge3d.terrain_info(TERRAIN)
assert painted["splat_hash"] != material["splat_hash"], painted
assert forge3d.undo()
assert forge3d.terrain_info(TERRAIN)["splat_hash"] == material["splat_hash"]
assert forge3d.redo()
assert forge3d.terrain_info(TERRAIN)["splat_hash"] == painted["splat_hash"]

# Foliage (P7.1): a paint-only scatter, painted in with the density brush.
run("entity.create", name="Trees", guid=TREES)
run("entity.add_component", entity=TREES, component="Scatter")
run("entity.set_property", entity=TREES, path="Scatter.terrain", value=TERRAIN)
run("entity.set_property", entity=TREES, path="Scatter.meshes", value=[{"guid": "", "path": "builtin:scatter/pine"}])
run("entity.set_property", entity=TREES, path="Scatter.area", value=[400.0, 400.0])
run("entity.set_property", entity=TREES, path="Scatter.paint_only", value=True)
run("scatter.paint_stroke", scatter=TREES, radius=40.0, strength=1.0, points=[[120, 120, 1.0], [160, 140, 1.0]])
scatter = forge3d.entity_get(TREES)["components"]["Scatter"]
assert scatter["paint_only"] and scatter["terrain"] == TERRAIN, scatter
try:
    ops.scatter.paint_stroke(scatter=SKY, points=[[0, 0, 1.0]])
    raise AssertionError("scatter.paint_stroke on a non-scatter must fail")
except forge3d.Error:
    pass

# Splines (P7.4): a road flattens the surface to its line; a river carves its bed.
road = ground([[-200, -40], [-80, -10], [40, -30], [180, 10]])
run("spline.create", kind="road", terrain=TERRAIN, points=road, guid=ROAD)
river_xz = [[-200, 120], [-60, 150], [80, 110], [200, 160]]
bank = ground(river_xz)
low = min(p[1] for p in bank)
river = [[p[0], low - i * 0.5, p[2]] for i, p in enumerate(bank)]
run("spline.create", kind="river", terrain=TERRAIN, points=river, guid=RIVER)
shaped = forge3d.terrain_info(TERRAIN)
assert shaped["hash"] == created["hash"], "splines must not change the sculpted heights"
assert shaped["surface_hash"] != shaped["hash"], shaped
on_road = ground([[-80, -10]], surface=True)[0][1]
assert abs(on_road - road[1][1]) < 0.5, (on_road, road[1])
bed = forge3d.terrain_sample(TERRAIN, [river_xz[1]], surface=True)[0]
assert bed < river[1][1] - 0.5, (bed, river[1])  # carved below the water line
splines = forge3d.scene_query(component="Spline")
assert sorted(s["guid"] for s in splines) == sorted([ROAD, RIVER]), splines

# Water (P7.3): a lake in the lowest hollow, 4 m deep.
grid = [[x, z] for x in range(-240, 241, 30) for z in range(-240, 241, 30)]
hs = forge3d.terrain_sample(TERRAIN, grid, surface=True)
lowest = min(range(len(grid)), key=lambda i: hs[i])
lx, lz = grid[lowest]
level = hs[lowest] + 4.0
run("water.create", kind="lake", terrain=TERRAIN, position=[lx, level, lz], guid=LAKE)
lake = forge3d.entity_get(LAKE)["components"]
assert lake["WaterBody"]["kind"] == 0 and lake["WaterBody"]["terrain"] == TERRAIN, lake
assert abs(lake["Transform"]["translation"][1] - level) < 1e-3, lake
try:
    ops.water.create(kind="pond")
    raise AssertionError("water.create with kind pond must fail")
except forge3d.Error:
    pass

run("bookmark.save", name="Overview", target=[0, 30, 40], distance=520.0, yaw=0.4, pitch=-0.5)
out = os.environ.get("F3D_LANDSCAPE_OUT", "landscape_python.png")
shot = forge3d.capture(out, width=480, height=270, bookmark="Overview")
assert (shot["width"], shot["height"]) == (480, 270)
assert os.path.getsize(out) > 1000

# Undo everything after the terrain and sky: the generated heights and no splat edits.
for _ in range(steps - 3):
    assert forge3d.undo()
back = forge3d.terrain_info(TERRAIN)
assert back["hash"] == created["hash"] and back["surface_hash"] == created["hash"], back
assert back["splat_hash"] == created["splat_hash"], back
assert forge3d.scene_query(component="Spline") == []
assert forge3d.scene_query(component="WaterBody") == []
for _ in range(steps - 3):
    assert forge3d.redo()
again = forge3d.terrain_info(TERRAIN)
assert again["surface_hash"] == shaped["surface_hash"] and again["splat_hash"] == painted["splat_hash"], again

undone = 0
while forge3d.undo():
    undone += 1
assert undone == steps, (undone, steps)
assert forge3d.scene_query() == []
print("landscape: %d steps, captured to %s, undo and redo ok" % (steps, out))
