# Golden-image tests

Each `<name>.json` here is a `forge3d.golden` spec: scene scripts, glTF models and extra
operations that build a scene, a capture (size, view mode, bookmark) and SSIM
thresholds. `<name>.png` is its reference image. ctest runs every spec under the
`golden` label with a software rasteriser, so results don't depend on the GPU:

    ctest --preset win-msvc-release -L golden      # WARP (D3D12)
    ctest --preset linux-gcc-release -L golden     # lavapipe (Vulkan)

A test passes when the mean SSIM and its worst 16×16 block reach the spec's
`threshold` (defaults 0.99 and 0.90). A failure writes the actual image and an
amplified difference image to `build/<preset>/test-output/golden/`; CI uploads them.

A reference rendered by a different rasteriser can sit next to the shared one as
`<name>.<backend>.png` (`<name>.vulkan.png` for lavapipe); it wins when present.

After an intended rendering change, re-render the references and look at every one:

    forge3d-cli --rhi d3d12 --adapter software golden tests/golden/<name>.json --update
