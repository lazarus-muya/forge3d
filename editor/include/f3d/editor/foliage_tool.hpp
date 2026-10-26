// SPDX-License-Identifier: Apache-2.0
#pragma once

// The foliage brush (phases.md P7.1): a left drag over the ground paints density into
// the target scatter (more trees, grass, rocks there) or, with erase, removes it. Live
// while dragging (world::ScatterPaintRecorder); one scatter.paint_stroke per stroke.

#include "f3d/core/guid.hpp"
#include "f3d/editor/viewport_tool.hpp"
#include "f3d/terrain/sculpt.hpp"
#include "f3d/world/scatter_paint.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace f3d::ops {
class OpStack;
}

namespace f3d::editor {

class FoliageTool final : public ViewportTool {
public:
    FoliageTool(scene::World& world, ops::OpStack& stack) noexcept : world_(world), stack_(stack) {
        brush.radius = 12.0f;
        brush.strength = 0.6f;
    }
    ~FoliageTool() override;
    FoliageTool(const FoliageTool&) = delete;
    FoliageTool& operator=(const FoliageTool&) = delete;

    terrain::Brush brush; ///< radius, strength, falloff, spacing
    bool erase_instances = false;
    /// The scatter painted; null: the selected scatter.
    core::Guid target;

    void update(const glm::vec3& origin, const glm::vec3& direction, bool pressed, float pressure) override;
    void leave() override;
    void cancel() override;
    [[nodiscard]] bool busy() const override { return stroke_ != nullptr; }
    void draw() const override;

    /// The scatter a stroke would paint now (target, else the selected scatter), or null.
    [[nodiscard]] core::Guid current_scatter() const;
    [[nodiscard]] std::uint64_t strokes() const noexcept { return strokes_; }

private:
    void end();

    scene::World& world_;
    ops::OpStack& stack_;
    std::unique_ptr<world::ScatterPaintRecorder> stroke_;
    core::Guid scatter_;
    bool created_mask_ = false;
    std::optional<glm::vec3> cursor_;
    std::uint64_t strokes_ = 0;
};

} // namespace f3d::editor
