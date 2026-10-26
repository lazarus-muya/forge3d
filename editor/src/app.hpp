// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/api/host.hpp"
#include "f3d/api/rpc.hpp"
#if defined(F3D_WITH_PYTHON)
#include "f3d/python/python.hpp"
#endif
#include "f3d/assets/asset_database.hpp"
#include "f3d/assets/asset_index.hpp"
#include "f3d/assets/mesh_library.hpp"
#include "f3d/assets/pipeline.hpp"
#include "f3d/assets/raycast.hpp"
#include "f3d/assets/source_watcher.hpp"
#include "f3d/core/jobs.hpp"
#include "f3d/core/math.hpp"
#include "f3d/core/vfs.hpp"
#include "f3d/editor/bench.hpp"
#include "f3d/editor/camera.hpp"
#include "f3d/editor/camera_actors.hpp"
#include "f3d/editor/command_registry.hpp"
#include "f3d/editor/console_model.hpp"
#include "f3d/editor/document.hpp"
#include "f3d/editor/import_queue.hpp"
#include "f3d/editor/launch_options.hpp"
#include "f3d/editor/measure_tool.hpp"
#include "f3d/editor/modeling_tool.hpp"
#include "f3d/editor/navigation.hpp"
#include "f3d/editor/preferences.hpp"
#include "f3d/editor/scene_editor.hpp"
#include "f3d/editor/foliage_tool.hpp"
#include "f3d/editor/paint_tool.hpp"
#include "f3d/editor/sculpt_tool.hpp"
#include "f3d/editor/spline_tool.hpp"
#include "f3d/editor/water_tool.hpp"
#include "f3d/editor/viewport_tool.hpp"
#include "f3d/editor/thumbnail_cache.hpp"
#include "f3d/editor/transform_tool.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/render/raster/capture.hpp"
#include "f3d/render/raster/material_preview.hpp"
#include "f3d/render/raster/raster_engine.hpp"
#include "f3d/render/raster/thumbnail_renderer.hpp"
#include "f3d/render/rhi/device.hpp"
#include "f3d/render/rhi/shader_library.hpp"
#include "f3d/scene/world.hpp"
#include "ui/editor_ui.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct SDL_Window;
union SDL_Event;

namespace f3d::editor {

class ImGuiLayer;

/// Owns the window, the GPU device, the UI and the frame loop:
/// poll SDL events -> update -> render -> present.
class App {
public:
    explicit App(LaunchOptions options);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// Runs until the window closes or the --frames limit is reached. Returns the exit code.
    int run();

private:
    bool init();
    void shutdown();
    bool pump_events();                           ///< false when the app should quit
    bool handle_shortcut(const SDL_Event& event); ///< true if a command or the keymap editor took the key
    void route_navigation_input(const SDL_Event& event);
    void deliver_replay_events();
    void update_scripted_sculpt();
    /// Global erosion of the selected (or first) terrain on a worker (P6.3).
    void start_erosion(const terrain::ErosionSettings& settings);
    void update(double dt_seconds);
    void render(bool last_frame);
    void capture(const render::rhi::Texture& source);
    void finish_bench();

    /// Starts walk or fly navigation (WASD, mouse look) in the viewport.
    void start_flight(FlightMode mode);
    /// A file dropped on the window from the system: a project opens, a model is imported
    /// where it was dropped, a texture / material / prefab is copied into the project (and
    /// a texture or material dropped on a mesh is applied to it once imported).
    void handle_file_drop(const char* file, const glm::vec2& window_position);
    /// Applies dropped textures and materials once the asset scan has listed them.
    void update_pending_drops(double dt_seconds);

    void discover_navigation_presets();
    bool select_navigation_preset(const std::string& name);
    void frame_scene();
    void frame_selection();

    // Selection & editing (P2.4, P2.5)
    void register_edit_commands();
    void process_pick(const ViewportPick& pick);
    [[nodiscard]] std::optional<glm::vec3> snap_point(const glm::vec3& origin, const glm::vec3& direction,
                                                      float max_angle);
    [[nodiscard]] std::optional<glm::vec3> surface_point(const glm::vec3& origin, const glm::vec3& direction);
    void apply_view_request();
    void draw_selection_overlay();
    void register_camera_commands();
    void register_add_commands();
    /// Creates `name` at the 3D cursor with `component` (may be null) and `properties`
    /// ({"Component.field": value}), selects it; one undo step. Null guid on failure.
    core::Guid add_entity(const std::string& name, const char* component, const nlohmann::json& properties);
    /// Adds a scatter from a preset (world::scatter_presets() id) or from the selected
    /// objects' meshes ("selected") on the selected terrain, else the one under the 3D
    /// cursor, else the first (flat ground without one); selects it, opens the Scatter panel.
    core::Guid add_scatter(const std::string& preset);
    /// Adds a modeling shape at the 3D cursor, selects it and opens the Modeling panel.
    core::Guid add_shape(modeling::Shape shape);
    /// Turns the selected object's mesh into an EditableMesh (one undo step).
    void make_editable();
    /// The Modeling commands (Tools > Modeling) and Add > Shape.
    void register_modeling_commands();
    /// Copies the viewport's render settings into the preferences and saves them.
    void save_render_preferences();

    // Documents (P2.6, app_document.cpp)
    enum class FileDialog { Open, Import, SaveAs };
    enum class PendingAction { None, New, Open, Quit };
    struct DialogResult {
        FileDialog kind = FileDialog::Open;
        std::filesystem::path path; ///< empty: cancelled
    };
    struct DialogRequest {
        App* app = nullptr;
        FileDialog kind = FileDialog::Open;
    };
    struct Recovery {
        std::filesystem::path project; ///< empty: an untitled scene
        std::filesystem::path autosave;
    };
    void show_file_dialog(FileDialog kind);
    // Landscape (app_landscape.cpp)
    /// Opens an image file dialog (several files allowed); `done` gets the paths on the main thread.
    void show_image_dialog(std::function<void(std::vector<std::string>)> done);
    /// Delivers a finished image dialog (main thread, once per frame).
    void update_landscape_dialogs();
    void handle_dialog_result(const DialogResult& result);
    /// Runs `action`, first asking to save unsaved changes (interactive runs only).
    void request(PendingAction action, std::filesystem::path path = {});
    void perform(PendingAction action, const std::filesystem::path& path);
    void save();
    /// The user's startup scene (Blender's startup file): what File > New starts from.
    /// Empty for automated runs (they always start empty).
    [[nodiscard]] std::filesystem::path startup_scene_path() const;
    /// An untitled scene: a copy of the startup scene if there is one, else empty.
    void new_scene();
    /// Saves the current scene, editor camera and viewport render settings as the defaults
    /// for new scenes. Reset deletes the startup scene (new scenes start empty again).
    void save_startup_scene();
    void reset_startup_scene();
    bool save_project(const std::filesystem::path& path);
    bool open_project(const std::filesystem::path& path);
    bool load_project(const std::filesystem::path& path, bool from_autosave);
    /// Imports a glTF through operations: into the scene (one undo step), or as a new scene.
    /// A model from outside the project is copied into it first (project_import.hpp), so
    /// the asset browser lists it; in an untitled scene, that happens on the first save.
    /// `position`: where to put the model's root (a model dropped on the viewport); none:
    /// where the file puts it, and the camera frames it.
    bool import_model(const std::filesystem::path& path, bool as_new_scene,
                      std::optional<glm::vec3> position = std::nullopt);
    /// Copies the models imported while untitled into the project saved at `project_dir`.
    void copy_untitled_imports(const std::filesystem::path& project_dir);
    void scene_replaced(const std::optional<scene::CameraBookmark>& camera);
    [[nodiscard]] scene::CameraBookmark camera_view() const;
    void set_camera_view(const scene::CameraBookmark& view);
    void update_document();
    void draw_prompts();
    void check_untitled_recovery();
    /// Mounts project:// on the document's directory (when it changes) and rescans its assets.
    void update_project_mount();
    void start_asset_scan();

    // Project assets (P3.3, app_assets.cpp)
    [[nodiscard]] std::shared_ptr<const assets::CookedModel> cooked_model(const core::Guid& guid);
    [[nodiscard]] std::shared_ptr<const assets::MeshData> resolve_mesh(const core::AssetRef& ref);
    /// A cooked model's material ({guid, "<model>#material/<n>"}) or a project .f3dmat (by GUID or VFS path).
    [[nodiscard]] std::shared_ptr<const assets::Material> resolve_material(const core::AssetRef& ref);
    /// viewport.capture's renderer: an offscreen capture, the editor camera as the default view.
    bool capture_scene(scene::World& world, const render::raster::CaptureRequest& request, std::string* error);
    /// Starts the JSON-RPC server if --rpc or the preference asks for it.
    void start_rpc();
    /// The Python panel's hooks (scripts run only with the preference on).
    PythonActions python_actions();
    /// Copies a slot's material into project://materials/ and assigns the copy (Material Editor).
    void make_editable_material(const core::Guid& entity, std::size_t slot);
    /// Assigns a material asset to every slot of the mesh under a ray (a material dropped on the viewport).
    void apply_material(const core::Guid& material, const glm::vec3& origin, const glm::vec3& direction);
    // Material library (app_assets.cpp)
    /// Creates project://materials/<name>.f3dmat: a copy of `copy_from`, or the default
    /// material if it is empty. Returns its path (empty on failure).
    std::string create_material(const core::AssetRef& copy_from);
    /// Deletes a project material; slots using it go back to the default (one undo step).
    void delete_material(const core::Guid& material);
    /// Assigns a project material to every slot of every selected mesh (one undo step).
    void assign_material_to_selection(const core::Guid& material);
    /// Assigns a project material to one slot of an entity's MeshRenderer.
    void assign_material_slot(const core::Guid& entity, std::size_t slot, const core::Guid& material);
    /// "project://<dir>/<name><extension>", the name made file-safe and numbered if taken.
    [[nodiscard]] std::string unused_project_path(const std::string& dir, const std::string& name,
                                                  std::string_view extension) const;
    /// The cooked mesh "guid#mesh/<n>" points at (shares ownership with its model), or null.
    [[nodiscard]] std::shared_ptr<const assets::CookedMesh> cooked_mesh(const core::AssetRef& ref);
    void open_asset_index(const std::filesystem::path& project_dir);
    void scan_assets_now();
    void place_asset(const core::Guid& guid, const glm::vec3& position);
    void create_prefab_from_selection();
    void set_asset_tags(const core::Guid& guid, const std::vector<std::string>& tags);
    void asset_imported(const core::Guid& guid);
    /// Polls the project's asset sources (on a worker) and rescans when one changed.
    void watch_asset_sources(double dt_seconds);
    /// Sets the base-colour texture of the mesh under a ray (a texture dropped on the viewport).
    void apply_texture(const core::Guid& texture, const glm::vec3& origin, const glm::vec3& direction);
    [[nodiscard]] std::uint64_t vram_budget_bytes() const;
    [[nodiscard]] bool is_automated_run() const;

    void load_user_preferences();
    void apply_preferences();
    void save_user_preferences();

    LaunchOptions options_;
    SDL_Window* window_ = nullptr;
    bool sdl_initialized_ = false;
    bool minimized_ = false;
    float content_scale_ = 1.0f;

    std::shared_ptr<ConsoleModel> console_;
    CommandRegistry commands_;
    Preferences preferences_;
    std::filesystem::path preferences_path_; ///< empty: automated run, don't persist
    std::unique_ptr<render::rhi::Device> device_;
    std::unique_ptr<render::rhi::Swapchain> swapchain_;
    std::unique_ptr<render::rhi::ShaderLibrary> shaders_;
    std::unique_ptr<render::raster::RasterEngine> viewport_renderer_;
    std::unique_ptr<render::raster::RasterEngine> preview_renderer_; ///< the camera preview (created on first use)
    std::unique_ptr<render::raster::Capturer> capturer_; ///< viewport.capture (created on first use)
    std::unique_ptr<api::RpcServer> rpc_;                ///< JSON-RPC (P5.2), when enabled
#if defined(F3D_WITH_PYTHON)
    std::unique_ptr<python::Interpreter> python_; ///< started on the first script (P5.4)
#endif
    bool show_stats_ = false; ///< viewport stats overlay
    std::unique_ptr<render::raster::ResidencyManager> residency_;
    std::unique_ptr<render::raster::MaterialPreview> material_preview_;
    std::unique_ptr<ImGuiLayer> imgui_;
    std::unique_ptr<EditorUi> ui_;

    // Files & assets (P3.1). jobs_ is declared after what its jobs use, so it is
    // destroyed (and waits for them) first.
    core::vfs::FileSystem vfs_;
    assets::AssetDatabase asset_db_;
    std::filesystem::path mounted_project_dir_;
    /// Models imported from outside any project while untitled, with their dependencies.
    std::vector<std::pair<std::filesystem::path, std::vector<std::string>>> untitled_imports_;
    core::jobs::CancellationToken scan_token_;
    /// Auto-reimport (P3.5): sources edited outside the editor are rescanned, re-imported, hot-swapped.
    assets::SourceWatcher source_watcher_;
    core::jobs::CancellationToken watch_token_;
    bool watch_poll_in_flight_ = false;
    double watch_poll_timer_ = 0.0;
    core::jobs::JobSystem jobs_;
    ImportQueue import_queue_{jobs_, vfs_};
    std::unique_ptr<assets::AssetIndex> asset_index_;
    assets::AssetIndex* asset_index_ptr_ = nullptr; ///< what the browser reads (null without a project)
    std::unique_ptr<render::raster::ThumbnailRenderer> thumbnail_renderer_;
    std::unique_ptr<ThumbnailCache> thumbnails_;
    std::unordered_map<core::Guid, std::shared_ptr<const assets::CookedModel>> cooked_models_;

    scene::World world_;
    assets::MeshLibrary meshes_;
    ops::ChangeBus changes_;
    ops::Context ops_context_{world_, changes_, &vfs_};
    ops::OpStack op_stack_{ops_context_};
    ops::OpRegistry op_registry_;
    api::Host api_host_{op_registry_, op_stack_, world_}; ///< the C API's view of this session (P5.1)
    api::PluginLoader plugins_{api_host_};
    TransformTool transform_tool_{world_, op_stack_, op_registry_};
    Document document_{world_, op_stack_};
    SceneEditor scene_editor_{world_, op_stack_, op_registry_};
    MeasureTool measure_;
    bool measuring_ = false;
    SculptTool sculpt_tool_{world_, op_stack_, &vfs_};
    PaintTool paint_tool_{world_, op_stack_};
    FoliageTool foliage_tool_{world_, op_stack_};
    SplineTool spline_tool_{world_, scene_editor_};
    WaterTool water_tool_{world_, scene_editor_};
    ModelingTool modeling_tool_{world_, op_stack_, op_registry_}; ///< face picking and mesh edits
    LandscapeTools landscape_tools_; ///< sculpt, paint, ...: at most one enabled
    float pen_pressure_ = 1.0f; ///< SDL pen pressure while a pen is near, else 1
    struct ErosionJob {
        core::jobs::CancellationToken token;
        std::atomic<float> progress{0.0f};
    };
    std::shared_ptr<ErosionJob> erosion_; ///< the running global erosion, if any
    // A replay driving the sculpt tool (benchmarks): the left button and cursor.
    bool replay_sculpt_pressed_ = false;
    glm::vec2 replay_sculpt_position_{0.0f};
    std::optional<ViewportPick> pending_pick_; ///< resolved at the start of the next update
    /// Files dropped from the system, waiting for the asset scan to list them.
    struct PendingDrop {
        std::string path;                                       ///< project:// path of the copy
        std::optional<std::pair<glm::vec3, glm::vec3>> ray;     ///< where on the viewport (origin, direction)
        double age = 0.0;                                       ///< seconds waited
    };
    std::vector<PendingDrop> pending_drops_;
    assets::SceneBvh snap_bvh_;
    EditorCamera camera_;
    CameraActors camera_actors_{world_, scene_editor_}; ///< scene cameras: gizmos, preview, pilot
    NavigationController navigation_;
    std::map<std::string, std::filesystem::path> navigation_presets_; ///< name -> file
    ViewMenu view_menu_;
    core::Bounds scene_bounds_;
    bool show_debug_shapes_ = false;
    std::optional<Replay> replay_;
    std::size_t next_replay_event_ = 0;

    std::mutex pending_dialog_mutex_; ///< file dialogs answer on another thread
    std::optional<DialogResult> pending_dialog_;
    struct ImageDialogRequest {
        App* app = nullptr;
        std::function<void(std::vector<std::string>)> done;
    };
    ImageDialogRequest image_dialog_;
    std::optional<std::vector<std::string>> pending_images_; ///< guarded by pending_dialog_mutex_
    DialogRequest dialog_request_;
    PendingAction pending_action_ = PendingAction::None;
    std::filesystem::path pending_path_;
    bool show_unsaved_prompt_ = false;
    bool continue_after_save_ = false; ///< run pending_action_ once Save As completes
    bool quit_confirmed_ = false;
    std::optional<Recovery> recovery_;
    std::string window_title_;

    std::uint64_t frame_index_ = 0;
    double time_to_first_frame_ms_ = 0.0;
    double shader_poll_timer_ = 0.0;
    bool capture_failed_ = false;
    FrameStats stats_;
};

} // namespace f3d::editor
