# forge3d — Progress Log

One entry per phase (phases.md §0.1 step 6): what was built, deviations and why, open `[human]` checks, follow-ups.

Build machine for these entries: Windows 11, Visual Studio 2026 Community (MSVC 19.50), CMake 4.4, Ninja (VS-bundled), vcpkg at baseline `20ceb8ad` (2026-10-14). clang-cl and Linux toolchains are not installed on this machine, so only the `win-msvc` presets are built locally.

---

## P0.1 Repository & build system — 2026-10-15

**Built**
- Repo files: `LICENSE` (Apache-2.0), `README.md`, `.gitignore`, `.gitattributes`, `.clang-format` (LLVM, 4-space, 120 cols), `.clang-tidy`, `.editorconfig`.
- `vcpkg.json` pinned to builtin-baseline `20ceb8adbf0504d4b0d5e4058a32f9d4fc896391`: sdl3, imgui[docking-experimental], entt, taskflow, glm, spdlog, tracy, doctest, fastgltf.
- `CMakePresets.json`: configure presets `win-msvc`, `win-clangcl`, `linux-gcc`, `linux-clang` (Ninja Multi-Config, vcpkg toolchain via `$env{VCPKG_ROOT}`, binary dir `build/<preset>`); build and test presets `<configure>-debug|release|relwithdebinfo`.
- `cmake/F3DOptions.cmake`: `f3d_options` (C++20, `/W4 /WX /permissive-` on MSVC, `-Wall -Wextra -Werror` on GCC/Clang), options `F3D_SANITIZE`, `F3D_TRACY`, `F3D_BUILD_TESTS`, `F3D_BUILD_SPIKES`.
- `cmake/F3DLayering.cmake`: `f3d_add_module(name SOURCES … DEPS … PRIVATE_DEPS … PUBLIC_LIBS … PRIVATE_LIBS …)` with an allowed-edge table; `f3d_verify_layering()` re-checks the final link graph so edges added with a bare `target_link_libraries` are caught too; Diligent may only be linked (privately) by `f3d_render_rhi`. Script mode for testing.
- All §0.5 targets with placeholder sources: 12 engine libraries, `f3d_editor` (editor code as a library so it is unit-testable) + `forge3d-editor`, `forge3d-cli`, `forge3d-mcp`, `forge3d-tests` (doctest).
- `tools/vsenv.cmd` (runs a command in the VS x64 developer environment), `tools/format.py` (clang-format / `--check`).

**Verification**
- (auto) `cmake --preset win-msvc && cmake --build --preset win-msvc-debug` — passes, zero warnings.
- (auto) Forbidden edge `f3d_core → f3d_scene` added temporarily: configure fails with "forge3d layering violation: 'f3d_core' must not depend on 'f3d_scene'" plus the allowed list. Also checked an edge added with plain `target_link_libraries` (caught by `f3d_verify_layering`). Reverted. Kept permanently as ctest `unit.layering.forbidden_edge` (script mode).
- `[human]` Linux presets build — **open** (no Linux toolchain here; CI will cover it once a remote exists).

**Deviations**
- **Diligent is not in vcpkg** (no `diligent-engine` port exists in the registry). Asked the user per §0.4; decision: fetch DiligentCore/DiligentTools with CMake `FetchContent` at a pinned release tag, linked only by `f3d_render_rhi`. Wired up in P0.3. Conflict with plan.md §2.7 ("diligent-engine in vcpkg.json") recorded here.
- Local `VCPKG_ROOT` (`C:\extras\cpp\vcpkg`) did not exist; vcpkg was cloned and bootstrapped there.
- doctest needs `DOCTEST_CONFIG_USE_STD_HEADERS` with MSVC 19.50 (its `std::ostream` forward declaration clashes with the STL).
- `.clang-tidy` additionally disables `bugprone-easily-swappable-parameters` (noisy for math code).

**Follow-ups**
- clang-format is pinned to 21.1.8 (`pip install clang-format==21.1.8`), same in CI.

---

## P0.2 Core utilities, headless flags & CI — 2026-10-15

**Built**
- `f3d/core/log.hpp`: `F3D_LOG_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL` over one spdlog logger whose sink is a thread-safe `dist_sink`. Pluggable sinks through forge3d's own `LogSink` interface (`log::add_sink/remove_sink`), so subscribers (the console panel in P1.2) never see spdlog types. stderr sink can be muted (`set_console_enabled`).
- `f3d/core/assert.hpp`: `F3D_ASSERT(cond, msg)` (debug only: log critical, break if a debugger is attached, abort; `cond` not evaluated in release) and `F3D_VERIFY(cond, msg)` (always on: log error, break if a debugger is attached, evaluates to `cond`). Breaking only happens with a debugger attached, so CI runs never trap.
- `f3d/core/guid.hpp`: `Guid` (two `uint64`, RFC 4122 v4 from a per-thread `mt19937_64` seeded by `random_device`), `==`, `<=>`, `std::hash` (splitmix finaliser, so sequential fixture GUIDs hash well too), `to_string` / `parse` (8-4-4-4-12, either case).
- `f3d/core/profile.hpp`: `F3D_ZONE`, `F3D_ZONE_N`, `F3D_ZONE_TEXT`, `F3D_FRAME_MARK` → Tracy when `F3D_TRACY=ON` (links `Tracy::TracyClient` publicly via f3d_core), no-ops otherwise.
- `f3d/core/time.hpp`: `seconds_since_process_start()` from the OS process-creation time (Windows `GetProcessTimes`, Linux `/proc/self/stat`), so "time to first frame" includes loader/DLL time.
- `forge3d-tests` doctest runner; ctest entry per `tests/unit/<module>/` (`unit.core`, `unit.editor`), plus `unit.layering.*`.
- `forge3d-editor`: SDL3 window (resizable, high-DPI), frame loop with `F3D_ZONE`/`F3D_FRAME_MARK`, flags `--frames N`, `--hidden`, `--help`; logs time to first frame. Option parsing is in `f3d_editor` and unit-tested.
- `.github/workflows/ci.yml`: lint job (clang-format 21.1.8 check, licence headers); build matrix windows-latest/win-msvc, ubuntu-latest/linux-gcc and linux-clang; vcpkg cloned at the manifest baseline with a `files` binary cache in `actions/cache`; Debug build; `ctest -L unit`; editor smoke test (Xvfb on Linux); uploads `bin/Debug`.
- `tools/check_license_headers.py` (C/C++/HLSL `//`, Python/CMake `#`, cmd `rem`).

**Verification**
- (auto) `ctest --preset win-msvc-debug -L unit` — 4/4 pass (incl. 1M-GUID uniqueness, round-trip, hashing, log sinks, VERIFY). Release preset: 5/5 pass.
- (auto) `forge3d-editor --frames 60 --hidden` exits 0; registered as ctest `integration.editor.smoke`. Time to first frame: **147 ms** (Debug, window only).
- (auto) `python tools/check_license_headers.py` passes; `python tools/format.py --check` passes.
- (auto) `F3D_TRACY=ON` build links (TracyClient.dll deployed next to the editor).
- `[human]` CI green on GitHub — **open**: needs a remote; not created (outward-facing, §0.4).

**Deviations**
- Added `F3D_LOG_DEBUG` / `F3D_LOG_CRITICAL` alongside the four required macros (assert failures log at critical).
- Linux build is written but untested here (part of the open P0.1 `[human]` check).

---

## P0.3 Diligent + SDL3 + ImGui spike — 2026-10-15

**Built**
- `cmake/F3DDiligent.cmake`: FetchContent of DiligentCore + DiligentTools at pinned commits (D3D12 + Vulkan only), `f3d_diligent` (backend factories) and `f3d_diligent_imgui` (Diligent's ImGui renderer compiled against vcpkg imgui). Reused by `f3d_render_rhi` in P1.1.
- `spikes/diligent/` (`F3D_BUILD_SPIKES=OFF` by default): `forge3d-spike-diligent` with `--rhi=d3d12|vulkan`, `--frames`, `--hidden`, `--capture`, `--no-viewports`, `--no-vsync`, `--validation`. Device + swapchain from an SDL3 window, ImGui docking + demo, multi-viewport via our own renderer hooks, compute gradient into a UAV texture shown with `ImGui::Image`, capability dump, backbuffer → PNG.
- vcpkg.json: `imgui[sdl3-binding]`, `stb`.
- Report: `docs/spikes/diligent.md` with a **GO** recommendation.

**Verification**
- (auto) `--frames 120 --hidden` exits 0 on D3D12 and Vulkan, Debug and Release; registered as ctest `integration.spike.diligent.{d3d12,vulkan}` in the spikes build.
- (visual) Captures show the ImGui demo and the compute gradient on both backends, plus a secondary viewport rendered through its own swapchain (`docs/spikes/img/`).
- (auto) D3D12 debug layer: no errors.
- `[human]` Go/no-go confirmation — **closed: GO** (user, 2026-10-15).

**Deviations**
- **Pinned commits instead of a tag.** Diligent's last tag (v2.5.6, 2024-09) is incompatible with imgui 1.92 (vcpkg's version). Pinned DiligentCore `199d9f3c` and DiligentTools `858b0380` (master, 2026-10-14). Same reproducibility as a tag.
- DiligentTools is not built; only its two ImGui renderer sources are compiled, so there is one imgui in the build.
- Platform side uses imgui's SDL3 backend, not Diligent's Win32/X11 ImGui backends.
- `core.longpaths` set through `GIT_CONFIG_*` environment variables during configure (SPIRV-Cross paths exceed MAX_PATH).

**Findings that affect later phases** (details in the spike report)
- Hybrid laptop: Diligent D3D12 picks the AMD iGPU (no RT), Vulkan picks the RTX 3050 (full RT, mesh shaders). P1.1 needs an explicit adapter policy and `--adapter`.
- Multi-viewport needs our own renderer hooks (Diligent's renderer is single-viewport) → own them in P1.2.
- One transient, non-reproducible 33 ms/present stall on Vulkan + vsync. Measure presents with Tracy in P1.1.

---

## P1.1 RHI wrapper & frame loop — 2026-10-15

**Built**
- `f3d::render::rhi` (`engine/render/rhi`): `Device`, `Swapchain`, `CommandContext`, and refcounted handle types `Buffer`, `Texture`, `Shader`, `Pipeline`, `Bindings` over `void*` + pimpl. Plain descriptor types in `types.hpp` (formats, bind flags, pipeline/sampler/vertex descs, adapter info, caps, memory usage). **No public RHI header includes or names Diligent.**
- Backend selection: D3D12 by default on Windows, Vulkan if D3D12 fails, Vulkan on Linux. An explicit `--rhi=` disables the fallback, so tests know which backend ran.
- Adapter selection (spike follow-up): `--adapter=<index>|discrete|integrated`. The default prefers a discrete GPU, never a software adapter if anything else exists. The chosen adapter is logged.
- Diligent messages routed into spdlog (info → debug level).
- GPU timers (`ScopedGpuTimer`, Diligent `DurationQueryHelper`), VRAM usage via DXGI (D3D12, and Vulkan on Windows through the device LUID).
- Swapchain back buffers render through the swapchain's own RTV (an sRGB view of a UNORM D3D12 buffer); the RHI keeps a small texture→RTV map, so `set_render_target(swapchain.back_buffer())` works like any texture.
- Shader pipeline: `shaders/<name>.<vs|ps|cs>.hlsl` → DXC (vcpkg `directx-dxc`) → `build/<preset>/shaders/<name>.<stage>.dxil` + `.spv` (`cmake/F3DShaders.cmake`, target `f3d_shaders`). `shaders/common.hlsli` provides `F3D_LOCATION(n)`. `ShaderLibrary` loads by name; in Debug builds it watches the sources (and `.hlsli` includes) and recompiles with the same DXC and flags (baked into a generated header), bumping `generation()`. A broken edit keeps the last good binary.
- Editor frame loop (`editor/src/app.cpp`): poll events → update (shader poll every 0.5 s) → render → present. Minimised windows don't render (`SDL_WaitEventTimeout`); resize → `Swapchain::resize`; DPI changes tracked (`content_scale_`, used from P1.2).
- Headless flags: `--rhi`, `--adapter`, `--no-vsync`, `--validation`, `--capture <png>` (back-buffer readback), `--bench <json>` (CPU frame stats avg/p50/p95/p99, GPU per-pass ms, RAM, VRAM, time to first frame).
- Core additions: `core::Image` + PNG I/O (stb), `process_memory`, `executable_dir`, `summarize`/percentiles, `FileWatcher` (polling).
- `tools/check_diligent_includes.py` (namespace use + Diligent header names harvested from the fetched sources), registered as ctest `unit.diligent_includes`, so CI runs it with the unit tests.

**Verification**
- (auto) `forge3d-editor --rhi=d3d12 --frames 120 --hidden` and `--rhi=vulkan …` exit 0 (`integration.editor.smoke.{d3d12,vulkan}`).
- (visual + auto) `--capture` on each backend writes the clear colour; `integration.editor.capture.<rhi>.check` reads the PNG and checks pixels against `linear_to_srgb(kWindowClearColor)` (±1).
- (auto) `integration.rhi`: a triangle drawn with the DXC-built shaders on D3D12 and Vulkan; pixel checks catch input-layout, uniform-buffer, winding and Y-flip differences. Both backends identical. Hot-reload test: edit → recompile → generation bump; broken edit → old binary kept.
- (auto) Diligent-include check: 0 problems. It found two comments naming Diligent types in public headers, now fixed.
- Debug and Release: 14/14 ctest pass, zero warnings.
- Cold start (Release): 0.83 s D3D12, 0.60 s Vulkan → `docs/budgets.md`.

**Deviations**
- Diligent fetched with `EXCLUDE_FROM_ALL` (only linked targets build); Diligent's shared-library variants no longer build.
- `CMAKE_CXX_SCAN_FOR_MODULES OFF` (CMake 4 scans every TU for modules by default; we use none).
- `tools/vsenv.cmd` now preserves the caller's `VCPKG_ROOT`: VsDevCmd silently points it at Visual Studio's bundled vcpkg, which had been used by earlier configures.
- CI GPU integration steps are `continue-on-error` (hosted runners: WARP / lavapipe) until verified on GitHub.

**Open**
- `[human]` CI on GitHub (no remote yet).
- Linux (X11 via `SDL_HINT_VIDEO_DRIVER=x11`) untested here.

---

## P1.2 ImGui shell & panels — 2026-10-15

**Built**
- `rhi::ImGuiRenderer` (`engine/render/rhi`): Diligent's ImGui renderer behind the RHI, with the multi-viewport renderer hooks from the spike. Each detached ImGui window gets its own `rhi::Swapchain`; secondary windows present without vsync, so only the main window waits. The platform layer supplies native windows through a resolver callback, so the RHI stays SDL-free. `texture_id(Texture)` for `ImGui::Image`. Renders through the target's RTV format, so sRGB back buffers convert ImGui colours correctly.
- `editor/src/ui/`: `ImGuiLayer` (context, SDL3 platform backend, scalable embedded font `AddFontDefaultVector`, dark style scaled by the display content scale, re-applied on DPI change), `Panel` base + `PanelManager` registry (stable `Title###id` window names, open state), `EditorUi` (main menu File/Window/Help, dockspace, panel toggles).
- Default dock layout via the DockBuilder: Viewport centre, Outliner left, Inspector right, Asset Browser + Console split along the bottom. Built when no saved layout exists, and by the command `editor.reset_layout` (Window ▸ Reset Layout). Layout is saved to the per-user pref dir (`SDL_GetPrefPath`); automated runs (`--hidden`, `--frames`, `--capture`, `--bench`) never read or write it, so captures are deterministic.
- `CommandRegistry` (id, name, callback): a minimal version now; P1.4 adds chords, the keymap editor and the palette. Registered `editor.reset_layout`, `editor.quit`, `editor.toggle_<panel>`.
- `ConsoleModel` (bounded, thread-safe `LogSink`, case-insensitive search, level filter, version counter) + console panel (level combo, search, clear, auto-scroll, list clipper, level colours). The model is attached before the device is created, so the console shows the whole startup log.
- Viewport panel: offscreen `RGBA8UnormSrgb` target sized to the panel × framebuffer scale, recreated on resize, shown with `ImGui::Image`; rendered between building the UI and drawing it.
- `--capture` now captures the viewport (phases.md §0.6); `--capture-ui` captures the whole window.
- Frame order: build UI → render viewport → clear back buffer + draw UI → capture → platform windows → present. GPU timers `viewport`, `ui`, `frame`.

**Verification**
- (visual) `--capture-ui` (`integration.editor.capture_ui`): the default layout with all five panels, menu bar, and live log lines in the console.
- (auto) Console model unit tests: log calls through the real log reach the model; level/search filters; bounded history; command registry.
- (auto) `--capture` pixel check now targets the viewport background (`kViewportClearColor`) on both backends.
- Visible-window runs with multi-viewport enabled exit 0 on D3D12 and Vulkan.
- 15/15 ctest pass, zero warnings.

**Deviations**
- Diligent's "built without PIX support" warning is logged at debug level.

**Open**
- `[human]` Undocking panels into separate OS windows (drag a tab out of the main window). The renderer path was verified programmatically in the spike; interactive dragging not tried.

---

## P1.3 Viewport basics & navigation — 2026-10-15

**Built**
- Conventions (`f3d/core/math.hpp`): right-handed, **Y up**, metres, clip depth 0..1 (`*_ZO`), column-major matrices = HLSL default packing. GLM moved into `f3d_core` (public), with `GLM_FORCE_INTRINSICS` + `GLM_ENABLE_EXPERIMENTAL` set for every target (ODR-safe). `core::Bounds`.
- Debug lines (`f3d/core/debug_draw.hpp`): thread-safe immediate-mode `debug::line/box/sphere` collector in core, so any layer can draw. `render::raster::ViewportRenderer` draws and drains it once per frame.
- `f3d_render_raster`'s first real code: `View`/`ViewConstants` (`shaders/view.hlsli`) and `ViewportRenderer`:
  - Infinite ground grid (`grid.ps.hlsl`): per-pixel ray/plane intersection, anti-aliased 1 m and 10 m lines that fade out when denser than ~2 px, red X / blue Z axis lines, distance and grazing-angle fade, depth-tested via `SV_Depth`.
  - Debug-line pass.
  - Pipelines rebuilt automatically when the shader library's hot-reload generation changes.
- Viewport colour pipeline: the scene renders into a **linear RGBA16F** target, and a resolve pass (`resolve.ps.hlsl`, exact sRGB encode; P4.4 adds tonemapping here) writes an **RGBA8 UNORM display texture**. The UI shows that texture and `--capture` writes it. This fixed a double gamma conversion: Diligent's ImGui shader linearises *after* sampling, so an sRGB-format texture was converted twice.
- `EditorCamera`: orbit pivot + distance + yaw/pitch (quaternion, no gimbal issues at the poles); perspective/orthographic (ortho height matches the perspective framing at the pivot); orbit, exact cursor-following pan, zoom, first-person look, fly move; `frame(bounds)`; front/back/right/left/top/bottom views.
- Input-map system: platform-independent `InputEvent`s; `forge3d.input_map` JSON (version 1): `bindings` of `{action, input: drag|wheel|key, button, key, modifiers}` + optional `settings`. Presets `editor/resources/input/{blender,maya,unreal}.json` (copied next to the executable), plus the user's `<pref dir>/input/custom.json`. Switch at runtime from View ▸ Navigation. `NavigationController` maps drags (orbit/pan/zoom/fly-look), the wheel, view keys and fly keys (Unreal: RMB + WASDQE).
- `--replay <input.json>` (`forge3d.input_replay`, events tagged by frame) feeds recorded events into the controller with a fixed 1/60 s step. The replay may choose a preset. The final camera is written to the `--bench` JSON.
- Viewport panel: depth + HDR + display targets, corner axis widget (ImGui overlay, depth-sorted), "preset · projection" overlay. View menu: navigation preset, orthographic, front/right/top, frame scene, grid, debug shapes (also `F3D_DEBUG_SHAPES=1`). `ConfigWindowsMoveFromTitleBarOnly`, so dragging in the viewport navigates instead of moving the window.

**Verification**
- (auto) For each preset, the scripted replay `tests/data/input/nav_<preset>.json` (orbit +120/+40 px, pan +60/−30 px, two wheel notches, in that preset's chords) ends at the camera computed directly with the camera primitives (yaw, pitch, distance, target, position within 1e-4). Checked twice: in-process (`unit.editor`) and end-to-end through `forge3d-editor --replay … --bench` (`integration.editor.replay.<preset>[.check]`).
- (auto) Camera conventions, view presets, fly mode, preset parsing and errors, debug-draw shapes: unit tests.
- (visual) `docs/img/p1.3-editor.png` (`--capture-ui`): grid with axis lines, corner axis widget, navigation overlay; with `F3D_DEBUG_SHAPES=1`, debug box/sphere/line on both backends.
- (auto) Viewport capture check updated: > 40 % background pixels in the clear colour, plus red and blue axis pixels.
- 21/21 ctest pass; zero warnings; no Diligent warnings or errors in the log on either backend.

**Deviations**
- `nlohmann-json` added to vcpkg.json now (P2.2 planned it) for presets and replay files.
- RHI `Bindings::set` with the already-bound resource is a no-op. Binding a different resource to used Bindings is not allowed (Diligent mutable-variable rule; frames in flight may read the old descriptors), so the renderer creates new Bindings when a target is recreated.
- Maya and Unreal presets share forge3d's Alt+H/J/K/G view keys (Unreal's convention); Maya has no single built-in equivalent.

**Open**
- `[human]` Navigation feel with a real mouse/tablet in each preset.

---

## P1.4 Preferences, palette & first mesh — 2026-10-15

**Built**
- Preferences (`f3d/editor/preferences.hpp`): `forge3d.preferences` JSON v2 in the user pref dir. UI scale (0 = follow display), theme (dark/light), navigation preset, keymap overrides. Migration chain (`migrate_v1_to_v2`: flat v1 → grouped v2), re-saved after migrating. Atomic save (temp + rename). Validation: out-of-range scale → automatic, unknown theme → dark. Automated runs never read or write it.
- Commands & keymap: `Command{id, name, callback, default_chord}`. `KeyChord` (`Ctrl+Shift+P` ⇄ parse). The registry resolves override-or-default chords, `find_by_chord`, `conflicts()` / `has_conflict`, and overrides ⇄ preferences. Shortcut dispatch in the App runs before viewport navigation and is skipped while typing. Default chords: palette Ctrl+Shift+P, open Ctrl+O, quit Ctrl+Q, preferences Ctrl+,, keymap Ctrl+Alt+K, console Ctrl+\`.
- Keymap editor panel (filter, click-to-rebind capturing the next chord, Escape cancels, Backspace unbinds, Unbind/Reset buttons, conflicts in red with a tooltip naming the other commands, conflict count). Preferences panel (automatic/explicit UI scale, theme, navigation preset; applied and saved immediately). Command palette (top-centre popup, fuzzy over name and id, arrows + Enter, shows shortcuts). File ▸ Open (SDL3 file dialog, result handed to the main thread), Edit ▸ Command Palette / Preferences / Keymap; menus show live shortcuts.
- Fuzzy matcher (`f3d/editor/fuzzy.hpp`): subsequence match, best alignment over all start positions; bonuses for word starts (separators and camelCase), consecutive runs and a first-character match; penalties for gaps and leading characters. Ties go to the shorter name, then alphabetical.
- First mesh: `assets::load_gltf` (fastgltf 0.9) flattens the default scene into world-space `MeshData`. It applies node transforms (normal matrix for normals, winding flip for mirroring transforms), uses base-colour factors as sRGB vertex colours, and generates smooth normals and indices when missing. `ViewportRenderer::set_mesh` uploads immutable VB/IB. A Lambert pass (`mesh.vs/ps`, fixed sun + hemispheric ambient via `SimpleLighting`) draws before the grid. `--open <model>` loads at start-up and frames the camera; the window title shows the model.
- Test models `tests/data/gltf/`: Box (CC-BY-4.0, Cesium), Fox (CC0 model, CC-BY rig), with `LICENSES.md`.

**Verification**
- (auto) Preferences round-trip, v1 fixture (`tests/data/preferences/v1.json`) migrates to v2, invalid input, atomic save/load. Chord parsing and keymap defaults/overrides/conflicts. Fuzzy ranking (`reset`→Reset Layout, `tc`→Toggle Console, `tab`→Toggle Asset Browser, `pal`→Command Palette, `vt`→View Top, …).
- (auto) glTF loader: Box (24 vertices, 36 indices, unit-cube bounds, unit normals), Fox, a generated triangle without normals/indices (normal +Z, node translation applied), error cases.
- (auto + visual) `integration.editor.open_gltf[.check]`: Fox framed and lit (coverage + shading-range check). DamagedHelmet captured on D3D12 and Vulkan (`--open tests/data/gltf/DamagedHelmet.glb --capture`): lit, correctly oriented, identical on both backends.
- (auto) `--bench` recorded in `docs/budgets.md`: D3D12 AMD iGPU 2.1 ms CPU / ~1.5 ms GPU per frame at 1600×900.
- Debug and Release: 24/24 ctest pass, zero warnings.

**Deviations**
- **DamagedHelmet is not committed.** Khronos lists it as derived from a CC-BY-NC-4.0 model, which can't be redistributed in an Apache-2.0 repository. It is git-ignored and fetched on demand by `tools/fetch_test_assets.py` for the local visual check. The committed ctest uses Fox instead. CesiumMilkTruck was also skipped (embedded trademarked logo).
- Textures and materials are ignored (base-colour factor only), as specified for this phase (Lambert from a fixed sun). PBR comes in P4.2.

**Open**
- `[human]` ≥ 60 fps on the reference iGPU (Iris Xe class) and a Tracy breakdown check (`-DF3D_TRACY=ON`; zones exist for the frame, UI, viewport passes, uploads and presents).
- Vulkan + NVIDIA uses ~364 MB RAM (driver), above the 250 MB idle budget, which P3.5 must address or justify.

---

## Stage A gate — 2026-10-15

| Gate item | Result |
|---|---|
| Both RHI backends run `--frames 300` cleanly | ✅ D3D12 and Vulkan, hidden, Release: exit 0, **no** warnings/errors in the log. Also 300-frame benches on both adapters. |
| Cold start measured against < 2 s | ✅ 0.33–1.04 s to an interactive window with the full UI (Release; per backend/adapter in `docs/budgets.md`) |
| Frame/input architecture written up | ✅ `docs/architecture/frame.md` |

**Open `[human]` items carried out of Stage A** (none block Stage B):
- P0.1/P0.2: Linux presets build; CI green on GitHub (needs a remote; not created).
- P1.2: undocking panels into OS windows by dragging.
- P1.3: navigation feel with a real mouse/tablet.
- P1.4: ≥ 60 fps + Tracy breakdown on the reference iGPU.

---

## P2.1 ECS & components — 2026-10-15

**Built** (mostly landed with the Stage A commit; recorded here)
- `f3d::scene::World` (`engine/scene`) wraps `entt::registry`. Every entity has `Name`, `core::Guid`, `Transform` (local TRS + cached world matrix + dirty flag) and `Hierarchy` (intrusive ordered tree: parent/first/last child/prev/next sibling). `MeshRenderer`, `Light`, `Camera` are optional.
- Ordered forest: roots form an ordered chain owned by the World; `create`/`set_parent` insert at an index (`kAppend`), `set_parent` refuses cycles and can keep the world transform. `Guid` → entity map is kept in sync on create/destroy (including destroys through the registry, via an `on_destroy<Guid>` hook).
- World-transform propagation is an iterative pre-order walk that only recomputes dirty subtrees; a 2000-deep chain is exercised in the test (no recursion).
- `render::raster::ViewportRenderer` reads `MeshRenderer` + `Transform` from the `World` and draws every mesh; `assets::spawn_gltf` builds the entity hierarchy from a glTF default scene.

**Verification**
- (auto) `unit.scene`: core components on create, GUID lookup across create/destroy, ordered children/roots, cycle rejection, 2000-deep propagation, reparent-keeps-world (200 random reparents), `set_world_matrix`/`decompose` (mirrors → negative X scale).
- (visual) `--open tests/data/gltf/Fox.glb --capture` renders the model built as entities.

**Deviations / fixes**
- The committed test used `CHECK(entity == entt::null)` and `std::vector<entt::entity>(n, entt::null)`, which are ambiguous against EnTT 3.16's conversion operators on MSVC (`/permissive-`): fixed with extra parentheses and an explicit `static_cast<entt::entity>(entt::null)`. Without this the Debug tree did not compile.

## P2.2 Reflection & inspector — 2026-10-15

**Built**
- `f3d/core/reflect.hpp` + `reflect.cpp`: a single type registry on `entt::meta` (EnTT 3.16's `entt::meta_factory<T>{}`). `FieldMeta` (tooltip, unit, range min/max/step, hidden, read_only) is attached per field via `.custom<FieldMeta>(...)`; `field_meta(meta_data)` reads it back. `register_value_types()` registers bool/int/uint/int64/float/double/string/`glm::vec2/3/4`/`quat`/`Guid`/`AssetRef` so the JSON layer treats them as scalars/arrays.
- Reflection-driven JSON: `value_to_json` / `value_from_json` handle scalars, enums (as integers), strings, glm vectors/quaternions, `Guid` (8-4-4-4-12), `AssetRef` (`{guid, path}`) and nested reflected classes (object of fields). `reflect_json.hpp` adds `to_json<T>` / `from_json<T>`.
- `f3d/scene/reflect.hpp` + `reflect.cpp`: `register_types()` registers `Name`, `Transform` (translation/rotation/scale only), `MeshRenderer`, `Light`, `Camera` and the `LightType` / `CameraProjection` enums, with field metadata. `for_each_component()` visits the reflected components present on an entity (a meta_any aliasing the live component).
- Inspector panel (`editor/src/ui/reflect_widgets.*` + `panels.cpp`) generated from reflection: bool → checkbox, enum → combo, integral → drag-int, float/double → drag-float, string → input, vec2/3/4 and quat → drag-floatN, `Guid`/`AssetRef` → text. A combo over the scene's entities provides the temporary selection the phase calls for.

**Verification**
- (auto) `unit.scene`: every component round-trips through JSON unchanged (serialise → deserialise → serialise equality); enums round-trip by value; field metadata is attached; unsupported types are rejected.
- (visual) `--open tests/data/gltf/Fox.glb --capture --capture-ui` shows the inspector with the selected entity's `Name`, `Transform` and `MeshRenderer` fields.

**Deviations**
- EnTT 3.16 replaced `entt::meta<T>()` with `entt::meta_factory<T>{}` and `.prop()` with `.custom<T>(...)` (one custom value per node, keyed by type) — `FieldMeta` is that single value.
- Enums serialise as integers (round-trip-exact); the inspector still shows names from the enum's registered enumerators.

## P2.3 Operation registry & undo — 2026-10-15

**Built**
- `f3d::ops::Operation` (`engine/ops`): `id`, `display_name`, reflected `params_type`, JSON params, `execute`/`undo`, `merge`, `memory_size`. `OperationBase<Params>` implements the params half from reflection. `Context` = `{World&, ChangeBus&}`.
- `OpRegistry`: id → `OpInfo{id, display_name, params_type}` + factory; `create(id, json)`, `list()`, and `param_schema(type)` (fields + range/unit/tooltip) for the later CLI/RPC/MCP surfaces.
- `OpStack`: execute with merge, undo/redo, nested `Transaction`s, a cap by entry count **and** bytes (oldest evicted first). `ChangeBus` fans out `ChangeEvent{kind, entity}` (`EntityCreated/EntityDestroyed/ComponentChanged/HierarchyChanged/SelectionChanged`).
- Core ops (`register_core_ops`): `entity.create` (explicit or generated GUID), `entity.delete` (reflection snapshot of the subtree; undo recreates it exactly), `entity.set_property` (`"Component.field"` + JSON value; merges consecutive edits to the same path so a drag is one entry), `entity.reparent` (keeps the world transform; undo restores old parent/index/local TRS), `entity.duplicate` (fresh GUIDs; undo destroys, redo reuses the same GUIDs).
- `f3d/scene/snapshot.hpp`: reflection-based subtree/world snapshots + `restore_subtree` (reused by `.forge` in P2.6).
- Editor: the inspector now reports edits as `(entity, path, json)` and the app routes them through `entity.set_property` on the stack; new **Operation Log** panel (undo history + redo entries) docked with the Outliner; `edit.undo` (Ctrl+Z), `edit.redo` (Ctrl+Shift+Z), `edit.redo_alt` (Ctrl+Y) commands and Edit-menu items.
- `docs/architecture/ops.md`.

**Verification**
- (auto) `unit.ops`: 9 cases — registry listing/schema/build; create/delete/reparent/duplicate undo & redo; delete restores the subtree byte-for-byte; drag merge → one entry; history cap evicts oldest; transactions (nested) undo/redo as one entry; **1,000 random operations** undo to the exact starting snapshot and redo to the exact final snapshot.
- (visual) `--capture-ui` shows the Operation Log tab next to the Outliner and the reflection inspector.
- 9/9 `unit` ctest pass, zero warnings.

**Deviations**
- `entity.set_property`'s `value` is arbitrary JSON and can't be a reflected field, so the op overrides `params_json`/`set_params_json` to carry it alongside the reflected `entity`/`path`; the schema still comes from reflection.
- `entity.create` takes an optional explicit GUID (redo/tests); normal editor calls leave it null and one is generated.

**Follow-ups**
- P2.6 should persist via `scene::snapshot_world` rather than re-inventing scene JSON.
- The importer still replaces the scene directly (world.clear + spawn); routing import through operations is deferred until the asset/op surfaces exist (M3/M5).

### P2.1–P2.3 review — 2026-10-15

A review before starting P2.4 found these defects in the (then uncommitted) P2.1–P2.3 work. All are fixed, with tests:

- **Inspector transform edits had no visible effect.** `entity.set_property` wrote `Transform.*` fields through reflection without marking the transform dirty, so world matrices (and the rendered mesh, and children) never updated. Now `Transform.*` edits call `World::mark_dirty` on execute and undo. Test: a parent edit propagates to its child's world matrix.
- **Over-eager merging.** Any two consecutive `entity.set_property` calls on the same path merged, including separate drags minutes apart, every keystroke of a text field, or two RPC calls. Merging is now opt-in: `OpStack::execute(op, merge)`. The inspector passes `merge` only for the second and later edits of one widget interaction. Merges also work inside transactions (they fold into the matching child), which gizmo drags need.
- **History cap.** `trim()` skipped the entry-count cap whenever the byte cap was 0. It also re-serialised every snapshot (`memory_size()` → `json::dump()`) on each loop iteration, which made the history quadratic. Entry sizes are now measured once, when recorded. `unit.ops` went from 27.7 s to 15.9 s (Debug); the remaining time is the 1,000-step test snapshotting the world at every step.
- **Stale history after opening a model.** `load_model` cleared the world but not the undo stack, and `World::clear` kept the selection. Undo after an open would resurrect entities from the previous file. Both are cleared now.
- **Schemas were useless.** Value types were registered without names, so `param_schema` reported every field type as `"?"`. They now have names (`guid`, `vec3`, `string`, …). `std::vector<T>` fields are reflected as JSON arrays.
- **Selection ops had no reflected params** (no schema) and accepted unknown entities. They now use `{entities: guid[], active}` / `{entity}` params; unknown entities are dropped or refused.
- `entity.set_property` with a malformed value threw out of `execute`; it now fails cleanly.
- The inspector edited quaternions as raw `xyzw` drags, which de-normalised them. Rotation is now edited as XYZ Euler degrees.

## P2.4 Selection, picking & gizmos — 2026-10-15

**Built**
- Selection: `scene::Selection` (ordered GUIDs + active), owned by the `World`, changed only by `selection.set/add/remove` (undoable, `SelectionChanged` events). `editor::combine_selection` implements replace / Shift-click toggle / Shift-box add / Ctrl remove.
- GPU picking: `ViewportRenderer::pick` / `pick_region` render an ID pass (`pick.ps.hlsl`, entity id + 1 → `R32_UINT`, depth-tested) and read it back (`CommandContext::read_texture_uint32`, `clear_color_uint`). The viewport turns a left click into a 1×1 pick and a left drag into a box pick. The app resolves both at the start of the next frame and runs `selection.set` only if the selection changed. Alt+LMB stays navigation (Maya/Unreal).
- Gizmos: ImGuizmo (vcpkg `imguizmo`) translate/rotate/scale drawn over the viewport. `editor::TransformTool` (UI-free) owns the logic: pivot modes **median**, **individual origins**, **active element** and **3D cursor**; **local/world** orientation; it skips children of selected parents. A drag opens one transaction, runs merging `entity.set_property` edits per frame and commits on release, so one drag = one undo entry. A click without movement records nothing; Escape mid-drag aborts.
- Snapping: grid increments (ImGuizmo snap: move m, rotate °, scale), **surface** (raycast), **vertex** (nearest vertex within a 12 px cone around the cursor ray). Both ignore the dragged entities. `assets::SceneBvh` is a median-split BVH over the meshes' world bounds; triangles are only tested for the meshes whose bounds the ray enters. `core::ray_aabb` and `core::ray_point_distance` were added alongside `snap_grid` and `ray_triangle`.
- Editor: a tool overlay in the viewport (Move/Rotate/Scale, World/Local, pivot, grid snap and steps, surface/vertex snap). Selection highlight: debug-line bounds, active entity brighter. A 3D cursor (Shift+right-click on a surface, or on the ground plane). The inspector shows the active selected entity, replacing the temporary combo. Commands: Delete (`Delete`), Duplicate (`Ctrl+D`, selects the copies), Select All (`Ctrl+A`), Deselect All (`Alt+A`), Move/Rotate/Scale tools (`W`/`E`/`R`), Toggle Local/World. Frame Selection now frames the selection. Shortcuts are ignored during navigation drags, so Unreal's RMB + WASD fly mode doesn't switch tools.
- `entity.duplicate` takes an optional `guid` for the copy's root, so the caller can select it.

**Verification**
- (auto) `integration.raster`: the picking test renders a known scene into the ID buffer on D3D12 and Vulkan and checks the entity at fixed pixels, a miss, and the region query.
- (auto) `unit.editor` `transform_tool_test.cpp`: a simulated 30-frame gizmo drag over two entities produces **exactly one** undo entry; undo/redo restore and reapply it. Also: rotation about each pivot mode, local-space orientation, children of selected parents moved once, cancel restores, a click records nothing, and the selection-combining rules.
- (auto) Snapping math: `snap_grid`, `ray_triangle`, `ray_aabb` (inside, parallel, behind, `t_max`, empty), `ray_point_distance`. The BVH agrees with brute force over a 10×10 grid of meshes and tests fewer than 5 meshes per ray; skip/exclude filters work; nearest-vertex snaps inside the cone and ignores vertices outside it.
- (visual) `--open tests/data/gltf/Fox.glb --capture-ui`: selected mesh highlighted, translate gizmo at the pivot, tool overlay, inspector showing the active entity.
- 27/27 ctest (unit + integration) pass, Debug, zero warnings.

**Deviations**
- Box select uses the ID buffer (as specified), so it selects only entities with at least one visible pixel in the box: occluded entities are not selected.
- The scene BVH is rebuilt for each snap query during a drag (cheap at current scene sizes; meshes are tested brute force once their bounds are hit). A per-mesh triangle BVH comes with cooked meshes (P3.2) if profiling asks for it.

**Open**
- `[human]` Gizmo feel: dragging, snapping and pivot modes with a real mouse.

## P2.5 Outliner, layers & tools — 2026-10-15

**Built**
- Layers: a `scene::Layer` component (entity → layer id) and an ordered `LayerTable` in the `World` (`LayerInfo{id, name, visible, locked}`). The default layer is implicit (null id). Visibility and lock are inherited down the hierarchy (`scene::is_visible` / `is_pickable`). The renderer skips hidden entities. The ID pass skips hidden and locked ones, so picking reaches what is behind them. The snap BVH skips hidden ones.
- Ops `layer.create/delete/rename/set_visible/set_locked/assign`, all undoable (delete restores the row at its index and every assignment).
- Camera bookmarks: `CameraBookmark` (orbit camera parameters) in a `BookmarkTable` on the `World`, saved with the scene. Ops `bookmark.save` (replace-by-name, undoable), `bookmark.delete`, and `bookmark.recall`. Recall is the first **transient** operation (`Operation::records_history() == false`): it sets `World::view_request()`, which the editor applies to its camera; it records nothing and keeps the redo history. View ▸ Bookmarks menu (save current view, recall, delete).
- `register_builtin_ops()` registers every engine op.
- `scene::snapshot_world` now includes `layers` and `bookmarks`. `scene::restore_world` rebuilds a scene from a snapshot; P2.6 will save with it.
- `editor::SceneEditor` (UI-free, unit tested): the edit actions behind the outliner, layers panel and commands. Selection changes (skipped when nothing changes), create entity, delete/duplicate selected roots, rename. `move_entities` handles outliner drags: inside / before / after a sibling / to the root, multi-entity in order, skipping covered children, refusing cycles, one undo step. Plus layer creation and assignment, and bookmark naming. The App's P2.4 edit commands moved onto it.
- Outliner panel: hierarchy tree (roots expanded), click / Ctrl-toggle / Shift-range selection, drag & drop to reparent or reorder (top/bottom quarter of a row = before/after, else inside; empty area = root), inline rename (double-click, F2 `edit.rename`, context menu), search filter (case-insensitive; shows matches with their ancestors). Context menu: rename, create child, duplicate, delete, move to root, layer assignment. Hidden entities are dimmed and labelled "(hidden)"; locked ones "(locked)".
- Layers panel (docked under the outliner): show/lock checkboxes, inline rename, entity counts, a "Default" row, context menu (assign selection, select entities, rename, delete), new layer, move selection to default.
- Measure tool (`editor::MeasureTool`, `M` / overlay checkbox): clicks place points on surfaces (or the ground plane), up to 3. Shows segment lengths and the angle at the middle point as viewport labels, drawn with debug lines. Right-click or Escape clears. The 3D cursor now uses the same surface query.
- `--scene <ops.json>` (phases.md §0.6): runs a `forge3d.ops` operation script after `--open` (`f3d/ops/script.hpp`, reusable by the CLI). Optional all-or-nothing transaction.

**Verification**
- (auto) `layer_ops_test.cpp`: create → rename → assign → hide → lock → delete; undo each step back to the exact earlier snapshot, then redo forward. Plus inherited visibility/lock, invalid requests leave no undo entry, bookmark save/replace/delete undo, recall transient (redo history kept), `restore_world` round trip.
- (auto) `integration.raster` pick test: a locked front triangle is skipped by `pick` and `pick_region`, so the one behind is picked; hidden layers are skipped too.
- (auto) `scene_editor_test.cpp`: outliner moves (inside, before, anchor-is-moved, cycles, covered children), delete/duplicate of selected roots as one undo step, rename no-op, layer/bookmark naming; measure-tool math. `script_test.cpp`: format/version checks, first-failure reporting, transaction rollback, the committed scene script.
- (visual) `integration.editor.outliner_layers`: `--open Fox.glb --scene tests/data/scenes/outliner_layers.json --capture-ui` shows the hierarchy (Fox, Props/Rocks locked, Crates hidden and dimmed, Lights/Sun), the multi-selection highlighted, the Layers panel with show/lock state and counts, and the inspector on the active entity.
- 28/28 ctest pass, Debug, zero warnings.

**Deviations**
- Layer visibility/lock is inherited down the hierarchy (not stated in the plan): a hidden parent hiding its children is what the outliner shows and what users expect.
- `bookmark.recall` changes no saved state, so it is transient (not undoable) instead of an undo entry.

**Open**
- `[human]` Outliner drag & drop and rename with a real mouse; measure-tool feel.

## P2.6 `.forge` v1 & CLI — 2026-10-15

**Built**
- `f3d::io` archive (`archive.hpp`): a streaming ZIP writer and a central-directory reader. Entries are stored (method 0) or zstd (method 93, vcpkg `zstd`), never deflated. UTF-8 names, fixed timestamps (same scene → same bytes), CRC-32 and size verified on read, clear limits (no ZIP64). A rejected entry (duplicate/bad name) leaves the archive usable; an I/O error doesn't.
- `.forge` v1 (`forge_file.hpp`, spec in `docs/format.md`): `scene.json` (`scene::snapshot_world`: entities, layers, bookmarks), optional `editor.json` (editor camera), `chunks/<name>` binary payloads, and `manifest.json` (stored, written last: `format`, `format_version`, `app_version`, a chunk table with kind/compression/sizes/CRC). Loading checks the manifest and chunk table, runs the **migration chain** (`MigrationChain`, one step per `v_n → v_n+1`; refuses newer files and missing steps), then `scene::restore_world`.
- **Atomic save**: `<file>.tmp` → fflush + `_commit`/`fsync` → close → rename over the target. `ForgeSaveOptions::fail_before_rename` simulates a crash for tests.
- Autosave & recovery: `editor::Document` (UI-free, unit tested). It holds the project path, dirty tracking through `OpStack::state_id()` (a new per-entry id, changed by merges; undoing back to the saved state is clean; transient ops don't count), save/open (with the editor camera), `recover()` (loads the autosave in place of the project, marked modified), and autosave timing. The editor autosaves a modified scene every `files.autosave_minutes` (new preference, default 5, 0 = off; slider under Preferences ▸ Files) to `<name>.autosave.forge`, or `<pref dir>/autosave/untitled.autosave.forge`. A real save deletes it. Opening a project with a newer autosave, or starting with an untitled autosave present, shows a **Recover / Open the saved file (Discard)** prompt.
- Editor File menu: New (Ctrl+N), Open… (Ctrl+O: `.forge` or glTF), Import glTF… (Ctrl+I), Save (Ctrl+S), Save As… (Ctrl+Shift+S). New/Open/Quit (including the window's close button) ask **Save / Don't Save / Cancel** when there are unsaved changes. The window title shows the document name and `*`. `--open` accepts `.forge` too. Automated runs never prompt, autosave or touch user files.
- **glTF import through operations**: `assets::gltf_import_ops` turns a glTF scene into a `forge3d.ops` script (`entity.create`, `entity.set_property` for non-identity TRS, `entity.add_component` for MeshRenderers). Importing into a scene is one undo step that selects the import; opening a model as a new scene clears the history. This retires the P2.3 follow-up "the importer writes the scene directly"; `spawn_gltf` stays for tests.
- New ops `entity.add_component` / `entity.remove_component` (MeshRenderer, Light, Camera; optional initial `value`; undo restores the removed values). Inspector: **Add Component** button and a **Remove Component** context menu on component headers. `OpInfo::extra_params` documents free-form params (`value`) in schemas.
- `forge3d-cli`: commands run in order on one scene + history: `new`, `open`, `save`, `run-op <id> --params <json>`, `run <ops.json>`, `undo`, `redo`, `list-ops [--json]`, `print [--out]`, `info`, `version`, `help`. Exit code 1 at the first failure. Logs go to stderr at warn level, so stdout is command output.
- `core::version()` (CMake project version) is written into manifests.

**Verification**
- (auto) M2 exit, `integration.cli.edit_save` → `integration.cli.reload` → `integration.cli.reload.check`. One CLI process creates 4 entities, transforms two (translation, rotation), reparents, deletes, creates a layer, undoes 3 and redoes 2, prints the scene and saves. A second process opens the file and prints. `cmake -E compare_files` finds the two dumps identical.
- (auto) Killing the process mid-save: `forge: a save that dies before the rename…` saves, modifies the scene, saves with `fail_before_rename`. The original file still loads with the old contents, the `.tmp` is left beside it, and the next good save replaces both.
- (auto) The v1 fixture `tests/data/forge/v1_basic.forge` (written once by the CLI, see `tests/data/forge/README.md`) loads with its 7 entities, 3 layers, bookmark, transforms and inherited lock, both in `unit.io` and via `integration.cli.open_fixture`.
- (auto) `unit.io`: CRC-32 check value; stored/zstd/incompressible/empty entries round-trip; duplicate names rejected; a flipped byte is caught by the CRC; non-ZIP input rejected; save→load reproduces scene, editor state and chunks; saves are byte-stable; migrations run in order (a synthetic v0 file migrated by a custom chain; a failing step and a too-new file report errors); autosave paths and newer-than checks.
- (auto) Interoperability: Python's `zipfile` reads the fixture (`unit.forge_fixture_zip`) and a CLI-written file (`integration.cli.zip_interop`) and verifies every CRC (zstd entries included, on Python 3.14).
- (auto) `document_test.cpp`: dirty tracking (undo back to the save is clean; merges and new edits are dirty; transient ops aren't), open/reset/failed open, autosave timing, recovery marked dirty, a real save removes the autosave, untitled autosave location. `import_ops_test.cpp`: the op-based glTF import reproduces `spawn_gltf`'s hierarchy, transforms and mesh refs and undoes in one step; add/remove component undo exactly.
- (visual) `--open tests/data/forge/v1_basic.forge` and `--open Fox.glb --capture-ui` (import via ops: same scene as before; inspector shows Add Component).
- 37/37 ctest pass (Debug and Release), zero warnings.

**Deviations**
- **No miniz.** The plan says "zip-style with miniz, store/zstd per entry". miniz can only write stored or deflate entries; it can't label an entry as ZIP method 93 (zstd). So `f3d::io` has its own small ZIP reader/writer (~400 lines), checked against Python's `zipfile` as an independent implementation. miniz was not added to vcpkg.json.
- No ZIP64 (limits documented): enough until terrain chunks get very large; the writer refuses rather than writing a broken file.
- Mesh references keep absolute source paths in v1; project-relative paths come with the VFS (P3.1) as a format migration.
- `editor.json` is an addition to the plan's entry list; it keeps the editor camera out of the scene.

**Open**
- `[human]` The interactive flows: Save As / Open dialogs, the unsaved-changes and recovery prompts in a real session.

## P3.1 VFS, asset identity & jobs — 2026-10-15

**Built** (design: `docs/architecture/assets.md`)
- `f3d::core::vfs`: `<mount>://<path>` with normalisation and mount-escape protection; `FileSystem` (mount/unmount, exists/read/list/write/remove, native ↔ virtual mapping); `DirectoryBackend` (atomic writes, modification stamps). `f3d::io::ArchiveBackend` mounts a `.forge` file read-only, with directories implied by entry names.
- `f3d::core::jobs::JobSystem` on Taskflow 4.1: two executors as priorities (interactive: all cores but one; background: a quarter), `CancellationToken`, and a main-thread continuation queue run once per frame. A job cancelled before it starts never runs; a cancelled job never runs its continuation; job exceptions are logged.
- Asset identity: `.meta` sidecars (`forge3d.meta` v1: GUID, importer, settings, source hash at last import) created on first sight. `AssetDatabase` maps GUID ↔ VFS path. Scans skip hidden directories and temp files, regenerate invalid metas, give copies with a duplicated GUID a new identity, report orphaned metas, and re-hash only changed files. `scan()` runs on a worker; `apply()` on the main thread.
- Editor: `engine://` mounts the resources; `project://` follows the document's directory (open / Save As / New). A background scan job runs on every mount change and `assets.rescan`. The Asset Browser lists the database (path, importer, import state, GUID tooltip, filter) until P3.3 replaces it. Automated runs don't scan, so tests never write `.meta` files into the source tree.

**Verification**
- (auto) Renaming/moving a source with its `.meta` keeps the GUID (`assets: renaming or moving…`); a `.meta` without its source is reported as orphaned and kept (`assets: orphaned .meta files…`). Also: first sight creates metas (not for non-assets or hidden cache dirs), rescans reuse hashes, copies get new GUIDs, edits re-hash, corrupt metas regenerate, meta parse/serialise round trip.
- (auto) A cancelled job never runs its continuation: cancelled while queued (work doesn't run), while running (the work sees the token), and after finishing but before the main thread ran it. Also: work on workers, continuations on the calling thread, interactive work runs while the background pool is blocked, 200 mixed jobs.
- (auto) VFS: parsing/normalisation/escape rejection, directory mounts (read/write/list/remove, read-only mount, native/virtual mapping), a `.forge` mounted as a read-only backend.
- 37/37 ctest pass, zero warnings.

**Deviations**
- Source hashes are FNV-1a 64 (change detection, not security); no extra dependency.
- Priorities are two executor pools rather than per-task priorities in one queue: simpler, and interactive work can never be starved by background work.

## P3.2 Importers & cooking — 2026-10-15

**Built** (design: `docs/architecture/assets.md` § Importers & cooking)
- **Overlay ports** (user decision: vcpkg overlay ports, pinned by commit + SHA512) in `cmake/vcpkg-ports/`, enabled by `vcpkg-configuration.json`. Each builds upstream's sources as a static library with a CMake package: `ufbx` v0.23.1, `bc7enc-rdo` @ b9438627 (encoders + BC7 decoder only), `xatlas` @ f700c779. From the baseline: `meshoptimizer`, `tinyobjloader`, `tinyexr`.
- Common intermediate `ModelScene` (renamed from `GltfScene`, kept as an alias); `MeshData` gains optional UV0. Importers: glTF (now with TEXCOORD_0), **FBX** (ufbx: Y-up metres, pivots baked, triangulated, materials' base colour) and **OBJ** (tinyobjloader: shapes, `.mtl` colours, smooth normals when missing). `load_model()` dispatches by extension; `MeshLibrary`, the editor's Open/Import and `--open` accept FBX and OBJ now.
- **Mesh cooking**: optional xatlas UVs, meshoptimizer vertex-cache/fetch optimisation, LOD chain by simplification. Output format `.f3dmesh`.
- **Texture cooking**: stb_image (PNG/JPEG/TGA/BMP/HDR; core now builds those decoders) and tinyexr (EXR). The mip chain is built in linear space (sRGB-aware, normals renormalised). BC1/BC3/BC4/BC5/BC7 via bc7enc_rdo; kinds colour/normal/linear/grayscale/HDR, defaulted from the file name. Output format `.f3dtex`; `decode_mip()` for previews.
- **Pipeline** `import_asset()`: cooks into `project://.forge3d/cache/<guid>/` and records settings plus the source hash in the `.meta`. Cancellable, reports progress. `load_cooked_model/texture()` read the cache back.
- **Import queue** (`editor::ImportQueue` + Import Queue panel): background scans queue stale assets, imports run in parallel with progress bars, failures are logged to the console and can be retried. Failed imports are not retried automatically until the source changes. Cancel All, Clear Finished. `forge3d-cli import <dir> [--force]`.
- Corpus `tests/data/import/` (CC0, generated by `tools/make_import_corpus.py`): OBJ cube + MTL, hand-written ASCII FBX cube, a 64×64-quad OBJ height field, PNG (RGBA checker, normal map, roughness), JPEG (non power of two), EXR (half floats above 1).

**Verification**
- (auto) Corpus test (`pipeline_test.cpp`): every file of `tests/data/import/` plus Box.glb and Fox.glb (10 assets: glTF ×2, FBX, OBJ ×2, PNG ×3, JPEG, EXR) imports without errors in a scratch project. The cooked outputs reload, and a rescan finds nothing stale, with settings defaults written to the `.meta` files. `integration.cli.import` cooks the corpus from the command line.
- (auto) LOD triangle counts shrink as expected: the height field (8,192 triangles) gets ≥ 3 levels, each ≤ 60 % of the previous level, within the error budget; tiny meshes (Box) stay at one level; `.f3dmesh` round-trips and rejects truncated data.
- (auto) OBJ and FBX importers agree on the 2 m cube (36 indices, 24 vertices, UVs, unit normals, the FBX node's metre translation). xatlas UVs lie in [0, 1]. Textures: BC7 sRGB with a full mip chain (PSNR > 35 dB against the source), non-power-of-two JPEG (partial blocks, odd mips), BC5 normal map (X/Y error < 5 %), BC4 grayscale (> 35 dB), EXR → RGBA16F keeping values > 1, explicit compression/no-mip settings, `.f3dtex` round trip.
- (auto) `import_queue_test.cpp`: parallel background imports with progress; a broken file fails, is not retried until forced, and imports once fixed; `cancel_all` drops queued imports without cooking anything.
- 39/39 ctest pass, zero warnings.

**Deviations**
- HDR textures are stored as RGBA16F: bc7enc_rdo has no BC6H encoder. BC6H is a follow-up if VRAM requires it.
- Meshlets: not built ("optional later" in the plan).
- Models need a native path (their importers read sibling files: `.bin`, `.mtl`), so they can't be imported from inside a `.forge` archive mount; textures can.
- The importer bug found by the LOD test is fixed: computing normals on the unwelded OBJ soup made every triangle flat-shaded and disconnected, so nothing simplified. Vertices are now indexed by their OBJ tuple first.

**Open**
- `[human]` The Import Queue panel with a real project folder (the automated runs never scan or import).

## P3.3 Index, browser & placement — 2026-10-16

**Built** (design: `docs/architecture/assets.md` § Index, thumbnails & placement)
- `assets::AssetIndex` on SQLite (vcpkg `sqlite3`): assets, tags, dependencies and thumbnails in `<project>/.forge3d/index.sqlite`. Incremental `sync()`, `search()` (text / type / tags, LIKE wildcards escaped), `tags()`, `dependencies()` / `dependents()`, and `load_records()`, which seeds `AssetDatabase::seed()` so a new session re-hashes only changed files.
- `.meta` gains `tags` and `dependencies`; importers report dependencies (glTF image URIs, OBJ `mtllib` and texture maps, FBX texture files). The pipeline keeps the tags when it rewrites a `.meta`.
- Thumbnails: `render::raster::ThumbnailRenderer` (offscreen, camera fitted to the bounding box) for models; `assets::texture_thumbnail` for textures. `editor::ThumbnailCache` caches PNGs keyed by the imported hash, runs texture work on workers and renders models on the main thread (2 per frame). `rhi::Device::create_texture` gains an initial-data overload; `ViewportRenderer::set_debug_lines(false)` keeps secondary renderers from draining the editor's debug lines.
- Asset browser panel (`editor/src/ui/asset_browser.*`): grid with thumbnails or list, search, type filter, tag filter, Rescan, query timing; context menu (Place at 3D Cursor, Re-import, Edit Tags…, uses / used by); models drag onto the viewport.
- Placement: a drop on the viewport raycasts the surface (or ground plane) and runs `assets::model_instance_ops` (generalised from the glTF import) in one transaction, then selects the new root. `MeshLibrary::set_resolver` resolves GUID `AssetRef`s to cooked meshes (`invalidate()` after re-import); the editor caches cooked models.
- `--scan-assets`: automated runs scan, import and thumbnail synchronously (used by the capture test). `core::disable_error_dialogs()` routes CRT asserts and errors to stderr in automated editor runs, the CLI and `forge3d-tests`, so a failed assert fails the run instead of opening an invisible modal dialog.

**Verification**
- (auto, perf) `perf.asset_index`: a generated library of 1,000 assets indexes (sync 9 ms Release) and every search returns in < 100 ms. Worst query 1.2 ms Release, 11.8 ms Debug. Recorded in `docs/budgets.md`.
- (auto) `asset_index_test.cpp`: incremental sync (unchanged = no writes; tag edits update only those rows; removed sources drop rows), search by text (case-insensitive, paths, wildcard escaping, limit) / type / tags; dependencies from the OBJ's material (`map_Kd ../textures/checker.png`) resolve to the texture asset both ways; persistence, thumbnail records, a seeded rescan that re-hashes nothing; placement through ops with GUID refs resolved and cached by `MeshLibrary`, invalidated after re-import, placed at the requested point.
- (auto) `integration.raster` thumbnail test: Fox rendered offscreen on D3D12 and Vulkan is framed (background corners, > 10 % coverage, not clipped, roughly centred).
- (visual) `integration.editor.asset_browser`: the corpus plus Box/Fox in a scratch project, scanned, imported and thumbnailed at start-up ("Assets: 10 found, 10 imported, 10 thumbnails"). The capture shows the browser grid with lit model thumbnails (Box, OBJ cube, FBX cube, Fox, height field) and texture thumbnails.
- 43/43 ctest (unit + integration + perf) pass, Debug and Release, zero warnings.

**Bugs found on the way**
- `make_view_constants` called `std::clamp(…, 60, far_plane * 0.9)`; with a short far plane (thumbnails of small models) the bounds were inverted, which is undefined behaviour (a Debug assert). It now uses min/max.
- The Debug assert above showed up as a silent hang (a modal CRT dialog in a hidden process). Hence `disable_error_dialogs()` for every unattended run.
- `decode_mip` unpacked BC1/BC3 without initialising rgbcx when nothing had been cooked in the process; it now initialises the tables itself.

**Deviations**
- Search is SQL `LIKE` on name/path, not SQLite FTS5 (not in the default vcpkg port features): about 1 ms for 1,000 assets, far below the budget.
- Thumbnails are stored as PNG files plus an index row (the plan's "cached as PNG"), not as BLOBs in the database.
- Dropping a texture onto the viewport does nothing yet (materials arrive in P4.2); it is logged.

**Open**
- `[human]` Dragging assets from the browser into the viewport with a real mouse; browser feel with a large real library.

## P3.4 Prefabs — 2026-10-16

**Built** (design: `docs/architecture/assets.md` § Prefabs; ops in `docs/architecture/ops.md`)
- Prefab asset `*.f3dprefab` (JSON: stable node ids, components, children, nested prefab references with their overrides). Components `PrefabInstance{prefab, overrides}` on instance roots and `PrefabLink{node, outer_node}` on instance entities (reflected, saved in scenes).
- Ops `prefab.create_from_selection`, `prefab.instantiate`, `prefab.apply_overrides`, `prefab.revert_overrides` and `prefab.update_instances`. Undo uses exact subtree snapshots plus the prefab file's previous text. `ops::Context` gains an optional `files` (project VFS); the editor and `forge3d-cli project <dir>` provide it.
- Per-property overrides are recorded by `entity.set_property` on instance entities (undo restores the list). Root placement is never an override; for nested roots it is an override of the outer instance.
- Propagation: syncing copies everything except overrides and the root's placement, creates entities for new nodes, removes entities for deleted nodes, and keeps user-added entities. One level of nesting; two levels are refused.
- Editor: Edit ▸ Create Prefab from Selection, an inspector Prefab section (override list, Apply to Prefab, Revert, Update Instances), prefabs in the asset browser (type filter, drag or double-click to place).

**Verification**
- (auto) `prefab_test.cpp` covers the M3 requirement: edit the prefab and every instance updates while its overridden properties stay, and undo works. Instance *a* changes a light's colour and intensity, adds a child and deletes one, then applies to the prefab. Instances *b* (with an intensity override) and *c* (moved) get the new colour, the new child and the deletion; *b* keeps its intensity and both keep their placement. One undo restores the file and every instance; redo reapplies exactly.
- (auto) Also: create/instantiate and the file format; instantiate redo reproduces identical GUIDs; a file changed outside the editor + `prefab.update_instances` (with undo); reverting one override or all of them (undo restores; undoing the edits removes their overrides); one level of nesting (a nested instance inside a prefab, its override carried, an inner-prefab change reaching nested copies, two levels refused); instances and overrides survive save/load.
- 42/42 unit + integration ctest pass, Debug and Release, zero warnings.

**Bug found on the way**
- EnTT identifies types by their pretty name, and MSVC prints every anonymous namespace as `` `anonymous-namespace' ``. So the anonymous `CreateParams` of `prefab_ops.cpp` and of `entity_ops.cpp` shared one meta type: field registrations mixed and memory was corrupted (a crash in `prefab.create_from_selection`). Parameter structs now have globally unique names; the reason is noted in the source.

**Deviations / limits**
- Prefabs are referenced by VFS path in `PrefabInstance` (the `.meta` GUID is not resolved yet): moving a prefab file breaks its instances' link until a GUID resolver is added.
- Adding or removing components on instance entities, deleting prefab nodes from an instance, and reordering siblings are not recorded as overrides. A sync restores the prefab's version (documented).
- External prefab edits are applied on request (`prefab.update_instances`, Update Instances button); P3.5's file watcher will trigger it.

**Open**
- `[human]` The prefab workflow in the editor UI (create, edit an instance, Apply / Revert).

## P3.5 Streaming & auto-reimport — 2026-10-16

**Built** (design: `docs/architecture/assets.md` § Streaming & auto-reimport)
- RHI: BC1–BC7 texture formats, textures created from a mip chain (`create_texture(desc, levels)`), several vertex streams per draw.
- `render::raster::ResidencyManager`: mesh LODs and texture mips resident by on-screen size, under a VRAM budget, least recently used first out, upgrades limited per frame. The viewport renderer draws project meshes through it, with a UV stream and a per-draw base-colour texture (`MeshRenderer.texture`, white if none; shaders sample it).
- Editor: the `vram_budget_mb` preference (default 2048, Preferences ▸ Streaming) and `--vram-cap <MB>`; **Window ▸ Residency** panel (used vs budget, per-asset resident/wanted level, screen px, MB); `--bench` reports `residency`. Dropping a texture from the browser onto a mesh sets its texture (undoable `entity.set_property`).
- Auto-reimport: `assets::SourceWatcher` (sources + `.meta` dependencies, polled on a worker once a second) → rescan → import queue → `asset_imported` drops the cooked model, mesh library entry and resident GPU copies → hot swap on the next frame. Re-imported prefabs update their instances (`prefab.update_instances`). Focus regained → rescan (new/deleted files). `core::FileWatcher` is now indexed by path (O(1) watch/unwatch for large projects).
- `tools/stress_streaming.py`: the 200-asset stress project and `--bench` check.

**Verification**
- (auto) 200-asset stress scene with a 512 MB cap stays under the cap in `--bench`: `tools/stress_streaming.py` (Release, real editor, 200 × 1024² textures on 200 quads): peak 105.5 MB of 512 MB, 200 textures resident; with `--cap-mb 64`: peak 63.4 MB, the cap binds and the capture looks the same. `integration.streaming` (200 textures that all want mip 0, 1.1 GB, under 512 MB): 510 MB used, never over after any frame; nearest at mip 0, farthest coarser; idle assets evicted (≥ 150); mesh LOD by screen size.
- (auto) Idle RAM with an empty scene < 250 MB: 134 MB (D3D12 / RTX 3050, the default), 109 MB (D3D12 / AMD), 166 MB (Vulkan / AMD). Recorded in `docs/budgets.md`.
- (auto) Touching a source texture triggers a re-import and swap: `integration.streaming` `auto-reimport` test (edit the PNG → watcher reports it once → rescan marks it stale → re-import writes new cooked data → residency reloads it into a new GPU texture; no spurious reports after a database resync).
- Unit: residency level math, `--vram-cap`, the preference, bench JSON. 45/45 ctest pass, Debug and Release, zero warnings.

**Deviations / limits**
- Vulkan on the NVIDIA driver idles at 366 MB (338 MB right after device creation), over the 250 MB budget. Not from implicit layers or Diligent's pools; treated as driver baseline. D3D12 is the default on Windows and meets the budget.
- CPU-side loading is synchronous on the main thread when an asset first becomes resident (cooked files are small, but a burst of large textures can hitch a frame). Async loading belongs with the P4.1 render-graph work.
- The stress check is a script, not a ctest test (it generates 1.1 GB of cooked data and takes ~40 s).

**Open**
- `[human]` Hot reload with a real paint program saving into a project; the Residency panel while flying around a large scene.

---

## Stage B gate — 2026-10-16

| Gate item | Result |
|---|---|
| Operation API documented and treated as stable | ✅ `docs/architecture/ops.md` (marked stable: the UI, CLI and later Python/RPC/MCP go through it) |
| `.forge` v1 spec | ✅ `docs/format.md` (container, entries, migrations, atomic save, autosave), sample `tests/data/forge/v1_basic.forge` |
| Idle RAM < 250 MB | ✅ on the default backend: 134 MB D3D12 / RTX 3050, 109 MB D3D12 / AMD (Vulkan / AMD 166 MB). ⚠️ Vulkan / NVIDIA 366 MB, driver baseline (see P3.5) |

**Open `[human]` items carried out of Stage B** (none block Stage C):
- P2.4: gizmo feel (drag, snapping, pivot modes) with a real mouse.
- P2.5: outliner drag & drop and rename; measure-tool feel.
- P2.6: Save As / Open dialogs, unsaved-changes and recovery prompts in a real session.
- P3.2: the Import Queue panel with a real project folder.
- P3.3: dragging assets into the viewport with a real mouse; browser feel with a large library.
- P3.4: the prefab workflow in the editor UI.
- P3.5: hot reload with a real paint program; the Residency panel in a large scene.

## P4.1 Render graph & engine interfaces — 2026-10-16

**Built** (design: `docs/architecture/render.md`)
- `f3d::render::graph::RenderGraph`: pass declaration (typed pass data, `read`/`write` with accesses), versioned handles (a write returns a new version and depends on the previous contents), imported and transient textures, `mark_output` and side-effect passes, reference-counting culling, lifetimes, lifetime-based aliasing into slots, state tracking with barriers per pass (UAV barriers between storage writes) and final states for imported textures, Graphviz dump. `TransientPool` keeps slot textures across frames (released after 3 unused frames). Each executed pass gets a GPU timer and a Tracy zone named after it.
- RHI: `ResourceState` and `CommandContext::transition()` / `state()`.
- Interfaces: `IRenderEngine` (`render(ctx, view, scene, target)`, with `RenderScene` and `RenderTarget`) and `View` in `render/graph`; `IUpscaler` (`add_passes(graph, inputs, out size)`) in `render/upscale`.
- `ViewportRenderer` became `render::raster::RasterEngine`, an `IRenderEngine` whose frame is a graph: mesh (clears, draws) → grid → debug lines → resolve into the imported output. Colour and depth are transients, so the viewport panel and thumbnails now own only the output texture. Picking is a one-pass graph with a transient depth buffer. The scene is passed per call (`RenderScene`) instead of `set_scene`.
- Editor command **Dump Render Graph** (`render.dump_graph`) writes the last viewport graph to `forge3d-render-graph.dot`.

**Verification**
- (auto) `unit.render_graph` (10 cases): versions; unused passes culled (transitively, with marked outputs and side effects kept); draw-on-top chains survive only with a reader; disjoint lifetimes share a slot and live resources never do (fixed chain plus 200 random graphs checked pairwise); different sizes/formats/usages are not aliased; barrier sequence for draw → overlay → resolve and the output's final state; UAV barrier between storage writes; the Graphviz dump.
- (visual) Golden comparison against the P2.x/P3.x capture: the Fox capture (`--open Fox.glb --capture`) and the empty viewport captured before and after the change are **pixel-identical** on D3D12 and Vulkan (max channel difference 0).
- 45/45 unit + integration ctest pass (Debug); Release builds with zero warnings.

**Deviations**
- Aliasing is per texture object, not per memory range: Diligent exposes no placed resources on every backend. Transients with the same size, format and usage share a GPU texture when their lifetimes don't overlap (documented in `render.md`).
- `IRenderEngine` lives in `render/graph` (with `View`), the lowest render module that every engine and upscaler can see; the scene types are forward-declared there, so the layering table is unchanged.
- No `IUpscaler` implementation yet (FSR and the bilinear fallback are P8.3).

## P4.2 PBR & materials — 2026-10-16

**Built** (design: `docs/architecture/render.md` § Lighting & materials)
- Clustered forward+: `light_cluster` compute pass (16 × 9 tiles × 24 exponential depth slices, up to 64 lights per cluster) in the render graph; `render::raster::lights.hpp` is the CPU reference of the cluster math. `gather_lights` collects `scene::Light` components: up to 4 directional lights (lux, default sun when the scene has none) and up to 4096 point/spot lights (candela, windowed inverse-square falloff, smooth spot cone).
- Metal/rough BRDF in `shaders/lighting.hlsli`: GGX, height-correlated Smith, Schlick Fresnel, Lambert diffuse; hemispheric ambient with the analytic split-sum environment BRDF (stand-in until the P4.3 sky IBL). `shaders/material.hlsli` samples the material's textures (normal map with a reconstructed Z, ORM, occlusion, emissive, alpha mask).
- `assets::Material` (reflected, glTF metal/rough model + texture slots), `.f3dmat` files (`forge3d.material` v1 JSON), ops `material.create` / `material.set_property` (file contents restored on undo, consecutive edits merged). `MeshRenderer.materials` assigns one material per slot; `MeshRenderer.texture` now overrides the base-colour texture.
- glTF import maps materials (factors, the five texture slots, alpha mode/cutoff, double-sided, `KHR_materials_emissive_strength`) to sub-assets `<model>#material/<n>`; OBJ materials map too. Meshes keep per-material submeshes through every LOD (`.f3dmesh` v2 with a submesh table; v1 files still load), and cooked models keep their materials and images.
- Material Editor panel: the active entity's slots, a preview sphere (`MaterialPreview`, an offscreen `RasterEngine`) and the slot's parameters; *Make Editable Copy* copies a model's material into `project://materials/` and assigns it (one undo step).
- `tools/fetch_test_assets.py` also fetches `MetalRoughSpheres.glb` (CC-BY-4.0, git-ignored for its size).

**Verification**
- (visual) DamagedHelmet and MetalRoughSpheres captured on D3D12: the helmet shows normal-mapped panels, emissive visor details and metal trims; the spheres show the roughness gradient (sharp to broad highlights) and the dielectric/metal split. Metals look dark because there is no environment lighting yet (P4.3). `docs/img/p4.2-spheres.png` is kept as a golden candidate for P4.5.
- (auto) Light clustering: `unit.render_raster` (slice/tile/index math, cluster bounds, sphere-box tests, CPU assignment: lights land in the expected clusters and only there) and `integration.raster` `cluster_test` (the GPU pass's lists equal the CPU reference).
- (auto) `unit.assets` `material_test`: JSON/`.f3dmat` round trip with defaults, sub-asset rebasing, glTF and OBJ material mapping, submesh grouping and cooking (v2 round trip, v1 still loads), mesh-library material/slot resolution, material ops with merged drags, undo and redo.
- (auto) `integration.raster` also checks on both backends that point lights light what is in their range.
- 45/45 unit + integration ctest pass, Debug and Release, zero warnings; clang-format and licence headers clean.
- `--bench` recorded in `docs/budgets.md` (both backends, both GPUs). Empty-scene idle RAM and cold start still within budget.

**Deviations / limits**
- Ambient is a hemisphere, not image-based; reflections on metals are missing until P4.3.
- Model textures are decoded synchronously when first resident: first frame with DamagedHelmet ~2.1 s (was ~1.0 s with baked vertex colours). Follow-up: asynchronous texture loading.
- No transparency (alpha blend) mode yet; glTF `BLEND` materials import as opaque.

### Decisions made autonomously
- **The DamagedHelmet capture isn't committed as a golden candidate.** Alternatives: commit both captures, or neither. The helmet is derived from a CC-BY-NC-4.0 model (see `tests/data/gltf/LICENSES.md`), and a render of it in an Apache-2.0 repository would carry that restriction. The CC-BY spheres capture is kept, with its credit in `LICENSES.md`.
- **The P4.2 uncommitted work was taken over as found.** It built and passed its tests, so the session finished it (docs, benches, formatting) rather than redoing it.

**Open**
- `[human]` Material editor workflow with a real mouse (edit a copy, undo, preview updates).

## P4.3 Sky, sun & shadows — 2026-10-16

**Built** (design: `docs/architecture/render.md` § Sky, image-based lighting and shadows)
- `scene::Sky` and `scene::ReflectionProbe` components (reflected, saved, addable in the inspector); `scene::celestial` / `solar_position` / `lunar_state`: sun from the NOAA equations, moon and its phase from Schlyter's orbital elements; north is -Z.
- Physical atmosphere after Hillaire 2020: transmittance, multiple-scattering and sky-view LUTs (compute), a sky pass with sun and moon discs, and a CPU reference of the model (`atmosphere.hpp`) that colours sun- and moonlight, lights the ground below the horizon and meters the exposure (incident EV100 from the horizontal illuminance, clamped to [1, 16], plus compensation).
- Sky-driven IBL: environment cube → GGX prefilter (6 mips, filtered importance sampling) → irradiance SH (L2); re-baked only when the sky's radiance changes. Shading uses SH diffuse + prefiltered specular for every mesh; without a Sky the same path uses the P4.2 hemisphere colours.
- Reflection probes: six-face captures (mesh + sky passes, mirrored D3D cube faces), prefiltered and projected into SH slots 1..64; nearest probe by radius per mesh; re-captured on change, on sky change and on **Rebake Reflection Probes** (`render.bake_probes`); one capture per frame.
- Cascaded shadow maps: 4 cascades × 2048² (D32 array), practical splits over 150 m, bounding-sphere fit (rotation-stable size), texel-snapped light-space centre, depth clamping, slope bias, per-cascade light-space culling, normal-offset 3×3 PCF, distance fade. The shadowing light: the sun (the moon at night) from a Sky, else the first `cast_shadows` directional light, else the default sun. Skipped when the scene has no meshes.
- Pre-exposed HDR: the mesh and sky passes multiply by the exposure (fp16-safe); sky textures store radiance × 2⁻¹².
- RHI: texture arrays and cubes, texture views (per mip/slice RTV, DSV, UAV, SRV), `generate_mips` (unbinds render targets: the backends re-commit them), comparison samplers, depth bias / depth clip, `read_texture_float`.
- Scene scripts `tests/data/scenes/sky_0600/1200/1800.json` (ground, cube, pillar, Sky) and `sky_afternoon.json` (a Sky only, to add to `--open`).

**Verification**
- (visual) `--scene tests/data/scenes/sky_<t>.json --capture`: at 06:00 the sun is low in the east, shadows are long and point west; at 12:00 the sky is deep blue and shadows are short; at 18:00 the western horizon glows warm and shadows point east. D3D12 and Vulkan match. Kept as `docs/img/p4.3-sky-0600.png`, `-1200.png`, `-1800.png`; MetalRoughSpheres under the afternoon sky (metals now reflect sky and ground) as `docs/img/p4.3-spheres-sky.png`.
- (auto) Cascade texel snapping (`unit.render_raster` `shadows_test`): with the cascade centre on a snapped position, camera moves of ±0.3 texel along each light-space axis leave the shadow matrix bit-identical; a 3-texel move shifts it by exactly 3 texels; turning the camera keeps every cascade's size; every slice corner lies inside its cascade sphere; splits blend linear and logarithmic correctly.
- (auto) `unit.scene` `sky_test`: noon elevations at the equinox/solstices (68.44° at 45 N in June), sunrise in the east and sunset in the west, the southern-hemisphere noon sun in the north, time zones, moon phases on known dates (full 2024-03-25, new 2024-04-08, first quarter 2024-03-17), axis conventions, the active sky.
- (auto) `unit.render_raster` `atmosphere_test`: vertical transmittance equals each layer's optical depth; noon zenith blue, sunset horizon red, haze whitens the sky around the sun; EV100 / exposure formulas and the noon/evening/night exposure.
- (auto) `integration.raster` `sky_test` (D3D12 and Vulkan): the transmittance LUT matches the CPU model; the environment's irradiance is the CPU single-scattering estimate plus a plausible multiple-scattering gain; zenith blue; morning SH leans east; a reflection probe in an empty scene captures the same SH as the sky (±15% for five directions, which also catches mirrored faces); moving the probe and a rebake re-capture it; a sphere's shadow darkens the ground under it and disabling shadows removes it; with no Sky a uniform hemisphere gives irradiance π·L in every direction and the prefiltered cube's sharpest mip reproduces it.
- 45/45 unit + integration ctest pass, Debug and Release, zero warnings; clang-format and licence headers clean.
- `--bench` recorded in `docs/budgets.md` (sky, helmet and idle; both backends, both GPUs). Idle RAM and VRAM unchanged from P3.5.

**Deviations / limits**
- No tonemapping yet (P4.4): bright skies clip to white, especially at dusk (the horizon toward the set sun is ~7× display white at the metered exposure). Emissive materials are unitless in glTF and look dim under physical exposure; their scaling belongs with P4.4's exposure work.
- The moon lights the scene and shows as a uniform disc (its phase sets the brightness, not the shape); moonlit sky scattering and stars are a constant night radiance.
- The CPU atmosphere is single scattering only: exposure and ground radiance underestimate the sky by the multiple-scattering share (10–40%).
- Probe captures have no shadows and see only the sky's environment (no probe-to-probe bounces). Probes aren't saved with their captures; they re-capture after loading.
- Shadow casters ignore alpha-masked cut-outs (they cast as solid).

### Decisions made autonomously
- **The sky's look-up tables are separate from the render graph** (sky LUTs, environment and probe captures run with explicit timers before the graph, like picking). Alternatives: graph passes with imported persistent textures. They are conditional and rarely change; keeping them out of the graph keeps its per-frame structure simple. The shadow map is imported into the graph because the mesh pass depends on it every frame.
- **Pre-exposure plus a 2⁻¹² scale for sky textures** instead of RGBA32F textures. RGBA32F filtering is optional on some Vulkan drivers (lavapipe for P4.5's golden tests), so fp16 with a scale was the portable choice.
- **CPU exposure metering** (incident light) until P4.4's histogram auto-exposure, rather than reading GPU results back (a stall) or a fixed EV.
- **Empty scenes skip shadows** and allocate a 1×1 placeholder map. Found by the idle bench: the first version cleared four 2048² cascades every frame (1.5 ms on the iGPU, +64 MB).
- **The default sun now casts shadows**, so editor scenes without lights look grounded. This changes P4.2-era captures (they predate the golden tests, P4.5).

**Open**
- `[human]` Time-of-day scrubbing in the inspector feels smooth; shadow quality and stability while flying around; reflection-probe placement in a real scene.

## P4.4 Tonemapping, debug modes & capture — 2026-10-16

**Built** (design: `docs/architecture/render.md` § Exposure, tonemapping, view modes and capture)
- Automatic exposure on the GPU: a 256-bin log-luminance histogram of the scene colour, a percentile average, reflected-light EV100, compensation, limits and time-based adaptation, kept in a GPU buffer the next frame's shaders read (no readback). Seeded from the CPU sky meter and snapped after a reset. Manual EV100 mode. Scenes without a Sky keep relative units (exposure 2^compensation).
- Tonemapping in the resolve: AgX (default), ACES (Hill's fit), none; CPU reference in `render_settings.cpp`. The background (scene alpha 0) gets the exact clear colour; the grid and debug lines now draw after tonemapping onto the display texture.
- View modes: lit, unlit, wireframe (pipeline variant), normals, shadow cascades; View ▸ Viewport Mode / Tonemapper / Exposure menus.
- Stats overlay in the viewport (View ▸ Stats Overlay, `view.toggle_stats`): fps, per-pass GPU ms, draw calls, lights, VRAM (`editor/viewport_stats.hpp`).
- `viewport.capture` operation (transient) and `render::raster::Capturer`: offscreen capture at any size from a Camera entity, a bookmark, the host's view or a framing of the scene. Registered by the editor (its camera as default view) and by `forge3d-cli`, which creates a GPU device on first use.

**Verification**
- (auto) `forge3d-cli run tests/data/scenes/sky_1200.json run-op viewport.capture --params {...}` writes PNGs headlessly: `integration.cli.capture` captures the noon scene in all five modes at 640×360 and `integration.cli.capture.check` reads them back (sizes; blue sky and red ground when lit; the ground's normal shown as (128, 255, 128); unlit base colour and no sky; wireframe edges; every mode differs). An unknown mode fails (`integration.cli.capture.bad_mode`).
- (visual) One capture per viewport mode: `docs/img/p4.4-lit.png`, `-unlit.png`, `-wireframe.png`, `-normals.png`, `-cascades.png`. The P4.3 sky scenes with AgX: the bright horizons no longer clip.
- (auto) `integration.raster` `exposure_test` (D3D12 and Vulkan): the metered EV100 of a zenith view equals the reflected-light reading of the sky-view LUT's zenith luminance (±3%); with no time passing nothing adapts; with time it moves part of the way; compensation shifts it by exactly 1 EV; manual mode gives exactly the EV asked for; no Sky means no metering and exposure 2^compensation.
- (auto) Unit: view-mode/tonemapper names; every tonemapper monotonic in [0, 1], mid grey mid-range, AgX desaturates bright red; metering formula; capture views from a bookmark (equal to `EditorCamera`'s matrices), a Camera entity (position, orientation, fov; scale ignored; missing entity fails), the host's view at a new aspect, a default framing; stats lines and the frame-time average.
- 48/48 unit + integration ctest pass, Debug and Release, zero warnings; clang-format and licence headers clean. `--bench` recorded in `docs/budgets.md`.

**Deviations / limits**
- The CLI renders meshes referenced by file path (scene scripts, imported glTF); project assets streamed by GUID are only drawn by the editor (the CLI has no residency manager yet).
- The stats overlay doesn't show the exposure: reading the GPU value back would stall the frame.
- Captures don't include the grid or debug lines.
- Unlit/normals/wireframe modes skip the sky (flat background).

### Decisions made autonomously
- **Exposure stays on the GPU** (a buffer read by the shaders), seeded from the CPU sky meter, rather than read back for the CPU. Alternatives: synchronous readback (stalls), a readback ring with a frame of latency (more RHI work). The GPU buffer has no latency and no stall.
- **Scenes without a Sky aren't auto-exposed.** Their lights use P4.2's relative units, so metering them would make the editor's default scenes jump in brightness; only the compensation applies.
- **The background stays the exact clear colour and overlays draw after tonemapping.** Found by the existing capture check (the grey background changed under AgX). The editor's UI colours shouldn't depend on the scene's exposure.
- **The CLI creates its GPU device on first capture** rather than at start-up, so non-rendering commands stay fast and work on machines without a GPU.
- **`viewport.capture` takes the mode as a string** (`"lit"`, …) instead of an enum index, for readable scripts and RPC/MCP schemas.

**Open**
- `[human]` The exposure and tonemapper menus and the stats overlay in an interactive session; adaptation feel when flying from bright to dark areas.

## P4.5 Golden-image tests — 2026-10-16

**Built**
- `core::compare_images` / `difference_image` (`f3d/core/image_compare.hpp`): mean SSIM on luma (Wang et al.: 11×11 Gaussian, σ 1.5), the worst 16×16 block's SSIM, mean/max channel difference, differing pixels; an amplified diff image.
- `forge3d-cli golden <spec> [--update] [--out <dir>]` plus global `--rhi` / `--adapter` options (index, discrete, integrated, **software**). A `forge3d.golden` spec lists scene scripts, glTF models, extra ops, the capture (size, mode, bookmark) and thresholds; a backend-specific reference (`<name>.vulkan.png`) wins over the shared one.
- RHI: `AdapterPreference::Software` and `apply_adapter_choice` (shared by the editor's and CLI's `--adapter`). The editor gains `--adapter software` and `--viewport WxH` (render the viewport at an exact size, for benchmarks); `--bench` records the viewport size.
- `tests/golden/`: 8 specs with WARP references (480×270): sky at 06:00 / 12:00, evening in cascade mode, normals and unlit modes, the textured Fox with the default light and under a Sky (IBL), a night scene with clustered point and spot lights. `README.md` explains running and updating them.
- ctest label `golden`: one test per spec on the software rasteriser (WARP on Windows, lavapipe on Linux), plus `golden.self_check` (the morning scene against the noon reference must fail on SSIM). CI: a golden step per platform (Linux non-blocking until lavapipe references exist) and an upload of `test-output/golden/`.

**Verification**
- (auto) `ctest -L golden` passes locally on WARP: 9/9 (8 specs + the self-check), 7.5 s. Two consecutive runs are bit-identical (SSIM 1.00000, 0 differing pixels): WARP is deterministic, so the default thresholds (0.99 mean, 0.90 worst block) leave room for driver/compiler noise only.
- (auto) `unit.core` `image_compare_test`: identical images; ±3 noise passes the block threshold while a missing shape fails it; diff image; size mismatch. `unit.editor`: `--viewport`, `--adapter software`.
- (auto) `--bench` at 1080p recorded in `docs/budgets.md` (both backends, both GPUs): AMD iGPU 4.2 ms viewport GPU / 5.7 ms CPU for the sky scene.
- 48/48 unit + integration, 9/9 golden; Debug and Release, zero warnings.

**Deviations**
- SSIM instead of FLIP: no FLIP implementation in vcpkg, and SSIM with a worst-block check catches local changes well enough for a deterministic rasteriser.
- References are rendered by WARP only; the lavapipe (Linux) job is non-blocking until `.vulkan.png` references are generated in CI.

### Decisions made autonomously
- **The golden runner is a CLI command**, not a separate executable: it reuses the CLI's scene building (scripts, ops, glTF import) and its headless `viewport.capture`. Alternatives: a doctest suite (no per-spec ctest entries, no `--update`), a Python driver (an extra dependency for C++-only CI).
- **SSIM, not FLIP** (above); **480×270 references** keep the repository small (about 210 KB for 8 images) while showing shadows, sky gradients and texture detail.
- **The Linux golden job doesn't block CI** until lavapipe references exist; generating them needs the CI runner (no Linux machine here), logged as pending.

**Open**
- `[human]` ≥ 60 fps at 1080p on the reference iGPU (M4 exit); CI run of the golden jobs on GitHub and lavapipe references (`--update` on the Linux runner, then review).

## P5.1 C ABI & plugins — 2026-10-16

**Built** (design: `docs/architecture/api.md`)
- `engine/api/include/forge3d_api.h`: pure-C API, version 1.0. `f3d_api_get(version)` returns the `F3dApi` function table (or NULL: same major, minor not newer than the host's); opaque `F3dHost`, `F3dWorld`, `F3dOpContext`; entities by GUID value (`F3dEntity`); `F3dResult` error codes with a thread-local `last_error`. Functions: register/run/undo/redo operations, read entities and properties (JSON), and change the scene inside an operation (create/delete entity, set property, add component, run a child operation).
- `api::Host` (wraps a session's registry, history and world) and C operations: a plugin's scene changes are recorded as child operations, so its operation undoes, redoes by replaying the recorded changes (not by calling the plugin again), and rolls back on failure.
- `api::PluginLoader`: loads `plugins/*.dll|.so|.dylib` next to the executable; checks `f3d_plugin_api_version` before any other plugin code runs, then calls `f3d_plugin_entry`. The editor loads plugins at start-up; `forge3d-cli` too (`--plugins <dir>`, `--no-plugins`).
- `samples/plugins/hello_op/` (plain C): `sample.scatter_cubes {count, seed, radius, mesh}`, deterministic per seed, built into `bin/<config>/plugins/`.

**Verification**
- (auto) `integration.api` `plugin_test`: the sample plugin loads (name and version from the plugin), its operation is listed with its four-parameter schema, runs through the C table (`run_op`) as one undo step (root + 5 cubes), `get_property` reads a cube's translation (with a size query), undo removes everything, redo restores a bit-identical scene, the same seed gives the same layout in a fresh session, a failing run (`count: -1`) changes nothing, and loading it twice is refused (duplicate operation id). Scene changes without an operation context are refused.
- (auto) A plugin built against a mismatched ABI version is refused with a clear error: `tests/plugins/future_api.c` reports API 2.0 and is rejected with "built for forge3d API 2.0, this host provides 1.0 (rebuild the plugin against this forge3d)"; its entry point never runs (it would rename itself). The handshake rejects a newer major and a newer minor.
- (auto) CLI: `list-ops` lists `sample.scatter_cubes`; `run-op sample.scatter_cubes ... info undo info` shows 11 entities in one undo step, then 0; with `--no-plugins` the operation doesn't exist.
- The editor logs "Plugin hello_op 1.0.0 loaded" at start-up.
- 52/52 unit + integration, 9/9 golden; Debug and Release, zero warnings (the C plugins build with /W4 /WX and -Wall -Wextra -Werror).

**Deviations / limits**
- Plugin operations' parameters aren't reflected types: their schema is the plugin's JSON (`OpInfo::extra_params`), and their parameters are passed as JSON text.
- Plugins can't add components or panels yet, only operations; reading is limited to entity lookup by name and property reads (enough for the M5 automation surfaces, which go through operations).
- Libraries are never unloaded (their operations may be in the undo history).

### Decisions made autonomously
- **The host passes `f3d_api_get` to the plugin's entry point** instead of the plugin importing it from the executable: plugins then need no import library and can be built with any toolchain. `f3d_api_get` is still exported for in-process embedding.
- **A separate exported version function** (`f3d_plugin_api_version`) lets the loader refuse a mismatched plugin before running any of its code, which the phase's "clear error" check relies on.
- **Plugin scene changes go through built-in operations as children**, rather than a snapshot of the world before and after: exact, cheap undo, and redo that doesn't call the plugin again.
- **The sample plugin is C, not C++**, to prove the ABI needs nothing beyond the header.

**Open**
- `[human]` A plugin built with a different compiler (MinGW / clang) loads into the MSVC build.

## P5.2 JSON-RPC server — 2026-10-16

**Built** (design: `docs/architecture/api.md` § JSON-RPC server)
- `api::RpcDispatcher`: JSON-RPC 2.0 (requests, notifications, batches, error codes) with the methods `session.authenticate`, `ops.list` (parameter rows + JSON Schema generated from reflection), `ops.run`, `ops.undo`, `ops.redo`, `scene.query`, `entity.get`, `entity.set`, `asset.search`, `viewport.capture` (a path, or the PNG as base64) and `events.subscribe` (`events.changed` notifications, filtered by kind).
- `api::RpcServer` on IXWebSocket (vcpkg `ixwebsocket`, no TLS feature): bound to 127.0.0.1, off by default, a 256-bit token in a user-only `rpc.json` (`{port, token, pid}`), required in the first message, 16 MB message cap, 16 connections. Socket threads only queue; `poll()` runs calls on the main thread and sends events after each reply.
- Editor: `--rpc[=port]` (0: a free port), `--rpc-token-file`, preferences `automation.rpc_enabled` / `rpc_port` (Preferences ▸ Automation), `poll()` every frame (also while minimised). `viewport.capture` and the RPC capture share `App::capture_scene`.

**Verification**
- (auto) `tests/integration/rpc/test_rpc.py` (ctest `integration.rpc`): starts the editor headless with `--rpc=0`, reads the token file, connects, builds a scene (ground, cube, sky: 12 operations through `ops.run` and `entity.set`), receives `entity_created` / `component_changed` events and only those it subscribed to, queries by component and by name, reads components back with `entity.get`, captures a 320×180 PNG (base64), checks error replies, undoes all 12 steps and finds the scene empty.
- (auto) A connection without a token (a call before authenticating) or with a wrong token gets error -32001 and is closed; the port refuses connections on every non-loopback address of the machine (here 172.28.128.1, 192.168.18.228, 192.168.56.1).
- (auto) `unit.api` `rpc_test`: authentication order and hang-ups, the JSON-RPC rules (version, method, ids, notifications, batches), operations and undo/redo, `entity.get/set`, `scene.query` filters, schema generation, asset search through the host, events per kind, token format. `unit.editor`: `--rpc`, `--rpc-token-file`, the preference defaults (off) and port clamping.
- 54/54 unit + integration, 9/9 golden; Debug and Release, zero warnings. CI installs Python's `websockets` for the RPC test.

**Deviations / limits**
- The CLI doesn't host the server (only the editor); MCP (P5.3) can launch a headless editor.
- On Windows the token file relies on the user config directory's ACL (private to the user); on POSIX it's chmod 600.
- Events are sent after the reply of the call that caused them, and events from UI edits on the next frame.

### Decisions made autonomously
- **IXWebSocket** (vcpkg, BSD-3-Clause, built without its TLS feature: loopback needs none) over uWebSockets (heavier, its own event loop) and a hand-written WebSocket (security-sensitive parsing). It's a new dependency, added to `vcpkg.json`; its licence goes into the P11.2 notices.
- **`session.authenticate` as the first message**, not a token in the URL: URLs end up in logs and browser history.
- **Port 7468** by default (unassigned by IANA), 0 for tests.

**Open**
- `[human]` A real client against a running interactive editor (e.g. a script from another tool); firewall prompts on a fresh Windows install (none expected: loopback only).

## P5.3 MCP bridge — 2026-10-16

**Built** (guide: `docs/guides/mcp.md`)
- `tools/forge3d-mcp`: an MCP server on stdio (protocol 2025-06-18, also accepting 2024-11-05 / 2025-03-26) that connects to the editor's JSON-RPC server (IXWebSocket client) using the editor's token file, or starts a headless editor (`--hidden --rpc=0`, its own token file) when none is reachable or with `--spawn-editor`, and stops it on exit.
- Tools generated from `ops.list` (operation ids with dots as underscores; the operation's JSON Schema as `inputSchema`; results as text plus `structuredContent`; failures as `isError`), plus `screenshot` (an MCP image), `undo`, `redo`, `scene_query`, `entity_get`, `asset_search`. Resources `forge3d://scene`, `forge3d://selection`, `forge3d://assets`. The RPC gained `selection.get`.
- `docs/guides/mcp.md`: setup with Claude Code (`claude mcp add`, `.mcp.json`), the tools and resources, security notes.

**Verification**
- (auto) `tests/integration/mcp/test_mcp.py` (ctest `integration.mcp`): a scripted stdio MCP client initializes, lists 34 tools (names `[A-Za-z0-9_-]`, schemas present), runs `entity_create` and `entity_set_property`, reads the scene and selection resources, reads the entity back, takes a 200×120 screenshot (a valid PNG image), sees failing and unknown tools reported as errors, undoes both steps, closes stdin; the bridge exits 0 and its editor is gone.
- `unit.api`: `selection.get`.
- 55/55 unit + integration, 9/9 golden; Debug and Release, zero warnings.

**Bug found on the way**
- `replies[reply["id"].get<long long>()] = std::move(reply)` emptied `reply` before reading its id: C++17 evaluates the right side of an assignment first, and nlohmann's `operator=` takes its argument by value. The exception was swallowed on IXWebSocket's thread, so the bridge waited forever. The key is now read first (commented in the source).

**Deviations / limits**
- Events from the editor aren't forwarded as MCP notifications (clients re-read resources); `listChanged` is false (operations registered later, e.g. by plugins loaded at start-up, appear on the next `tools/list`).
- The default token-file path mirrors SDL's per-user pref path; a custom editor location needs `--token-file`.

### Decisions made autonomously
- **A C++ bridge**, not Python: it ships as `forge3d-mcp` next to the editor with no runtime to install.
- **Tool names with dots replaced by underscores**: several MCP clients only accept `[A-Za-z0-9_-]` in tool names.
- **The bridge starts its own headless editor** when none is running, so an assistant works without the user launching the editor first.

**Open**
- `[human]` Works from a real MCP client (Claude Code) with the documented config.

## P5.4 Python module — 2026-10-16

**Built** (guide: `docs/guides/python.md`)
- `modules/python` (`f3d_python`, built only with `FORGE3D_WITH_PYTHON=ON`): embedded CPython (vcpkg `python3` 3.14.8, through a vcpkg manifest feature `python`) with a nanobind module `forge3d`. `forge3d.call(method, params)` goes through `api::RpcDispatcher` in process (an always-authenticated session), so Python and JSON-RPC behave identically; `forge3d.ops.<domain>.<verb>(**params)` is generated from `ops.list` (docstrings from the parameter rows), plus `run_op`, `undo`, `redo`, `scene_query`, `entity_get`, `selection`, `capture`, `asset_search`, `refresh_ops`, and `forge3d.Error` with the JSON-RPC code.
- Presets `win-msvc-full` and `linux-gcc-full` (with their build and test presets) turn the option and the vcpkg feature on; the core presets stay without Python.
- `forge3d-cli run script.py` (and `compare a.png b.png [--ssim]`); without Python the command explains how to get it.
- Editor: Window ▸ Python (console with expression echo, script runner) and the preference `automation.python_enabled` (Preferences ▸ Automation), off by default and separate from the RPC server; the interpreter starts on first use.

**Verification**
- (auto) M5 exit: `integration.python.m5_scene` (`forge3d-cli run tests/integration/python/m5_scene.py`) builds the ground/cube/sky scene with `forge3d.ops`, checks queries and a `forge3d.Error`, captures from the "View" bookmark and undoes all 13 steps; `integration.python.m5_rpc` builds the same scene over JSON-RPC in a headless editor and captures it; `integration.python.m5_match` compares them: SSIM 1.00000, 0 pixels differ.
- (auto) The core build (`win-msvc`, `FORGE3D_WITH_PYTHON=OFF`) builds and passes 56/56 in Debug and Release; `integration.cli.no_python` checks the message. The full preset passes 14 unit + 44 integration (Debug); the editor starts with the Python panel.
- `unit.editor`: `python_enabled` defaults to off and round-trips.

**Bugs found on the way**
- nanobind's helpers crash before any nanobind module has initialized (its internals don't exist yet): the interpreter now imports `forge3d` with the plain C API first.
- nanobind hides `_DEBUG` from `Python.h` unless `Py_DEBUG` is set (to link a release Python in debug builds); vcpkg's debug builds link the debug CPython, whose `PyConfig` is larger, so the debug runtime caught stack corruption. Debug builds now define `Py_DEBUG` for nanobind and the module.

**Deviations / limits**
- Only the Debug configuration of the full preset was built here (the release one is identical but for the library; CI pending).
- The standard library comes from vcpkg's install tree in development builds; packages must ship it next to the executable (`python/Lib`, P11.2).
- No `pip`: scripts get the standard library only.

### Decisions made autonomously
- **The Python module reuses the RPC dispatcher** instead of separate bindings per method: one implementation, guaranteed parity with JSON-RPC and MCP, and new RPC methods appear in Python as `forge3d.call(...)` immediately.
- **`forge3d.ops` is generated at run time from `ops.list`** (not a generated .py file), so plugin operations appear too.
- **vcpkg's CPython behind a manifest feature** rather than the system Python: reproducible, and the core presets never download or build it.
- **No CI job for the full preset yet**: building CPython in every CI run is slow; logged for the user to decide (a cached job or a nightly).

**Open**
- `[human]` The Python panel in an interactive session; a full-preset CI job.

---

## Stage C gate — 2026-10-16

| Gate item | Result |
|---|---|
| From here on, every new feature ships with its ops **and** an integration test driven through the RPC/Python surface | ✅ in place: operations are listed with JSON Schemas (`ops.list`), callable over JSON-RPC (`integration.rpc`), MCP (`integration.mcp`) and Python (`integration.python.*`); Stage D phases add an RPC or Python test per feature |

**Open `[human]` items carried out of Stage C** (none block Stage D):
- P4.2: the material editor with a real mouse. P4.3: time-of-day scrubbing, shadow stability, probe placement. P4.4: the exposure/tonemapper menus and stats overlay. P4.5: 1080p ≥ 60 fps on the reference iGPU; golden jobs on GitHub CI and lavapipe references. P5.1: a plugin from another compiler. P5.2: a real RPC client against an interactive editor. P5.3: a real MCP client (Claude Code). P5.4: the Python panel; a full-preset CI job.

---

## P6.1 Heightfield & CDLOD — 2026-10-16

**Built** (design: `docs/architecture/render.md` § Terrain)
- `engine/terrain`: `Terrain` component (tiles of 256 quads, size, height range, `r16`/`r32f`, LOD distance); `Heightfield` (CPU floats, per-tile dirty flags, revision, min/max pyramid per 32-quad node) in a `HeightfieldStore` in the registry context; seeded fBm `generate_noise`; CPU CDLOD `select_lods` (frustum culling, whole and quarter nodes); `raycast` (local and world); `terrain.create` op (name, guid, tiles, size, height range, format, seed, frequency, octaves; keeps its heights so redo is exact).
- Scene: a component-type registry (`scene::register_component<T>(name, addable)`) replaces the hard-coded component lists in reflection and snapshots, so modules outside `f3d_scene` (terrain now, scatter later) add components.
- RHI: `Format::R16Unorm`; `CommandContext::update_texture` (sub-rectangle uploads).
- Raster: `shaders/terrain.vs.hlsl` (instanced 32×32 grid, geomorphing, GPU normals) reusing `mesh.ps`/`pick.ps`; terrain in the lit, wireframe, shadow (all cascades) and pick passes; dirty-tile uploads.
- Editor: surface snapping and placement use the nearer of the mesh BVH and terrain hits. CLI and editor register `terrain.create`.
- Scenes: `tests/data/scenes/terrain_1k.json`, `terrain_8k.json` (Near/Mid/Far bookmarks).

**Verification**
- (visual) 8k² noise terrain captured from Near (180 m), Mid (1.5 km) and Far (4.2 km), lit and wireframe: no cracks, no holes at LOD seams; the wireframe shows the LOD rings and their morph. Popping can't be judged from stills: the morph covers the outer 30 % of every LOD range, so a vertex reaches the coarser grid before the switch (the `[human]` fly-through stays open).
- (auto) `unit.terrain`: 6 cases (sizes, bilinear heights and normals; raycasts hit the surface; CDLOD selection covers the terrain exactly once, LODs grow with distance and neighbours differ by at most one level; frustum culling; noise determinism and range; `terrain.create` undo/redo and the component in snapshots).
- (auto) `integration.cli.terrain_run` (create, info, undo) and `integration.python.terrain` (Gate C: `forge3d.ops.terrain.create` over the RPC dispatcher, component query, invalid-format error, capture, undo all, redo).
- (auto) `--bench` on 8k² recorded in `docs/budgets.md`: AMD iGPU 6.4 ms CPU / 4.8 ms GPU per frame on D3D12 (≈ 155 fps), RTX 3050 2.0 / 1.1 ms.
- Debug: core preset 58/58 unit+integration, full preset 61/61; Release golden 9/9. Zero warnings.

**Bugs found on the way**
- Diligent rejects R16 uploads whose row stride isn't a multiple of 4 bytes (8193 samples × 2 bytes): rows are padded.
- Terrain bindings held stale views after the shadow map or environment were recreated: they are rebuilt when any shared resource changes.
- The first 8k bookmarks put the camera below the surface or beyond the far plane; they now target points above the noise.

**Deviations / limits**
- Heights stay as floats on the CPU (4 bytes/sample, 268 MB at 8k²) even for R16 terrains: simpler sculpting and raycasts; halving it is possible later.
- The shadow selection is not culled per cascade (2.6–2.9 ms on the iGPU at 8k²).
- Noise generation (7 octaves of gradient noise over 67 M samples, split across up to 16 threads) takes ≈ 4 s at 8k² in Release; P6.5 replaces it with the procedural stack.

### Decisions made autonomously
- **A component-type registry in `f3d_scene`** instead of letting `f3d_scene` depend on `f3d_terrain` (layering forbids it) or special-casing terrain in snapshots. Alternatives: a hard-coded list per module (doesn't scale to scatter/plugins). Chosen: one registration call per component; built-ins use it too.
- **Heights in a registry-context store keyed by GUID**, not inside the component: components are copied by snapshots and reflection, and an 8k² array must not be. The op owns a `shared_ptr` so redo restores the same data.
- **Reuse `mesh.ps` and `pick.ps`** for terrain (the vertex shader emits the mesh varyings) instead of a terrain pixel shader: lighting, shadows, IBL, view modes and picking stay identical with no copy; P6.4 adds layered materials.
- **Quarter nodes** instead of per-node index buffers or skirts: CDLOD's standard way to draw a partly refined node with one grid mesh; no skirts needed because morphing keeps seams closed.
- **The shadow pass uses an unculled selection** (the camera's LODs without frustum culling), so terrain behind the camera still casts; per-cascade culling is the optimisation if needed.
- **The `--bench` run uses the Near bookmark**: the most nodes and the heaviest shadow load (the worst case).

**Open**
- `[human]` ≥ 60 fps on the reference iGPU; a fly-through for popping in an interactive editor.


---

## P6.2 Sculpt brushes — 2026-10-16

**Built**
- `f3d/terrain/sculpt.hpp`: brushes raise, lower, smooth (box blur scaled with the radius), flatten (to a fixed height or the height under the first dab), terrace, noise (seeded gradient noise) and stamp (an image's red channel, rotated, scaled to a height); radius, strength, falloff (smooth, linear, sphere, constant), dab spacing; pressure scales strength. Heights stay inside the terrain's range.
- `DabSpacer` (even dabs along the stroke, identical live or replayed), `StrokeRecorder` (applies dabs, snapshots tiles before their first change, `restore()` for cancel), `TileDiff` (per tile: before and after XOR before, zstd).
- `terrain.sculpt_stroke` op: one op per stroke; replays from its points; undo/redo from the diff; `memory_size()` counts the compressed diff. `make_sculpt_stroke_op` wraps a stroke the editor already applied.
- Editor: `SculptTool` (live strokes, Escape cancels, brush outline draped on the terrain, Alt+drag still navigates), the Terrain panel (Window menu: create a terrain; sculpt switch and brush settings), SDL pen pressure (`SDL_EVENT_PEN_AXIS`), and replays with a `"sculpt"` brush that drive the tool (benchmarks).
- RPC: `terrain.info` (size, range, hash of the heights) and `terrain.sample` (heights under world points); Python `forge3d.terrain_info` / `terrain_sample`; MCP tool `terrain_sample`. `--bench` reports terrain nodes and committed strokes.

**Verification**
- (auto) `unit.terrain` (`sculpt_test.cpp`): **100 scripted strokes** (all tools but stamp, random radii, strengths, falloffs, pressures; some off the edge) → undo all → heights **bit-identical** to the original; redo all → bit-identical to the sculpted heights. Also: falloffs, dab spacing (live = replay), each tool's effect, range clamping, diffs cover only touched tiles (1 inside a tile, 4 at a corner), live op = replayed op bit for bit, stamp from a PNG, bad parameters.
- (auto) `unit.editor` (`sculpt_tool_test.cpp`): a drag is one undo step, Escape restores, brush JSON, replay `"sculpt"`.
- (auto) `unit.api`: `terrain.info`/`terrain.sample` on a moved terrain, before/after a stroke, after undo (hash back), errors.
- (auto) Gate C: `integration.python.terrain` sculpts a pad, a ridge and terraces through `forge3d.ops.terrain.sculpt_stroke`, checks heights with `terrain_sample`, and that undo restores the hash exactly.
- (auto) `--bench` of continuous brushing on 8k² recorded in `docs/budgets.md`: 6.5 ms per frame on the AMD iGPU (D3D12), 2.3 ms on the RTX 3050.
- Debug core 58/58, full preset all passing; Release golden 9/9; zero warnings.

**Deviations / limits**
- **Brushes run on the CPU, not as compute shaders** (see decisions). Large radii at 8k² cost CPU time proportional to the area: a 500 m brush at 1 m spacing touches ~785k samples per dab.
- Pen pressure is wired from SDL but untested with a real tablet (`[human]`).
- The stamp reads 8-bit images (PNG16 heightmaps arrive with P6.5's importers).

### Decisions made autonomously
- **CPU brushes instead of GPU compute.** The spec says "compute brushes". Alternatives: (a) GPU compute with readback of touched tiles for undo, raycasts and saving; (b) GPU compute with the GPU as the authority. Chosen: CPU, because the CPU heights are already the authority (raycasts, undo diffs, P6.6 saving), results are bit-identical on every machine (the 100-stroke undo test and replay need that), and the benchmark shows ~150 fps while brushing on the iGPU. A compute path can come later behind the same `StrokeRecorder` interface if big brushes need it.
- **Diffs store before + (after XOR before)**, not before + after: the XOR is zero outside the brush and compresses to almost nothing, and redo is still exact. zstd level 1, not 3: half the commit time, slightly bigger diffs.
- **The editor applies strokes live and commits the already-applied stroke** (`make_sculpt_stroke_op`) instead of undoing and replaying it on release: no hitch at mouse-up; the unit test proves the live result equals a replay from the op's parameters.
- **Strength is unitless (0–1)**; raise/lower/noise move `strength × radius / 20` metres per dab at the centre, so brushes feel the same at any terrain scale.
- **`terrain.info` / `terrain.sample` RPC queries** were added (not in the spec) so scripts and agents can check sculpting results, and so later determinism checks (P6.3, P6.5) can compare hashes over RPC.
- **Bench via a replay that drives the sculpt tool** rather than a dedicated bench flag: it exercises the real path (raycast, live dabs, uploads, commits).

**Open**
- `[human]` ≥ 30 fps on the reference iGPU; tablet pressure feel.


---

## P6.3 Erosion — 2026-10-16

**Built** (design: `docs/architecture/render.md` § Terrain, Erosion)
- `f3d/terrain/erosion.hpp`: grid-based hydraulic erosion (virtual pipes: rain, flux, water, velocity, capacity-driven erosion/deposition, semi-Lagrangian sediment transport, evaporation) and thermal erosion (talus angle), alone or together; seeded rain; cancellable through a progress callback; deterministic for any thread count. `core::jobs::parallel_for` (a shared Taskflow pool) runs the passes.
- `terrain.erode` op (mode, iterations, seed, rain, evaporation, capacity, dissolving, deposition, min_slope, talus_angle, thermal_rate): progress through the new `ops::Context::progress` (returning false cancels: the op fails, nothing changes), undo/redo as a tile diff (`diff_tiles`). The CLI prints progress in 10 % steps.
- Brush mode: an `erode` sculpt tool (a few iterations on each dab's neighbourhood, blended in by the falloff; `erosion_iterations`).
- Editor: Terrain panel ▸ Erosion (mode, iterations, parameters, Erode, a progress bar, Cancel). The erosion runs on a background job and is committed with `make_erode_op` (the precomputed heights; re-eroded if the terrain changed meanwhile).
- `tests/data/scenes/erosion_1k.json` (a steep 1k terrain with an "Overview" and a "Close" bookmark).

**Verification**
- (auto) `unit.terrain` (`erosion_test.cpp`): the same input and settings give bit-identical heights, another seed differs; thermal conserves mass and relaxes a needle to near the talus slope; hydraulic lowers the upper slope and deposits at the foot; cancelling leaves the heights unchanged; `terrain.erode` undo/redo (hash), progress reaches 1, the same params in another session give the same hash, a precomputed result equals a direct run, cancelling via `Context::progress` fails without an undo step, bad params; the erode brush changes nothing outside its radius and undoes exactly.
- (auto) Gate C: `integration.python.terrain` erodes over the Python/RPC surface, undoes, erodes again and gets the same hash.
- (auto) 4k² global erosion time recorded in `docs/budgets.md`: 39–44 s (`perf.erosion`), ≈ 35 s through the CLI, on the dev laptop CPU.
- (visual) `erosion_1k.json` "Close" bookmark, 200 iterations, lit and normals captures: before, smooth noise hills; after, rills and gullies cut down the slopes, sharper ridges, and smoother filled floors in the valleys (deposits). An early version made cell-sized spikes (the erosion rate was 10× too high); fixed before the captures above.
- Debug core 58/58, full preset 61/61, Release golden 9/9; zero warnings.

**Deviations / limits**
- **CPU, not compute shaders**, and **over the 30 s target** on this machine (see decisions and budgets).
- Scratch memory: 44 bytes per sample (704 MB at 4k², 2.8 GB at 8k²). An 8k² global erosion may not fit on an 8 GB machine; the op fails cleanly on `bad_alloc`.
- Water and sediment don't persist between runs (no water map yet).

### Decisions made autonomously
- **CPU erosion instead of GPU compute.** Alternatives: (a) compute shaders through a GPU service injected into `ops::Context` (terrain ops can't depend on the renderer: layering); (b) CPU. Chosen: CPU for now. It keeps the CPU heights authoritative (like P6.2), works in the headless CLI and in tests without a GPU, and is deterministic on every machine (the determinism check). It misses the 30 s target here (~40 s), so a compute path behind `erode()` is the logged follow-up.
- **Two fused passes per iteration**, with sediment advected before erosion (not after, as in Mei et al.), so a pass reads only the previous state. Same physics up to the order of operator splitting; 30 % less memory traffic.
- **Rain is per cell and per iteration from a hash**, not uniform, so water finds channels and the seed matters; it is added at the end of the previous iteration (one hash per cell per iteration).
- **Global erosion in the editor runs on a worker** and commits the precomputed result (`make_erode_op`); scripted runs (RPC/Python/CLI) are synchronous with progress. If the terrain changes while the worker runs, the op erodes again on commit rather than overwrite the change.
- **The erode brush uses fixed internal settings** (both modes, more rain, `erosion_iterations` per dab) rather than exposing every erosion parameter on the brush.
- **`ops::Context::progress`** is a general hook (not erosion-specific), so other long operations can report and be cancelled the same way.

**Open**
- `[human]` Erosion time on the reference machine; the Erosion panel's progress/Cancel in an interactive session; tablet use of the erode brush.

## Object scatter (user request, early part of P7.1) — 2026-10-17

**Built**
- `f3d::world::Scatter` component (`engine/world/include/f3d/world/scatter.hpp`): meshes (picked at random per instance), area, density, minimum spacing, scale/yaw ranges, align-to-normal, sink, height/slope/terrain-layer rules, seed, instance cap, draw distance, shadows.
- `generate_scatter()`: deterministic for a seed and for any thread count. Rules run in parallel, using per-candidate hashed randoms. Spacing is then enforced in index order with a grid. Instances are cached per entity and regenerated when the settings, the entity's position, or the terrain's heights or splat maps change.
- Raster engine: `collect_draws()` feeds MeshRenderer entities and scatter instances (culled by draw distance and frustum) to the lit, shadow and pick passes. Picking an instance selects its scatter entity.
- Built-in placeholder meshes (pine, broadleaf, grass tuft, rock) and Trees/Grass/Rocks presets.
- Editor: commands `add.scatter_trees|grass|rocks|selected` (command palette and Add ▸ Scatter); a Scatter panel (Tools menu) for the surface, the meshes, Fit to terrain and Reseed.
- Tests: `tests/unit/world/scatter_test.cpp` (determinism, spacing, rules, cache, reflection, built-in meshes).

### Decisions made autonomously
- **Derived instances, not stored ones.** Instances are regenerated from the settings, not saved in `.forge`. P7.1's chunked storage and the density/erase brushes (`scatter.paint_stroke`) come later, when hand-painted instances need persisting.
- **One draw call per instance** through the existing mesh path, until P7.2's GPU-driven instancing. Per-preset draw distances keep the count manageable; grass is a 96 m patch by default, not a whole terrain.
- **A dedicated panel for meshes and surface**, because the generic inspector can't edit `vector<AssetRef>` or pick a Guid.

**Open**
- `[human]` Frame rate with dense scatters in the editor. Sculpting under a large scatter regenerates it on every height change (no throttling yet).

## Fog, post-processing, screen percentage and modeling (user request; parts of P8.2/P8.3) — 2026-10-17

**Built**
- Atmosphere (`f3d/render/raster/fog.hpp`, `shaders/fog*.hlsl*`): `HeightFog` (Unreal's Exponential Height Fog: density, falloff, albedo, Henyey-Greenstein anisotropy, sky/sun lighting, start distance, max opacity, sky cutoff) and `FogVolume` (Local Fog Volume: a box with density, height falloff, soft edges, emission) components. Near the camera the fog is a froxel volume (inject + light with the sun's cascaded shadows, the clustered point/spot lights and the sky's SH ambient; temporal accumulation with jittered samples; front-to-back integration), beyond it the height fog is integrated analytically; one full-screen pass composites both, sky included. `Light::volumetric_scattering` scales a light in the fog.
- `PostProcess` component: energy-conserving bloom (13-tap downsample chain with Karis average, tent upsample), optional threshold, natural cos^4 vignette, white balance (Bradford adaptation from a colour temperature and tint), saturation, contrast, film grain. Applied in the resolve.
- Screen percentage (25-200 %, Unreal's viewport option): the scene passes run at the scaled size; the resolve upscales (Catmull-Rom); an output-sized depth is rebuilt for the grid and debug lines. Scalability presets (Low/Medium/High/Epic) set screen percentage, fog quality and shadow resolution together. Render menu: Screen Percentage, Scalability, Volumetric Fog quality, Post Processing; saved in the preferences (`rendering` group).
- RHI: 3D textures (`TextureDimension::Tex3D`).
- Modeling (`engine/modeling`, new module `f3d_modeling`): `EditableMesh` polygon meshes saved with the scene, triangulated on demand (`modeling::sync_meshes`, called by the raster engine) into `modeling://<guid>` meshes the entity's MeshRenderer draws. Shapes: plane, cube, sphere, cylinder, cone, torus, capsule, stairs (parameters editable until the first edit). Operations `modeling.set_shape|set_mesh|extrude|inset|delete_faces|flip|subdivide|mirror|merge_vertices|set_origin`, all undoable and available to the CLI/RPC/Python.
- Editor: Add ▸ Shape and Add ▸ Visual Effects (Exponential Height Fog, Local Fog Volume, Post Process); Tools ▸ Modeling (Select Faces tool, Extrude, Inset, Delete Faces, Flip, Subdivide, Mirror, Merge, Origin, Shade Flat/Smooth, Make Editable, Modeling panel). The face-select tool is a ViewportTool (click, Shift+click, Delete, Esc).
- `viewport.capture` and golden specs take `screen_percentage` and `fog_quality`; captures with volumetric fog render 30 more frames so the fog converges.
- Tests: `tests/unit/modeling/modeling_test.cpp` (closed outward-facing shapes, volumes, extrude/inset/delete/flip, Catmull-Clark, mirror, merge, raycast, from_triangles, ops undo/redo, sync), `tests/unit/render_raster/fog_test.cpp` (analytic fog integral vs numerical, grid sizes, gathering, screen percentage, scalability, white balance, bloom levels); goldens `fog_night` and `shapes_post`.

### Decisions made autonomously
- **Volumetric fog without TAA**: temporal accumulation inside the froxel volume (90 % history, reprojected, per-frame R2 jitter in depth and in the cell) instead of waiting for P8.2's TAA. Values are pre-exposed in the volume (RGBA16F range), at the cost of a short lag when the exposure changes quickly.
- **Fog quality tiers** follow Unreal: High = 8-pixel tiles x 64 slices (the default), Epic 6 px x 96, Medium 12 x 64, Low 16 x 48, Off = analytic height fog only.
- **Bloom and grading only with a PostProcess entity**, so existing scenes and golden images are unchanged; the resolve keeps its exact path when no option is on.
- **Screen percentage before FSR**: a Catmull-Rom spatial upscale now; P8.3's dynamic resolution and FSR plug into the same scaled render size.
- **Editable meshes are stored in the scene** (JSON arrays), not as project mesh assets: shapes and edits are scene operations with undo, and nothing needs an import. Big meshes (a few hundred thousand faces) make large `.forge` files; exporting to a mesh asset is a later step.
- **Edits work on faces**, picked in the viewport; no edge/vertex modes or bevel/boolean yet. Subdivision is Catmull-Clark (smooth surfaces); "Make Editable" converts an imported mesh (welded, coplanar triangle pairs merged to quads) but drops its per-submesh materials.

**Open**
- `[human]` Frame time of volumetric fog on the reference iGPU (High vs Medium); the look of fog and bloom in interactive sessions; modeling workflow in the editor.

## Landscape build-out: textures, foliage brushes, splines, water (user request; P6.4, P7.3, P7.4, parts of P7.1) — 2026-10-17

The user asked for Unreal-level landscape tools with an easier workflow: layer textures, roads, realistic water and foliage brushes, all from one Landscape panel.

**Built**
- Layer textures (P6.4): `TerrainLayer` albedo/normal/roughness/AO/height slots, `normal_directx`, `normal_strength`, `height_contrast`; `Terrain.texture_resolution`. Texture sets detected by file name (`guess_texture_slot/set`, ambientCG and Poly Haven naming). 12 procedural built-in surfaces (`terrain/builtin_textures.*`: grass, meadow, forest floor, dirt, mud, rock, cliff, sand, gravel, snow, asphalt, cobblestone) as `builtin:terrain/<id>/<slot>`; `default_layers()` uses them. `MeshLibrary` image resolver with a file fallback; the app resolves GUID textures and `project://` paths.
- Renderer: per-terrain layer `Texture2DArray`s (colour+height, normal+roughness+AO, mips); `terrain.ps` with top-4 height blending, triplanar whiteout normals and anti-tiling (rotated second sample + macro noise).
- Ops: `terrain.set_layers` (splat remap on reorder/remove), `terrain.auto_material` (grassland, alpine, desert, forest, coast). Heights and splat maps saved in `.forge` chunks (`io/world_chunks.*`).
- Foliage (part of P7.1): `ScatterMask` density painting (`world/scatter_paint.*`), `Scatter.paint_only`, op `scatter.paint_stroke` (exact undo), masks saved in `.forge`.
- Splines (P7.4, `world/spline.*`): Road/Path/River/Fence/Custom presets; centripetal Catmull-Rom; non-destructive `update_landscape()` into derived surface heightfields (`terrain::surface_of`), rebuilt per touched tile (flatten/carve, layer paint); road ribbons with built-in textures; fence posts/rails and meshes along a spline; scatter exclusion; river water strips; op `spline.create` (adds a dirt/gravel layer when needed).
- Water (P7.3): `WaterBody` (lake/ocean), `flood_basin`, `lake_surface`, op `water.create`; `raster_water.cpp` with `water_copy` and `water` passes before fog; `water.hlsli/vs/ps`, `water_copy.ps` (Gerstner swell, flow-mapped rivers, refraction, absorption, SSR with refinement and environment fallback, sun GGX, shore and crest foam, ripples faded with distance).
- Editor: `ViewportTool` + `LandscapeTools` (one tool at a time; Delete goes to the tool first); `PaintTool`, `FoliageTool`, `SplineTool`, `WaterTool` (click the terrain: a lake filled `fill_depth` above the click, selected; ring and level preview while hovering). The Landscape panel (`ui/terrain_panel.*`) has Sculpt, Paint (layer stack, texture slots with an image dialog and drag-drop, auto material, rules), Foliage, Water (lake tool and fill depth, Add ocean, list of water bodies, level, look presets applied as one undo step, colours, clarity, waves, foam, refraction, roughness, extent, a "dry" warning) and Splines tabs.
- Extras: `rhi::Filter::Anisotropic` (8x; comparison samplers stay linear), used for the terrain layer arrays so distant ground stays sharp; the water tool outlines the shore a click would flood while hovering (a 96-cell flood, redone when the cursor moves a cell, the depth or the ground changes).
- RPC/Python/MCP: `terrain.info` also returns `surface_hash`, `layers` and `splat_hash`; `terrain.sample` takes `surface` (the ground as splines shape it). Python `terrain_sample(..., surface=True)`; the MCP `terrain_sample` tool documents `surface`. `f3d_api` now depends on `f3d_world` (allowed by the layering).
- Tests: `unit/terrain/layers_test.cpp` (8 cases), `unit/world/scatter_paint_test.cpp` (5), `unit/world/spline_test.cpp` (6), `unit/world/water_test.cpp` (6: bowl flooding against the analytic disc, extent and step caps, a ridge holding back the next basin, dry seeds, lake surface following the level and sculpting, `water.create` undo/redo and errors, reflection). Gate C: `integration.python.landscape` (`tests/integration/python/landscape.py`) builds a landscape end to end: auto material, a paint stroke (splat hash changes, undo restores it), a painted scatter, a road (surface flattened to the road, sculpted hash unchanged) and a river (bed carved below the water), a lake, a capture, then undo of everything back to the generated terrain and an empty scene.
- Docs: `architecture/render.md` (water passes in the pass table; Terrain layers and Water sections), `architecture/ops.md` (7 new ops), `architecture/api.md`, `format.md` (Terrain, Scatter, Spline, WaterBody fields; `terrain/<guid>/heights|splat` and `scatter/<guid>/mask` chunk layouts), `guides/python.md` (a landscape example, checked by running it), `guides/mcp.md`. phases.md: P6.4, P7.3, P7.4 `[~]`.

**Checks**
- (auto) `ctest -LE perf` on win-msvc-full-release: 75/75 (unit, integration, golden: no reference image moved). The debug preset builds with no warnings.
- (visual) Layers, roads, fences, rivers and lakes captured; lake at noon (12 h) and dusk (19 h) for P7.3, after the last water shader changes.
- `tools/format.py --check` still fails, as it already did before this work (many committed, unchanged files such as `core/src/jobs.cpp` and `terrain/src/erosion.cpp` fail it with clang-format 21.1.8). Only this session's new files (`water_tool.*`, `water_test.cpp`) were formatted, so the tree isn't reformatted in the middle of two sessions' uncommitted changes.

### Decisions made autonomously
- **Painted scatter is a density mask**, not stored instances: undo is exact and instances stay derived. P7.1's chunked instance storage and `scatter.fill` remain, so P7.1 stays `[ ]`.
- **Splines are non-destructive through a derived surface heightfield** (a full copy per terrain with splines; memory ×2 for that terrain), rebuilt per touched tile; sculpting and painting edit the authored data underneath.
- **Terrain data is saved as one heights chunk and one splat chunk per terrain**, not per tile; P6.6's per-tile incremental save remains.
- **Layer rules are baked on the CPU** (multithreaded), not on the GPU as P6.4 suggests: the splat maps are authoritative on the CPU like the heights, and baking stays deterministic and testable without a device.
- **Layer texture arrays are uncompressed RGBA8** (8 bytes/texel/layer), built on the main thread when layer textures change (cached per image).
- **Built-in textures are procedural**, so landscapes look textured without downloads or bundled files.
- **Water is drawn in its own pass over a copy of colour and depth** (no RHI copy needed), before fog; reflections are screen-space with an environment fallback (no planar reflections). P7.3 was built before its prerequisite P6.6, since nothing in it depends on per-tile persistence.
- **The water tool only places lakes**; oceans come from a button (one per scene is typical) and every setting is edited in the Water tab, so the tool stays a single click.
- **`terrain.info`/`terrain.sample` grew surface and splat queries** so Python, RPC and MCP clients can check what splines and painting did without a capture.

**Open**
- `[human]` The Landscape panel workflow in the editor (all five tabs), frame rate with layer arrays and water on the reference iGPU, the look of water in motion.
- Follow-ups (not required): BC-compressed layer arrays, river banks look jagged at a distance on hilly ground, picking water in the viewport, Shift+click spline point insertion (a click near the curve already inserts between its points), a visible ripple tiling pattern on large calm lakes seen from above.
- Commit split: `rhi/types.hpp` and `rhi/src/device.cpp` hold both this work (`Filter::Anisotropic`) and the fog session's `Tex3D`; `app.cpp`/`app.hpp` and other shared editor files also mix the two, so committing them separately needs hunk-level staging.

## Editor workflow: materials, defaults, drops, modal modeling, walk/fly (user request) — 2026-10-17

The user listed ten editor features and asked for the missing ones to be built and the done ones skipped.

**Already there (skipped):** editing a selected object's materials (Material Editor, P4.2), the camera preview window (Unreal-style picture in picture), dragging assets from the Asset Browser into the viewport (models, prefabs, textures and materials were already handled on drop).

**Built**
- Material library: a Material Library panel (`ui/material_panel.*`) lists `project://` materials with a filter. New Material, Duplicate, Rename, Delete and Assign to Selection are in its context menu. Double-click assigns; materials drag onto meshes in the viewport or onto a slot in the Material Editor. The selected material is previewed and edited in place. App: `create_material`, `delete_material` (clears the slots that use it in the same undo step), `assign_material_to_selection`, `assign_material_slot`, `unused_project_path` (shared with prefab and editable-copy naming). New op `material.delete` (file and `.meta`; undo keeps the GUID). Asset Browser: materials can be dragged; double-click or Assign to Selection. Command `material.new`.
- Default project settings: File > Defaults > Save / Reset Startup Scene (`file.save_startup`, `file.reset_startup`). It writes `<user prefs>/startup.forge` (the scene and editor camera) and saves the viewport render settings to the preferences. New Scene, and a launch without a file, start from it (`Document::start_from`: loaded, but untitled and clean).
- Drag and drop from the system (`SDL_EVENT_DROP_FILE`, `App::handle_file_drop`): `.forge` opens; glTF/FBX/OBJ import where they land on the viewport (else at the 3D cursor), one undo step (`import_model(..., position)`). Images, `.f3dmat` and `.f3dprefab` are copied into `<project>/Imported/` and, once the scan lists them, applied to the mesh they were dropped on, or placed for prefabs (`update_pending_drops`).
- Right-click cancels, Blender-style: gizmo drags, box selection, a tool's stroke (sculpt, paint, foliage) and modal modeling edits. The right button then doesn't start navigation (`ViewportPanel::cancellable`).
- Modal modeling (`ModelingTool`): with face editing active (Tab over the viewport toggles it on an editable mesh), E extrudes and I insets the picked faces, following the mouse (along the normal, or towards the centre). Ctrl+R loop-cuts the ring under the cursor, with a preview; the wheel or typed digits set the number of cuts. Typed numbers set values exactly and Ctrl snaps; a click or Enter confirms (one undo step), a right-click or Esc cancels. A status line in the viewport shows the value and keys. The Modeling panel has an "Adjust last operation" section (distance, amount, cuts, position: undo and re-run) and a Loop Cut button. New `modeling::edge_ring`, `modeling::loop_cut` and op `modeling.loop_cut`. `ViewportTool` gained `key_pressed`, `wheel` and `status`.
- Walk / fly navigation in every preset (`NavigationController::begin_flight`): Shift+` walks, Shift+F flies. The mouse looks (relative mouse mode); WASD or the arrows move, Q/E go down and up, Shift is faster and the wheel sets the speed; Tab switches walk and fly. Walk stays level and stands on the terrain, meshes or ground plane (eye 1.7 m, gravity, steps up). A click, Enter or Space keeps the view; a right-click or Esc goes back. While piloting a camera, the camera moves with the walk or flight, and its preview now stays up and follows live (`CameraActors::preview_camera` no longer hides the piloted camera).
- Scatter painting checked: new `unit/editor/foliage_tool_test.cpp` drives the brush like the viewport does. A drag paints live and is one undo step; erase thins; cancel restores; nothing happens without a scatter. The Scatter panel now has its own Paint section (paint toggle, Painted only, Erase, radius, strength), so painting isn't only reachable from Landscape > Foliage.

**Checks**
- (auto) Debug build has no warnings. Related tests: editor, modeling, material and scatter-paint cases, 92/92, including new `modeling_tool_test.cpp` (3), `foliage_tool_test.cpp` (3), loop cut and walk/fly cases, `material.delete` and `Document::start_from`. `forge3d-editor --hidden --frames 30` runs cleanly. The rest of the suite wasn't run (as asked).

### Decisions made autonomously
- **"Default project settings" = a startup scene** (Blender's startup file) plus the render preferences, rather than a separate settings format: the sky, fog, post-process, lights and camera users want by default are scene entities already.
- **Rename changes a material's display name** (`material.set_property name`), not its file, so references by path (editable copies have no GUID) never break.
- **Loop cut picks the edge nearest the cursor on the hovered face** and places a single cut where the cursor is along it (snapping to the middle near it); several cuts are spaced evenly.
- **Tab only toggles face editing on meshes that are already editable**; it doesn't convert imported models on its own (Make Editable is an explicit, undoable step).

**Open**
- `[human]` Try the modal edits, walk/fly (relative mouse mode) and file drops interactively; drop coordinates with ImGui multi-viewports on a second monitor.

## Editor: terrain textures, colour pickers, theme colours, sidebar preferences (user request) — 2026-10-17

The user asked for four features and to check which already existed.

**Already there (extended, not rebuilt):** terrain layers from external images (Landscape > Paint: pick a colour map and the rest of its set is found in the same folder, ambientCG and Poly Haven naming, `nor_gl`/`nor_dx`), tiling in metres, and a fixed anti-tiling mix in `terrain.ps`.

**Built**
- Terrain: per-layer `TerrainLayer::anti_tiling` (0-1, default 0.5; `params2.y`). `terrain.ps` now uses texture variation (Quilez's technique 3): a slow noise picks one of eight shifted copies of the colour and normal/roughness/AO textures and blends neighbouring copies, with explicit gradients so the offset jumps don't break mip selection. `anti_tiling` is the share of copies that are shifted, so 0 is the plain repeat (a seamless texture tiles exactly). The far, rotated macro sample is scaled by it too. Poly Haven's packed `*_arm_*` map is recognised (`terrain::is_packed_orm`): it fills the roughness (green) and AO (red) slots when the set has no separate maps, and multi-selecting it no longer turns it into the colour map. Layer names come from the set (`terrain::texture_set_name`: "aerial_rocks_02_diff_2k" -> "Aerial rocks 02", not "aerial"). The tiling slider spans 0.1-256 m.
- Colour pickers: `FieldMeta::with_color(hdr)` (and `"color": true` in op schemas). The inspector and Material Editor show a colour picker (hue wheel) instead of number drags for material base colour (with alpha) and emissive, light colour, height-fog and fog-volume colour and glow, sky ground colour, water colours and terrain layer colour. The picker edits display (sRGB) values and stores linear ones, so the swatch matches the render (ImGui colours are sRGB in this backend). Shared helper `edit_linear_color` (`ui/reflect_widgets.hpp`), also used by the Landscape panel.
- Theme colours: `Preferences::colors` (`ThemeColors`: accent, background, text; viewport selected object, active object, mesh edges, selected faces, face under cursor), saved under `ui.colors`; optional in v2 files, so no version bump. The accent recolours ImGui's whole blue accent family (by hue, keeping each shade's relative saturation and brightness). The viewport overlays (`App::draw_selection_overlay`, `ModelingTool::draw`, selected camera gizmos) read `current_theme_colors()`.
- Preferences window: a Blender-style sidebar (Interface, Themes, Viewport, Navigation, Keymap, Save & Load, System, Automation). Keymap embeds the keymap editor (`KeymapPanel::draw_editor`). Viewport has the render quality preferences (screen percentage, fog, shadows, post-process). Colour and slider drags apply live and write the file once, on release (`on_preferences_changed(bool save)`).

**Checks**
- (auto) Release build clean, including the DXC shader build. New test cases: theme colours in `preferences_test.cpp`; packed ARM maps and set names in `layers_test.cpp`. ctest: unit + golden + render 28/28, editor/terrain/preferences 23/23. `forge3d-editor --hidden --frames 30` runs cleanly.

### Decisions made autonomously
- **"Seamless tile matching" = hiding visible repeats**, not editing the images: Poly Haven/ambientCG textures already tile seamlessly, and what shows on a terrain is the grid of repeats.
- **Colour pickers edit in sRGB, store linear**: values in files and ops don't change meaning.
- **Changing the base theme resets only background and text**; the accent and viewport colours stay.

**Open**
- `[human]` Look at the anti-tiling on a real Poly Haven set at a few tiling sizes; try the theme colours and the sidebar layout interactively.
