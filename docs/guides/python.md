# Scripting forge3d with Python

The `-full` builds (`cmake --preset win-msvc-full`, `linux-gcc-full`; CMake option
`FORGE3D_WITH_PYTHON=ON`, vcpkg feature `python`) embed CPython with a `forge3d` module.

**Running scripts.** `forge3d-cli run script.py` (any number of `run` and other commands in
one session), or in the editor: turn on Preferences ▸ Automation ▸ Python scripts, then
Window ▸ Python (a console and a script runner). Running Python is a separate permission
from the JSON-RPC server: a script can do anything your user account can.

**The module.**

```python
import forge3d

ops = forge3d.ops                       # every operation, generated from the session's list
rock = "00000000-0000-4000-8000-000000000001"
ops.entity.create(name="Rock", guid=rock)
ops.entity.set_property(entity=rock, path="Transform.translation", value=[0, 1, 0])
forge3d.undo(); forge3d.redo()

forge3d.scene_query(component="MeshRenderer")   # [{guid, name, parent, components}]
forge3d.entity_get(rock)["components"]["Transform"]
forge3d.terrain_info(terrain)["hash"]            # size, height range, layers; hashes of the heights,
                                                # the surface (with roads, riverbeds) and the painted layers
forge3d.terrain_sample(terrain, [[0, 0], [10, 5]]) # heights under world x, z
forge3d.terrain_sample(terrain, [[0, 0]], surface=True) # ... of the ground as splines shape it
forge3d.capture("shot.png", width=1280, height=720, bookmark="View")
forge3d.call("selection.get")                   # any JSON-RPC method by name
```

Operations take keyword arguments matching their parameters (`help(forge3d.ops.entity.create)`
lists them) and return `{"undo_steps": n}`. Failures raise `forge3d.Error` with `.code`, the
JSON-RPC error code. `forge3d.refresh_ops()` picks up operations registered later (plugins).

The module calls the same dispatcher as the JSON-RPC server, in process: a script and a
remote client building the same scene get the same result (the M5 test captures both and
compares them pixel for pixel).

**Building a landscape.** Terrain, layers, foliage, roads, rivers and lakes are all
operations, so a script can build a whole landscape (`tests/integration/python/landscape.py`
is a complete example):

```python
t = "00000000-0000-4000-8000-000000000a01"
ops.terrain.create(name="Valley", guid=t, tiles=2, size=512.0, max_height=80.0, seed=7)
ops.terrain.auto_material(terrain=t, preset="grassland")   # also alpine, desert, forest, coast
ops.terrain.paint_stroke(terrain=t, layer=1, radius=30.0, points=[[-100, -100, 1.0]])

def on_ground(xz):                                         # [x, z] -> [x, ground height, z]
    return [[p[0], h, p[1]] for p, h in zip(xz, forge3d.terrain_sample(t, xz))]

ops.spline.create(kind="road", terrain=t, points=on_ground([[-200, -40], [0, -20], [180, 10]]))
ops.water.create(kind="lake", terrain=t, position=[-120, 30.0, 80])  # floods the basin up to y = 30
```

A spline flattens and paints the ground without touching the sculpted heights:
`terrain_info(t)["hash"]` stays the same while `["surface_hash"]` changes, and
`terrain_sample(t, points, surface=True)` reads the shaped ground.
