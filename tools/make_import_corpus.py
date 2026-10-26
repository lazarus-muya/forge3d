# SPDX-License-Identifier: Apache-2.0
"""Generates the importer test corpus in tests/data/import/ (phases.md P3.2).

    python tools/make_import_corpus.py

Everything here is synthetic (CC0), so the files can live in the repository:
an OBJ cube with a material, an ASCII FBX cube, a dense OBJ height field (for
LOD tests), and PNG / JPEG / EXR textures of each kind. The output is
deterministic; the files are committed, so this only needs re-running when the
corpus changes. glTF samples come from tests/data/gltf/.
"""
import math
import pathlib
import struct

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent / "tests" / "data" / "import"


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def cube_obj():
    write(ROOT / "models" / "cube.mtl", "newmtl Brick\nKd 0.8 0.25 0.15\nmap_Kd ../textures/checker.png\n")
    v = [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1), (-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1)]
    vt = [(0, 0), (1, 0), (1, 1), (0, 1)]
    vn = [(0, 0, 1), (0, 0, -1), (1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0)]
    faces = [((1, 2, 3, 4), 1), ((6, 5, 8, 7), 2), ((2, 6, 7, 3), 3), ((5, 1, 4, 8), 4), ((4, 3, 7, 8), 5),
             ((5, 6, 2, 1), 6)]
    lines = ["# forge3d import corpus: a 2 m cube (CC0)", "mtllib cube.mtl", "o Cube"]
    lines += [f"v {x} {y} {z}" for x, y, z in v]
    lines += [f"vt {u} {w}" for u, w in vt]
    lines += [f"vn {x} {y} {z}" for x, y, z in vn]
    lines.append("usemtl Brick")
    for quad, n in faces:
        lines.append("f " + " ".join(f"{p}/{t + 1}/{n}" for t, p in enumerate(quad)))
    write(ROOT / "models" / "cube.obj", "\n".join(lines) + "\n")


def height_field_obj(n=64):
    """A (n x n)-quad terrain patch with smooth bumps: 2 * n * n triangles, simplifies well."""
    lines = ["# forge3d import corpus: a bumpy height field for LOD tests (CC0)", "o Patch"]
    for j in range(n + 1):
        for i in range(n + 1):
            x, z = i / n * 20 - 10, j / n * 20 - 10
            y = 0.8 * math.sin(x * 0.35) * math.cos(z * 0.3) + 0.15 * math.sin(x * 1.3 + z * 0.7)
            lines.append(f"v {x:.5f} {y:.5f} {z:.5f}")
    for j in range(n):
        for i in range(n):
            a = j * (n + 1) + i + 1
            b, c, d = a + 1, a + n + 2, a + n + 1
            lines.append(f"f {a} {d} {c} {b}")
    write(ROOT / "models" / "height_field.obj", "\n".join(lines) + "\n")


def cube_fbx():
    verts = [-1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1, -1, 1, -1, 1, 1, -1, -1, -1, -1, 1, -1, -1]
    # Quads, the last index of each polygon stored as -(index + 1).
    polys = [0, 1, 3, -3, 2, 3, 5, -5, 4, 5, 7, -7, 6, 7, 1, -1, 1, 7, 5, -4, 6, 0, 2, -5]
    uv = [0, 0, 1, 0, 1, 1, 0, 1]
    uv_index = [0, 1, 2, 3] * 6
    text = f"""; FBX 7.4.0 project file
; forge3d import corpus: a 2 m cube, hand-written ASCII FBX (CC0)
FBXHeaderExtension:  {{
	FBXHeaderVersion: 1003
	FBXVersion: 7400
}}
GlobalSettings:  {{
	Version: 1000
	Properties70:  {{
		P: "UpAxis", "int", "Integer", "",1
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",2
		P: "FrontAxisSign", "int", "Integer", "",1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",100
	}}
}}
Objects:  {{
	Geometry: 1000, "Geometry::Cube", "Mesh" {{
		Vertices: *{len(verts)} {{
			a: {",".join(map(str, verts))}
		}}
		PolygonVertexIndex: *{len(polys)} {{
			a: {",".join(map(str, polys))}
		}}
		GeometryVersion: 124
		LayerElementUV: 0 {{
			Version: 101
			Name: "UVMap"
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "IndexToDirect"
			UV: *{len(uv)} {{
				a: {",".join(map(str, uv))}
			}}
			UVIndex: *{len(uv_index)} {{
				a: {",".join(map(str, uv_index))}
			}}
		}}
		Layer: 0 {{
			Version: 100
			LayerElement:  {{
				Type: "LayerElementUV"
				TypedIndex: 0
			}}
		}}
	}}
	Model: 2000, "Model::Cube", "Mesh" {{
		Version: 232
		Properties70:  {{
			P: "Lcl Translation", "Lcl Translation", "", "A",0,1,0
		}}
	}}
	Material: 3000, "Material::Blue", "" {{
		Version: 102
		ShadingModel: "phong"
		Properties70:  {{
			P: "DiffuseColor", "Color", "", "A",0.1,0.2,0.8
		}}
	}}
}}
Connections:  {{
	C: "OO",2000,0
	C: "OO",1000,2000
	C: "OO",3000,2000
}}
"""
    write(ROOT / "models" / "cube_ascii.fbx", text)


def textures():
    out = ROOT / "textures"
    out.mkdir(parents=True, exist_ok=True)
    y, x = np.mgrid[0:64, 0:64]
    checker = ((x // 8 + y // 8) % 2).astype(np.uint8)
    rgba = np.zeros((64, 64, 4), np.uint8)
    rgba[..., 0] = np.where(checker, 220, 40)
    rgba[..., 1] = (x * 4).astype(np.uint8)
    rgba[..., 2] = (y * 4).astype(np.uint8)
    rgba[..., 3] = np.clip(255 - y * 3, 0, 255).astype(np.uint8)  # alpha gradient
    Image.fromarray(rgba, "RGBA").save(out / "checker.png")

    # A smooth non-power-of-two "photo" (JPEG).
    y, x = np.mgrid[0:60, 0:100]
    photo = np.stack([x * 2.5, y * 4.0, 128 + 60 * np.sin(x / 9.0)], axis=-1)
    Image.fromarray(np.clip(photo, 0, 255).astype(np.uint8), "RGB").save(out / "photo.jpg", quality=92)

    # A tangent-space normal map of a dome (name ends in _normal: imported as BC5).
    y, x = np.mgrid[0:32, 0:32]
    nx, ny = (x - 15.5) / 20.0, (y - 15.5) / 20.0
    nz = np.sqrt(np.clip(1 - nx * nx - ny * ny, 0, 1))
    normal = np.stack([nx, ny, nz], axis=-1)
    normal /= np.linalg.norm(normal, axis=-1, keepdims=True)
    Image.fromarray(((normal * 0.5 + 0.5) * 255).round().astype(np.uint8), "RGB").save(out / "rock_normal.png")

    # A grayscale roughness map (imported as BC4).
    rough = (128 + 100 * np.sin(x / 4.0) * np.cos(y / 5.0)).astype(np.uint8)
    Image.fromarray(rough, "L").save(out / "rock_roughness.png")

    write_exr(out / "sky.exr")


def write_exr(path, width=16, height=8):
    """A tiny uncompressed scanline OpenEXR (half RGB) with values above 1."""
    y, x = np.mgrid[0:height, 0:width]
    r = (0.2 + x / width * 4.0).astype(np.float16)
    g = (0.3 + y / height * 2.0).astype(np.float16)
    b = np.full((height, width), 1.5, np.float16)

    def attr(name, kind, data):
        return name.encode() + b"\0" + kind.encode() + b"\0" + struct.pack("<i", len(data)) + data

    chlist = b"".join(c.encode() + b"\0" + struct.pack("<iB3xii", 1, 0, 1, 1) for c in "BGR") + b"\0"
    box = struct.pack("<iiii", 0, 0, width - 1, height - 1)
    header = b"\x76\x2f\x31\x01" + struct.pack("<i", 2)
    header += attr("channels", "chlist", chlist)
    header += attr("compression", "compression", b"\0")
    header += attr("dataWindow", "box2i", box)
    header += attr("displayWindow", "box2i", box)
    header += attr("lineOrder", "lineOrder", b"\0")
    header += attr("pixelAspectRatio", "float", struct.pack("<f", 1.0))
    header += attr("screenWindowCenter", "v2f", struct.pack("<ff", 0.0, 0.0))
    header += attr("screenWindowWidth", "float", struct.pack("<f", 1.0))
    header += b"\0"
    line_bytes = width * 2 * 3
    table_size = 8 * height
    first = len(header) + table_size
    offsets = [first + i * (8 + line_bytes) for i in range(height)]
    body = b""
    for row in range(height):
        body += struct.pack("<ii", row, line_bytes) + b[row].tobytes() + g[row].tobytes() + r[row].tobytes()
    path.write_bytes(header + b"".join(struct.pack("<Q", o) for o in offsets) + body)


def main():
    cube_obj()
    height_field_obj()
    cube_fbx()
    textures()
    write(ROOT / "LICENSE.md",
          "# Import corpus\n\nGenerated by `tools/make_import_corpus.py`; synthetic, released under CC0 1.0.\n"
          "The glTF samples used alongside it are in `tests/data/gltf/` (see its LICENSES.md).\n")
    print(f"wrote {sorted(str(p.relative_to(ROOT)) for p in ROOT.rglob('*') if p.is_file())}")


if __name__ == "__main__":
    main()
