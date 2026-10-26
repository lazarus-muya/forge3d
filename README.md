# forge3d

forge3d is a lightweight, open-source 3D landscape and scene builder written in C++20. It covers procedural and sculpted terrain, material painting, scattering, water and splines, real-time lighting with GI, and automation through Python, a C plugin API and MCP. It exports to glTF, USD, Blender and Unreal. See [`design`](design) for the architecture, [`plan.md`](plan.md) for milestones and [`phases.md`](phases.md) for the execution order.

**Status:** Stage A (foundations) is complete. The editor opens a docking UI with a D3D12/Vulkan viewport, Blender/Maya/Unreal navigation, a keymap editor, a command palette and glTF loading. Progress is recorded in [`docs/progress-log.md`](docs/progress-log.md).

## Building

### Prerequisites

| Tool | Version | Notes |
|---|---|---|
| CMake | ≥ 3.28 | |
| Ninja | any recent | Bundled with Visual Studio's C++ workload on Windows |
| C++ compiler | MSVC 2022+ (VS 17.10+), GCC 13+ or Clang 17+ | |
| [vcpkg](https://github.com/microsoft/vcpkg) | a clone, with `VCPKG_ROOT` pointing at it | Dependencies come from [`vcpkg.json`](vcpkg.json) in manifest mode |
| Git | any recent | Diligent Engine is fetched at configure time |
| Python | ≥ 3.10 | Only for the scripts in `tools/` (formatting, licence checks, optional test assets) |

GPU: D3D12 (Windows) or Vulkan 1.2+ (Windows, Linux).

### One-time vcpkg setup

```sh
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh -disableMetrics      # Linux, or Git Bash on Windows: ./bootstrap-vcpkg.bat -disableMetrics
# then set VCPKG_ROOT to this directory's absolute path, persistently
```

In cmd or PowerShell on Windows, the bootstrap command is `.\bootstrap-vcpkg.bat -disableMetrics`. Bash treats `\` as an escape character, so use `/` there.

### Windows (MSVC)

Run CMake from a Visual Studio x64 developer prompt, or prefix each command with [`tools\vsenv.cmd`](tools/vsenv.cmd). That script finds Visual Studio, loads its x64 environment and keeps your `VCPKG_ROOT`; Visual Studio's own prompt replaces it with the bundled vcpkg, which does not work with this project's baseline.

```bat
tools\vsenv.cmd cmake --preset win-msvc
tools\vsenv.cmd cmake --build --preset win-msvc-release
tools\vsenv.cmd ctest --preset win-msvc-release
```

The first configure builds every vcpkg dependency and clones Diligent, which takes 20–40 minutes. Later configures reuse vcpkg's binary cache.

If configure fails with a path-too-long error, enable long paths in Windows (`LongPathsEnabled` in the registry, or the group policy) and use a short checkout path.

### Linux (GCC or Clang)

Not yet verified outside CI. Install the system packages SDL3 needs to build, then use the `linux-gcc` or `linux-clang` preset. On Ubuntu:

```sh
sudo apt-get install -y ninja-build pkg-config autoconf autoconf-archive automake libtool \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
  libxkbcommon-dev libwayland-dev wayland-protocols libegl1-mesa-dev libgl1-mesa-dev libdbus-1-dev \
  libudev-dev libibus-1.0-dev libdrm-dev libgbm-dev libvulkan1

cmake --preset linux-gcc
cmake --build --preset linux-gcc-release
ctest --preset linux-gcc-release
```

### Presets

| Configure preset | Platform |
|---|---|
| `win-msvc` | Windows, MSVC |
| `win-clangcl` | Windows, clang-cl |
| `linux-gcc` | Linux, GCC |
| `linux-clang` | Linux, Clang |

All presets use the Ninja Multi-Config generator and build into `build/<preset>/`. Build and test presets are `<preset>-debug`, `<preset>-release` and `<preset>-relwithdebinfo`. Warnings are errors on every compiler.

### Build options

Pass these at configure time, for example `cmake --preset win-msvc -DF3D_TRACY=ON`.

| Option | Default | Effect |
|---|---|---|
| `F3D_BUILD_TESTS` | `ON` | Build the doctest suites and register them with ctest |
| `F3D_TRACY` | `OFF` | Compile in Tracy profiler zones |
| `F3D_SANITIZE` | `OFF` | AddressSanitizer + UBSan on GCC/Clang; ASan only on MSVC |
| `F3D_BUILD_SPIKES` | `OFF` | Build the throw-away technology spikes in `spikes/` |

### Tests

```sh
ctest --preset win-msvc-debug -L unit          # fast, no GPU needed
ctest --preset win-msvc-debug -L integration   # opens a window and renders on both backends
```

The unit label also runs the layering checks (forbidden module dependencies, Diligent headers outside `render/rhi`). Before committing, run `python tools/format.py --check` and `python tools/check_license_headers.py`; CI runs both.

## Running the editor

The editor is `build/<preset>/bin/<Config>/forge3d-editor` (`.exe` on Windows):

```bat
build\win-msvc\bin\Release\forge3d-editor.exe --open tests\data\gltf\Fox.glb
```

| Flag | Effect |
|---|---|
| `--open <file.glb/.gltf>` | Load a glTF model at start-up (also File ▸ Open) |
| `--rhi d3d12\|vulkan` | Choose the graphics backend; falls back to the other if it fails |
| `--adapter <index\|discrete\|integrated>` | Choose the GPU |
| `--no-vsync` | Present without vsync |
| `--validation` | Enable the graphics API's validation layer |
| `--frames <n>`, `--hidden` | Run a fixed number of frames, optionally without a visible window |
| `--capture <png>` | Save the viewport on the last frame; add `--capture-ui` to save the whole window instead |
| `--bench <json>` | Write frame times, GPU pass times and memory use |
| `--replay <json>` | Play back recorded navigation input |

Ctrl+Shift+P opens the command palette; Edit ▸ Keymap and Edit ▸ Preferences change shortcuts, the navigation preset (Blender, Maya, Unreal) and the UI scale.

Larger optional test models (not redistributable with this repository) can be downloaded with `python tools/fetch_test_assets.py`.

## License

Apache-2.0. See [`LICENSE`](LICENSE).
