// SPDX-License-Identifier: Apache-2.0
#pragma once

// The transform gizmo's logic (phases.md P2.4), independent of ImGuizmo so it
// can be unit tested: pivot modes, local/world orientation, and turning a drag
// into exactly one undoable transaction of entity.set_property operations.

#include "f3d/core/guid.hpp"
#include "f3d/core/math.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace f3d::scene {
class World;
} // namespace f3d::scene

namespace f3d::ops {
class OpStack;
class OpRegistry;
} // namespace f3d::ops

namespace f3d::editor {

enum class GizmoOperation { Translate, Rotate, Scale };
enum class GizmoSpace { World, Local };
/// What rotations and scales pivot around.
enum class PivotMode {
    Median,            ///< the average of the selected origins
    IndividualOrigins, ///< each entity around its own origin
    ActiveElement,     ///< the active (last-clicked) entity's origin
    Cursor,            ///< the 3D cursor
};
/// Where a translate drag snaps to, besides the grid increment.
enum class SnapTarget { None, Surface, Vertex };

struct SnapSettings {
    bool grid = false;           ///< increments below apply (the gizmo snaps while dragging)
    float translate_step = 0.5f; ///< metres
    float rotate_step = 15.0f;   ///< degrees
    float scale_step = 0.1f;     ///< factor
    SnapTarget target = SnapTarget::None;
    float vertex_radius_px = 12.0f; ///< how far from the cursor a vertex snaps
};

[[nodiscard]] std::string_view to_string(GizmoOperation op) noexcept;
[[nodiscard]] std::string_view to_string(GizmoSpace space) noexcept;
[[nodiscard]] std::string_view to_string(PivotMode mode) noexcept;
[[nodiscard]] std::string_view to_string(SnapTarget target) noexcept;

class TransformTool {
public:
    TransformTool(scene::World& world, ops::OpStack& stack, const ops::OpRegistry& registry);

    GizmoOperation operation = GizmoOperation::Translate;
    GizmoSpace space = GizmoSpace::World;
    PivotMode pivot = PivotMode::Median;
    SnapSettings snap;
    glm::vec3 cursor{0.0f}; ///< the 3D cursor (pivot mode Cursor)

    /// The selected entities the tool moves: existing ones whose ancestors are
    /// not also selected (moving a parent already moves its children).
    [[nodiscard]] std::vector<core::Guid> targets() const;

    /// Where the gizmo is drawn: the pivot position, oriented by the active
    /// entity in local space (identity in world space), unit scale. Empty when
    /// nothing is selected.
    [[nodiscard]] std::optional<glm::mat4> gizmo_matrix() const;

    /// Starts a drag: records the start state and opens a transaction.
    bool begin();
    /// Applies the manipulated gizmo matrix (the start gizmo with the drag's
    /// delta applied) to the targets, through entity.set_property.
    void update(const glm::mat4& gizmo);
    /// Commits the drag as one undo entry (nothing if nothing changed).
    void end();
    /// Abandons the drag and restores the start state.
    void cancel();

    [[nodiscard]] bool active() const noexcept { return active_; }
    /// The gizmo matrix the current drag started from.
    [[nodiscard]] const glm::mat4& start_gizmo() const noexcept { return start_gizmo_; }

private:
    struct Target {
        core::Guid guid;
        glm::mat4 start_world{1.0f};
        glm::mat4 parent_world{1.0f}; ///< constant during the drag (parents are never targets' descendants)
    };
    [[nodiscard]] glm::vec3 pivot_point(const std::vector<core::Guid>& guids) const;
    void set_property(const core::Guid& entity, const char* path, const glm::vec3& value);
    void set_property(const core::Guid& entity, const char* path, const glm::quat& value);

    scene::World& world_;
    ops::OpStack& stack_;
    const ops::OpRegistry& registry_;
    bool active_ = false;
    GizmoOperation drag_operation_ = GizmoOperation::Translate;
    glm::mat4 start_gizmo_{1.0f};
    std::vector<Target> drag_targets_;
};

} // namespace f3d::editor
