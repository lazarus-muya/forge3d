# The `.forge` project format — version 1

> Stage B gate deliverable (phases.md P2.6). Implemented by `f3d::io`
> (`engine/io/include/f3d/io/{archive,forge_file}.hpp`). This document is the
> specification; when they disagree, fix whichever is wrong and add a test.

A `.forge` file holds one scene: its entities, layers and camera bookmarks,
optional editor state, and binary payloads. It is a standard ZIP archive, so
any ZIP tool can list it and read its JSON.

## Container

- **ZIP** (PKWARE APPNOTE), little-endian, no encryption, no spanning, no ZIP64.
  Limits: 65,535 entries, 4 GiB per entry and per file.
- **Compression per entry**: method **0** (stored) or method **93** (zstd,
  APPNOTE 6.3.7). Deflate is never written, and readers reject other methods.
  A writer may store an entry it was asked to compress when zstd doesn't make
  it smaller.
- **Names** are UTF-8 (general-purpose flag bit 11) with `/` separators.
- **Timestamps** are fixed (1980-01-01 00:00): the same scene always saves to
  the same bytes, which keeps diffs and caches stable. The file's own
  modification time is the save time.
- Every entry's CRC-32 and size are checked when it is read.
- Python 3.14+ `zipfile` reads every entry (`tools/check_forge_zip.py`); older
  versions list zstd entries but can't decompress them.

## Entries

| Entry | Compression | Required | Contents |
|---|---|---|---|
| `scene.json` | zstd | yes | the scene (below) |
| `editor.json` | zstd | no | editor state that is not part of the scene |
| `chunks/<name>` | zstd | no | binary payloads; `<name>` may contain `/` (e.g. `terrain/tile_3_7.r16`) |
| `manifest.json` | stored | yes | format identification and the chunk table; written last |

Readers find entries through the central directory, so their order doesn't
matter. `manifest.json` is stored uncompressed so it can be inspected with any
tool.

### `manifest.json`

```json
{
  "format": "forge3d.project",
  "format_version": 1,
  "app_version": "0.1.0",
  "chunks": [
    { "name": "scene.json", "kind": "scene", "compression": "zstd",
      "size": 320, "stored_size": 234, "crc32": "e04d44b0" }
  ]
}
```

- `format` must be `forge3d.project`.
- `format_version` is the integer format version (this document: 1).
- `app_version` is the forge3d version that wrote the file (informational).
- `chunks` lists every entry except the manifest: `name`, `kind` (`scene`,
  `editor` or `binary`), `compression` (`store` / `zstd`, as actually stored),
  `size` (uncompressed bytes), `stored_size`, `crc32` (hex). A reader fails if
  a listed entry is missing or its size differs; unlisted entries are ignored.

### `scene.json`

Produced by `scene::snapshot_world` and read by `scene::restore_world`.
Keys are sorted, so the JSON is canonical.

```json
{
  "roots": [ <entity>, ... ],
  "layers": [ <LayerInfo>, ... ],
  "bookmarks": [ <CameraBookmark>, ... ]
}
```

`roots` are in scene order. Each **entity**:

```json
{
  "guid": "00000000-0000-4000-8000-000000000003",
  "components": {
    "Name": { "value": "Rock B" },
    "Transform": { "translation": [2, 0, 1], "rotation": [1, 0, 0, 0], "scale": [1, 1, 1] },
    "MeshRenderer": { "mesh": { "guid": null, "path": "C:/models/Rock.glb#mesh/0" }, "visible": true }
  },
  "children": [ <entity>, ... ]
}
```

- `guid`: the entity's identity, 8-4-4-4-12 lower-case hex. Unique in the file.
- `children` are in sibling order. The tree is the scene hierarchy.
- `components` maps a reflected component type name to its fields. Every entity
  has `Name` and `Transform`; the others are optional:

| Component | Fields |
|---|---|
| `Name` | `value` (string) |
| `Transform` | local `translation` `[x, y, z]` (m), `rotation` quaternion `[w, x, y, z]`, `scale` `[x, y, z]`. Y up, right-handed |
| `MeshRenderer` | `mesh` (asset reference: `{guid, path}`; `path` = source file + `#mesh/<n>`), `visible` (bool), `texture` (base-colour override, asset reference), `materials` (asset reference per submesh slot; empty = the model's own) |
| `Light` | `type` (0 directional, 1 point, 2 spot), `color` (linear RGB), `intensity`, `range` (m), `inner_cone`, `outer_cone` (rad), `cast_shadows`, `volumetric_scattering` (scale of its light in volumetric fog) |
| `Camera` | `projection` (0 perspective, 1 orthographic), `fov_y` (rad), `ortho_height` (m), `near_plane`, `far_plane` |
| `Sky` | `time_of_day` (h), `year`, `month`, `day`, `latitude`, `longitude` (deg), `utc_offset` (h), `north_offset` (deg), `sun_illuminance` (lx), `haze`, `ground_albedo` (RGB), `moon`, `shadows` (bools), `exposure_compensation` (EV) |
| `ReflectionProbe` | `radius` (m), `resolution` (cube face size, px) |
| `HeightFog` | `density` (extinction at the entity's height, 1/m), `height_falloff` (1/m), `albedo` (RGB), `anisotropy` (Henyey-Greenstein g), `sky_lighting`, `sun_lighting`, `start_distance` (m), `max_opacity`, `cutoff_distance` (m, 0 = 30 km), `volumetric` (bool), `volumetric_distance` (m), `volumetric_density`, `visible` |
| `FogVolume` | `size` (m, box around the entity), `density` (1/m at the bottom), `albedo` (RGB), `height_falloff` (1/m), `edge_fade` (0-1), `emissive` (RGB per m), `visible` |
| `PostProcess` | `bloom_intensity`, `bloom_radius` (0-1), `bloom_threshold` (EV), `vignette`, `temperature` (K), `tint`, `saturation`, `contrast`, `film_grain`, `visible` |
| `EditableMesh` | a polygon mesh in local space: `positions` (`[[x, y, z], ...]`), `face_sizes` (corners per face), `corners` (position index per corner, faces back to back, counter-clockwise from outside); `smoothing_angle` (deg), `uv_scale` (m); `shape` (0 none, 1 plane, 2 cube, 3 sphere, 4 cylinder, 5 cone, 6 torus, 7 capsule, 8 stairs: still editable as that shape), `shape_size`, `shape_segments_a`, `shape_segments_b`. The entity's `MeshRenderer` points at `modeling://<guid>`, the triangulated mesh the editor and renderer derive from it |
| `Terrain` | `tiles` (256-quad tiles per side), `size` (m), `min_height`, `max_height` (m), `format` (0 R16, 1 R32F), `lod_distance`, `texture_resolution` (texels per layer texture side), `layers` (bottom to top, at most 16: `name`, `material`, `color`, `roughness`, `tiling` (m), `anti_tiling` (0-1: shifted copies hide the repeats; 0 = plain, default 0.5), `triplanar`, `albedo_texture`, `normal_texture`, `roughness_texture`, `ao_texture`, `height_texture` (asset references; `builtin:terrain/<surface>/<slot>` for the built-in surfaces; a packed `*_arm_*`/`*_orm_*` map in the roughness slot is read from green, in the AO slot from red), `normal_directx`, `normal_strength`, `height_contrast`, `rule`). Heights and layer weights are chunks (below) |
| `Scatter` | `terrain`, `meshes` (asset references), `area` (m), `density`, `min_spacing`, `scale_min`, `scale_max`, `random_yaw`, `align_to_normal`, `sink`, `height_min`, `height_max`, `slope_min`, `slope_max`, `layer`, `layer_threshold`, `seed`, `max_instances`, `draw_distance`, `cast_shadows`, `visible`, `paint_only`. Instances are derived, not stored; a painted density mask is a chunk |
| `Spline` | `kind` (0 road, 1 path, 2 river, 3 fence, 4 custom), `terrain`, `points` (control points relative to the entity), `closed`, `width`, `shape_terrain`, `falloff`, `depth` (m), `paint_layer`, `paint_strength`, `surface` (built-in surface id), `surface_tiling`, `meshes`, `mesh_spacing`, `mesh_offset`, `both_sides`, `clear_scatter`, `clear_margin`, `flow_speed` (m/s), `visible`. What it does to the terrain is derived on load, never saved |
| `WaterBody` | `kind` (0 lake, 1 ocean), `terrain` (lakes), `extent` (m, x z), `shallow_color`, `deep_color` (linear RGB), `clarity` (m), `wave_height`, `wave_length` (m), `wind_direction` (deg), `wave_speed`, `foam`, `refraction`, `roughness`, `visible`. The entity's height is the water level; a lake's surface is derived from the terrain |
| `Layer` | `layer` (guid of a row in `layers`; null = the default layer) |

Enums are stored as integers. Fields missing from a component take their
default values, and unknown fields are ignored (forward compatibility within a
version).

**LayerInfo**: `{ "id": guid, "name": string, "visible": bool, "locked": bool }`.
The default layer (null id) is implicit and never listed.

**CameraBookmark**: `{ "name", "target": [x, y, z], "distance", "yaw", "pitch"
(radians), "orthographic" }`: an orbit camera around `target`.

The selection is not saved.

### `editor.json`

Optional editor state. Version 1 defines:

```json
{ "camera": <CameraBookmark> }
```

the editor camera when the file was saved (its `name` is unused). Readers
ignore what they don't understand.

### `chunks/<name>`

Binary payloads owned by a subsystem, keyed by the owning entity's GUID. All
integers are little-endian `uint32`; each payload starts with a magic and a
version (1). Readers skip chunks they don't know, and a chunk that doesn't match
its entity (another size) is skipped with a warning.

| Chunk | Payload |
|---|---|
| `terrain/<guid>/heights` | `"F3HF"`, version, `samples` per side, then `samples²` `float32` heights (m, terrain-local), row-major from −x −z |
| `terrain/<guid>/splat` | `"F3SP"`, version, `tiles`, `maps` (1-4), then `maps` RGBA8 images of `tiles × 128` texels per side: the weight of layers `4m … 4m+3` (summing to 255; all zero = layer 0). Absent until layers are painted or baked |
| `scatter/<guid>/mask` | `"F3SM"`, version, `width`, `height`, then `width × height` bytes of painted density (0-255) over the scatter's area. Only for scatters that were painted |

A terrain saved without a heights chunk (files written before these chunks existed) loads with no heights: it is not drawn until recreated.

## Asset paths

In version 1, mesh references hold the source file's path as it was imported
(usually absolute). Project-relative paths (`project://`) and asset GUIDs come
with the asset database (P3.1); that change will be a format migration.

## Versioning and migration

- A file's `format_version` is fixed when it is written. A build reads every
  older version by running the migration chain
  (`f3d::io::forge_migrations()`): one step per version
  (`v_n → v_n+1`), each a function over the decoded document (manifest,
  scene, editor and chunks). A missing step is an error, and so is a file newer
  than the build.
- Every format change bumps `format_version`, adds a migration step and checks
  a fixture of the old version into `tests/data/forge/`. Fixtures are never
  regenerated (`tests/data/forge/README.md`).
- Adding optional fields with defaults doesn't need a new version.

## Saving

- **Atomic**: the file is written to `<file>.tmp` in the same directory,
  flushed to the disk (`_commit` / `fsync`), closed, then renamed over
  `<file>`. A crash at any point leaves the previous `<file>` intact (a stale
  `.tmp` may remain; the next save replaces it).
- **Autosave**: the editor writes a modified scene every N minutes (preference
  `files.autosave_minutes`, default 5, 0 = off) to `<name>.autosave.forge` next
  to the project, or `<user config dir>/autosave/untitled.autosave.forge` for an
  untitled scene. A real save deletes the autosave. When a project is opened
  and its autosave is newer, the editor offers to recover it; a recovered scene
  is marked modified so the user saves it deliberately.

## Tools

- `forge3d-cli open <file> info` / `print` inspects a file;
  `forge3d-cli run-op … save <file>` edits one headlessly.
- `python tools/check_forge_zip.py <file>` checks a file with Python's `zipfile`.
