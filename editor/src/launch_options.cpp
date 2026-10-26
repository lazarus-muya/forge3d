// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/launch_options.hpp"

#include <charconv>
#include <optional>

namespace f3d::editor {
namespace {

template <class T>
std::optional<T> parse_number(std::string_view text) {
    T value{};
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

} // namespace

LaunchParseResult parse_launch_options(std::span<const std::string_view> args) {
    LaunchParseResult result;
    LaunchOptions& o = result.options;

    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        std::string_view inline_value;
        bool has_inline_value = false;
        if (arg.starts_with("--")) {
            if (const auto eq = arg.find('='); eq != std::string_view::npos) {
                inline_value = arg.substr(eq + 1);
                arg = arg.substr(0, eq);
                has_inline_value = true;
            }
        }
        // The flag's value from `--flag=value` or the next argument.
        const auto take_value = [&]() -> std::optional<std::string_view> {
            if (has_inline_value) {
                return inline_value;
            }
            if (i + 1 < args.size()) {
                return args[++i];
            }
            return std::nullopt;
        };
        const auto fail = [&](std::string message) {
            result.error = std::move(message);
            return result;
        };
        const auto is_switch = [&] { return !has_inline_value; };

        if (arg == "--frames") {
            const auto value = take_value();
            const auto frames = value ? parse_number<int>(*value) : std::nullopt;
            if (!frames || *frames < 0) {
                return fail("--frames expects a non-negative integer");
            }
            o.frames = *frames;
        } else if (arg == "--hidden") {
            if (!is_switch()) {
                return fail("--hidden does not take a value");
            }
            o.hidden = true;
        } else if (arg == "--rhi") {
            const auto value = take_value();
            const auto backend = value ? render::rhi::parse_backend(*value) : std::nullopt;
            if (!backend) {
                return fail("--rhi expects d3d12 or vulkan");
            }
            o.rhi = backend;
        } else if (arg == "--adapter") {
            const auto value = take_value();
            render::rhi::DeviceDesc choice;
            choice.adapter_preference = o.adapter_preference;
            if (!value || !render::rhi::apply_adapter_choice(*value, choice)) {
                return fail("--adapter expects an index, 'discrete', 'integrated' or 'software'");
            }
            o.adapter_index = choice.adapter_index;
            o.adapter_preference = choice.adapter_preference;
        } else if (arg == "--no-vsync") {
            if (!is_switch()) {
                return fail("--no-vsync does not take a value");
            }
            o.vsync = false;
        } else if (arg == "--validation") {
            if (!is_switch()) {
                return fail("--validation does not take a value");
            }
            o.validation = true;
        } else if (arg == "--capture") {
            const auto value = take_value();
            if (!value || value->empty()) {
                return fail("--capture expects a PNG path");
            }
            o.capture = std::string(*value);
        } else if (arg == "--capture-ui") {
            if (!is_switch()) {
                return fail("--capture-ui does not take a value (use it with --capture <png>)");
            }
            o.capture_ui = true;
        } else if (arg == "--bench") {
            const auto value = take_value();
            if (!value || value->empty()) {
                return fail("--bench expects a JSON path");
            }
            o.bench = std::string(*value);
        } else if (arg == "--replay") {
            const auto value = take_value();
            if (!value || value->empty()) {
                return fail("--replay expects an input JSON path");
            }
            o.replay = std::string(*value);
        } else if (arg == "--open") {
            const auto value = take_value();
            if (!value || value->empty()) {
                return fail("--open expects a .gltf/.glb path");
            }
            o.open = std::string(*value);
        } else if (arg == "--scene") {
            const auto value = take_value();
            if (!value || value->empty()) {
                return fail("--scene expects an operation script (.json)");
            }
            o.scene = std::string(*value);
        } else if (arg == "--vram-cap") {
            const auto value = take_value();
            std::uint32_t mb = 0;
            if (!value || value->empty() ||
                std::from_chars(value->data(), value->data() + value->size(), mb).ec != std::errc() || mb == 0) {
                return fail("--vram-cap expects a size in MB");
            }
            o.vram_cap_mb = mb;
        } else if (arg == "--viewport") {
            const auto value = take_value();
            const std::size_t x = value ? value->find('x') : std::string_view::npos;
            std::uint32_t w = 0;
            std::uint32_t h = 0;
            if (x == std::string_view::npos || std::from_chars(value->data(), value->data() + x, w).ec != std::errc() ||
                std::from_chars(value->data() + x + 1, value->data() + value->size(), h).ec != std::errc() || w == 0 ||
                h == 0 || w > 16384 || h > 16384) {
                return fail("--viewport expects WxH, e.g. 1920x1080");
            }
            o.viewport_size = std::pair{w, h};
        } else if (arg == "--rpc") {
            std::uint16_t port = 0xFFFF; // the preference's port
            if (has_inline_value) {
                if (std::from_chars(inline_value.data(), inline_value.data() + inline_value.size(), port).ec !=
                    std::errc()) {
                    return fail("--rpc expects a port, e.g. --rpc=7468 (0: any free port)");
                }
            }
            o.rpc_port = port;
        } else if (arg == "--rpc-token-file") {
            const auto value = take_value();
            if (!value || value->empty()) {
                return fail("--rpc-token-file expects a path");
            }
            o.rpc_token_file = std::string(*value);
        } else if (arg == "--scan-assets") {
            o.scan_assets = true;
        } else if (arg == "--help" || arg == "-h") {
            o.help = true;
        } else {
            return fail("unknown argument: " + std::string(args[i]));
        }
    }
    return result;
}

std::string_view launch_usage() noexcept {
    return "Usage: forge3d-editor [options]\n"
           "  --frames N            run N frames, then exit 0\n"
           "  --hidden              don't show the window\n"
           "  --rhi=d3d12|vulkan    rendering backend (default: d3d12 on Windows, falling back to vulkan)\n"
           "  --adapter=X           GPU: adapter index, 'discrete' (default), 'integrated' or 'software'\n"
           "  --no-vsync            present without waiting for vertical sync\n"
           "  --validation          enable backend validation layers\n"
           "  --capture <png>       write the final frame of the viewport to a PNG\n"
           "  --capture-ui          with --capture: capture the whole window (UI included)\n"
           "  --bench <json>        write frame-time, RAM and VRAM statistics\n"
           "  --replay <json>       feed recorded input events (forge3d.input_replay)\n"
           "  --open <model>        load a glTF model (.gltf / .glb)\n"
           "  --scene <ops.json>    run an operation script (forge3d.ops) after --open\n"
           "  --scan-assets         scan/import/thumbnail the project's assets at start-up (also when automated)\n"
           "  --vram-cap <MB>       asset streaming VRAM budget (overrides the preference)\n"
           "  --viewport WxH        render the viewport at exactly WxH (benchmarks)\n"
           "  --rpc[=port]          start the JSON-RPC server on 127.0.0.1 (0: any free port)\n"
           "  --rpc-token-file <f>  where the RPC port and session token are written\n"
           "  -h, --help            show this help\n";
}

} // namespace f3d::editor
