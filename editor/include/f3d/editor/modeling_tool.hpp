// SPDX-License-Identifier: Apache-2.0
#pragma once

// Face editing of the selected EditableMesh (f3d/modeling): a viewport tool that picks
// faces (click: select, Shift+click: add or remove), and the edit actions the Modeling
// panel and the Tools menu run on the picked faces (or the whole mesh) as modeling.*
// operations.
//
// Interactive edits (Blender's modal tools) while the tool is active: E extrudes the
// picked faces and I insets them, following the mouse; Ctrl+R loop-cuts the edge ring
// under the cursor (the wheel sets the number of cuts). Typing a number sets the value
// exactly, Ctrl snaps; a click or Enter confirms (one undo step), a right-click or
// Escape cancels. The last edit can then be adjusted (adjust_last()): it is undone and
// run again with the new value.

#include "f3d/core/guid.hpp"
#include "f3d/editor/viewport_tool.hpp"
#include "f3d/modeling/editable_mesh.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace f3d::ops {
class OpRegistry;
class OpStack;
} // namespace f3d::ops

namespace f3d::editor {

class ModelingTool final : public ViewportTool {
public:
    /// An interactive or adjustable edit.
    enum class Edit : std::uint8_t { None, Extrude, Inset, LoopCut };

    /// The last edit run on the picked faces, as adjust_last() re-runs it.
    struct LastEdit {
        Edit kind = Edit::None;
        std::vector<std::uint32_t> faces; ///< the picked faces before it ran
        float value = 0.0f;               ///< extrude distance, inset amount, loop cut factor
        std::uint32_t cuts = 1;           ///< loop cut
        std::uint32_t a = 0, b = 0;       ///< loop cut edge
    };

    ModelingTool(scene::World& world, ops::OpStack& stack, const ops::OpRegistry& registry);

    /// The mesh being edited: the selection's primary entity if it has an EditableMesh,
    /// else none. Changing it clears the face selection.
    void set_target(const core::Guid& entity);
    [[nodiscard]] const core::Guid& target() const noexcept { return target_; }
    /// The target's mesh, or null.
    [[nodiscard]] const modeling::EditableMesh* mesh() const;

    [[nodiscard]] const std::vector<std::uint32_t>& faces() const noexcept { return faces_; }
    void select_all_faces();
    void clear_faces() { faces_.clear(); }

    // ViewportTool
    void update(const glm::vec3& origin, const glm::vec3& direction, bool pressed, float pressure) override;
    void leave() override;
    /// Cancels the interactive edit in progress, else clears the picked faces.
    void cancel() override;
    bool erase() override { return modal_ == Edit::None && delete_faces(); }
    bool key_pressed(const std::string& key, bool ctrl, bool shift, bool alt) override;
    bool wheel(float notches) override;
    [[nodiscard]] bool busy() const override { return modal_ != Edit::None; }
    [[nodiscard]] std::string status() const override;
    void draw() const override;

    // Interactive edits (the viewport's E, I, Ctrl+R). False if they cannot start.
    bool begin_extrude();
    bool begin_inset();
    bool begin_loop_cut();
    [[nodiscard]] Edit modal() const noexcept { return modal_; }
    /// The value the interactive edit would apply now.
    [[nodiscard]] float modal_value() const noexcept { return modal_value_; }
    /// Confirms the interactive edit (as a click does). False if none or it failed.
    bool confirm();

    // Edits (one undo step each). False if there is nothing to edit or the operation failed.
    bool set_shape(const modeling::ShapeParams& params);
    bool extrude(float distance);
    bool inset(float amount);
    /// Loop cut across the edge (a, b) (position indices).
    bool loop_cut(std::uint32_t a, std::uint32_t b, std::uint32_t cuts, float factor);
    bool delete_faces();
    bool flip();
    bool subdivide(std::uint32_t levels);
    bool mirror(int axis);
    bool merge_vertices(float distance);
    bool set_origin(const char* mode);
    bool set_smoothing(float degrees);

    /// The last extrude, inset or loop cut if it is still the newest history entry of the
    /// target (nothing undone or done since), else null.
    [[nodiscard]] const LastEdit* last_edit() const;
    /// Undoes the last edit and runs it again with `value` (and `cuts` for a loop cut).
    bool adjust_last(float value, std::uint32_t cuts = 1);

    // Settings of the edits (the Modeling panel edits them).
    float extrude_distance = 0.25f;
    float inset_amount = 0.2f;
    std::uint32_t loop_cuts = 1;
    std::uint32_t subdivide_levels = 1;
    float merge_distance = 0.001f;

private:
    bool run(const char* id, nlohmann::json params, bool keep_faces);
    /// Runs `edit` (from faces_), remembering it for adjust_last().
    bool run_edit(const LastEdit& edit);
    /// Drops face indices past the mesh's face count (after undo).
    void prune();
    /// Puts `mesh` in the target's component without an operation (a modal preview).
    void show(const modeling::EditableMesh& mesh);
    void end_modal();
    /// The interactive edit's value for a cursor ray (world space).
    void track(const glm::vec3& origin, const glm::vec3& direction);
    /// The loop cut edge and factor under a ray (local space), from the hovered face.
    void track_loop_cut(const glm::vec3& local_origin, const glm::vec3& local_direction);

    scene::World& world_;
    ops::OpStack& stack_;
    const ops::OpRegistry& registry_;
    core::Guid target_;
    std::vector<std::uint32_t> faces_;
    std::optional<std::uint32_t> hovered_;
    bool was_pressed_ = false;

    // The interactive edit.
    Edit modal_ = Edit::None;
    modeling::EditableMesh original_; ///< the mesh before the preview
    glm::vec3 pivot_{0.0f};           ///< world: the picked faces' centre
    glm::vec3 axis_{0.0f, 1.0f, 0.0f}; ///< world: their averaged normal
    float local_scale_ = 1.0f;        ///< local units per world unit along the axis
    float span_ = 1.0f;               ///< world: the picked faces' mean distance from the pivot
    bool shown_ = false;              ///< a preview is in the component (original_ is restored on end)
    std::optional<float> reference_;  ///< the first frame's measure (distance along / from the axis)
    float modal_value_ = 0.0f;
    std::string typed_;               ///< a number typed during the edit
    std::optional<std::pair<std::uint32_t, std::uint32_t>> cut_edge_;

    std::optional<LastEdit> last_;
    std::uint64_t last_state_ = 0; ///< OpStack::state_id() after the last edit
    core::Guid last_target_;
};

} // namespace f3d::editor
