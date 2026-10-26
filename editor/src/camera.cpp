// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/camera.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace f3d::editor {

namespace {
constexpr float kHalfPi = std::numbers::pi_v<float> * 0.5f;
}

glm::quat EditorCamera::orientation() const noexcept {
    return glm::angleAxis(yaw, core::kWorldUp) * glm::angleAxis(pitch, core::kWorldRight);
}

glm::vec3 EditorCamera::forward() const noexcept {
    return orientation() * core::kWorldForward;
}
glm::vec3 EditorCamera::right() const noexcept {
    return orientation() * core::kWorldRight;
}
glm::vec3 EditorCamera::up() const noexcept {
    return orientation() * core::kWorldUp;
}

glm::mat4 EditorCamera::view_matrix() const noexcept {
    // Inverse of the camera's world transform (rotation, then translation to the position).
    const glm::mat4 world = glm::translate(glm::mat4(1.0f), position()) * glm::mat4_cast(orientation());
    return glm::inverse(world);
}

float EditorCamera::ortho_height() const noexcept {
    return 2.0f * distance * std::tan(fov_y * 0.5f);
}

glm::mat4 EditorCamera::projection_matrix(float aspect) const noexcept {
    aspect = std::max(aspect, 1e-4f);
    if (projection == Projection::Orthographic) {
        const float h = ortho_height() * 0.5f;
        const float w = h * aspect;
        // Keep the whole scene in front of the camera: the near plane is behind the eye.
        return glm::orthoRH_ZO(-w, w, -h, h, -far_plane * 0.5f, far_plane);
    }
    return glm::perspectiveRH_ZO(fov_y, aspect, near_plane, far_plane);
}

float EditorCamera::units_per_pixel(float viewport_height) const noexcept {
    return ortho_height() / std::max(viewport_height, 1.0f);
}

void EditorCamera::orbit(float delta_yaw, float delta_pitch) noexcept {
    yaw = std::remainder(yaw + delta_yaw, glm::two_pi<float>());
    pitch = std::clamp(pitch + delta_pitch, -kHalfPi, kHalfPi);
}

void EditorCamera::pan(float right_amount, float up_amount) noexcept {
    target += right() * right_amount + up() * up_amount;
}

void EditorCamera::zoom(float factor) noexcept {
    distance = std::max(distance * factor, kMinDistance);
}

void EditorCamera::look(float delta_yaw, float delta_pitch) noexcept {
    const glm::vec3 eye = position();
    orbit(delta_yaw, delta_pitch);
    target = eye + forward() * distance;
}

void EditorCamera::move(const glm::vec3& camera_space_delta) noexcept {
    target += right() * camera_space_delta.x + up() * camera_space_delta.y - forward() * camera_space_delta.z;
}

void EditorCamera::frame(const core::Bounds& bounds) noexcept {
    if (bounds.empty()) {
        return;
    }
    target = bounds.center();
    const float radius = std::max(glm::length(bounds.extent()) * 0.5f, 0.1f);
    distance = radius / std::sin(fov_y * 0.5f) * 1.1f;
}

void EditorCamera::set_view(ViewPreset preset) noexcept {
    switch (preset) {
    case ViewPreset::Front: // camera on +Z looking down -Z
        yaw = 0.0f;
        pitch = 0.0f;
        break;
    case ViewPreset::Back:
        yaw = glm::pi<float>();
        pitch = 0.0f;
        break;
    case ViewPreset::Right: // camera on +X looking down -X
        yaw = kHalfPi;
        pitch = 0.0f;
        break;
    case ViewPreset::Left:
        yaw = -kHalfPi;
        pitch = 0.0f;
        break;
    case ViewPreset::Top:
        yaw = 0.0f;
        pitch = -kHalfPi;
        break;
    case ViewPreset::Bottom:
        yaw = 0.0f;
        pitch = kHalfPi;
        break;
    }
}

} // namespace f3d::editor
