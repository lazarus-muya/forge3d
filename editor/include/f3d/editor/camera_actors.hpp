// SPDX-License-Identifier: Apache-2.0
#pragma once

// Scene cameras in the editor, the way Unreal shows camera actors: a body and a
// frustum drawn in the viewport, a picture-in-picture preview of the selected
// (or pinned) camera, and piloting, where the viewport looks through a camera
// and moving the view moves the camera (one undo step per move).

#include "f3d/core/guid.hpp"
#include "f3d/core/math.hpp"
#include "f3d/editor/camera.hpp"
#include "f3d/render/view.hpp"
#include "f3d/scene/components.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace f3d::scene {
class World;
} // namespace f3d::scene

namespace f3d::editor {

class SceneEditor;

/// The view through a scene camera whose world matrix is `world` (it looks down local -Z),
/// for a `width` x `height` pixel target. Scale in `world` is ignored.
[[nodiscard]] render::View scene_camera_view(const glm::mat4& world, const scene::Camera& camera,
                                             std::uint32_t width, std::uint32_t height);

/// Points the editor camera along a camera matrix: same eye position and look direction
/// (roll is dropped: the editor camera has none). Keeps `camera.distance`.
void align_editor_camera(EditorCamera& camera, const glm::mat4& world);

/// The world matrix (no scale) of the editor camera's eye.
[[nodiscard]] glm::mat4 editor_camera_matrix(const EditorCamera& camera);

class CameraActors {
public:
    CameraActors(scene::World& world, SceneEditor& editor);

    /// True if `entity` exists and has a Camera component.
    [[nodiscard]] bool is_camera(const core::Guid& entity) const;
    /// The active selection if it is a camera, else null.
    [[nodiscard]] core::Guid selected_camera() const;
    /// The entity's name (empty if it does not exist).
    [[nodiscard]] std::string name(const core::Guid& entity) const;

    /// The camera the viewport previews: the active selection if it is a camera, else
    /// the pinned camera; null when there is none. A piloted camera stays previewed, so
    /// its frame (aspect, lens, no editor helpers) follows the walk or flight live.
    [[nodiscard]] core::Guid preview_camera() const;
    /// The view through `entity`'s camera at this size; none if it is not a camera.
    [[nodiscard]] std::optional<render::View> view(const core::Guid& entity, std::uint32_t width,
                                                   std::uint32_t height) const;
    /// The camera's frame aspect (width / height).
    [[nodiscard]] float aspect(const core::Guid& entity) const;

    // --- Pinning: keep a camera's preview up after it is deselected ----------------------

    [[nodiscard]] const core::Guid& pinned() const noexcept { return pinned_; }
    void toggle_pin(const core::Guid& entity);

    // --- Piloting (Unreal: Pilot / Eject) ------------------------------------------------

    [[nodiscard]] const core::Guid& piloted() const noexcept { return piloted_; }
    /// Looks through `entity` (field of view and projection too); later view moves move it.
    bool pilot(const core::Guid& entity, EditorCamera& camera);
    /// Stops piloting and gives the editor camera back its own lens.
    void eject(EditorCamera& camera);
    /// Call once per frame after navigation: moves the piloted camera with the view, or
    /// the view with the camera when something else (gizmo, undo, inspector) moved it.
    void update(EditorCamera& camera);

    // --- Editing -------------------------------------------------------------------------

    /// Creates "Camera N" at the editor camera's eye, looking where it looks; selects it.
    core::Guid create_at_view(const EditorCamera& camera);
    /// Moves `entity` to the editor camera's eye and direction (Unreal: Snap Object to View).
    bool snap_to_view(const core::Guid& entity, const EditorCamera& camera);

    /// Draws every camera's body and frustum (debug lines); selected cameras are tinted.
    void draw_gizmos();

    /// The camera whose body is nearest the pixel (x, y) of `view`, within `radius`
    /// pixels; null when none is.
    [[nodiscard]] core::Guid pick(const render::View& view, float x, float y, float radius) const;
    /// Cameras whose position projects into the pixel box [x0, x1) x [y0, y1) of `view`.
    [[nodiscard]] std::vector<core::Guid> pick_region(const render::View& view, float x0, float y0, float x1,
                                                      float y1) const;

private:
    /// Writes the editor camera's eye into the piloted camera's transform.
    void write_pilot_transform(const EditorCamera& camera);

    scene::World& world_;
    SceneEditor& editor_;
    core::Guid pinned_;
    core::Guid piloted_;
    /// The editor camera's own lens, restored on eject.
    Projection saved_projection_ = Projection::Perspective;
    float saved_fov_ = 0.0f;
    float saved_near_ = 0.0f;
    float saved_far_ = 0.0f;
    glm::mat4 last_view_{1.0f};  ///< editor view matrix when the pilot was last in step
    glm::mat4 last_world_{1.0f}; ///< piloted camera's world matrix then
    bool pilot_moving_ = false;  ///< a pilot move's undo transaction is open
};

} // namespace f3d::editor
