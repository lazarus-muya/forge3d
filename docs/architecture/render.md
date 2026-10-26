# Rendering architecture

How a forge3d frame is rendered: the render graph, the engine and upscaler interfaces, and the raster engine's passes. Frame structure and input are in [`frame.md`](frame.md).

## Layers

```text
render/raster    RasterEngine (IRenderEngine), ResidencyManager, ThumbnailRenderer
render/upscale   IUpscaler                              (FSR arrives in P8.3)
render/graph     RenderGraph, TransientPool, IRenderEngine, View
render/rhi       Device, CommandContext, resources, ResourceState (the only Diligent user)
```

`IRenderEngine::render(ctx, view, scene, target)` draws a `RenderScene` (world + mesh library) for a `View` into a `RenderTarget` (an RGBA8 output texture holding sRGB-encoded values, left in the shader-resource state). The editor viewport, thumbnails and captures all go through it. `IUpscaler::add_passes` adds upscaling passes to an engine's graph.

## Render graph (`f3d::render::graph`)

Each frame an engine builds a `RenderGraph`, compiles it and executes it.

```cpp
graph::RenderGraph g;
auto out   = g.import_texture("Output", target.output, rhi::ResourceState::ShaderResource);
auto color = g.create_texture("Scene colour", {w, h, rhi::Format::RGBA16Float});
struct Draw { graph::TextureHandle color; };
const Draw& draw = g.add_pass<Draw>("mesh",
    [&](graph::PassBuilder& b, Draw& d) { d.color = b.write(color); },          // setup: runs now
    [](const Draw& d, graph::PassContext& pc) { /* pc.cmd, pc.texture(d.color) */ }); // execute: later
g.compile();
g.execute(ctx, pool);
```

| Concept | Rule |
|---|---|
| Resources | *Imported* textures (owned outside, never aliased, their last version is an output) and *transient* textures (exist only during the frame, placed by the graph). |
| Versions | `write()` returns a new version of the handle; later passes use it. A write also depends on the previous version when an earlier pass produced it (drawing on top). |
| Accesses | `ShaderRead`, `DepthTest`, `CopySource` are reads; `RenderTarget`, `DepthWrite`, `Storage`, `CopyDest` are writes. Bind flags of transients follow from their accesses. |
| Culling | Reference counting from the outputs (imported resources, `mark_output`, `side_effect()` passes) backwards. A pass whose results nobody reads is not executed and its transients take no memory. |
| Lifetimes | First and last position in the execution order of the passes that use a resource. |
| Aliasing | Transients sorted by first use take the first *slot* with the same size, format, mips and bind flags whose last user finished earlier. Slots are backed by textures from a `TransientPool`, which keeps them across frames and frees textures unused for 3 frames. |
| Barriers | The graph tracks each physical texture's state through the passes and emits the transitions each pass needs before it runs (`CommandContext::transition`); consecutive storage writes get a UAV barrier. Imported textures are left in their `final_state`. |
| Timers | Each executed pass gets a GPU timer and a Tracy zone named after it (the names appear in `--bench`). |
| Debug dump | `RenderGraph::to_graphviz()`; in the editor, **Dump Render Graph** (command palette, `render.dump_graph`) writes the last viewport frame to `forge3d-render-graph.dot`. |

**Aliasing granularity.** Diligent has no placed resources on all backends, so aliasing is at the texture-object level: two transients share a GPU texture, not a memory heap range. The effect for an editor (fewer textures alive at once) is the same for same-sized targets, which is what frames are made of.

## Raster engine passes

| Pass | Reads | Writes | Notes |
|---|---|---|---|
| light_cluster | Lights (buffer) | Cluster counts / indices (buffers) | compute; bins point and spot lights into the froxel grid (skipped with no local lights) |
| shadows | — | Shadow map (D32, 4-slice array, imported) | the shadowing directional light's four cascades; skipped without casters |
| mesh | Cluster lists, Shadow map | Scene colour (RGBA16F), Depth (D32) | clears both; MeshRenderers with their materials, metal/rough BRDF, IBL, shadows; pre-exposed |
| sky | Depth (test) | Scene colour | with a Sky entity: sky-view LUT + sun and moon discs where no geometry was |
| water_copy | Scene colour, Depth | Scene colour copy, Depth copy (R32F) | only with visible water: a full-screen copy the water pass refracts and traces through (no RHI copy needed) |
| water | Copies, Shadow map | Scene colour, Depth (write) | lakes, oceans and river strips (P7.3); before the fog, so fog covers water like any surface |
| exposure | Scene colour | (exposure state buffer) | with a Sky: luminance histogram → adapted exposure for the next frame |
| resolve | Scene colour | Output | tonemapping (AgX / ACES / none) and sRGB encode; background pixels get the clear colour |
| grid | Depth (test) | Output | full-screen ground grid, after tonemapping |
| debug_lines | Depth (test) | Output | `f3d::core::debug` lines of the frame, after tonemapping |

Picking renders its own one-pass graph: the ID target is imported (read back afterwards), the depth buffer is a transient.

## Lighting & materials (P4.2)

**Clustered forward+.** The view frustum is split into 16 × 9 screen tiles × 24
depth slices (exponential between the near and far planes). The `light_cluster`
compute pass (`shaders/light_cluster.cs.hlsl`) tests every point/spot light's
range sphere against each cluster's view-space box and writes up to 64 light
indices per cluster. The mesh pass shades a pixel with its cluster's lights
only. `render::raster::lights.hpp` holds the CPU reference of the same math;
`unit.render_raster` checks the formulas and `integration.raster` compares the
GPU lists with the CPU reference.

**Lights.** `scene::Light` components are gathered each frame
(`gather_lights`): up to 4 directional lights (illuminance in lux, applied
everywhere through the view constants; the default sun when the scene has none)
and up to 4096 local lights (intensity in candela, windowed inverse-square
falloff that reaches zero at the range; spots add a smooth angular falloff).

**BRDF** (`shaders/lighting.hlsli`). GGX distribution, height-correlated Smith
visibility, Schlick Fresnel, Lambert diffuse; `Diffuse = albedo × (1 − metallic)`,
`F0 = lerp(0.04, albedo, metallic)`. Ambient is a sky/ground hemisphere with the
analytic split-sum environment BRDF (P4.2; replaced by image-based lighting in P4.3, below).

**Materials.** `assets::Material` is the glTF metal/rough model: base colour,
metallic, roughness, emissive (+ strength), normal scale, occlusion strength,
alpha mode (opaque / mask + cutoff), double-sided, and texture slots (base
colour, normal, ORM, separate occlusion, emissive). Project materials are
`.f3dmat` JSON files edited through `material.create` / `material.set_property`
(undo restores the file); imported models carry theirs as read-only sub-assets
(`<model>#material/<n>`). `MeshRenderer.materials` assigns one per slot. The
**Material Editor** panel shows the active entity's slots, a preview sphere
(`render::raster::MaterialPreview`, an offscreen `RasterEngine`) and the slot's
parameters; *Make Editable Copy* copies a model's material into
`project://materials/` and assigns it in one undo step.

## Sky, image-based lighting and shadows (P4.3)

**Before the graph** (outside it, like picking; each step has a GPU timer):
1. *Sky LUTs* (`render::raster::SkyRenderer`, Hillaire 2020): transmittance (256 × 64) and
   multiple scattering (32 × 32) when the atmosphere changes, the sky-view LUT (192 × 108)
   every frame with a physical sky.
2. *Environment*, when the sky's radiance changes (sun, haze, ground, camera height in
   50 m steps): the sky into a 64² cube with mips, GGX-prefiltered into 6 mips (roughness =
   mip / 5, filtered importance sampling, 64 samples), and projected onto irradiance SH
   (L2, cosine-convolved) in slot 0 of a structured buffer.
3. *Reflection probes*: at most one capture per frame (see below).

**Sky.** A `scene::Sky` component (time of day, date, latitude/longitude, time zone, north
offset, sun illuminance, haze, ground albedo, moon, shadows, exposure compensation) on the
first visible entity drives everything. `scene::celestial` places the sun (NOAA equations)
and moon (Schlyter's elements, with its phase); the world's north is -Z (turned by
`north_offset`), east +X. Without a Sky the renderer keeps P4.2's look: a default sun and
a hemisphere environment from `SimpleLighting`, exposure 1.

**CPU atmosphere** (`atmosphere.hpp`): the same model as the shaders, single scattering
only. It colours the sun- and moonlight at the camera (transmittance), estimates the sky
dome's illuminance, gives the ground radiance below the horizon, and meters the
exposure: EV100 from the horizontal illuminance (incident-light meter, clamped to [1, 16]
so night stays dark), plus `exposure_compensation`. Results are cached per Sky value and
camera height (10 m steps). GPU and CPU agree within the multiple-scattering gain
(`integration.raster` sky test).

**Exposure and value ranges.** The scene colour is pre-exposed: the mesh and sky passes
multiply by the exposure, so daylight values (10⁵ lux) never overflow fp16. Sky textures
(sky-view LUT, environment and probe cubes) store radiance × 2⁻¹² (`kEnvScale`) for the
same reason. Tonemapping still clamps until P4.4.

**IBL in shading.** Diffuse = albedo / π × SH irradiance; specular = prefiltered cube at
`roughness × 5` × the analytic environment BRDF. `ObjectConstants.object_id.y` selects the
SH slot, and each draw binds its specular cube (part of the bindings key).

**Reflection probes.** `scene::ReflectionProbe` (radius, face resolution, a power of two in
32..512) is captured into a cube: six 90° views rendered by the mesh and sky passes with
the sky's environment, no shadows and exposure `kEnvScale`. The faces are D3D cube faces,
which are mirrored for a right-handed camera, so the capture flips X in the projection and
swaps front faces. Then mips, prefilter, SH (slot 1..64). A mesh uses the nearest probe
whose radius contains its bounds' centre, else the sky. Re-captured when the probe moves or
changes, when the sky's environment changes, and on **Rebake Reflection Probes**
(`render.bake_probes`).

**Cascaded shadow maps** (`shadows.hpp`). The first directional light that casts shadows
(the sun or, at night, the moon from a Sky; else the first scene light with `cast_shadows`;
else the default sun) gets four cascades over `ShadowSettings::max_distance` (150 m),
practical splits (λ 0.75), 2048² each in a D32 array. Each cascade is a bounding sphere of
its frustum slice: its size depends only on the projection, so turning the camera never
resizes it, and its light-space centre is snapped to whole texels, so moving the camera
never makes edges crawl (`unit.render_raster` checks both). Casters are drawn depth-only
with depth clamping (pancaking) and slope-scaled bias, culled per cascade in light space.
Receivers use a normal offset and 3×3 PCF on a comparison sampler, and fade out over the
last 10% of the distance. Thumbnails and the material preview use 512² cascades; scenes
without meshes skip the pass and bind a 1×1 placeholder.

**RHI additions** (P4.3): 2D-array and cube textures, texture views (mip/slice ranges as
render targets, depth targets, UAVs and shader resources), mip generation, comparison
samplers, depth bias and depth clamping, float texture readback.

## Exposure, tonemapping, view modes and capture (P4.4)

**Settings** (`render_settings.hpp`, `RasterEngine::settings()`, View ▸ Viewport Mode /
Tonemapper / Exposure): view mode, tonemapper, exposure mode (automatic or a manual EV100),
compensation, limits, adaptation speeds and metering percentiles.

**Automatic exposure** (with a Sky; scenes without one keep relative units, exposure
2^compensation). The scene colour is pre-exposed by the exposure in a one-element GPU
buffer, so the CPU never waits for it. Each frame the `exposure` pass builds a 256-bin
histogram of log2 luminance over [2⁻¹⁰, 2²⁰] cd/m² (un-exposing each pixel; background
pixels are skipped), averages the bins between the 10th and 90th percentiles, converts that
to EV100 with a reflected-light meter (log2(L × 100 / 12.5)), subtracts the compensation
(the setting's plus the Sky's), clamps to the limits and adapts towards it
(1 − e^(−Δt·speed), faster when it gets brighter). The result lights the next frame. After
`reset_exposure()` (a new scene, a capture) the buffer is seeded with the CPU sky meter and
the first metering snaps instead of adapting.

**Tonemapping** in the resolve: AgX (default; Wrensch's fit of Sobotka's AgX), the ACES
RRT+ODT fit (Hill) or a clamp. `render_settings.cpp` has the CPU reference. Pixels nothing
was drawn on (scene alpha 0) take the clear colour instead, untonemapped, so the editor's
background is exact. The grid and debug lines are drawn after the resolve, straight onto the
display texture (depth-tested against the scene), so their colours don't depend on exposure.

**View modes** (`g_FrameParams.w`): lit; unlit (base colour + emissive, no lighting, no
tonemapping); wireframe (a wireframe pipeline variant, flat colour); normals (world shading
normals × 0.5 + 0.5, raw); cascades (lit, tinted red/green/blue/yellow by shadow cascade).
The sky and the exposure pass only run in the lit modes.

**Stats overlay** (View ▸ Stats Overlay, `view.toggle_stats`): averaged frame rate, the
viewport's GPU passes (most expensive first), draw calls and lights, VRAM.

**Capture** (`capture.hpp`): `Capturer` renders a world offscreen at any size with its own
`RasterEngine` (two frames so the exposure is metered, more while reflection probes are
pending) and writes a PNG. The `viewport.capture` operation is transient and calls a function
the host registers: the editor's (its device, mesh library and residency, the editor camera
as default view) or the CLI's (a device created on first use; meshes referenced by file
path). `capture_view` picks the view: a Camera entity, a bookmark (the editor camera's orbit
convention), the host's view at the capture's aspect ratio, or a view framing the scene.

## Golden-image tests (P4.5)

`tests/golden/<name>.json` specs (scene scripts, glTF models, extra ops, a capture, SSIM
thresholds) with `<name>.png` references; `forge3d-cli golden <spec>` builds the scene,
captures it and compares (`core::compare_images`: mean SSIM on luma with an 11×11 Gaussian
window, plus the worst 16×16 block). ctest runs them under the `golden` label on a
software rasteriser (`--adapter software`: WARP on Windows, lavapipe on Linux), so the
results don't depend on the machine's GPU; failures leave the actual and an amplified diff
image in `test-output/golden/`, which CI uploads. `<name>.vulkan.png` overrides the shared
reference for lavapipe. See `tests/golden/README.md` for updating references.

## Terrain (P6.1)

`f3d::terrain` holds the data; the raster engine draws it (`raster_terrain.cpp`).

- **Data.** A `Terrain` component (tiles per side, size in metres, height range, `R16` or
  `R32F`, `lod_distance`) on an ordinary entity. Heights live outside the registry in a
  `HeightfieldStore` (registry context, keyed by GUID): `tiles × 256` quads per side, so
  `tiles × 256 + 1` samples shared between neighbouring tiles, kept as floats on the CPU
  with per-tile dirty flags and a revision counter. A min/max pyramid per 32-quad node
  feeds LOD selection.
- **CDLOD** (Strugar 2009). `select_lods` walks the quadtree on the CPU (frustum-culled
  by node bounds, LOD ranges doubling from `lod_distance` finest-node sizes). A node whose
  children are only partly in range draws the out-of-range quarters itself (`quads` = half
  the node), so one 32×32 grid mesh (and its quarter index range) covers everything. Nodes
  are a `uint4` structured buffer; each pass is one instanced draw for whole nodes and one
  for quarters (`g_TerrainDraw.x` = first node).
- **Vertex shader** (`terrain.vs.hlsl`): samples the height texture, morphs odd grid
  vertices onto the coarser LOD's grid over the last 30 % of the LOD's range (no cracks
  or popping), and computes the normal from neighbouring samples at full resolution. Its
  output matches `mesh.vs`, so `mesh.ps` (lighting, shadows, IBL, view modes) and
  `pick.ps` shade terrain unchanged. The lit pass uses `terrain.ps`, which blends the
  material layers (below) and then lights them like `mesh.ps`.
- **Upload.** One texture per terrain (`R16Unorm`, rows padded to 4 bytes, or `R32F`);
  dirty tiles go up with `CommandContext::update_texture`.
- **Passes.** Lit/wireframe in the mesh pass, every shadow cascade (a separate, unculled
  selection around the camera so off-screen hills still cast), and picking (the ID
  buffer).
- **Sculpting** (P6.2, `f3d/terrain/sculpt.hpp`). Brushes run on the CPU, where the
  heights are authoritative; each dab marks its tiles dirty and the renderer uploads only
  those (one 257² sub-rectangle each). `DabSpacer` places dabs every `spacing × radius`
  along the stroke, the same whether points arrive live or from a script, so a recorded
  stroke replays bit for bit. `StrokeRecorder` copies each tile before its first change;
  the stroke's `TileDiff` keeps per tile the heights before and `after XOR before`, zstd
  level 1 (mostly zeros outside the brush). The editor's `SculptTool` applies the stroke
  live and hands the recorder to `terrain.sculpt_stroke` on release, so committing costs
  only the compression (8–9 ms for a 150-dab stroke at 8k²).
- **Erosion** (P6.3, `f3d/terrain/erosion.hpp`). Hydraulic erosion is the virtual-pipe
  shallow-water model (Mei et al. 2007) on the height grid in cell units; thermal erosion
  moves material steeper than the talus angle to lower neighbours. One iteration is two
  multithreaded passes (fluxes + thermal outflow; then water, upstream sediment,
  erosion/deposition and the thermal exchange into double buffers), so results don't
  depend on the thread count; rain is a hash of (seed, iteration, cell). Scratch: 44
  bytes per sample (704 MB at 4k²). `terrain.erode` runs it globally (the editor runs it
  on a worker with progress and Cancel, then commits the result as one undo step); the
  `erode` brush runs a few iterations around each dab and blends them in by the falloff.
- **Raycast.** `terrain::raycast` clips the ray to the terrain's box, marches it at half a
  sample spacing against the bilinear surface and refines the crossing by bisection; the
  editor's surface snap and placement take the nearer of the mesh BVH hit and the
  terrain hit.

## Terrain layers (P6.4)

- **Layers.** `Terrain.layers` (at most 16, bottom to top) each have up to five texture
  slots (albedo, normal, roughness, AO, height), a tiling, triplanar, normal strength and
  height contrast. Built-in surfaces (`terrain/builtin_textures.hpp`: grass, meadow, forest
  floor, dirt, mud, rock, cliff, sand, gravel, snow, asphalt, cobblestone) are generated
  procedurally and referenced as `builtin:terrain/<id>/<slot>`, so landscapes look textured
  with no downloads; files and project assets work the same way.
- **Weights.** The splat maps (`Splatmap`: one RGBA8 map per 4 layers, `tiles × 128`
  texels per side) are a `Texture2DArray`, one slice per map; painted or baked tiles are
  uploaded as they change.
- **Textures.** Per terrain, two `Texture2DArray`s with a slice per layer at
  `Terrain.texture_resolution`: colour + height (alpha) and normal + roughness + AO, with
  mips, sampled with 8x anisotropic filtering. Missing slots get neutral values (a layer
  without textures uses its colour).
  They are uncompressed RGBA8 (8 bytes per texel per layer) and rebuilt on the main thread
  when a layer's images change (decoded images are cached).
- **Shading** (`terrain.ps.hlsl`). The four heaviest layers of a pixel are blended by
  weight and texture height (height contrast sharpens the borders), normals blended with
  whiteout (triplanar on cliffs), and tiling is hidden by a second, rotated sample and a
  macro-scale noise variation.
- **Splines** (P7.4) never change the sculpted heights: `world::update_landscape` keeps a
  derived surface heightfield per terrain with splines (flattened roads, carved riverbeds,
  painted layers), rebuilt per touched tile; the renderer, raycasts, scatter and water use
  `terrain::surface_of`. Roads also get a textured ribbon mesh, fences their posts and
  rails.

## Water (P7.3)

`world::WaterBody` entities (lakes and oceans) and River splines are drawn by
`raster_water.cpp` in the `water_copy` and `water` passes, after the sky and before fog.

- **Geometry.** A lake's surface is the basin flooded from the entity
  (`world::flood_basin`: 4-connected cells below the level within `extent`, at most 512
  per side, plus a one-cell skirt under the shore), cached until the lake or its terrain
  changes. An ocean is a 192² grid around the camera reaching 12 km, denser near the
  centre. Rivers are the splines' water strips, with a flow direction per vertex.
- **Vertex shader** (`water.vs`): Gerstner swell from the wave height, length, speed and
  wind direction.
- **Pixel shader** (`water.ps`): ripple normals (two scrolling layers; flow-mapped with
  two cross-faded phases on rivers; faded with distance), refraction of the scene copy
  where the water is deep enough, absorption by the distance travelled through the water
  into the shallow-to-deep colour, reflections traced in screen space over the depth copy
  (with refinement) falling back to the sky's environment, Fresnel, the sun's GGX
  highlight (shadowed), and foam at shores (shallow water) and on crests.
- **Choices.** Reflections are screen-space only (no planar reflection pass); water is
  drawn on the main thread with one draw per body.

