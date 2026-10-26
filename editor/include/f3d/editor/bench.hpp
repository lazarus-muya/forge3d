// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/core/math.hpp"
#include "f3d/core/stats.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace f3d::editor {

/// Result of a `--bench` run (phases.md §0.6).
struct BenchReport {
    std::string backend;
    std::string adapter;
    std::uint32_t width = 0; ///< window
    std::uint32_t height = 0;
    std::uint32_t viewport_width = 0; ///< what the viewport rendered
    std::uint32_t viewport_height = 0;
    bool vsync = true;
    std::uint64_t frames = 0;
    double time_to_first_frame_ms = 0.0;
    core::Summary cpu_frame_ms;
    std::vector<std::pair<std::string, double>> gpu_pass_ms; ///< average per pass
    std::uint64_t ram_bytes = 0;
    std::uint64_t ram_peak_bytes = 0;
    std::uint64_t vram_bytes = 0;
    std::uint64_t vram_budget_bytes = 0;

    /// Asset residency at the end of the run (phases.md P3.5).
    struct Residency {
        std::uint64_t budget_bytes = 0;
        std::uint64_t used_bytes = 0;
        std::uint64_t peak_bytes = 0;
        std::size_t meshes = 0;
        std::size_t textures = 0;
    };
    std::optional<Residency> residency;

    /// Final editor camera (navigation replay tests compare it).
    struct Camera {
        glm::vec3 position{0.0f};
        glm::vec3 target{0.0f};
        float yaw = 0.0f; ///< radians
        float pitch = 0.0f;
        float distance = 0.0f;
        bool orthographic = false;
        std::string navigation_preset;
    };
    std::optional<Camera> camera;

    /// Terrain work (phases.md P6.1, P6.2): nodes drawn in the last frame, strokes committed.
    struct Terrain {
        std::uint32_t nodes = 0;
        std::uint64_t sculpt_strokes = 0;
    };
    std::optional<Terrain> terrain;
};

[[nodiscard]] std::string to_json(const BenchReport& report);
bool write_bench_report(const std::filesystem::path& path, const BenchReport& report);

/// Accumulates per-frame CPU times and per-pass GPU times for a BenchReport.
class FrameStats {
public:
    /// Frames before this count are excluded (shader/PSO warm-up).
    explicit FrameStats(std::size_t warmup_frames = 10) : warmup_(warmup_frames) {}

    void add_frame(double cpu_ms, const std::vector<std::pair<std::string, double>>& gpu_ms);

    [[nodiscard]] core::Summary cpu_summary() const;
    [[nodiscard]] std::vector<std::pair<std::string, double>> gpu_averages() const;
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }

private:
    std::size_t warmup_;
    std::uint64_t frames_ = 0;
    std::vector<double> cpu_ms_;
    std::vector<std::pair<std::string, std::pair<double, std::uint64_t>>> gpu_; // name -> (sum, count)
};

} // namespace f3d::editor
