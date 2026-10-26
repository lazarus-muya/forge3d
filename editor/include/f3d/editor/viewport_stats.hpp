// SPDX-License-Identifier: Apache-2.0
#pragma once

// The viewport's stats overlay (phases.md P4.4): frame rate, GPU time per render
// pass, draw calls and VRAM, as text lines (the panel draws them).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace f3d::editor {

struct ViewportStats {
    float frame_ms = 0.0f;                              ///< averaged
    std::vector<std::pair<std::string, double>> gpu_ms; ///< per pass (Device::gpu_timings)
    std::uint32_t draw_calls = 0;
    std::uint32_t local_lights = 0;
    std::uint64_t vram_used = 0;   ///< bytes (0: unknown)
    std::uint64_t vram_budget = 0; ///< bytes (0: unknown)
    float ev100 = 0.0f;            ///< exposure; NaN: not metered (no Sky)
};

/// Exponential moving average of frame times.
class FrameTimeAverage {
public:
    void add(float ms) noexcept;
    [[nodiscard]] float value() const noexcept { return value_; }

private:
    float value_ = 0.0f;
    bool started_ = false;
};

/// The overlay's lines: "fps / ms", the viewport's GPU passes (most expensive first,
/// UI and frame totals excluded), draw calls and lights, VRAM, exposure.
[[nodiscard]] std::vector<std::string> format_viewport_stats(const ViewportStats& stats);

} // namespace f3d::editor
