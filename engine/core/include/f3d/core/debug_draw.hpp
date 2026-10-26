// SPDX-License-Identifier: Apache-2.0
#pragma once

// Immediate-mode debug lines. Any code (any thread) can add lines during a
// frame; the viewport renderer takes and draws them once per frame.
//
//   f3d::core::debug::line(a, b, debug::kRed);
//   f3d::core::debug::box(bounds.min, bounds.max, debug::rgba(1, 1, 0));
//   f3d::core::debug::sphere(center, 2.0f, debug::kWhite);

#include "f3d/core/math.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace f3d::core::debug {

/// Packed RGBA8 colour (red in the lowest byte), as the line shader reads it.
[[nodiscard]] constexpr std::uint32_t rgba8(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
    return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) |
           (static_cast<std::uint32_t>(b) << 16) | (static_cast<std::uint32_t>(a) << 24);
}
/// From linear-ish 0..1 floats (clamped).
[[nodiscard]] std::uint32_t rgba(float r, float g, float b, float a = 1.0f) noexcept;

inline constexpr std::uint32_t kWhite = rgba8(255, 255, 255);
inline constexpr std::uint32_t kRed = rgba8(230, 60, 60);
inline constexpr std::uint32_t kGreen = rgba8(90, 200, 80);
inline constexpr std::uint32_t kBlue = rgba8(70, 120, 235);
inline constexpr std::uint32_t kYellow = rgba8(240, 200, 60);

struct LineVertex {
    glm::vec3 position;
    std::uint32_t color;
};
static_assert(sizeof(LineVertex) == 16);

void line(const glm::vec3& a, const glm::vec3& b, std::uint32_t color);
void box(const glm::vec3& min, const glm::vec3& max, std::uint32_t color);
/// Oriented box: `transform` maps the unit cube [-1, 1]^3 scaled by `half_extents`.
void box(const glm::mat4& transform, const glm::vec3& half_extents, std::uint32_t color);
/// Three great circles.
void sphere(const glm::vec3& center, float radius, std::uint32_t color, int segments = 32);

/// Moves this frame's lines out (pairs of vertices, line list). Called by the renderer.
[[nodiscard]] std::vector<LineVertex> take_lines();
[[nodiscard]] std::size_t pending_vertex_count();

} // namespace f3d::core::debug
