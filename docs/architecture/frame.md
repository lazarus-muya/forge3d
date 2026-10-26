# Frame & input architecture

How a forge3d editor frame is produced, how input reaches the camera and commands, and the conventions the renderer relies on. This document is the Stage A gate deliverable (phases.md). It describes the state after P1.4; since P4.1 the viewport passes run on the render graph ([`render.md`](render.md)), inside the same frame structure.

## Modules involved

```text
editor (f3d_editor)        App, EditorUi, panels, ImGuiLayer, NavigationController, CommandRegistry
render/raster              RasterEngine (IRenderEngine): the scene as a render graph into the viewport's output
render/graph               RenderGraph, TransientPool, IRenderEngine, View
render/rhi                 Device, Swapchain, CommandContext, ShaderLibrary, ImGuiRenderer (only place with Diligent)
assets                     glTF -> MeshData (P1.4 minimal path)
core                       log, assert, math conventions, debug-draw collector, file watcher, image I/O
```

`f3d_render_rhi` is the only module that includes Diligent; `tools/check_diligent_includes.py` (ctest `unit.diligent_includes`) enforces it.

## The frame (`editor/src/app.cpp`)

```text
App::run
 └─ per frame
    1. pump_events           SDL events → ImGui backend, then:
                               a. shortcut?   key chord → keymap capture | command   (consumed)
                               b. navigation? viewport-hovered mouse/wheel/keys → NavigationController
                               c. window      close / minimise / resize (Swapchain::resize) / DPI scale
    2. deliver_replay_events --replay only: events whose frame ≤ current frame
    3. update(dt)            pending File ▸ Open result, shader hot reload (0.5 s poll, Debug),
                             fly-mode movement, debug shapes, viewport overlay text
    4. render
       4a. ImGuiLayer::begin_frame + EditorUi::draw      build the UI; panels size themselves
                                                         (the viewport recreates its output on resize)
       4b. ViewportPanel::render → RasterEngine           scene → viewport output (below)
       4c. back buffer: clear + ImGuiRenderer::render     the UI, sampling the viewport display texture
       4d. capture (last frame, --capture[-ui])            read back viewport display or back buffer
       4e. ImGui platform windows                         each detached window: own swapchain, no vsync
       4f. Swapchain::present(vsync)
    5. stats                 CPU frame time, GPU timers (frame, ui, viewport, mesh, grid, debug_lines, resolve)
```

Minimised: no rendering; the loop blocks in `SDL_WaitEventTimeout(100 ms)`.
Replays use a fixed 1/60 s step so continuous input is deterministic.

### Viewport passes (`RasterEngine::render`)

Since P4.1 these are render-graph passes; colour and depth are transients ([`render.md`](render.md)).

| Pass | Target | Notes |
|---|---|---|
| clear | colour (RGBA16F, linear) + depth (D32) | `kViewportClearColor` (linear) |
| mesh | colour + depth (write) | Lambert sun + hemispheric ambient (`SimpleLighting`), world-space vertices |
| grid | colour, depth test only | full-screen triangle; per-pixel ray/plane hit, `SV_Depth`; no depth write (no z-fighting with lines) |
| debug lines | colour, depth test only | everything added to `f3d::core::debug` this frame |
| resolve | display (RGBA8 **UNORM**) | exact linear → sRGB encode; P4.4 tonemapping goes here |

The display texture is deliberately UNORM holding sRGB-encoded bytes. Diligent's ImGui shader converts sampled colour sRGB→linear (it must, for ImGui's own sRGB-authored colours), so an sRGB-*format* texture would be linearised twice. `--capture` writes the display texture as is.

### Swapchain and sRGB

The swapchain is `RGBA8UnormSrgb`. On D3D12 the buffer is UNORM and Diligent renders through an sRGB RTV, so the RHI binds swapchain back buffers through the swapchain's RTV (a texture → RTV map in `Device::Impl`), not the texture's default view.

## Input

```text
SDL event ──► ImGui (always)
          ├─► handle_shortcut: KeyDown, not repeat, not a modifier key
          │     keymap editor capturing? → bind chord
          │     ImGui text input active?  → ignore
          │     CommandRegistry::find_by_chord → execute (consumed)
          └─► route_navigation_input (if not consumed, not replaying)
                MouseDown only over the viewport image; drags continue outside it until release
                (SDL_CaptureMouse); wheel/keys only while hovered (keys also during a drag)
                → InputEvent → NavigationController::handle(event, camera)
--replay file ──► InputEvent (same type) → NavigationController
```

- **InputEvent** (`f3d/editor/input.hpp`): mouse down/up/move (logical pixels), wheel notches, key down/up (lower-case SDL key names, e.g. `keypad 1`), modifiers (ctrl/shift/alt).
- **Input maps** (`editor/resources/input/*.json`, `forge3d.input_map` v1): `bindings: [{action, input: drag|wheel|key, button, key, modifiers}]` + optional `settings` (orbit/look/zoom speeds, wheel factor, fly speed/boost). Presets: Blender, Maya, Unreal; plus `<pref dir>/input/custom.json`. Switchable at runtime (View ▸ Navigation, Preferences).
- **Actions**: `nav.orbit|pan|zoom|fly` (drags), `nav.zoom` (wheel), `view.front|back|right|left|top|bottom|toggle_ortho` (camera), `fly.forward|back|left|right|up|down|boost` (held while `nav.fly` drag is active), anything else → `NavigationController::on_action` → App (`view.frame_selection`) or the command registry.
- **EditorCamera**: pivot + distance + yaw/pitch (quaternion; pitch clamped to ±90°). Pan moves the pivot by exactly the world distance under the cursor at pivot depth. Orthographic height = perspective framing at the pivot.

### Replay format (`forge3d.input_replay` v1)

```json
{ "format": "forge3d.input_replay", "version": 1, "preset": "blender", "viewport": [800, 600],
  "events": [ { "frame": 1, "type": "mouse_down", "button": "middle", "x": 400, "y": 300, "modifiers": [] },
              { "frame": 2, "type": "mouse_move", "x": 460, "y": 320 },
              { "frame": 9, "type": "wheel", "wheel": 1 } ] }
```

## Commands, keymap, preferences

- **CommandRegistry**: `{id, name, callback, default_chord}`. Effective chord = user override (may be "unbound") or default. `conflicts()` lists chords shared by 2+ commands (shown in the keymap editor; logged at start-up). The command palette (Ctrl+Shift+P) ranks commands by `fuzzy_score` over name and id.
- **Preferences** (`<pref dir>/preferences.json`, `forge3d.preferences` v2): UI scale (0 = follow display), theme, theme colours (`ui.colors`: accent, window_bg, text, and the viewport's selected, active, edge, face_selected, face_hovered as [r, g, b, a] sRGB 0-1; missing ones take the theme's defaults), navigation preset, keymap overrides. Loading migrates older versions (`migrate_v1_to_v2`), then rewrites the file. Saved atomically (temp + rename) on every change (a colour or slider drag once, on release). The Preferences window is a Blender-style sidebar: Interface, Themes, Viewport, Navigation, Keymap (the keymap editor, also its own window), Save & Load, System, Automation. Automated runs (`--hidden`, `--frames`, `--capture`, `--bench`, `--replay`) use defaults and never touch the user's files, including `imgui.ini`.

## Conventions

| Convention | Value |
|---|---|
| Handedness / up | right-handed, **+Y up**, 1 unit = 1 m |
| Camera | looks down −Z when unrotated |
| Clip space | depth 0..1 (`*_ZO` projections), NDC y up (both backends; verified by `integration.rhi`) |
| Front faces | counter-clockwise (glTF) |
| Matrices | GLM column-major = HLSL default packing; shaders use `mul(M, v)` |
| Vertex inputs | `F3D_LOCATION(n) … : ATTRIBn` (`shaders/common.hlsli`) |
| Colour | linear HDR inside the viewport; vertex/UI colours sRGB-encoded and linearised in shaders |
| Shaders | `shaders/<name>.<vs|ps|cs>.hlsl` → DXC → `.dxil` (D3D12) + `.spv` (Vulkan); hot reload in Debug |
| Bindings | re-setting the same resource is a no-op; a different resource needs new `Bindings` (frames in flight) |
