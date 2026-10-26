# Files, jobs & assets

> phases.md M3 (P3.1–P3.5). How forge3d finds files, runs background work, and
> identifies, imports and streams assets.

## Virtual file system (`f3d/core/vfs.hpp`)

Code addresses files as `<mount>://<path>`:

| Mount | Backend | Contents |
|---|---|---|
| `engine://` | `DirectoryBackend` (read-only) | the editor's resources (input presets, ...) |
| `project://` | `DirectoryBackend` | the directory of the open `.forge` file: the user's models and textures |
| any | `io::ArchiveBackend` (read-only) | the entries of a `.forge` archive (`scene.json`, `chunks/...`) |

- Paths are normalised (`\` → `/`, `.` and empty segments dropped); a `..` that
  would leave the mount, drive letters and nested schemes are rejected.
- `DirectoryBackend::write` creates parent directories and is atomic (temp
  file + rename). `FileInfo::modified` lets callers skip unchanged files.
- `FileSystem::to_virtual(native)` maps an OS path back to the longest
  matching directory mount (e.g. a file dropped into the editor).
- Thread-safe: mounting takes a lock, backends are called without it, and the
  archive backend serialises its reads.

The editor mounts `project://` whenever the document's directory changes (open,
Save As, New) and rescans the project's assets in the background.

## Jobs (`f3d/core/jobs.hpp`)

`JobSystem` runs work on two Taskflow executors, one per priority:

- **Interactive** (all cores but one): work the user is waiting for.
- **Background** (a quarter of the cores): scans, imports, cooking.

Separate pools mean interactive work never queues behind background work.

```cpp
jobs.submit(Priority::Background, token,
    [](const CancellationToken& t) { return AssetDatabase::scan(vfs, "project://"); }, // worker
    [&](ScanResult r) { db.apply(std::move(r)); });                                     // main thread
```

- Continuations run on the main thread in `run_continuations()`, which the
  editor calls once per frame.
- **Cancellation** (`CancellationToken`, shared flag): a job cancelled before
  it starts doesn't run; a running job can poll its token; a cancelled job's
  continuation is never run, even if the work had finished.
- Exceptions thrown by jobs are caught and logged.
- `wait_idle()` blocks until submitted work is done (shutdown, tests).

## Asset identity (`f3d/assets/asset_database.hpp`)

Every source file the pipeline knows (by extension: `.gltf/.glb` → `gltf`,
`.fbx`, `.obj`, `.png/.jpg/.jpeg/.tga/.bmp` → `texture`, `.exr/.hdr` →
`texture_hdr`) gets a **`<source>.meta`** sidecar the first time it is seen:

```json
{ "format": "forge3d.meta", "version": 1,
  "guid": "6f1c…", "importer": "gltf",
  "settings": { },
  "source_hash": "fnv1a64:0123456789abcdef" }
```

- The **GUID** is the asset's identity. It moves with the `.meta`, so renaming or
  moving a source together with its `.meta` keeps every reference valid.
- `settings` holds the importer settings (P3.2).
- `source_hash` is the hash of the source **when it was last imported** (empty
  = never). The scan computes the current hash; `AssetRecord::stale()` compares
  the two.

`AssetDatabase` maps GUID ↔ VFS path. A scan:

- walks the mount recursively, skipping hidden entries (`.git`, `.forge3d/`
  caches) and `*.tmp`;
- creates missing `.meta` files and regenerates invalid ones;
- gives a **new GUID** to a source whose `.meta` duplicates another's (a file
  copied together with its `.meta`); the first path in sorted order keeps the
  original;
- reports **orphaned** `.meta` files (source gone) without deleting them, since
  the source may only be moved temporarily;
- re-hashes only files whose size or modification stamp changed since the
  previous scan.

`scan()` is static and only touches the files, so it runs on a worker;
`apply()` installs the result on the main thread.

## Importers & cooking (`f3d/assets/{mesh,cook,texture,pipeline}.hpp`)

**Models** → the common intermediate `ModelScene` (node hierarchy with local
TRS + meshes as `MeshData`: positions, normals, sRGB vertex colours from the
material's base colour, optional UV0; right-handed, Y up, metres, CCW):

| Format | Library | Notes |
|---|---|---|
| `.gltf` / `.glb` | fastgltf | default scene, node TRS, base-colour factors, TEXCOORD_0 |
| `.fbx` | ufbx (overlay port) | converted to Y-up metres (`MODIFY_GEOMETRY`), geometry transforms (pivots) baked, faces triangulated, missing normals generated, V flipped |
| `.obj` | tinyobjloader | one node per shape, `.mtl` diffuse colour, vertices indexed by OBJ tuple before smooth normals are computed, V flipped |

Imported meshes are welded (meshoptimizer remap over every stream).

**Mesh cooking** (`cook_mesh`, settings in the `.meta`): optional xatlas UV
atlas (`generate_uvs`), vertex-cache + vertex-fetch optimisation, then LODs:
each level simplifies the previous one towards `lod_ratio` (0.5) of its
triangles within `lod_error` (0.05 of the mesh size); levels stop when the
simplifier can't remove 10 % more or fewer than 12 triangles would remain;
`max_lods` (4) caps the chain.

**`.f3dmesh`** (little-endian): `"F3DM"`, version 1, flags (bit 0: UVs), vertex
count, index count (all LODs), LOD count, bounds (6 floats), name (length +
bytes), LOD table (`first_index`, `index_count`, `error` per level), vertices
(`MeshVertex`, 28 bytes: position, normal, RGBA8), UVs (2 floats each, if
flagged), indices (u32; the LODs' ranges back to back, sharing the vertices).

**Textures**: stb_image (PNG, JPEG, TGA, BMP, HDR) and tinyexr (EXR) decode
the source; a 2×2 box-filtered mip chain is built in linear space (colour is
linearised from sRGB first; normal maps are renormalised every level); then
bc7enc_rdo (overlay port) compresses each level in 4×4 blocks (edges clamped).

| Kind (`settings.kind`) | Default by file name | Format |
|---|---|---|
| `color` | anything else | BC7, sRGB |
| `normal` | `*normal*`, `*_n`, `*_nrm`, `*_nor` | BC5 (RG; Z reconstructed) |
| `linear` | `*_orm*`, `*_arm*`, `*metallicroughness*` | BC7, linear |
| `grayscale` | roughness, metallic, `*_ao`, occlusion, height, `*_disp`, mask | BC4 |
| `hdr` | `.exr`, `.hdr` | RGBA16F (no BC6H encoder yet) |

`settings.compression` overrides the format (`none`, `bc1`, `bc3`, `bc4`,
`bc5`, `bc7`); `settings.mipmaps: false` keeps one level.

**`.f3dtex`**: `"F3DT"`, version 1, format (`TextureFormat` value), sRGB flag,
mip count, a table of (width, height, byte size) per level, then the levels'
data, largest first. `decode_mip()` unpacks any level to RGBA8 (thumbnails,
previews, tests).

**Pipeline** (`import_asset`): reads the source through the VFS (models through
its native path, since their importers open sibling files), cooks into
`project://.forge3d/cache/<guid>/` (`model.json` + `mesh_<n>.f3dmesh`, or
`texture.f3dtex`), then rewrites the `.meta` with the settings used (defaults
filled in, for the user to edit) and the imported source hash, so the asset is
no longer stale. `load_cooked_model` / `load_cooked_texture` read the cache
back. It can be cancelled between meshes and reports progress.

**Import queue** (`editor::ImportQueue`): every background scan queues the
stale assets; imports run in parallel on the background pool; the Import Queue
panel shows progress bars, failures (also logged to the console) and Retry /
Cancel All / Clear Finished. A failed import isn't retried automatically until
its source changes. `forge3d-cli import <dir> [--force]` does the same
headlessly.

## Index, thumbnails & placement (`f3d/assets/asset_index.hpp`, P3.3)

**Asset index**: SQLite (vcpkg `sqlite3`) at `<project>/.forge3d/index.sqlite`,
WAL mode. Tables: `assets` (guid, path, name, type, importer, source/imported
hash, size, modification stamp), `tags`, `dependencies` (guid → VFS path) and
`thumbnails` (guid → PNG path + the imported hash it shows). It is a cache of the
`.meta` files and the scan, so deleting it loses nothing, and a schema change
rebuilds it. `sync()` writes only the rows that changed. The browser queries it
(case-insensitive substring of name/path, type, every selected tag): about 1 ms
for 1,000 assets (`docs/budgets.md`). At project open it **seeds** the asset
database, so a new session's scan only re-hashes files whose size or stamp
changed.

**Tags** live in the `.meta` (`"tags": [...]`) so they move with the asset; the
browser's *Edit Tags…* rewrites the `.meta` and rescans. **Dependencies** are
found at import (glTF image URIs, OBJ `mtllib` + texture maps, FBX texture
files), stored in the `.meta` as VFS paths, and resolved to assets through the
index (`dependencies()` / `dependents()`).

**Thumbnails** (`editor::ThumbnailCache`): 128 px PNGs in
`project://.forge3d/thumbnails/<guid>.png`, recorded in the index with the
imported hash. A cached PNG is reused until the asset is re-imported. Textures
are thumbnailed on workers (`texture_thumbnail`: the largest cooked mip ≤ 128 px,
decoded). Models are rendered offscreen on the main thread, two per frame
(`render::raster::ThumbnailRenderer`): the cooked model alone, three-quarter
view, the camera distance fitted so every bounding-box corner is in frame.

**Browser**: grid (thumbnails, size slider) or list; search box, type filter,
tag filter; context menu Place at 3D Cursor / Re-import / Edit Tags / uses &
used-by. Results are re-queried only when the query or the database changes.

**Placement**: dragging a model from the browser onto the viewport raycasts the
scene (or the ground plane) under the cursor and runs
`assets::model_instance_ops` as one transaction: `entity.create` for a root at
that point plus the model's node hierarchy, with `MeshRenderer`s referencing
`AssetRef{guid, "<source>#mesh/<n>"}`, then selects it. `MeshLibrary` resolves
GUID references through a resolver (the editor loads the cooked model), keyed by
GUID so references survive the source being moved; `invalidate(guid)` drops them
after a re-import. Double-click, or Place at 3D Cursor, places at the cursor.
Dropping an asset that isn't imported yet queues its import.

## Prefabs (`f3d/ops/prefab.hpp`, P3.4)

A prefab is a reusable entity subtree saved as `*.f3dprefab` (JSON) in the
project; the asset database lists it (type `prefab`, nothing to cook) and the
browser places it like a model.

```json
{ "format": "forge3d.prefab", "version": 1, "name": "Lamp",
  "root": { "node": "<id>", "components": { "Name": …, "Transform": …, "Light": … },
            "children": [ { "node": "<id>", "prefab": "project://prefabs/bulb.f3dprefab",
                            "components": { "Name": …, "Transform": … }, "overrides": [ … ] } ] } }
```

- **Node ids** are stable: an instance's entities carry `PrefabLink{node}`, so
  the prefab can change without losing the mapping. The instance root carries
  `PrefabInstance{prefab path, overrides}`.
- **Overrides** are per property (`{node, "Component.field", value}`), recorded
  by `entity.set_property` when it edits an instance entity. The instance root's
  Name/Transform are its placement, never overrides (for a nested root they are
  an override of the outer instance).
- **Syncing** an instance (`prefab.update_instances`, and every other instance
  after `prefab.apply_overrides`) copies every property except overrides and the
  root's placement, creates entities for new nodes, deletes entities whose node
  is gone, and keeps entities the user added (no `PrefabLink`).
- **Nesting**: a node may reference another prefab (`"prefab"`): the instance is
  expanded with its own `PrefabInstance` and the outer prefab's overrides for it.
  One level only; a nested prefab that itself nests is refused at instantiation.
- Undo: ops that change instances restore exact subtree snapshots (same GUIDs);
  ops that write the prefab file restore its previous text.
- Not tracked as overrides yet: adding/removing components on instance entities,
  deleting a prefab node from an instance (a later sync recreates it), sibling
  order changes.

Editor: **Edit ▸ Create Prefab from Selection** (writes
`project://prefabs/<name>.f3dprefab`; the scene must be saved first); the
inspector's **Prefab** section lists the overrides with Apply to Prefab /
Revert / Update Instances.

## Streaming & auto-reimport (`f3d/render/raster/residency.hpp`, `f3d/assets/source_watcher.hpp`, P3.5)

**Residency.** `render::raster::ResidencyManager` owns the GPU copies of
project meshes and textures. The viewport renderer asks for each drawn asset
with its on-screen radius in pixels (`mesh(ref, px)`, `texture(ref, px)`) and
gets what is resident now: possibly a coarser level, or nothing on the first
frame (the mesh is skipped, the texture falls back to white). `end_frame()`
(once per frame, after present) settles the next state:

1. assets not drawn for `idle_frames` (300) are evicted;
2. every requested asset wants the level that matches its screen size: mesh
   LOD `lod_for(px)` (LOD 0 above 200 px, then one level per halving), texture
   top mip `mip_for(px, width)` (one texel per pixel across the diameter);
3. while the wanted total is over the budget, idle resident assets go first
   (least recently used), then the smallest on screen are coarsened one level at
   a time, then dropped;
4. downgrades apply at once; upgrades are uploaded largest on screen first,
   within 256 MB per frame.

So the resident total is never above the budget after `end_frame()`. Meshes
upload only the vertices their LOD uses; textures upload mips `top..`, so a
smaller top mip means less memory. The budget is the `vram_budget_mb`
preference (default 2048, Preferences ▸ Streaming), overridden by
`--vram-cap <MB>`. **Window ▸ Residency** shows used vs budget and, per asset,
the resident and wanted level, screen size and size in memory. `--bench`
reports `residency { budget_mb, used_mb, peak_mb, meshes, textures }`.

**Textures on meshes.** `MeshRenderer.texture` (an asset reference) overrides
the base-colour texture of every slot's material (empty: the materials' own,
see `render.md` § Lighting & materials).
Dropping a texture from the asset browser onto a mesh in the viewport sets it
through `entity.set_property` (undoable).

**Auto-reimport.** `assets::SourceWatcher` watches the source of every asset
and the files it depends on (`.meta` `dependencies`). In interactive sessions
the editor polls it on a background worker once a second; a change starts a
rescan, the rescan finds the new hash (the asset is stale) and the import
queue re-imports it. When an import finishes, `App::asset_imported` drops the
cached cooked model, the mesh library entry and the resident GPU copies
(`ResidencyManager::invalidate`), so the next frame loads the new data: a hot
swap without restarting. A re-imported prefab also runs
`prefab.update_instances` on its instances. New and deleted files are picked
up when the editor window regains focus (a rescan), or with the asset
browser's Rescan button (command `assets.rescan`).
Automated runs (`--frames`, `--hidden`, …) never watch files.
