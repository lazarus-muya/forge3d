// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/viewport_stats.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>

namespace f3d::editor {

void FrameTimeAverage::add(float ms) noexcept {
    if (!started_) {
        value_ = ms;
        started_ = true;
        return;
    }
    value_ += (ms - value_) * 0.1f;
}

std::vector<std::string> format_viewport_stats(const ViewportStats& stats) {
    std::vector<std::string> lines;
    const float fps = stats.frame_ms > 0.0f ? 1000.0f / stats.frame_ms : 0.0f;
    lines.push_back(fmt::format("{:.0f} fps  {:.2f} ms", fps, stats.frame_ms));

    // The viewport's passes; "frame", "viewport" and "ui" are totals or not the scene.
    std::vector<std::pair<std::string, double>> passes;
    double viewport = -1.0;
    for (const auto& [name, ms] : stats.gpu_ms) {
        if (name == "viewport") {
            viewport = ms;
        } else if (name != "frame" && name != "ui") {
            passes.emplace_back(name, ms);
        }
    }
    std::stable_sort(passes.begin(), passes.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (viewport >= 0.0) {
        lines.push_back(fmt::format("GPU {:.2f} ms", viewport));
    }
    for (const auto& [name, ms] : passes) {
        lines.push_back(fmt::format("  {:<14} {:.3f} ms", name, ms));
    }
    lines.push_back(fmt::format("{} draws  {} lights", stats.draw_calls, stats.local_lights));
    if (stats.vram_used > 0) {
        const double mb = static_cast<double>(stats.vram_used) / (1024.0 * 1024.0);
        if (stats.vram_budget > 0) {
            lines.push_back(
                fmt::format("VRAM {:.0f} / {:.0f} MB", mb, static_cast<double>(stats.vram_budget) / (1024.0 * 1024.0)));
        } else {
            lines.push_back(fmt::format("VRAM {:.0f} MB", mb));
        }
    }
    if (!std::isnan(stats.ev100)) {
        lines.push_back(fmt::format("EV100 {:.1f}", stats.ev100));
    }
    return lines;
}

} // namespace f3d::editor
