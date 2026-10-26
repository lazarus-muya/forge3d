// SPDX-License-Identifier: Apache-2.0
#include "f3d/core/debug_draw.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <numbers>

namespace f3d::core::debug {
namespace {

struct Lines {
    std::mutex mutex;
    std::vector<LineVertex> vertices;
};

Lines& lines() {
    static Lines l;
    return l;
}

std::uint8_t to_byte(float v) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

} // namespace

std::uint32_t rgba(float r, float g, float b, float a) noexcept {
    return rgba8(to_byte(r), to_byte(g), to_byte(b), to_byte(a));
}

void line(const glm::vec3& a, const glm::vec3& b, std::uint32_t color) {
    Lines& l = lines();
    std::scoped_lock lock(l.mutex);
    l.vertices.push_back({a, color});
    l.vertices.push_back({b, color});
}

void box(const glm::vec3& min, const glm::vec3& max, std::uint32_t color) {
    const glm::vec3 c = (min + max) * 0.5f;
    box(glm::translate(glm::mat4(1.0f), c), (max - min) * 0.5f, color);
}

void box(const glm::mat4& transform, const glm::vec3& half_extents, std::uint32_t color) {
    std::array<glm::vec3, 8> corners;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 local((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
        corners[static_cast<std::size_t>(i)] = glm::vec3(transform * glm::vec4(local * half_extents, 1.0f));
    }
    // Edges join corners that differ in exactly one bit.
    Lines& l = lines();
    std::scoped_lock lock(l.mutex);
    for (int i = 0; i < 8; ++i) {
        for (int bit = 1; bit < 8; bit <<= 1) {
            if ((i & bit) == 0) {
                l.vertices.push_back({corners[static_cast<std::size_t>(i)], color});
                l.vertices.push_back({corners[static_cast<std::size_t>(i | bit)], color});
            }
        }
    }
}

void sphere(const glm::vec3& center, float radius, std::uint32_t color, int segments) {
    segments = std::max(segments, 4);
    Lines& l = lines();
    std::scoped_lock lock(l.mutex);
    const float step = 2.0f * std::numbers::pi_v<float> / static_cast<float>(segments);
    for (int axis = 0; axis < 3; ++axis) {
        for (int i = 0; i < segments; ++i) {
            const float a0 = step * static_cast<float>(i);
            const float a1 = step * static_cast<float>(i + 1);
            const auto point = [&](float a) {
                const float c = std::cos(a) * radius;
                const float s = std::sin(a) * radius;
                switch (axis) {
                case 0:
                    return center + glm::vec3(0.0f, c, s); // YZ plane
                case 1:
                    return center + glm::vec3(c, 0.0f, s); // XZ plane
                default:
                    return center + glm::vec3(c, s, 0.0f); // XY plane
                }
            };
            l.vertices.push_back({point(a0), color});
            l.vertices.push_back({point(a1), color});
        }
    }
}

std::vector<LineVertex> take_lines() {
    Lines& l = lines();
    std::scoped_lock lock(l.mutex);
    std::vector<LineVertex> out;
    out.swap(l.vertices);
    return out;
}

std::size_t pending_vertex_count() {
    Lines& l = lines();
    std::scoped_lock lock(l.mutex);
    return l.vertices.size();
}

} // namespace f3d::core::debug
