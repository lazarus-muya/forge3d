// SPDX-License-Identifier: Apache-2.0
#pragma once

// The spline tool (phases.md P7.4): draws and edits roads, paths, rivers and fences
// with clicks on the terrain.
//   - With no spline selected, the first click creates one (spline.create, of
//     `new_kind`) and selects it.
//   - A click on the ground adds a point at the end (or, near the curve, between the
//     two points around it).
//   - Dragging a point moves it over the ground (one undo step per drag).
//   - Delete removes the selected point (the last one deletes the spline).
//   - Escape drops the selection of a point, then leaves the tool.
// Every change is an operation (entity.set_property on Spline.points).

#include "f3d/core/guid.hpp"
#include "f3d/editor/viewport_tool.hpp"
#include "f3d/world/spline.hpp"

#include <optional>

namespace f3d::editor {

class SceneEditor;

class SplineTool final : public ViewportTool {
public:
    SplineTool(scene::World& world, SceneEditor& scene_editor) noexcept : world_(world), scene_editor_(scene_editor) {}

    /// What the first click makes when no spline is selected.
    world::SplineKind new_kind = world::SplineKind::Road;

    void update(const glm::vec3& origin, const glm::vec3& direction, bool pressed, float pressure) override;
    void leave() override;
    void cancel() override;
    bool erase() override;
    [[nodiscard]] bool busy() const override { return dragging_; }
    void draw() const override;

    /// The spline being edited (the selected entity, if it has a Spline), or null.
    [[nodiscard]] core::Guid current_spline() const;
    [[nodiscard]] int selected_point() const noexcept { return selected_; }
    /// Starts a new spline of `kind` with the next click (drops the current selection).
    void start_new(world::SplineKind kind);

private:
    /// Writes the spline's points (relative to its entity), one undo step unless `merge`.
    bool set_points(const core::Guid& spline, const std::vector<glm::vec3>& points, bool merge);

    scene::World& world_;
    SceneEditor& scene_editor_;
    std::optional<glm::vec3> cursor_; ///< ground under the cursor
    int hover_ = -1;                  ///< control point under the cursor
    int selected_ = -1;
    bool was_pressed_ = false;
    bool dragging_ = false;
    bool drag_moved_ = false;
};

} // namespace f3d::editor
