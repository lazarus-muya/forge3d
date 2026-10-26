// SPDX-License-Identifier: Apache-2.0
#pragma once

// The terrain layer paint tool (phases.md P6.4). While enabled, a left drag over a
// terrain paints (or erases) one material layer live (terrain::PaintRecorder); on
// release the stroke becomes one terrain.paint_stroke operation (one undo step).

#include "f3d/core/guid.hpp"
#include "f3d/editor/viewport_tool.hpp"
#include "f3d/terrain/layers.hpp"
#include "f3d/terrain/sculpt.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace f3d::ops {
class OpStack;
}

namespace f3d::editor {

class PaintTool final : public ViewportTool {
public:
    PaintTool(scene::World& world, ops::OpStack& stack) noexcept : world_(world), stack_(stack) {}
    ~PaintTool() override;
    PaintTool(const PaintTool&) = delete;
    PaintTool& operator=(const PaintTool&) = delete;

    terrain::Brush brush; ///< radius, strength, falloff, spacing
    std::uint32_t layer = 0;
    bool erase_layer = false; ///< take the layer away instead of painting it

    void update(const glm::vec3& origin, const glm::vec3& direction, bool pressed, float pressure) override;
    void leave() override;
    void cancel() override;
    [[nodiscard]] bool busy() const override { return stroke_ != nullptr; }
    void draw() const override;

    [[nodiscard]] const std::optional<glm::vec3>& cursor() const noexcept { return cursor_; }
    [[nodiscard]] std::uint64_t strokes() const noexcept { return strokes_; }

private:
    void end();

    scene::World& world_;
    ops::OpStack& stack_;
    std::unique_ptr<terrain::PaintRecorder> stroke_;
    core::Guid terrain_;
    glm::vec3 terrain_origin_{0.0f};
    std::optional<glm::vec3> cursor_;
    core::Guid cursor_terrain_;
    std::uint64_t strokes_ = 0;
};

} // namespace f3d::editor
