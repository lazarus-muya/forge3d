// SPDX-License-Identifier: Apache-2.0
#pragma once

// The water tool (phases.md P7.3): a click on the terrain makes a lake there.
//   - The lake's level is the clicked ground plus `fill_depth`, so the basin around
//     the click floods up to that height (water.create, then selected).
//   - While hovering, a ring shows where the water would start, a tick its level, and
//     an outline the shore it would have (a coarse flood, redone when the cursor or
//     the depth changes).
//   - Escape leaves the tool.
// The lake is then edited like any entity (its height is the level) or from the
// Landscape panel's Water tab.

#include "f3d/core/guid.hpp"
#include "f3d/editor/viewport_tool.hpp"
#include "f3d/world/water.hpp"

#include <cstdint>
#include <optional>

namespace f3d::editor {

class SceneEditor;

class WaterTool final : public ViewportTool {
public:
    WaterTool(scene::World& world, SceneEditor& scene_editor) noexcept : world_(world), scene_editor_(scene_editor) {}

    /// Metres of water above the clicked ground.
    float fill_depth = 3.0f;

    void update(const glm::vec3& origin, const glm::vec3& direction, bool pressed, float pressure) override;
    void leave() override;
    void cancel() override;
    [[nodiscard]] bool busy() const override { return false; }
    void draw() const override;

    /// Makes a lake at `ground` (world) on `terrain` (null: the one below), level
    /// `ground.y + fill_depth`, and selects it. Returns its guid (null on failure).
    core::Guid place_lake(const glm::vec3& ground, const core::Guid& terrain);

private:
    scene::World& world_;
    SceneEditor& scene_editor_;
    /// Recomputes the preview flood if the cursor, the depth or the ground changed.
    void update_preview();

    std::optional<glm::vec3> cursor_; ///< terrain under the cursor
    core::Guid cursor_terrain_;
    bool was_pressed_ = false;
    // The preview: a coarse flood from preview_at_ (world), terrain-local cells.
    world::FloodResult preview_;
    glm::vec3 preview_at_{0.0f};
    glm::vec3 preview_origin_{0.0f}; ///< the terrain's origin (world)
    float preview_depth_ = -1.0f;
    std::uint64_t preview_revision_ = 0;
};

} // namespace f3d::editor
