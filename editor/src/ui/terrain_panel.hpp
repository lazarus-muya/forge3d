// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Landscape panel (phases.md P6.1 - P7.4): everything for building a landscape in
// one place, a tab per job:
//   Sculpt   create a terrain; the sculpt brush; global erosion
//   Paint    material layers (textures, tiling, rules), auto-material presets, the paint brush
//   Foliage  scatter types on the terrain and the density / erase brush
//   Water    oceans and lakes (a click floods a basin)
//   Splines  roads, paths, rivers and fences drawn with clicks
// Each tab has a big tool switch; at most one viewport tool is on at a time.

#include "f3d/core/guid.hpp"
#include "f3d/terrain/erosion.hpp"
#include "f3d/terrain/terrain.hpp"
#include "panel.hpp"

#include <entt/entity/fwd.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace f3d::editor {

class SceneEditor;
class SculptTool;
class PaintTool;
class FoliageTool;
class SplineTool;
class WaterTool;
class LandscapeTools;
class ViewportTool;

namespace panel_ids {
inline constexpr const char* kTerrain = "terrain";
} // namespace panel_ids

/// The viewport tools the panel switches (any may be null).
struct LandscapeToolSet {
    SculptTool* sculpt = nullptr;
    PaintTool* paint = nullptr;
    FoliageTool* foliage = nullptr;
    SplineTool* spline = nullptr;
    WaterTool* water = nullptr;
    LandscapeTools* tools = nullptr; ///< keeps at most one on
};

/// What the panel asks the app to do.
struct TerrainActions {
    /// Erodes the selected terrain (or the first one) on a worker; one undo step when done.
    std::function<void(const terrain::ErosionSettings&)> erode;
    std::function<void()> cancel_erosion;
    /// The running erosion's progress (0..1), or none.
    std::function<std::optional<float>()> erosion_progress;
    /// Opens an image file dialog (several files allowed); `done` gets the chosen paths.
    std::function<void(std::function<void(std::vector<std::string>)> done)> pick_images;
    /// Adds a scatter preset ("trees", "grass", "rocks", "selected") on the terrain.
    std::function<void(const std::string& preset)> add_scatter;
};

class TerrainPanel final : public Panel {
public:
    TerrainPanel(SceneEditor* scene_editor, LandscapeToolSet tools, TerrainActions actions = {});

protected:
    void draw_contents() override;

private:
    /// The terrain the panel edits: the selected one, else the first; null if none.
    [[nodiscard]] entt::entity target_terrain() const;
    void draw_create();
    void draw_sculpt_tab();
    void draw_brush();
    void draw_erosion();
    void draw_paint_tab(entt::entity terrain);
    void draw_layer_list(entt::entity terrain, std::vector<terrain::TerrainLayer>& layers);
    void draw_layer_details(entt::entity terrain, std::vector<terrain::TerrainLayer>& layers);
    void draw_texture_slot(entt::entity terrain, std::vector<terrain::TerrainLayer>& layers, std::size_t index,
                           terrain::LayerTextureSlot slot, const char* label);
    void draw_foliage_tab(entt::entity terrain);
    void draw_water_tab(entt::entity terrain);
    void draw_spline_tab(entt::entity terrain);
    /// A large on/off button for a viewport tool.
    void tool_switch(const char* label, ViewportTool* tool, const char* tooltip);

    /// Replaces Terrain.layers (one undo step; `merge` folds a continuing drag into the last).
    void set_layers(entt::entity terrain, const std::vector<terrain::TerrainLayer>& layers, bool merge = false);
    /// Puts images into a layer's slots: each by its name (Normal, Roughness, ...), the
    /// rest of a set found next to a single colour map.
    void assign_images(entt::entity terrain, std::size_t layer, const std::vector<std::string>& paths,
                       std::optional<terrain::LayerTextureSlot> slot);

    SceneEditor* scene_editor_ = nullptr;
    LandscapeToolSet tools_;
    TerrainActions actions_;
    terrain::ErosionSettings erosion_;
    // terrain.create settings
    int tiles_ = 4;
    float size_ = 1024.0f;
    float max_height_ = 256.0f;
    int seed_ = 1;
    char stamp_path_[512] = {};
    // Paint tab
    int selected_layer_ = 0;
    char layer_name_[128] = {};
    int layer_name_for_ = -1; ///< layer whose name is in layer_name_
};

} // namespace f3d::editor
