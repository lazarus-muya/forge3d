// SPDX-License-Identifier: Apache-2.0
#pragma once

// Erosion (phases.md P6.3).
//
// Hydraulic: a grid-based shallow-water simulation with virtual pipes (Mei, Decaudin and
// Hu, "Fast Hydraulic Erosion Simulation and Visualization on GPU", 2007): rain, water
// flow between neighbouring cells, soil picked up where the water is fast and the
// ground steep, carried along, and deposited where it slows down; evaporation.
// Thermal: material steeper than the talus angle slides to lower neighbours.
//
// Every pass reads the previous state and writes separate buffers, so the result is the
// same for any thread count (deterministic for the same heights and settings; rain is a
// hash of the seed, the iteration and the cell). Heights are converted to cell units
// internally, so the settings behave the same at any terrain scale.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace f3d::terrain {

enum class ErosionMode : std::uint8_t { Hydraulic, Thermal, Both };

[[nodiscard]] std::string_view to_string(ErosionMode mode) noexcept;
/// "hydraulic", "thermal" or "both"; false if unknown.
bool parse(std::string_view text, ErosionMode& mode) noexcept;

struct ErosionSettings {
    ErosionMode mode = ErosionMode::Both;
    std::uint32_t iterations = 200;
    std::uint32_t seed = 1;
    // Hydraulic
    float rain = 0.012f;        ///< water per iteration (cells of depth, randomised 0.5-1.5x per cell)
    float evaporation = 0.015f; ///< fraction of the water lost per iteration
    float capacity = 1.0f;      ///< sediment a unit of water carries per unit of speed x slope
    float dissolving = 0.3f;    ///< 0..1: how fast soil is picked up
    float deposition = 0.3f;    ///< 0..1: how fast sediment settles
    float min_slope = 0.05f;    ///< flats still erode a little
    // Thermal
    float talus_angle = 35.0f;  ///< degrees: steeper slopes crumble
    float thermal_rate = 0.25f; ///< 0..1: share of the excess moved per iteration
};

/// Called after each iteration with the fraction done; return false to cancel.
using ErosionProgress = std::function<bool(float)>;

/// Erodes a width x height grid of heights (metres, row-major, `spacing` metres apart)
/// in place, keeping them within [min_height, max_height]. Returns false if cancelled
/// (the heights are then unchanged). Throws std::bad_alloc if the scratch buffers
/// (erosion_memory()) don't fit.
bool erode(std::span<float> heights, std::uint32_t width, std::uint32_t height, float spacing,
           const ErosionSettings& settings, float min_height, float max_height,
           const ErosionProgress& progress = {});

/// Scratch memory erode() allocates for a grid (bytes).
[[nodiscard]] std::size_t erosion_memory(std::uint32_t width, std::uint32_t height) noexcept;

} // namespace f3d::terrain
