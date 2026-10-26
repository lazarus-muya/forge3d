// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/render/rhi/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace f3d::editor {

/// Command-line options of forge3d-editor (phases.md §0.6).
struct LaunchOptions {
    int frames = -1;     ///< --frames N: run N frames then exit 0; -1 runs until the window closes
    bool hidden = false; ///< --hidden: don't show the window
    bool help = false;   ///< --help / -h

    /// --rhi=d3d12|vulkan. Unset: platform default with fallback; set: that backend only.
    std::optional<render::rhi::Backend> rhi;
    /// --adapter=<index>|discrete|integrated|software
    std::optional<std::uint32_t> adapter_index;
    render::rhi::AdapterPreference adapter_preference = render::rhi::AdapterPreference::HighPerformance;
    bool vsync = true;       ///< --no-vsync turns it off
    bool validation = false; ///< --validation: backend debug layers

    std::string capture;     ///< --capture <png>: write the final frame of the viewport
    bool capture_ui = false; ///< --capture-ui: capture the whole window instead
    std::string bench;       ///< --bench <json>: write frame-time / memory stats
    std::string replay;      ///< --replay <input.json>: feed recorded input events
    std::string open;        ///< --open <model.gltf|glb>: load a model at start-up
    std::string scene;       ///< --scene <ops.json>: run an operation script after --open
    /// --scan-assets: scan, import and thumbnail the project's assets at start-up, even in
    /// automated runs (which otherwise never write into the directories they open).
    bool scan_assets = false;
    /// --vram-cap <MB>: asset streaming VRAM budget, overriding the preference.
    std::optional<std::uint32_t> vram_cap_mb;
    /// --viewport WxH: render the viewport at exactly this resolution (benchmarks), shown
    /// scaled into the panel.
    std::optional<std::pair<std::uint32_t, std::uint32_t>> viewport_size;
    /// --rpc[=port]: start the JSON-RPC server (port 0: any free port).
    std::optional<std::uint16_t> rpc_port;
    /// --rpc-token-file <path>: where {port, token} is written (default: the user config dir).
    std::string rpc_token_file;
};

struct LaunchParseResult {
    LaunchOptions options;
    std::string error; ///< empty on success

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

/// Parses arguments (without argv[0]). Accepts `--flag value` and `--flag=value`.
[[nodiscard]] LaunchParseResult parse_launch_options(std::span<const std::string_view> args);

[[nodiscard]] std::string_view launch_usage() noexcept;

} // namespace f3d::editor
