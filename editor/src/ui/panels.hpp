// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/core/guid.hpp"
#include "f3d/editor/camera.hpp"
#include "f3d/editor/console_model.hpp"
#include "f3d/editor/measure_tool.hpp"
#include "f3d/editor/selection_tools.hpp"
#include "f3d/editor/transform_tool.hpp"
#include "f3d/editor/viewport_stats.hpp"
#include "f3d/render/raster/raster_engine.hpp"
#include "f3d/render/rhi/device.hpp"
#include "panel.hpp"

#include <entt/entity/entity.hpp>
#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace f3d::scene {
class World;
} // namespace f3d::scene

namespace f3d::assets {
class AssetDatabase;
} // namespace f3d::assets

namespace f3d::ops {
class OpStack;
} // namespace f3d::ops

namespace f3d::editor {

class SceneEditor;
class CommandRegistry;
class ImportQueue;

/// Called when the inspector edits a field: (entity, "<Component>.<field>", value,
/// continuing). `continuing` edits merge into the previous undo step (a drag).
using InspectorEditCallback =
    std::function<void(const core::Guid&, const std::string&, const nlohmann::json&, bool continuing)>;

/// A selection request from the viewport, in viewport pixels. The app resolves
/// it with the ID buffer (picking) and turns it into a selection.set operation.
struct ViewportPick {
    std::uint32_t x0 = 0; ///< [x0, x1) x [y0, y1); a click is a 1x1 box
    std::uint32_t y0 = 0;
    std::uint32_t x1 = 0;
    std::uint32_t y1 = 0;
    bool box = false;
    SelectMode mode = SelectMode::Replace;
};

class LandscapeTools;
class ViewportTool;
class CameraActors;
class NavigationController;

/// The viewport's hooks into the app (selection, gizmo, snapping).
struct ViewportTools {
    TransformTool* transform = nullptr;
    std::function<void(const ViewportPick&)> on_pick;
    /// Surface / vertex snapping while translating: the point to move the pivot
    /// to for the cursor ray, or none. `max_angle`: the vertex snap radius.
    std::function<std::optional<glm::vec3>(const glm::vec3& origin, const glm::vec3& direction, float max_angle)>
        snap_point;
    /// Where a ray meets the scene (or the ground plane): the 3D cursor (Shift +
    /// right-click) and the measure tool's points.
    std::function<std::optional<glm::vec3>(const glm::vec3& origin, const glm::vec3& direction)> surface_point;
    /// The measure tool; while `*measuring`, left clicks place its points instead of selecting.
    MeasureTool* measure = nullptr;
    bool* measuring = nullptr;
    /// The landscape tools (sculpt, paint, foliage, splines, water); while one is enabled,
    /// left drags go to it instead of selecting.
    LandscapeTools* landscape = nullptr;
    const float* pen_pressure = nullptr; ///< current pen pressure (1 for a mouse)
    /// An asset dropped on the viewport (from the asset browser), with the ray under the cursor.
    std::function<void(const core::Guid& asset, const glm::vec3& origin, const glm::vec3& direction)> on_asset_drop;
    /// Scene cameras: the preview of the selected / pinned camera, pin, pilot and eject.
    CameraActors* cameras = nullptr;
    /// Renders the camera preview (a renderer of its own, created on first use).
    std::function<render::raster::RasterEngine*()> preview_renderer;
    /// The camera speed shown in the viewport's camera menu.
    NavigationController* navigation = nullptr;
};

namespace panel_ids {
inline constexpr const char* kViewport = "viewport";
inline constexpr const char* kOutliner = "outliner";
inline constexpr const char* kInspector = "inspector";
inline constexpr const char* kConsole = "console";
inline constexpr const char* kOperationLog = "operation_log";
inline constexpr const char* kImportQueue = "import_queue";
inline constexpr const char* kResidency = "residency";
} // namespace panel_ids

/// The 3D view: the render engine's output texture, resized to the panel, shown
/// with ImGui::Image, with a corner axis widget overlaid.
class ViewportPanel final : public Panel {
public:
    ViewportPanel(render::rhi::Device& device, render::raster::RasterEngine& renderer, render::RenderScene scene,
                  EditorCamera& camera, ViewportTools tools = {});

    /// Renders the scene into the viewport target (call after the UI was built this frame).
    void render(render::rhi::CommandContext& ctx);

    /// The displayed (sRGB-encoded RGBA8) image, as --capture writes it.
    [[nodiscard]] const render::rhi::Texture& display_target() const noexcept { return display_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    /// Size of the image in logical pixels (navigation uses it for pan speed).
    [[nodiscard]] glm::vec2 logical_size() const noexcept { return logical_size_; }
    /// True while the mouse is over the viewport image (navigation input goes to the camera).
    [[nodiscard]] bool hovered() const noexcept { return hovered_; }
    /// Text shown in the viewport's top-left corner (navigation preset, projection).
    void set_overlay_text(std::string text) { overlay_text_ = std::move(text); }
    /// Renders at exactly this size (shown scaled into the panel); 0x0: the panel's size.
    void set_forced_size(std::uint32_t width, std::uint32_t height) noexcept { forced_size_ = {width, height}; }
    /// Shows the stats overlay (frame rate, GPU passes, draws, VRAM) while `*show` is true.
    void set_stats_toggle(const bool* show) noexcept { show_stats_ = show; }
    /// The camera's view at the viewport's current size (picking, rays).
    [[nodiscard]] render::raster::View view() const;
    /// True while the transform gizmo is hovered or dragged.
    [[nodiscard]] bool gizmo_busy() const noexcept { return gizmo_busy_; }
    /// True while something a right-click cancels is in progress (Blender): a gizmo drag,
    /// a box selection, a tool's stroke or modal edit. The right button then cancels
    /// instead of navigating.
    [[nodiscard]] bool cancellable() const;
    /// The cursor ray through a point in ImGui screen coordinates; false if the point is
    /// not on the viewport image (as of the last frame).
    bool ray_at(const glm::vec2& screen, glm::vec3& origin, glm::vec3& direction) const;

protected:
    void draw_contents() override;
    [[nodiscard]] bool zero_padding() const override { return true; }

private:
    void resize(std::uint32_t width, std::uint32_t height);
    void draw_axis_widget(const glm::vec2& image_min, const glm::vec2& image_size) const;
    void draw_gizmo(const glm::vec2& image_min, const glm::vec2& image_size);
    void handle_selection_input(const glm::vec2& image_min, const glm::vec2& image_size);
    void draw_toolbar(const glm::vec2& image_min, const glm::vec2& image_size);
    void handle_measure_input(const glm::vec2& image_min, const glm::vec2& image_size);
    void handle_tool_input(const glm::vec2& image_min);
    [[nodiscard]] ViewportTool* active_tool() const noexcept;
    void draw_measure_labels(const glm::vec2& image_min, const glm::vec2& image_size) const;
    void draw_stats(const glm::vec2& image_min, const glm::vec2& image_size);
    void draw_camera_menu(const glm::vec2& image_min);
    void draw_camera_preview(const glm::vec2& image_min, const glm::vec2& image_size);
    [[nodiscard]] bool measuring() const noexcept { return tools_.measuring && *tools_.measuring && tools_.measure; }

    render::rhi::Device& device_;
    render::raster::RasterEngine& renderer_;
    render::RenderScene scene_;
    EditorCamera& camera_;
    render::rhi::Texture display_; ///< the engine's output (sRGB-encoded RGBA8)
    render::rhi::Texture preview_; ///< the camera preview (picture in picture)
    std::optional<render::raster::View> preview_view_; ///< rendered into `preview_` this frame
    const bool* show_stats_ = nullptr;
    glm::uvec2 forced_size_{0u, 0u};
    FrameTimeAverage frame_time_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    glm::vec2 logical_size_{0.0f};
    glm::vec2 image_min_{0.0f}; ///< the image's top left, ImGui screen coordinates
    bool hovered_ = false;
    std::string overlay_text_;
    ViewportTools tools_;
    glm::mat4 drag_gizmo_{1.0f}; ///< the gizmo matrix while dragging (fed back to ImGuizmo)
    bool gizmo_busy_ = false;
    bool drag_cancelled_ = false; ///< Escape pressed mid-drag: ignore the gizmo until release
    bool box_pending_ = false;    ///< left button went down on the image, not on the gizmo
    bool tool_drag_ = false;      ///< a landscape tool drag started on the image and the button is held
    glm::vec2 box_start_{0.0f};   ///< logical pixels, relative to the image
};

class ConsolePanel final : public Panel {
public:
    explicit ConsolePanel(std::shared_ptr<ConsoleModel> model);

protected:
    void draw_contents() override;

private:
    std::shared_ptr<ConsoleModel> model_;
    int min_level_ = 0; ///< index into the level combo (trace .. critical)
    std::array<char, 256> search_{};
    bool auto_scroll_ = true;
    std::uint64_t seen_version_ = 0;
};

/// Placeholder until the asset browser (P3.3).
/// Shows the reflected components of the active selected entity. Edits are
/// routed through `on_edit` (entity.set_property). `world` may be null.
class InspectorPanel final : public Panel {
public:
    /// `editor` (optional) adds/removes components through operations.
    InspectorPanel(scene::World* world, SceneEditor* editor, InspectorEditCallback on_edit);

protected:
    void draw_contents() override;

private:
    scene::World* world_ = nullptr;
    SceneEditor* editor_ = nullptr;
    InspectorEditCallback on_edit_;
};

/// Shows the undo history (most recent last) and the redo entries.
class OperationLogPanel final : public Panel {
public:
    explicit OperationLogPanel(ops::OpStack* stack);

protected:
    void draw_contents() override;

private:
    ops::OpStack* stack_ = nullptr;
};

/// The import queue (P3.2): progress of running imports, failures, controls.
class ImportQueuePanel final : public Panel {
public:
    ImportQueuePanel(ImportQueue* queue, const assets::AssetDatabase* database);

protected:
    void draw_contents() override;

private:
    ImportQueue* queue_ = nullptr;
    const assets::AssetDatabase* database_ = nullptr;
};

/// GPU residency of streamed assets (P3.5): VRAM used against the budget, and
/// per asset the resident vs wanted LOD / mip.
class ResidencyPanel final : public Panel {
public:
    explicit ResidencyPanel(const render::raster::ResidencyManager* residency);

protected:
    void draw_contents() override;

private:
    const render::raster::ResidencyManager* residency_ = nullptr;
};

} // namespace f3d::editor
