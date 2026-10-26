# forge3d — Budgets

Targets from `design` §5, with measured values. `--bench` writes the raw JSON. Each row says which build, machine and phase it came from.

**Dev machine (not the reference hardware):** Windows 11 laptop with an AMD Radeon integrated GPU (D3D12 FL 12.1, no RT) and an NVIDIA GeForce RTX 3050 Laptop GPU; MSVC 19.50. The reference machine (Intel Iris Xe / GTX 1060-class, 8 GB RAM) is a `[human]` check. This laptop's AMD iGPU is faster than an Iris Xe, so iGPU numbers here are optimistic.

| Budget | Target | Measured | Build / machine / phase |
|---|---|---|---|
| Cold start to interactive viewport | < 2 s | **0.33–1.04 s** (Vulkan/iGPU 0.33, Vulkan/dGPU 0.68, D3D12/iGPU 0.80, D3D12/dGPU 0.99), visible window, full editor UI | Release, dev machine, P1.4 |
| Editor idle RAM (empty scene) | < 250 MB | **134 MB** D3D12 / RTX 3050 (default), **109 MB** D3D12 / AMD iGPU, **166 MB** Vulkan / AMD. Vulkan / NVIDIA: **366 MB** (over; 338 MB right after device creation, driver working set) | Release, empty scene, 300 frames, P3.5 (Stage B gate) |
| Streamed assets VRAM | under the cap | 200 × 1024² RGBA8 textures: 105.5 MB peak at a 512 MB cap; 63.4 MB peak at a 64 MB cap (all 200 resident at coarser mips) | Release, RTX 3050, P3.5 `tools/stress_streaming.py` |
| Viewport, Low preset, 1080p, reference iGPU | ≥ 60 fps | AMD iGPU: **2.1 ms CPU / ~1.5 ms GPU per frame** (≈ 470 fps) at 1600×900, D3D12, DamagedHelmet, Lambert pass | Release, P1.4. `[human]` reference iGPU |
| ↳ 1080p, sky + shadows + IBL + AgX (`--viewport 1920x1080`) | ≥ 60 fps | AMD iGPU: **5.7 ms CPU / 4.2 ms viewport GPU** (shadows 1.6, mesh 1.1, exposure 0.4, resolve 0.35) ≈ 175 fps, D3D12, `sky_1200` scene | Release, P4.5. `[human]` reference iGPU (M4 exit) |
| ↳ with PBR + clustered lights | ≥ 60 fps | AMD iGPU: **1.65 ms CPU / 0.54 ms viewport GPU** (mesh 0.25, light_cluster 0.007) at 1600×900, D3D12, DamagedHelmet with its 5 PBR textures | Release, P4.2. `[human]` reference iGPU |
| VRAM | user cap, default 2 GB | 39–49 MB (D3D12), 103–151 MB (Vulkan) with DamagedHelmet | Release, P1.4 |
| Install size | < 60 MB | spike exe 8.7 MB + 2.9 MB DLLs (Diligent D3D12 + Vulkan statically linked) | Release, P0.3 — due P11.2 |
| Terrain brushing 8k² | ≥ 30 fps | — | due P6.2 |
| Scatter 1M instances | ≥ 60 fps GTX 1060 | — | due P7.2 |

## Raw runs

### P4.5 — 1080p (`--viewport 1920x1080 --no-vsync`, Release, 300 frames)

| Backend / adapter | Case | CPU avg / p95 (ms) | GPU viewport ms (mesh / shadows / sky / exposure / resolve) · ui | RAM MB | VRAM MB |
|---|---|---|---|---|---|
| D3D12 / RTX 3050 | sky | 1.75 / 2.54 | 1.02 (0.32 / 0.20 / 0.06 / 0.11 / 0.11) · 0.19 | 151.7 | 143.9 |
| D3D12 / RTX 3050 | helmet | 1.42 / 2.28 | 0.48 (0.15 / 0.09 / – / – / 0.06) · 0.21 | 262.6 | 251.4 |
| D3D12 / AMD iGPU | sky | 5.73 / 7.55 | 4.25 (1.12 / 1.64 / 0.19 / 0.39 / 0.35) · 0.79 | 193.2 | 147.7 |
| D3D12 / AMD iGPU | helmet | 3.95 / 5.11 | 2.56 (0.72 / 1.20 / – / – / 0.20) · 0.81 | 359.1 | 274.0 |
| Vulkan / RTX 3050 | sky | 1.66 / 2.15 | 1.35 (0.51 / 0.24 / 0.06 / 0.19 / 0.15) · 0.18 | 370.5 | 214.5 |
| Vulkan / AMD iGPU | sky | 5.55 / 7.09 | 4.36 (1.09 / 1.64 / 0.20 / 0.53 / 0.37) · 0.92 | 282.4 | 226.7 |

The AMD iGPU here is faster than the reference Iris Xe; the 1080p budget (≥ 60 fps, 16.7 ms) has about 3× headroom on it. Shadows stay the largest pass (P8.4 presets).

### P4.4 — exposure and tonemapping (Release, 1600×900, 300 frames; cases as P4.3)

| Backend / adapter | sky: CPU avg / p95 (ms) | sky: viewport GPU ms (exposure / resolve) | helmet: viewport GPU ms | idle RAM MB |
|---|---|---|---|---|
| D3D12 / RTX 3050 | 1.57 / 2.67 | 0.60 (0.055 / 0.037) | 0.28 | 146.1 |
| D3D12 / AMD iGPU | 3.94 / 4.64 | 2.62 (0.147 / 0.104) | 1.72 | 122.2 |
| Vulkan / RTX 3050 | 1.18 / 2.06 | 0.65 (0.160 / 0.040) | 0.25 | 367.6 |
| Vulkan / AMD iGPU | 3.56 / 4.11 | 2.52 (0.196 / 0.105) | 1.76 | 178.7 |

The histogram costs 0.05–0.2 ms (one pixel per thread with group-shared bins); the AgX resolve 0.04–0.1 ms. Everything else is unchanged from P4.3.

### P4.3 — sky, IBL and shadows (Release, 1600×900, 300 frames)

`sky` = `--scene tests/data/scenes/sky_1200.json --no-vsync` (Sky at noon, ground, two boxes, 4 × 2048² cascades); `helmet` = `--open DamagedHelmet.glb --no-vsync` (default sun, shadows on); `idle` = empty scene (vsync on).

| Backend / adapter | Case | CPU avg / p95 (ms) | GPU ms: viewport (mesh / shadows / sky / sky LUTs) · ui | RAM MB | VRAM MB |
|---|---|---|---|---|---|
| D3D12 / RTX 3050 | sky | 1.46 / 2.38 | 0.51 (0.11 / 0.20 / 0.02 / 0.05) · 0.19 | 144.4 | 119.9 |
| D3D12 / RTX 3050 | helmet | 1.27 / 1.80 | 0.27 (0.07 / 0.09 / – / –) · 0.21 | 258.2 | 227.5 |
| D3D12 / RTX 3050 | idle | (vsync) | 0.12 · 0.19 | 141.0 | 54.4 |
| D3D12 / AMD iGPU | sky | 3.80 / 4.46 | 2.48 (0.41 / 1.60 / 0.05 / 0.15) · 0.75 | 185.6 | 118.2 |
| D3D12 / AMD iGPU | helmet | 3.07 / 3.73 | 1.81 (0.32 / 1.19 / – / –) · 0.78 | 351.4 | 244.6 |
| D3D12 / AMD iGPU | idle | (vsync) | 0.85 · 0.75 | 115.6 | 48.9 |
| Vulkan / RTX 3050 | sky | 1.11 / 1.89 | 0.45 (0.10 / 0.19 / 0.02 / 0.05) · 0.30 | 369.3 | 214.5 |
| Vulkan / RTX 3050 | idle | (vsync) | 0.11 · 0.17 | 368.6 | 150.5 |
| Vulkan / AMD iGPU | sky | 3.61 / 5.16 | 2.52 (0.40 / 1.64 / 0.06 / 0.14) · 0.89 | 269.0 | 214.0 |
| Vulkan / AMD iGPU | idle | (vsync) | 0.79 · 0.92 | 165.1 | 111.2 |

Notes:
- Shadows are the largest cost on the iGPU (1.1–1.6 ms for four 2048² cascades, most of it clearing and rasterising the depth slices). Shadow resolution and cascade count are quality-preset knobs (P8.4); Low should drop to 1024².
- Idle RAM is unchanged from P3.5 (empty scenes skip shadows and allocate only a 1×1 placeholder map): 141 / 116 / 165 MB on D3D12 dGPU / D3D12 iGPU / Vulkan iGPU, within the 250 MB budget. Vulkan / NVIDIA stays at its driver baseline (~369 MB).
- The environment re-bake and probe captures don't run in steady frames; the sky LUTs (transmittance and multiple scattering only on change, the sky-view LUT every frame) cost 0.05 ms dGPU / 0.15 ms iGPU.

### P6.3 — global erosion on 4k² (Release, dev machine CPU)

`ctest --preset win-msvc-release -L perf -R erosion` (`tests/perf/erosion_perf_test.cpp`): a 4097² noise terrain, default settings (hydraulic + thermal, 200 iterations), 16 worker threads.

| Run | Time | Per iteration | Scratch |
|---|---|---|---|
| 1 (after a build) | 44.5 s | 222 ms | 704 MB |
| 2 | 39.4 s | 197 ms | 704 MB |
| `forge3d-cli` `terrain.erode` (incl. the tile diff) | ≈ 35 s | 166 ms | |

**Over the 30 s target** (set for the reference iGPU, assuming compute shaders). The CPU path is memory-bandwidth bound (16 threads are 6.4× one thread); fusing five passes into two and removing per-neighbour divisions took it from 236 to 166–197 ms per iteration. A GPU compute path behind the same `erode()` interface is the follow-up if the reference machine needs it (logged in the progress log). The time is linear in the iteration count.

 `forge3d-editor --hidden --frames 300 --no-vsync --viewport 1600x900 --scene tests/data/scenes/terrain_8k.json --replay tests/data/input/sculpt_8k.json --bench` (Release)

The 8k² terrain from P6.1, three 90-frame raise strokes (radius 40 m, spacing 0.25: 120–180 dabs each) driven through the editor's sculpt tool by the replay; every frame applies the new dabs, uploads the dirty tiles and renders. Stroke commits (compressing the diff) are in the p99/max.

| Backend / adapter | CPU frame avg / p95 / p99 / max (ms) | GPU viewport (ms) | RAM MB |
|---|---|---|---|
| D3D12 / RTX 3050 | 2.30 / 3.40 / 9.10 / 18.75 | 0.96 | 682.5 |
| D3D12 / AMD iGPU | 6.54 / 8.51 / 13.37 / 15.06 | 5.08 | 852.2 |
| Vulkan / RTX 3050 | 2.45 / 6.94 / 12.37 / 14.22 | 1.56 | 904.0 |
| Vulkan / AMD iGPU | 5.96 / 7.44 / 9.71 / 12.40 | 4.76 | 1066.0 |

≈ 150 fps while brushing on the AMD iGPU (target ≥ 30 fps on the reference iGPU: `[human]`, with tablet feel). Brushing adds ~0.3 ms CPU per frame over the static view; committing a stroke takes 8–9 ms (zstd level 1; level 3 took 15–17 ms).

 `forge3d-editor --hidden --frames 300 --no-vsync --viewport 1600x900 --scene tests/data/scenes/terrain_8k.json --bench` (Release)

8192² quads (32 × 32 tiles, R16), noise, "Near" bookmark (180 m from a ridge, the most nodes), four shadow cascades.

| Backend / adapter | CPU frame avg / p95 (ms) | GPU ms: viewport (mesh / shadows) · ui | RAM MB | VRAM MB | First frame |
|---|---|---|---|---|---|
| D3D12 / RTX 3050 | 1.96 / 2.73 | 1.09 (0.33 / 0.30) · 0.33 | 664.9 | 265.7 | 4.74 s |
| D3D12 / AMD iGPU | 6.44 / 8.73 | 4.80 (0.84 / 2.86) · 0.79 | 848.3 | 528.9 | 6.27 s |
| Vulkan / RTX 3050 | 1.89 / 2.75 | 1.58 (0.47 / 0.51) · 0.20 | 883.3 | 438.5 | 4.23 s |
| Vulkan / AMD iGPU | 5.82 / 7.23 | 4.60 (0.84 / 2.57) · 0.93 | 1063.8 | 720.6 | 6.68 s |

Notes:
- ≈ 155 fps on the AMD iGPU (target ≥ 60 fps on the reference iGPU: `[human]`).
- Shadows dominate on the iGPU: the shadow selection is unculled (so hills behind the camera cast) and drawn into all four cascades. Culling it per cascade is a follow-up if the reference iGPU misses the target.
- RAM: the 8193² heights are kept as floats on the CPU (268 MB) for sculpting and raycasts; the first frame includes generating 67 M noise samples on one thread.

### P4.2 — PBR, `forge3d-editor --hidden --frames 300 --no-vsync --open DamagedHelmet.glb --bench` (Release, 1600×900)

| Backend / adapter | CPU frame avg / p95 (ms) | GPU ms: viewport (mesh / light_cluster) · ui | RAM MB | VRAM MB | First frame |
|---|---|---|---|---|---|
| D3D12 / RTX 3050 | 1.26 / 2.01 | 0.151 (0.041 / 0.012) · 0.20 | 256.1 | 158.9 | 2.14 s |
| D3D12 / AMD iGPU | 1.65 / 1.78 | 0.539 (0.253 / 0.007) · 0.78 | 297.6 | 175.4 | 2.12 s |
| Vulkan / RTX 3050 | 1.03 / 1.39 | 0.135 (0.054 / 0.011) · 0.19 | 532.5 | 294.5 | 1.81 s |
| Vulkan / AMD iGPU | 1.63 / 1.82 | 0.624 (0.285 / 0.004) · 0.88 | 506.1 | 433.6 | 1.68 s |

Empty scene (budget rows): D3D12 / RTX 3050 140 MB, first frame 0.95 s; D3D12 / AMD 115 MB, 0.77 s; Vulkan / AMD 165 MB, 0.31 s. Budgets still met.

Notes:
- RAM and VRAM with the helmet roughly doubled against P1.4. The model's five 2048² images (base colour, normal, ORM, occlusion, emissive) are now decoded and uploaded as RGBA8 with mips (~110 MB VRAM); P1.4 baked the base colour into vertex colours.
- First frame with a model takes ~2.1 s on D3D12 (was ~1.0 s) because those images are decoded synchronously on the main thread. Not a budget breach (the budget is the empty scene), but asynchronous texture loading is a follow-up (see P3.5 limits).

### P3.5 — idle RAM and streaming stress (Release, RTX 3050 laptop)

Idle: `forge3d-editor --hidden --frames 300 --bench` with no scene (3 runs on D3D12 / RTX 3050: 133.7, 133.8, 133.5 MB).

| Backend / adapter | RAM working set MB | VRAM MB |
|---|---|---|
| D3D12 / RTX 3050 (default) | 133.6 | 48.2 |
| D3D12 / AMD iGPU | 108.6 | 38.6 |
| Vulkan / RTX 3050 | 366.5 (338.2 after 1 frame) | 150.5 |
| Vulkan / AMD iGPU | 165.6 | 99.9 |

Budget < 250 MB: **met on the default backend** (D3D12) on both GPUs and on Vulkan / AMD. Vulkan on the NVIDIA driver is over: the extra ~230 MB is already there after device creation, does not come from implicit layers (same result with `VK_LOADER_LAYERS_DISABLE=~implicit~`), and does not come from Diligent's memory pools (they are not preallocated). Treated as driver baseline; Vulkan is the fallback backend on Windows.

Streaming stress: `python tools/stress_streaming.py --editor … --cli … --out build/stress_streaming [--cap-mb N]` makes a project with 200 uncompressed 1024² textures (5.6 MB each with mips, 1.1 GB at full resolution) on 200 quads, imports it (`--scan-assets`), runs 240 frames with `--vram-cap` and checks the bench report.

| Cap | Peak / used MB | Resident | CPU frame avg |
|---|---|---|---|
| 512 MB | 105.5 / 105.5 | 200 textures, 1 mesh | 7.3 ms (vsync) |
| 64 MB | 63.4 / 63.4 | 200 textures (coarser mips), 1 mesh | — |

At 512 MB the scene wants less than the cap (the screen-size rule picks mips from 1 to 7). At 64 MB the cap binds and residency coarsens the smallest quads; the capture is unchanged to the eye. The manager-level test `integration.streaming` drives 200 textures that all want mip 0 (1.1 GB) under 512 MB: 510 MB used, never over.

### P3.3 — asset index, `perf.asset_index` (Release, RTX 3050 laptop, NVMe)

A generated library of 1,000 assets (600 PNG, 400 OBJ in 40 folders; every 10th tagged):

| Step | Time |
|---|---|
| First scan: hash 1,000 sources + write 1,000 `.meta` files | 3.75 s (file creation dominates) |
| Index sync, first (1,000 inserts) / unchanged re-sync | 9.2 ms / 2.3 ms |
| Search: all 1,000 / text "rock" / type texture / tag / text + type + 2 tags | 1.23 / 0.82 / 1.01 / 0.53 / 0.29 ms |

Budget (phases.md P3.3): search < 100 ms. **Met**, worst query 1.2 ms (11.8 ms in Debug).
A second session seeds its scan from the index and re-hashes nothing (unit test `asset index: persists…`).


### P1.4 — `forge3d-editor --hidden --frames 300 --no-vsync --open DamagedHelmet.glb --bench` (Release, 1600×900)

| Backend / adapter | CPU frame avg / p95 (ms) | GPU ms: viewport (mesh / grid / resolve) · ui | RAM MB | VRAM MB |
|---|---|---|---|---|
| D3D12 / RTX 3050 | 1.26 / 2.09 | 0.11 (0.018 / – / –) · 0.19 | 132.8 | 48.8 |
| D3D12 / AMD iGPU | 2.14 / 4.76 | 0.38 (0.054 / 0.135 / 0.159) · 0.86 | 108.3 | 39.2 |
| Vulkan / RTX 3050 | 1.15 / 2.14 | 0.08 (0.018 / – / –) · 0.18 | 364.1 | 150.5 |
| Vulkan / AMD iGPU | 1.62 / 2.54 | 0.34 (0.059 / – / –) · 0.98 | 166.8 | 102.6 |

Notes:
- The UI pass costs more than the scene on the iGPU (0.86–0.98 ms). Worth watching once the panels have real content.
- Vulkan on the NVIDIA driver adds ~230 MB of resident memory compared with D3D12 on the same GPU, so the 250 MB idle budget is at risk on Vulkan + NVIDIA. Track in P3.5 (idle RAM check); options include the driver's shader-cache settings and measuring after the first frames.

### P1.1 — clear only (Debug, D3D12, RTX 3050, vsync on)

`cpu_frame_ms avg 6.91` (vsync-bound, 144 Hz panel), `gpu frame 0.028 ms`, RAM 122.9 MB, VRAM 37.7 MB, time to first frame 1204 ms (Debug).
