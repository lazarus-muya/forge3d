# Operations & Undo

> Stage B gate deliverable (phases.md P2.3, plan.md M2). This API is treated as
> **stable** from here on: the editor UI, the CLI, Python, JSON-RPC and MCP all
> change the scene through it, and nothing else writes to ECS components.

## Model

An **operation** is one undoable scene mutation. `f3d::ops::Operation`
(`engine/ops/include/f3d/ops/operation.hpp`):

```cpp
std::string_view id() const;            // "domain.verb", e.g. "entity.set_property"
std::string display_name() const;       // shown in the operation log / Undo menu
entt::meta_type params_type() const;    // reflected parameter struct (RPC/MCP schema)
nlohmann::json params_json() const;     // parameters as JSON
void set_params_json(const nlohmann::json&);
bool execute(Context&);                 // applies; records state for undo
void undo(Context&);
bool merge(const Operation& next);      // fold a consecutive edit (drag) into this one
std::size_t memory_size() const;        // counted against the byte cap
bool records_history() const;           // false: transient, never recorded (bookmark.recall)
```

`Context` is `{ scene::World& world; ChangeBus& changes; }`. Concrete operations
with a reflected parameter struct derive from `OperationBase<Params>`, which
implements `params_type`/`params_json`/`set_params_json` from reflection.

Parameters are ordinary reflected structs (P2.2), so `OpRegistry::create(id,
json)` can build any operation from JSON, and `param_schema(type)` produces the
field list (name, type, range, unit, tooltip) that the CLI/RPC/MCP surfaces will
publish. Field types are reported by their registered names: `bool`, `int32`,
`uint32`, `int64`, `float`, `double`, `string`, `vec2`, `vec3`, `vec4`, `quat`
(JSON `[w, x, y, z]`), `guid` (8-4-4-4-12 string, `null` for none), `asset_ref`
(`{guid, path}`), `guid[]`, or the name of a reflected struct/enum.

## Registry

`OpRegistry` maps an id to `OpInfo{id, display_name, params_type}` and a factory.
`register_builtin_ops(registry)` (`f3d/ops/builtin.hpp`) adds every engine
operation: entity (`register_core_ops`), selection (`register_selection_ops`),
layer (`register_layer_ops`) and bookmark (`register_bookmark_ops`). Look-up is by id;
`list()` returns registration order; `create(id, params)` builds and configures an
operation.

## History (`OpStack`)

`OpStack` runs operations and keeps the undo/redo history.

- `execute(op, merge = false)` runs `op`, then records it as a new entry.
- `execute(op, true)` marks a **continuing** edit (the second and later frames of a
  drag, further keystrokes in one text field). If the previous entry `merge`s the
  new operation (same entity and property for `entity.set_property`), the new one
  is discarded and the previous entry's undo still restores the state from
  **before the first edit**, so a whole drag is one undo step. Merging is opt-in:
  two separate edits of the same property (or two RPC calls) stay two undo steps.
- Inside a transaction, a merging operation folds into the transaction's latest
  child that accepts it, so a multi-entity gizmo drag (edits alternating between
  entities) is still one child per entity and property.
- Transient operations (`records_history() == false`) run but are never recorded:
  they don't create an undo step and don't clear the redo history.
- `undo()` / `redo()` move the top entry between the two stacks.
- **Transactions** (`begin_transaction` / `end_transaction` / `abort_transaction`)
  group operations so they undo/redo as one entry. Transactions nest: an inner
  transaction commits into the open outer one.
- The history is capped by **entry count** and **bytes** (`set_limits`; 0 bytes
  means no byte cap); the oldest entries are evicted first, and the newest entry is
  always kept. Each entry's `memory_size()` is measured once, when it is recorded
  (and again after a merge).

## Change events

`ChangeBus` fans out `ChangeEvent{kind, entity}` (`EntityCreated`,
`EntityDestroyed`, `ComponentChanged`, `HierarchyChanged`, `SelectionChanged`) to
subscribers (plus `LayersChanged`, `BookmarksChanged` and `ViewRequested`, whose
entity is null). The renderer, outliner and (later) live-link and RPC clients
subscribe instead of polling. Operations emit events on execute and undo.

## Core operations

| id | parameters | notes |
|---|---|---|
| `entity.create` | `name`, `parent`, `guid` (optional) | undo destroys the created entity; redo reuses the same GUID |
| `entity.delete` | `entity` | snapshots the subtree; undo recreates it exactly (same GUIDs, order, components, transforms) |
| `entity.set_property` | `entity`, `path` (`"Component.field"`), `value` | merges continuing edits to the same path; a malformed value fails without changing anything; `Transform.*` edits refresh the world-transform cache |
| `entity.reparent` | `entity`, `parent`, `index` | keeps the world transform; undo restores the old parent, index and local TRS |
| `entity.duplicate` | `entity`, `guid` (optional, for the copy's root) | recreates the subtree with fresh GUIDs; undo destroys it, redo reuses the same GUIDs |
| `selection.set` | `entities` (`guid[]`, first = primary), `active` | unknown entities are dropped; undo restores the previous selection |
| `selection.add` | `entity` | fails for an unknown entity; becomes the active entity |
| `selection.remove` | `entity` | fails if the entity isn't selected |

| `layer.create` | `name`, `guid` (optional), `index` (-1: last) | adds a row to the layer table |
| `layer.delete` | `layer` | its entities fall back to the default layer; undo restores the row (at its index) and the assignments |
| `layer.rename` | `layer`, `name` | |
| `layer.set_visible` | `layer`, `value` | hidden: not drawn, picked or snapped to |
| `layer.set_locked` | `layer`, `value` | locked: drawn, not pickable |
| `layer.assign` | `entities` (`guid[]`), `layer` (null: default) | records each entity's previous layer |
| `bookmark.save` | a `CameraBookmark`: `name`, `target`, `distance`, `yaw`, `pitch`, `orthographic` | replaces a bookmark of the same name (undo restores it) |
| `bookmark.delete` | `name` | undo re-inserts it at its index |
| `bookmark.recall` | `name` | **transient**: sets `World::view_request()` for the editor camera |

Layers: an entity's `scene::Layer` component names a row of the World's
`LayerTable`; no component (or a null id) is the implicit default layer, always
visible and unlocked. Visibility and lock are inherited: an entity is hidden
(locked) if it or any ancestor is on a hidden (locked) layer
(`scene::is_visible`, `scene::is_pickable`). Hidden entities are not drawn or
snapped to; hidden and locked entities are left out of the picking ID buffer.

| `entity.add_component` | `entity`, `component` (`MeshRenderer`, `Light`, `Camera`, `Sky`, `ReflectionProbe`), `value` (optional) | undo removes it |
| `entity.remove_component` | `entity`, `component` | undo restores it with its values |
| `prefab.create_from_selection` | `entity`, `path` (`project://….f3dprefab`) | writes the prefab file and turns the subtree into an instance; undo restores both |
| `prefab.instantiate` | `prefab` (path), `parent`, `position`, `guid` (optional) | redo recreates the same GUIDs |
| `prefab.apply_overrides` | `entity` (any entity of the instance) | the instance becomes the prefab (its root placement aside); every other instance is synced; undo restores the file and all instances |
| `prefab.revert_overrides` | `entity`, `node` (optional), `path` (optional) | drops matching overrides and re-syncs the instance |
| `prefab.update_instances` | `prefab` (path) | re-syncs every instance with the file (after an outside edit) |
| `viewport.capture` | `path`, `width`, `height`, `mode` (`lit`, `unlit`, `wireframe`, `normals`, `cascades`), `camera` (a Camera entity, optional), `bookmark` (optional) | **transient**: renders offscreen and writes a PNG; without camera or bookmark the editor's view (the CLI: a view framing the scene) |
| `terrain.create` | `name`, `guid`, `tiles` (256 quads each, per side), `size`, `min_height`, `max_height`, `format` (`r16`, `r32f`), `seed`, `frequency`, `octaves` | a Terrain entity with fBm heights; redo restores the same heights (P6.1) |
| `terrain.erode` | `terrain`, `mode` (`hydraulic`, `thermal`, `both`), `iterations`, `seed`, `rain`, `evaporation`, `capacity`, `dissolving`, `deposition`, `min_slope`, `talus_angle`, `thermal_rate` | global erosion; progress through `Context::progress` (returning false cancels: the op fails, nothing changes); undo/redo via a tile diff (P6.3) |
| `terrain.sculpt_stroke` | `terrain`, `tool` (`raise`, `lower`, `smooth`, `flatten`, `terrace`, `noise`, `stamp`, `erode`), `radius`, `strength`, `falloff` (`smooth`, `linear`, `sphere`, `constant`), `spacing`, `use_target`, `target_height`, `terrace_step`, `noise_scale`, `noise_seed`, `stamp`, `stamp_height`, `stamp_rotation`, `erosion_iterations`, `points` (`[[x, z, pressure], ...]`, terrain-local) | one stroke = one undo step; undo/redo write back the touched tiles from a zstd diff (exact); the diff counts against the byte cap (P6.2) |
| `terrain.paint_stroke` | `terrain`, `layer` (index in `Terrain.layers`), `erase`, `radius`, `strength`, `falloff`, `spacing`, `points` (`[[x, z, pressure], ...]`, terrain-local) | paints layer weights into the splat maps; one stroke = one undo step via a zstd splat diff (P6.4) |
| `terrain.apply_rules` | `terrain` | re-bakes the layers' height/slope rules into the splat maps (painted weights are replaced); exact undo (P6.4) |
| `terrain.set_layers` | `terrain`, `layers` (the new `Terrain.layers`, 1-16), `source` (per new layer, its old index or −1) | replaces the layer stack; painted weights follow their layers through reorders and removals (P6.4) |
| `terrain.auto_material` | `terrain`, `preset` (`grassland`, `alpine`, `desert`, `forest`, `coast`) | a ready layer stack with built-in textures and rules fitted to the terrain's height range, baked into the splat maps (P6.4) |
| `scatter.paint_stroke` | `scatter`, `erase`, `radius`, `strength`, `falloff`, `spacing`, `points` (`[[x, z, pressure], ...]`, world) | paints the scatter's density mask (created on first use, saved as a chunk); instances stay derived; exact undo (P7.1) |
| `spline.create` | `name`, `guid`, `kind` (`road`, `path`, `river`, `fence`, `custom`), `terrain`, `points` (world), `paint_surface` (`auto`, a built-in surface id, or empty) | a Spline entity with its kind's preset at the first point; `auto` adds a dirt (roads, paths) or gravel (rivers) layer to the terrain if missing and paints it. The terrain surface, ribbon, props and river water are derived (`world::update_landscape`); edit points and settings with `entity.set_property` (P7.4) |
| `water.create` | `name`, `guid`, `kind` (`lake`, `ocean`), `terrain` (lakes; null: the one below), `position` (world; y is the water level) | a WaterBody entity with its kind's preset; a lake floods the basin under it (`world::flood_basin`), an ocean is an endless plane (P7.3) |
| `material.delete` | `path` (`project://….f3dmat`) | removes the material file and its `.meta`; undo writes both back (same GUID). The editor's Material Library clears the slots that use it in the same undo step |
| `modeling.loop_cut` | `entity`, `a`, `b` (the edge crossed, position indices), `cuts` (1-64), `factor` (0-1, one cut) | splits the quads of the edge ring through (a, b) with `cuts` edge loops; faces beside the ring that aren't split get the new positions in their outline; exact undo |

Prefab ops need `Context::files` (the project's VFS). `entity.set_property` on an
entity of a prefab instance also records a per-property override on the instance
(undo removes it); see `docs/architecture/assets.md` § Prefabs.

The selection (`scene::Selection`, owned by the `World`) is scene state: it is
only changed through the `selection.*` operations, which emit
`SelectionChanged`. It refers to entities by GUID, so it survives undo/redo of
deletes; readers skip GUIDs that no longer resolve.

Delete/duplicate use reflection snapshots (`f3d/scene/snapshot.hpp`), which the
`.forge` scene serialiser (P2.6) reuses.

## Editor integration

The inspector reads fields from reflection and reports edits as
`(entity, "<Component>.<field>", json, continuing)`; the app builds an
`entity.set_property` operation and runs it on the stack, merging when
`continuing` (the same widget interaction produced the previous edit). Rotations
are edited as XYZ Euler degrees and sent as quaternions.

Viewport picking, box selection and Delete/Duplicate/Select All run `selection.*`
and `entity.*` operations (Delete and Duplicate as one transaction with their
selection change). A gizmo drag (`editor::TransformTool`) opens one transaction
when the drag starts, runs merging `entity.set_property` edits every frame, and
commits it on release; pressing Escape mid-drag aborts the transaction, which
undoes what it did. Undo/redo are `edit.undo` (Ctrl+Z),
`edit.redo` (Ctrl+Shift+Z) and `edit.redo_alt` (Ctrl+Y). The **Operation Log**
panel lists the undo history (most recent highlighted) and the redo entries.

## Scripts

`f3d/ops/script.hpp` runs a list of operations from JSON (`--scene <ops.json>`
in the editor, `forge3d-cli` in P2.6):

```json
{ "format": "forge3d.ops", "version": 1,
  "ops": [ { "op": "entity.create", "params": { "name": "Rock" } } ] }
```

It stops at the first failure and reports `op #N <id>: <why>`. With a
transaction name, the whole script is one undo step and a failure rolls back
everything it did.

## Testing

`tests/unit/ops/ops_test.cpp`:

- registry listing/schema/JSON build;
- create/delete/reparent/duplicate undo & redo, including exact subtree restore;
- drag merge → one entry; separate edits stay separate; merges inside a
  transaction; history caps (count and bytes) evict oldest;
- `Transform.*` edits propagate to children's world matrices;
- transactions (and nesting) undo/redo as one entry;
- **1,000 random operations**: undo everything reproduces the starting scene
  snapshot byte-for-byte, redo everything reproduces the final scene.

`tests/unit/ops/layer_ops_test.cpp`, `script_test.cpp`,
`tests/unit/editor/{transform_tool,scene_editor}_test.cpp` and
`tests/integration/raster/pick_test.cpp` cover layers, bookmarks, scripts, gizmo
drags, outliner moves and picking with locked/hidden layers.
