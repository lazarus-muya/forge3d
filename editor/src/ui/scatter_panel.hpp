// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Scatter panel: create a scatter (trees, grass, rocks, or the selected objects'
// meshes) and edit what the inspector can't: the surface it stands on and its meshes.
// The other settings are Scatter fields in the inspector.

#include "f3d/core/guid.hpp"
#include "panel.hpp"

#include <functional>
#include <string>

namespace f3d::editor {

class SceneEditor;
class FoliageTool;
class LandscapeTools;

namespace panel_ids {
inline constexpr const char* kScatter = "scatter";
} // namespace panel_ids

/// What the panel asks the app to do.
struct ScatterActions {
    /// Creates a scatter from a preset ("trees", "grass", "rocks") or from the selected
    /// objects' meshes ("selected"), and selects it; one undo step.
    std::function<void(const std::string& preset)> create;
};

class ScatterPanel final : public Panel {
public:
    /// `foliage` and `tools`: the scatter paint brush (optional).
    ScatterPanel(SceneEditor* scene_editor, ScatterActions actions = {}, FoliageTool* foliage = nullptr,
                 LandscapeTools* tools = nullptr);

protected:
    void draw_contents() override;

private:
    void draw_create();
    void draw_active();
    void draw_paint(const core::Guid& scatter, bool paint_only);

    SceneEditor* scene_editor_ = nullptr;
    ScatterActions actions_;
    FoliageTool* foliage_ = nullptr;
    LandscapeTools* tools_ = nullptr;
};

} // namespace f3d::editor
