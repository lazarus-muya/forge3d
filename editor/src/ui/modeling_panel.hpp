// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Modeling panel (Tools > Modeling): add shapes, change an unedited shape's
// parameters, pick faces and run the edits (extrude, inset, delete, flip, subdivide,
// mirror, merge, origin, shading) on the selected EditableMesh.

#include "f3d/core/guid.hpp"
#include "f3d/modeling/editable_mesh.hpp"
#include "panel.hpp"

#include <functional>

namespace f3d::editor {

class LandscapeTools;
class ModelingTool;

namespace panel_ids {
inline constexpr const char* kModeling = "modeling";
} // namespace panel_ids

/// What the panel asks the app to do.
struct ModelingActions {
    /// Adds a shape at the 3D cursor and selects it (one undo step).
    std::function<void(modeling::Shape)> add_shape;
    /// Turns the selected object's mesh into an editable mesh (one undo step).
    std::function<void()> make_editable;
};

class ModelingPanel final : public Panel {
public:
    ModelingPanel(ModelingTool* tool, LandscapeTools* tools, ModelingActions actions);

protected:
    void draw_contents() override;

private:
    void draw_shape(const modeling::EditableMesh& mesh);
    void draw_faces(const modeling::EditableMesh& mesh);
    void draw_mesh_tools(const modeling::EditableMesh& mesh);
    /// The last extrude, inset or loop cut's values, re-run when they change.
    void draw_adjust_last();

    ModelingTool* tool_ = nullptr;
    LandscapeTools* tools_ = nullptr;
    ModelingActions actions_;
    modeling::ShapeParams editing_{};  ///< the shape parameters being edited
    core::Guid editing_target_;        ///< whose parameters editing_ holds
    bool shape_pending_ = false;       ///< editing_ changed and is applied when the widget is released
    float smoothing_ = 30.0f;          ///< the smoothing slider's value while it is dragged
};

} // namespace f3d::editor
