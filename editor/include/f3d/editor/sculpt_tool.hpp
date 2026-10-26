// SPDX-License-Identifier: Apache-2.0
#pragma once

// The terrain sculpt tool (phases.md P6.2). While enabled, a left drag over a terrain
// is one stroke: dabs are applied live (terrain::StrokeRecorder) and, on release, the
// stroke is committed as one terrain.sculpt_stroke operation (one undo step) that
// carries its points, so it can be replayed from a script. Input comes from the
// viewport (mouse or pen) or, in benchmarks, from a replay.

#include "f3d/core/guid.hpp"
#include "f3d/core/math.hpp"
#include "f3d/editor/viewport_tool.hpp"
#include "f3d/terrain/sculpt.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace f3d::core::vfs {
class FileSystem;
}
namespace f3d::scene {
class World;
}
namespace f3d::ops {
class OpStack;
} // namespace f3d::ops

namespace f3d::editor {

class SculptTool final : public ViewportTool {
public:
    SculptTool(scene::World& world, ops::OpStack& stack, const core::vfs::FileSystem* files) noexcept
        : world_(world), stack_(stack), files_(files) {}
    ~SculptTool() override;
    SculptTool(const SculptTool&) = delete;
    SculptTool& operator=(const SculptTool&) = delete;

    terrain::Brush brush;

    /// One frame of input: the cursor ray (world), whether the button is held, and the
    /// pen pressure (1 for a mouse). Starts, continues or ends the stroke.
    void update(const glm::vec3& origin, const glm::vec3& direction, bool pressed, float pressure) override;
    /// The cursor left the viewport: ends the stroke (if any) and hides the brush.
    void leave() override;
    /// Escape: puts the terrain back as it was before the stroke.
    void cancel() override;
    [[nodiscard]] bool stroking() const noexcept { return stroke_ != nullptr; }
    [[nodiscard]] bool busy() const override { return stroking(); }
    /// Where the brush is over a terrain (world), if anywhere.
    [[nodiscard]] const std::optional<glm::vec3>& cursor() const noexcept { return cursor_; }
    /// Strokes committed since the tool was created (tests, benchmarks).
    [[nodiscard]] std::uint64_t strokes() const noexcept { return strokes_; }

    /// Queues the brush outline (debug lines draped over the terrain).
    void draw() const override;

    /// Sets brush fields from JSON named like terrain.sculpt_stroke's parameters
    /// (tool, radius, strength, falloff, ...). False (and `error`) on unknown values.
    bool apply_json(const nlohmann::json& settings, std::string* error = nullptr);

private:
    void end();

    scene::World& world_;
    ops::OpStack& stack_;
    const core::vfs::FileSystem* files_;
    std::unique_ptr<terrain::StrokeRecorder> stroke_;
    core::Guid terrain_;               ///< the stroke's terrain
    glm::vec3 terrain_origin_{0.0f};   ///< its world origin
    std::optional<glm::vec3> cursor_;
    core::Guid cursor_terrain_;
    std::uint64_t strokes_ = 0;
    bool stamp_failed_ = false; ///< warned for this press already
};

} // namespace f3d::editor
