// SPDX-License-Identifier: Apache-2.0
#pragma once

// The viewport measure tool (phases.md P2.5): click two points for a distance,
// a third for the angle at the middle point. Pure math; the viewport places the
// points (surface raycast) and draws them with debug lines.

#include "f3d/core/math.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace f3d::editor {

class MeasureTool {
public:
    static constexpr std::size_t kMaxPoints = 3;

    /// Adds a point; a fourth click starts a new measurement.
    void add(const glm::vec3& point);
    void clear() noexcept { points_.clear(); }
    [[nodiscard]] const std::vector<glm::vec3>& points() const noexcept { return points_; }

    /// Length of segment i (points i -> i+1), metres.
    [[nodiscard]] std::optional<float> segment(std::size_t i) const;
    /// Total length along the points.
    [[nodiscard]] float total() const;
    /// Angle at the middle point (degrees, 0..180) once there are three points.
    [[nodiscard]] std::optional<float> angle_degrees() const;

    /// Queues the measurement's debug lines (f3d/core/debug_draw.hpp).
    void draw() const;

private:
    std::vector<glm::vec3> points_;
};

} // namespace f3d::editor
