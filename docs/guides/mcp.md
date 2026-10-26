# Driving forge3d from an AI assistant (MCP)

`forge3d-mcp` is a [Model Context Protocol](https://modelcontextprotocol.io) server. An MCP
client (Claude Code, Claude Desktop, or any other) starts it and talks to it over
stdin/stdout; it forwards everything to the editor's JSON-RPC server
(`docs/architecture/api.md`).

## Setting it up

1. Let the editor accept automation: start it with `--rpc`, or turn on
   Preferences ▸ Automation ▸ JSON-RPC server (takes effect on the next start). The
   editor writes its port and a session token to `rpc.json` in your config directory
   (`%APPDATA%\forge3d\forge3d\` on Windows, `~/.local/share/forge3d/forge3d/` on Linux).
2. Register `forge3d-mcp` with your client.

**Claude Code**, from a shell:

    claude mcp add forge3d -- "C:/Program Files/forge3d/forge3d-mcp.exe"

or in a project's `.mcp.json`:

```json
{
  "mcpServers": {
    "forge3d": {
      "command": "C:/Program Files/forge3d/forge3d-mcp.exe",
      "args": []
    }
  }
}
```

If no editor is running (or `rpc.json` points at one that has quit), `forge3d-mcp`
starts a headless editor of its own and stops it when the client disconnects. Pass
`--spawn-editor` to always do that (an isolated scene), `--token-file <path>` to use another
token file, and `--editor <path>` if the editor isn't next to `forge3d-mcp`.

## What the assistant gets

**Tools.** One per editor operation, named after its id with dots as underscores
(`entity.create` → `entity_create`, `entity.set_property` → `entity_set_property`,
plugin operations too), with the operation's parameters as the input schema. Every one is
undoable in the editor's history. Plus:

| Tool | What it does |
|---|---|
| `screenshot` | renders the scene and returns a PNG image (`width`, `height`, `mode`, `camera` or `bookmark`) |
| `undo`, `redo` | step the history |
| `scene_query` | entities (guid, name, parent, component types), filtered by name or component |
| `entity_get` | every component of an entity as JSON |
| `terrain_sample` | a terrain's size, height range, layer count and hashes, and its heights under `[x, z]` points (`surface: true`: as roads and rivers shape it) |
| `asset_search` | the open project's assets |

**Resources.** `forge3d://scene` (the scene tree), `forge3d://selection` and
`forge3d://assets` (the asset catalogue), all JSON.

Entities are named by GUID strings; property paths are `"Component.field"`, e.g.
`entity_set_property {"entity": "…", "path": "Transform.translation", "value": [0, 1, 0]}`.

## Security

The editor's server only listens on 127.0.0.1 and only accepts clients that know the
session token, which only your user account can read. `forge3d-mcp` gives the assistant
the same powers as the editor's operations (it can change and capture the scene, and
`viewport.capture` can write a PNG where it's told); it can't run arbitrary code.
