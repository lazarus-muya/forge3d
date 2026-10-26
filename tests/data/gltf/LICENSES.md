# Test models: licences

Binary glTF models from the [Khronos glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) repository, unmodified. Each entry lists the licence and attribution from the model's `metadata.json`.

| File | Licence | Attribution |
|---|---|---|
| `Box.glb` | [CC-BY-4.0](https://creativecommons.org/licenses/by/4.0/legalcode) | © 2017 Cesium |
| `Fox.glb` | Model: [CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0/legalcode). Rigging & animation: [CC-BY-4.0](https://creativecommons.org/licenses/by/4.0/legalcode) | Model: PixelMannen (2014). Rigging & animation: tomkranis (2014); glTF conversion: @AsoboStudio and @scurest (2017) |

## Not committed

`DamagedHelmet.glb` (used for the P1.4 visual check) is derived from a **CC-BY-NC-4.0** model (theblueturtle_, 2016; rebuild CC-BY-4.0 by ctxwing, 2018). A non-commercial licence is incompatible with redistributing it in an Apache-2.0 project, so it is git-ignored and downloaded on demand with `python tools/fetch_test_assets.py`.

`MetalRoughSpheres.glb` (P4.2 material check): [CC-BY-4.0](https://creativecommons.org/licenses/by/4.0/legalcode), © 2017 Analytical Graphics, Inc. (Ed Mackey). Redistributable, but 11 MB, so it is fetched by the same script and git-ignored too.
