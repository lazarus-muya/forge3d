// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/measure_tool.hpp"

#include "f3d/core/debug_draw.hpp"

#include <algorithm>
#include <cmath>

namespace f3d::editor {

void MeasureTool::add(const glm::vec3& point) {
    if (points_.size() >= kMaxPoints) {
        points_.clear();
    }
    points_.push_back(point);
}

std::optional<float> MeasureTool::segment(std::size_t i) const {
    if (i + 1 >= points_.size()) {
        return std::nullopt;
    }
    return glm::distance(points_[i], points_[i + 1]);
}

float MeasureTool::total() const {
    float sum = 0.0f;
    for (std::size_t i = 0; i + 1 < points_.size(); ++i) {
        sum += glm::distance(points_[i], points_[i + 1]);
    }
    return sum;
}

std::optional<float> MeasureTool::angle_degrees() const {
    if (points_.size() < 3) {
        return std::nullopt;
    }
    const glm::vec3 a = points_[0] - points_[1];
    const glm::vec3 b = points_[2] - points_[1];
    const float la = glm::length(a);
    const float lb = glm::length(b);
    if (la < 1e-6f || lb < 1e-6f) {
        return std::nullopt;
    }
    const float c = std::clamp(glm::dot(a, b) / (la * lb), -1.0f, 1.0f);
    return glm::degrees(std::acos(c));
}

void MeasureTool::draw() const {
    const std::uint32_t color = core::debug::rgba8(255, 220, 60);
    for (const glm::vec3& p : points_) {
        constexpr float r = 0.08f;
        core::debug::line(p - glm::vec3(r, 0, 0), p + glm::vec3(r, 0, 0), color);
        core::debug::line(p - glm::vec3(0, r, 0), p + glm::vec3(0, r, 0), color);
        core::debug::line(p - glm::vec3(0, 0, r), p + glm::vec3(0, 0, r), color);
    }
    for (std::size_t i = 0; i + 1 < points_.size(); ++i) {
        core::debug::line(points_[i], points_[i + 1], color);
    }
}

} // namespace f3d::editor
