# C API and plugins

`engine/api/include/forge3d_api.h` is forge3d's stable binary interface (phases.md P5.1):
pure C, usable from any language with a C FFI, and the only header a plugin needs.

## Versioning

`F3D_API_VERSION` is `major << 16 | minor`. A host serves a request when the majors match
and the requested minor isn't newer than its own (`f3d_api_get`, `api::is_compatible`).
Minor versions only add functions at the end of `F3dApi` (`F3dApi.size` tells a caller
which exist); anything else bumps the major.

## The function table

`f3d_api_get(F3D_API_VERSION)` returns `const F3dApi*` (or NULL). Handles are opaque:

| Handle | What it is |
|---|---|
| `F3dHost*` | a session: an editor or a CLI run (`api::Host`: operation registry, history, world) |
| `F3dWorld*` | a scene |
| `F3dOpContext*` | a running operation (only valid inside its execute callback) |
| `F3dEntity` | an entity by value: its 128-bit GUID (`{0, 0}` is null) |

Functions return `F3dResult`; `last_error()` describes the last failure on the calling thread.

- **Operations**: `register_op`, `run_op` (by id with JSON parameters, through the history:
  undoable), `undo`, `redo`, `host_world`.
- **Reading**: `entity_count`, `find_entity` (by name), `get_property` (`"Component.field"` as
  JSON; size-query with a null buffer).
- **Changing the scene**, inside an operation only: `create_entity`, `delete_entity`,
  `set_property`, `add_component`, `run_child_op`. Each runs the matching built-in operation
  and records it as a child of the running one.

## Plugin operations

`register_op(host, F3dOpDesc{id, display_name, params_schema_json, execute, user_data})`
adds an operation the registry lists with its schema (the RPC/MCP surfaces of P5.2–P5.3
read the same list). When it runs, `execute(ctx, params_json, user_data)` changes the
scene through `ctx`; the host keeps those changes as child operations, so:

- **undo** undoes the children in reverse;
- **redo** replays the recorded children, without calling the plugin again (random or
  time-dependent plugins redo exactly what they did);
- a **failure** (`execute` returns an error) undoes whatever that call changed, and the
  history is left as it was.

## Loading

`api::PluginLoader` loads shared libraries from `plugins/` next to the executable (the
editor at start-up; the CLI unless `--no-plugins`, or from `--plugins <dir>`). A plugin
exports:

```c
F3D_PLUGIN_DECLARE_VERSION   /* uint32_t f3d_plugin_api_version(void) */
F3D_PLUGIN_EXPORT F3dResult f3d_plugin_entry(F3dGetApiFn get_api, F3dHost* host, F3dPluginInfo* info);
```

The loader calls `f3d_plugin_api_version` first and refuses an incompatible plugin before
running any of its other code ("built for forge3d API 2.0, this host provides 1.0"), then
calls `f3d_plugin_entry` once: the plugin fetches the table with `get_api`, names itself in
`info` and registers its operations. Libraries stay loaded for the life of the process,
because their operations can sit in the undo history.

`samples/plugins/hello_op/` (plain C) registers `sample.scatter_cubes {count, seed, radius,
mesh}` and is built into `bin/<config>/plugins/`.

## JSON-RPC server (P5.2)

`api::RpcServer` exposes a session over JSON-RPC 2.0 on a WebSocket. It is **off by
default**: the editor starts it with `--rpc[=port]` (0: any free port) or the
`automation.rpc_enabled` preference (Preferences ▸ Automation; port 7468).

**Security.** The server listens on 127.0.0.1 only. At start it generates a 256-bit
session token (OS RNG) and writes `{port, token, pid}` to `rpc.json` in the user's config
directory (or `--rpc-token-file`), readable by the user only. A connection's first message
must be `session.authenticate {token}` (compared in constant time); anything else, or a
wrong token, gets an error and the connection is closed. Messages over 16 MB close the
connection; at most 16 connections. The token file is removed when the server stops.

**Threads.** IXWebSocket runs a thread per connection; those threads only queue messages.
The editor's frame loop calls `poll()`, which runs every call on the main thread (the same
thread as the UI and the history), then sends the reply and any change events the call
produced.

**Methods** (`api::RpcDispatcher`; params are objects):

| Method | Params | Result |
|---|---|---|
| `session.authenticate` | `token` | `{authenticated, api_version}` |
| `ops.list` | — | `[{id, name, params, schema}]`: parameter rows and a JSON Schema per operation |
| `ops.run` | `id`, `params` | `{undo_steps}`; through the history (undoable) |
| `ops.undo` / `ops.redo` | — | `{done, undo_steps}` |
| `scene.query` | `name?` (substring, any case), `component?` | `[{guid, name, parent, components}]` |
| `selection.get` | — | `{entities, active}` (P5.3) |
| `entity.get` | `entity` | the summary plus every reflected component as JSON, and `children` |
| `entity.set` | `entity`, `path`, `value` | `ops.run entity.set_property` |
| `terrain.info` | `entity` | `{samples, size, spacing, origin, min_height, max_height, hash, surface_hash, layers, splat_hash}` (P6.2; `hash`: FNV-1a 64 of the sculpted heights; `surface_hash`: of the surface with the splines' roads and riverbeds, equal to `hash` without splines; `splat_hash`: of the painted layer weights, empty before any; P6.4/P7.4) |
| `terrain.sample` | `entity`, `points` (`[[x, z], ...]`, world), `surface` (bool, default false) | `{heights}` (world metres, bilinear; with `surface`, of the ground as shaped by splines) |
| `asset.search` | `text?`, `type?`, `tags?`, `limit?` | `[{guid, path, name, type, stale}]` (needs a project) |
| `viewport.capture` | `width?`, `height?`, `mode?`, `camera?`, `bookmark?`, `path?` | `{path}` or `{png_base64}` |
| `events.subscribe` | `kinds?` (empty: all) | then notifications `events.changed {kind, entity, asset?}` |

Errors use the JSON-RPC codes (-32700 parse, -32600 invalid request, -32601 unknown method,
-32602 invalid params, -32603 internal) and ours: -32001 not authenticated, -32002 the
operation failed, -32003 not available in this session. Batches and notifications follow
the JSON-RPC 2.0 rules.
