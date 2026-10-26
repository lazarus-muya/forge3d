// SPDX-License-Identifier: Apache-2.0
#pragma once

// forge3d math: GLM with the project's conventions.
//
//  - Right-handed, Y up. One unit is one metre.
//  - Clip-space depth 0..1 (use the *_ZO projection helpers), NDC y up.
//  - Matrices are column-major (GLM's layout = HLSL's default packing), so
//    shaders compute mul(M, v) with M uploaded as is.
//
// GLM_FORCE_INTRINSICS / GLM_ENABLE_EXPERIMENTAL are set for every target by f3d_core.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace f3d::core {

inline const glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};
inline const glm::vec3 kWorldRight{1.0f, 0.0f, 0.0f};
inline const glm::vec3 kWorldForward{0.0f, 0.0f, -1.0f}; ///< the direction an unrotated camera looks

/// Axis-aligned bounding box. Default-constructed boxes are empty (min > max).
struct Bounds {
    glm::vec3 min{1e30f};
    glm::vec3 max{-1e30f};

    [[nodiscard]] bool empty() const noexcept { return min.x > max.x || min.y > max.y || min.z > max.z; }
    [[nodiscard]] glm::vec3 center() const noexcept { return (min + max) * 0.5f; }
    [[nodiscard]] glm::vec3 extent() const noexcept { return max - min; }
    void add(const glm::vec3& p) noexcept {
        min = glm::min(min, p);
        max = glm::max(max, p);
    }
    void add(const Bounds& b) noexcept {
        if (!b.empty()) {
            add(b.min);
            add(b.max);
        }
    }
    /// The box enclosing this box transformed by `m` (empty stays empty).
    [[nodiscard]] Bounds transformed(const glm::mat4& m) const noexcept {
        if (empty()) {
            return {};
        }
        // Arvo's method: centre + absolute-value matrix times the half extent.
        const glm::vec3 c = glm::vec3(m * glm::vec4(center(), 1.0f));
        const glm::vec3 h = extent() * 0.5f;
        const glm::mat3 r(m);
        const glm::vec3 e = glm::abs(r[0]) * h.x + glm::abs(r[1]) * h.y + glm::abs(r[2]) * h.z;
        return {c - e, c + e};
    }
};

} // namespace f3d::core
