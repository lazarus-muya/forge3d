// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "asset_browser.hpp"
#include "f3d/editor/command_registry.hpp"
#include "f3d/editor/console_model.hpp"
#include "f3d/editor/preferences.hpp"
#include "f3d/render/rhi/device.hpp"
#include "material_panel.hpp"
#include "modeling_panel.hpp"
#include "panel.hpp"
#include "panels.hpp"
#include "python_panel.hpp"
#include "scatter_panel.hpp"
#include "scene_panels.hpp"
#include "terrain_panel.hpp"
#include "tool_panels.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace f3d::editor {

/// State and actions behind the View menu (owned by the App).
struct ViewMenu {
    std::vector<std::string> navigation_presets;
    std::string current_preset;
    std::function<void(const std::string&)> select_preset;
    std::function<void()> frame_scene;
    bool* show_grid = nullptr;
    bool* show_debug_shapes = nullptr;
    bool* show_stats = nullptr;                                ///< viewport stats overlay
    render::raster::RenderSettings* render_settings = nullptr; ///< view mode, tonemapper, exposure
    render::raster::ShadowSettings* shadow_settings = nullptr; ///< resolution (Scalability)
    std::function<void()> render_settings_changed;             ///< saves them to the preferences
};

struct EditorUiDesc {
    render::rhi::Device* device = nullptr;
    render::raster::RasterEngine* renderer = nullptr;
    EditorCamera* camera = nullptr;
    scene::World* world = nullptr;                               ///< for the inspector (shows the active selection)
    assets::MeshLibrary* meshes = nullptr;                       ///< what the viewport draws with the world
    render::raster::MaterialPreview* material_preview = nullptr; ///< the material editor's sphere
    MaterialEditorActions material_actions;
    MaterialLibraryActions material_library_actions;
    SceneEditor* scene_editor = nullptr;              ///< outliner, layers, bookmarks
    LandscapeToolSet landscape;                       ///< the Landscape panel's tools
    TerrainActions terrain_actions;                   ///< the Landscape panel's erosion and file dialogs
    ScatterActions scatter_actions;                   ///< the Scatter panel's presets
    ModelingTool* modeling_tool = nullptr;            ///< face editing (Modeling panel)
    ModelingActions modeling_actions;                 ///< the Modeling panel's shapes and Make Editable
    const assets::AssetDatabase* asset_db = nullptr;  ///< the project's assets (asset browser)
    assets::AssetIndex* const* asset_index = nullptr; ///< the project's index (changes with the project)
    ThumbnailCache* thumbnails = nullptr;
    AssetBrowserActions asset_actions;
    ImportQueue* import_queue = nullptr;                         ///< background imports
    const render::raster::ResidencyManager* residency = nullptr; ///< streamed assets (residency panel)
    ops::OpStack* op_stack = nullptr;                            ///< for undo/redo and the operation log
    InspectorEditCallback on_property_edit;
    ViewportTools viewport_tools; ///< selection, gizmo and snapping hooks
    std::shared_ptr<ConsoleModel> console;
    CommandRegistry* commands = nullptr;
    Preferences* preferences = nullptr;
    ViewMenu* view_menu = nullptr;
    PythonActions python;
    std::function<void(bool save)> on_preferences_changed; ///< save: also write the file
    std::function<void()> on_keymap_changed;
    bool build_default_layout = true;
};

/// The editor's ImGui shell: main menu, dockspace with the default layout, the
/// registered panels and the command palette.
class EditorUi {
public:
    explicit EditorUi(const EditorUiDesc& desc);

    /// Builds this frame's UI (between ImGuiLayer::begin_frame and render).
    void draw();

    /// Requests the default dock layout on the next frame (command editor.reset_layout).
    void reset_layout() noexcept { layout_requested_ = true; }
    [[nodiscard]] bool quit_requested() const noexcept { return quit_requested_; }
    void clear_quit_request() noexcept { quit_requested_ = false; }

    [[nodiscard]] ViewportPanel& viewport() noexcept { return *viewport_; }
    [[nodiscard]] KeymapPanel& keymap() noexcept { return *keymap_; }
    [[nodiscard]] PanelManager& panels() noexcept { return panels_; }

private:
    void register_commands();
    void draw_main_menu();
    void draw_view_menu();
    void draw_add_menu();
    void draw_scene_menu();
    void draw_tools_menu();
    void draw_render_menu();
    void draw_camera_menu();
    void draw_bookmarks_menu();
    void save_bookmark();
    void draw_render_settings_menu(render::raster::RenderSettings& settings);
    /// Screen percentage, scalability, volumetric fog and post-processing.
    void draw_quality_menus(render::raster::RenderSettings& settings);
    void command_menu_item(const char* id, const char* label = nullptr);
    /// A checkable item that shows/hides panel `id`.
    void panel_menu_item(const char* id, const char* label = nullptr);
    void apply_default_layout(unsigned int dockspace_id);

    CommandRegistry& commands_;
    EditorCamera& camera_;
    ops::OpStack* op_stack_ = nullptr;
    SceneEditor* scene_editor_ = nullptr;
    OutlinerPanel* outliner_ = nullptr;
    ViewMenu* view_menu_ = nullptr;
    PanelManager panels_;
    CommandPalette palette_;
    ViewportPanel* viewport_ = nullptr;
    KeymapPanel* keymap_ = nullptr;
    bool layout_requested_ = false;
    bool quit_requested_ = false;
    bool show_demo_ = false;
};

} // namespace f3d::editor
