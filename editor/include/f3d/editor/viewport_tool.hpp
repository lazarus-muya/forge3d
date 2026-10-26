// SPDX-License-Identifier: Apache-2.0
#pragma once

// Viewport tools of the Landscape panel: sculpt, paint, foliage, spline and water
// tools. While one is enabled, the viewport sends it the cursor ray every frame
// instead of selecting (Alt+drag still navigates). At most one is enabled at a time
// (LandscapeTools).

#include "f3d/core/math.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace f3d::scene {
class World;
}

namespace f3d::editor {

class ViewportTool {
public:
    virtual ~ViewportTool() = default;

    bool enabled = false;
    /// Input comes from a replay (benchmarks), not from the viewport.
    bool scripted = false;

    /// One frame of input: the cursor ray (world), whether the left button is held, and
    /// the pen pressure (1 for a mouse).
    virtual void update(const glm::vec3& origin, const glm::vec3& direction, bool pressed, float pressure) = 0;
    /// The cursor left the viewport: ends what is in progress and hides the cursor.
    virtual void leave() = 0;
    /// Escape: drops what is in progress.
    virtual void cancel() = 0;
    /// Delete / Backspace (spline points). True if it was used.
    virtual bool erase() { return false; }
    /// A key pressed over the viewport (SDL key name, lower case: "e", "keypad 5", "return"),
    /// before the editor's shortcuts. True if the tool used it.
    virtual bool key_pressed(const std::string& key, bool ctrl, bool shift, bool alt) {
        static_cast<void>(key), static_cast<void>(ctrl), static_cast<void>(shift), static_cast<void>(alt);
        return false;
    }
    /// Mouse wheel notches over the viewport while the tool is busy. True if used (no zoom).
    virtual bool wheel(float notches) {
        static_cast<void>(notches);
        return false;
    }
    /// A line of text for the viewport (what a modal edit is doing); empty: nothing.
    [[nodiscard]] virtual std::string status() const { return {}; }
    /// True while a stroke or drag is in progress.
    [[nodiscard]] virtual bool busy() const = 0;
    /// Queues the tool's cursor and handles (debug lines).
    virtual void draw() const = 0;
};

/// The viewport tools, at most one enabled. Enabling one (its `enabled` flag, from a
/// panel or a command) turns the previously enabled one off at the next sync().
class LandscapeTools {
public:
    void add(ViewportTool* tool) { tools_.push_back(tool); }
    /// Enforces "at most one" (the most recently enabled wins) and returns it, or null.
    ViewportTool* sync();
    /// The enabled tool as of the last sync(), or null.
    [[nodiscard]] ViewportTool* active() const noexcept { return active_; }
    /// Enables `tool` (null: none) and disables the others.
    void activate(ViewportTool* tool);
    /// Enables `tool`, or disables it if it is the active one.
    void toggle(ViewportTool* tool);

private:
    std::vector<ViewportTool*> tools_;
    ViewportTool* active_ = nullptr;
};

/// A brush ring draped over the terrain under `centre` (world): the rim and a dashed
/// half-radius ring, plus a short tick upwards. Nothing if no terrain is there.
void draw_brush_ring(scene::World& world, const glm::vec3& centre, float radius, std::uint32_t color);

} // namespace f3d::editor
