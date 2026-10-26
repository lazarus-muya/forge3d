// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/core/math.hpp"

namespace f3d::editor {

enum class Projection { Perspective, Orthographic };

/// Axis-aligned views (Blender numpad / Unreal Alt+H/J/K).
enum class ViewPreset { Front, Back, Right, Left, Top, Bottom };

/// The editor's orbit camera: a pivot (`target`), a distance and an orientation
/// (yaw around +Y, then pitch around the camera's X axis; pitch < 0 looks down).
/// An unrotated camera looks down -Z.
class EditorCamera {
public:
    glm::vec3 target{0.0f};
    float distance = 12.0f;
    float yaw = glm::radians(45.0f);
    float pitch = glm::radians(-30.0f);
    Projection projection = Projection::Perspective;
    float fov_y = glm::radians(50.0f);
    float near_plane = 0.05f;
    float far_plane = 5000.0f;

    [[nodiscard]] glm::quat orientation() const noexcept;
    [[nodiscard]] glm::vec3 forward() const noexcept;
    [[nodiscard]] glm::vec3 right() const noexcept;
    [[nodiscard]] glm::vec3 up() const noexcept;
    [[nodiscard]] glm::vec3 position() const noexcept { return target - forward() * distance; }

    [[nodiscard]] glm::mat4 view_matrix() const noexcept;
    [[nodiscard]] glm::mat4 projection_matrix(float aspect) const noexcept;
    /// Height of the orthographic view volume (matches the perspective framing at `target`).
    [[nodiscard]] float ortho_height() const noexcept;
    /// World units per pixel at the target distance, for a viewport `viewport_height` pixels high.
    [[nodiscard]] float units_per_pixel(float viewport_height) const noexcept;

    // --- Navigation primitives (angles in radians, distances in world units) ---
    /// Turntable orbit around the target. Pitch is clamped to [-90°, 90°].
    void orbit(float delta_yaw, float delta_pitch) noexcept;
    /// Moves the target in the view plane.
    void pan(float right_amount, float up_amount) noexcept;
    /// Multiplies the distance (< 1 zooms in). Clamped to a small minimum.
    void zoom(float factor) noexcept;
    /// Rotates the view around the camera position (first-person look); the target follows.
    void look(float delta_yaw, float delta_pitch) noexcept;
    /// Moves camera and target together along camera axes (fly mode).
    void move(const glm::vec3& camera_space_delta) noexcept;

    /// Frames a bounding box: target at its centre, distance so it fits the view.
    void frame(const core::Bounds& bounds) noexcept;
    void set_view(ViewPreset preset) noexcept;

    static constexpr float kMinDistance = 0.01f;
};

} // namespace f3d::editor
