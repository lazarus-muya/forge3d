# SPDX-License-Identifier: Apache-2.0
"""The M5 scene, built with the forge3d Python module (phases.md P5.4).

    forge3d-cli run tests/integration/python/m5_scene.py

Builds the same scene as m5_rpc.py does over JSON-RPC (a ground, a cube and a sky at
15:00, seen from the "View" bookmark), captures it, checks it, and undoes everything.
The capture goes to $F3D_M5_OUT (default m5_python.png); ctest compares it with the
RPC capture.
"""
import os

import forge3d

GROUND = "00000000-0000-4000-8000-0000000007a1"
CUBE = "00000000-0000-4000-8000-0000000007a2"
SKY = "00000000-0000-4000-8000-0000000007a3"
BOX = {"path": "tests/data/gltf/Box.glb#mesh/0"}

ops = forge3d.ops
ops.entity.create(name="Ground", guid=GROUND)
ops.entity.add_component(entity=GROUND, component="MeshRenderer")
ops.entity.set_property(entity=GROUND, path="MeshRenderer.mesh", value=BOX)
ops.entity.set_property(entity=GROUND, path="Transform.scale", value=[20, 0.1, 20])
ops.entity.set_property(entity=GROUND, path="Transform.translation", value=[0, -0.06, 0])
ops.entity.create(name="Cube", guid=CUBE)
ops.entity.add_component(entity=CUBE, component="MeshRenderer")
ops.entity.set_property(entity=CUBE, path="MeshRenderer.mesh", value=BOX)
ops.entity.set_property(entity=CUBE, path="Transform.translation", value=[0, 0.5, 0])
ops.entity.create(name="Sky", guid=SKY)
ops.entity.add_component(entity=SKY, component="Sky")
ops.entity.set_property(entity=SKY, path="Sky.time_of_day", value=15.0)
ops.bookmark.save(name="View", target=[0, 0.5, 0], distance=8.0, yaw=0.6, pitch=-0.3)
STEPS = 13

assert [e["name"] for e in forge3d.scene_query()] == ["Ground", "Cube", "Sky"]
assert forge3d.scene_query(component="Sky")[0]["guid"] == SKY
cube = forge3d.entity_get(CUBE)
assert cube["components"]["Transform"]["translation"] == [0, 0.5, 0], cube

# Errors surface as forge3d.Error with the JSON-RPC code.
try:
    ops.entity.delete(entity="00000000-0000-4000-8000-000000000000")
    raise AssertionError("deleting a missing entity must fail")
except forge3d.Error as e:
    assert e.code == -32002, e.code

out = os.environ.get("F3D_M5_OUT", "m5_python.png")
shot = forge3d.capture(out, width=480, height=270, bookmark="View")
assert (shot["width"], shot["height"]) == (480, 270)
assert os.path.getsize(out) > 1000

undone = 0
while forge3d.undo():
    undone += 1
assert undone == STEPS, undone
assert forge3d.scene_query() == []
print("m5_scene: built, captured to %s, undid %d steps" % (out, undone))
