// SPDX-License-Identifier: Apache-2.0
#include "editor_ui.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/editor/scene_editor.hpp"
#include "f3d/ops/stack.hpp"

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder API
#include <nlohmann/json.hpp>

#include <ImGuizmo.h> // after imgui.h

namespace f3d::editor {

EditorUi::EditorUi(const EditorUiDesc& desc)
    : commands_(*desc.commands), camera_(*desc.camera), op_stack_(desc.op_stack), scene_editor_(desc.scene_editor),
      view_menu_(desc.view_menu), palette_(*desc.commands), layout_requested_(desc.build_default_layout) {
    viewport_ = &panels_.add<ViewportPanel>(*desc.device, *desc.renderer, render::RenderScene{desc.world, desc.meshes},
                                            *desc.camera, desc.viewport_tools);
    outliner_ = &panels_.add<OutlinerPanel>(desc.scene_editor);
    panels_.add<LayersPanel>(desc.scene_editor);
    panels_.add<InspectorPanel>(desc.world, desc.scene_editor, desc.on_property_edit);
    panels_.add<MaterialEditorPanel>(desc.world, desc.meshes, desc.scene_editor, desc.material_preview,
                                     desc.material_actions);
    panels_.add<MaterialLibraryPanel>(desc.asset_db, desc.meshes, desc.scene_editor, desc.material_preview,
                                      desc.material_library_actions);
    panels_.add<OperationLogPanel>(desc.op_stack);
    panels_.add<AssetBrowserPanel>(desc.asset_db, desc.asset_index, desc.thumbnails, desc.asset_actions);
    panels_.add<ImportQueuePanel>(desc.import_queue, desc.asset_db);
    panels_.add<ResidencyPanel>(desc.residency);
    panels_.add<TerrainPanel>(desc.scene_editor, desc.landscape, desc.terrain_actions);
    panels_.add<ScatterPanel>(desc.scene_editor, desc.scatter_actions, desc.landscape.foliage, desc.landscape.tools);
    panels_.add<ModelingPanel>(desc.modeling_tool, desc.viewport_tools.landscape, desc.modeling_actions);
    panels_.add<ConsolePanel>(desc.console);
    panels_.add<PythonPanel>(desc.python);
    keymap_ = &panels_.add<KeymapPanel>(commands_, desc.on_keymap_changed);
    static const std::vector<std::string> kNoPresets;
    panels_.add<PreferencesPanel>(*desc.preferences, view_menu_ ? view_menu_->navigation_presets : kNoPresets,
                                  keymap_, desc.on_preferences_changed);
    register_commands();
}

void EditorUi::register_commands() {
    const auto chord = [](const char* text) { return parse_key_chord(text); };
    if (op_stack_) {
        commands_.add({"edit.undo", "Undo", [this] { op_stack_->undo(); }, chord("Ctrl+Z")});
        commands_.add({"edit.redo", "Redo", [this] { op_stack_->redo(); }, chord("Ctrl+Shift+Z")});
        commands_.add({"edit.redo_alt", "Redo", [this] { op_stack_->redo(); }, chord("Ctrl+Y")});
    }
    commands_.add({"editor.command_palette", "Command Palette", [this] { palette_.open(); }, chord("Ctrl+Shift+P")});
    commands_.add({"editor.reset_layout", "Reset Layout", [this] { reset_layout(); }, std::nullopt});
    commands_.add({"editor.quit", "Quit", [this] { quit_requested_ = true; }, chord("Ctrl+Q")});
    commands_.add({"editor.preferences", "Preferences",
                   [this] { panels_.find(panel_ids::kPreferences)->set_open(true); }, chord("Ctrl+,")});
    commands_.add(
        {"editor.keymap", "Keymap", [this] { panels_.find(panel_ids::kKeymap)->set_open(true); }, chord("Ctrl+Alt+K")});
    for (const auto& panel : panels_.panels()) {
        Panel* p = panel.get();
        const std::optional<KeyChord> toggle_chord =
            p->id() == panel_ids::kConsole ? chord("Ctrl+`") : std::optional<KeyChord>{};
        commands_.add(
            {"editor.toggle_" + p->id(), "Toggle " + p->title(), [p] { p->set_open(!p->is_open()); }, toggle_chord});
    }
    commands_.add({"edit.rename", "Rename", [this] { outliner_->begin_rename_active(); }, chord("F2")});
    commands_.add({"view.save_bookmark", "Save Camera Bookmark", [this] { save_bookmark(); }, std::nullopt});
    commands_.add({"view.front", "View Front", [this] { camera_.set_view(ViewPreset::Front); }, std::nullopt});
    commands_.add({"view.right", "View Right", [this] { camera_.set_view(ViewPreset::Right); }, std::nullopt});
    commands_.add({"view.top", "View Top", [this] { camera_.set_view(ViewPreset::Top); }, std::nullopt});
    commands_.add({"view.toggle_ortho", "Toggle Orthographic",
                   [this] {
                       camera_.projection = camera_.projection == Projection::Perspective ? Projection::Orthographic
                                                                                          : Projection::Perspective;
                   },
                   std::nullopt});
    commands_.add({"view.frame_selection", "Frame Selection",
                   [this] {
                       if (view_menu_ && view_menu_->frame_scene) {
                           view_menu_->frame_scene();
                       }
                   },
                   std::nullopt});
}

void EditorUi::apply_default_layout(unsigned int dockspace_id) {
    // Viewport (centre), Outliner (left), Inspector (right), Asset Browser + Console (bottom).
    const ImGuiViewport* main = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, main->WorkSize);

    ImGuiID centre = dockspace_id;
    ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.18f, nullptr, &centre);
    const ImGuiID left_bottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.35f, nullptr, &left);
    const ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.24f, nullptr, &centre);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.28f, nullptr, &centre);
    const ImGuiID bottom_right = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.45f, nullptr, &bottom);

    const auto dock = [this](const char* id, ImGuiID node) {
        if (Panel* p = panels_.find(id)) {
            p->set_open(true);
            ImGui::DockBuilderDockWindow(p->window_name().c_str(), node);
        }
    };
    dock(panel_ids::kViewport, centre);
    dock(panel_ids::kOutliner, left);
    dock(panel_ids::kLayers, left_bottom);
    dock(panel_ids::kOperationLog, left);
    dock(panel_ids::kInspector, right);
    dock(panel_ids::kImportQueue, bottom);
    dock(panel_ids::kResidency, bottom);
    dock(panel_ids::kAssetBrowser, bottom);
    dock(panel_ids::kConsole, bottom_right);
    ImGui::DockBuilderFinish(dockspace_id);
}

void EditorUi::command_menu_item(const char* id, const char* label) {
    const Command* c = commands_.find(id);
    if (!c) {
        return;
    }
    const auto chord = commands_.chord(id);
    const std::string shortcut = chord ? to_string(*chord) : std::string();
    if (ImGui::MenuItem(label ? label : c->name.c_str(), shortcut.empty() ? nullptr : shortcut.c_str())) {
        commands_.execute(id);
    }
}

void EditorUi::draw_main_menu() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        command_menu_item("file.new");
        command_menu_item("file.open");
        command_menu_item("file.import");
        ImGui::Separator();
        command_menu_item("file.save");
        command_menu_item("file.save_as");
        ImGui::Separator();
        if (ImGui::BeginMenu("Defaults")) {
            command_menu_item("file.save_startup", "Save Startup Scene");
            ImGui::SetItemTooltip("New scenes start as a copy of this one: its sky, lights, fog, post-process,\n"
                                  "objects and camera. The viewport's render settings are saved too.");
            command_menu_item("file.reset_startup", "Reset Startup Scene");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        command_menu_item("editor.quit");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (op_stack_) {
            command_menu_item("edit.undo");
            command_menu_item("edit.redo");
            ImGui::Separator();
        }
        command_menu_item("edit.duplicate");
        command_menu_item("edit.delete");
        command_menu_item("edit.rename");
        ImGui::Separator();
        command_menu_item("editor.command_palette");
        ImGui::Separator();
        command_menu_item("editor.preferences", "Preferences...");
        command_menu_item("editor.keymap", "Keymap...");
        ImGui::EndMenu();
    }
    draw_view_menu();
    draw_add_menu();
    draw_scene_menu();
    draw_tools_menu();
    draw_render_menu();
    draw_camera_menu();
    if (ImGui::BeginMenu("Animation")) {
        ImGui::TextDisabled("No animation system yet");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        for (const auto& panel : panels_.panels()) {
            ImGui::MenuItem(panel->title().c_str(), nullptr, panel->open_flag());
        }
        ImGui::Separator();
        command_menu_item("editor.reset_layout");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("Dear ImGui Demo", nullptr, &show_demo_);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void EditorUi::draw_view_menu() {
    if (!ImGui::BeginMenu("View")) {
        return;
    }
    if (view_menu_ && ImGui::BeginMenu("Navigation")) {
        for (const auto& preset : view_menu_->navigation_presets) {
            if (ImGui::MenuItem(preset.c_str(), nullptr, preset == view_menu_->current_preset) &&
                view_menu_->select_preset) {
                view_menu_->select_preset(preset);
            }
        }
        ImGui::EndMenu();
    }
    bool ortho = camera_.projection == Projection::Orthographic;
    const auto ortho_chord = commands_.chord("view.toggle_ortho");
    if (ImGui::MenuItem("Orthographic", ortho_chord ? to_string(*ortho_chord).c_str() : nullptr, &ortho)) {
        camera_.projection = ortho ? Projection::Orthographic : Projection::Perspective;
    }
    ImGui::Separator();
    command_menu_item("view.front", "Front");
    command_menu_item("view.right", "Right");
    command_menu_item("view.top", "Top");
    command_menu_item("view.frame_selection", "Frame Selection");
    ImGui::Separator();
    if (view_menu_ && view_menu_->show_grid) {
        ImGui::MenuItem("Grid", nullptr, view_menu_->show_grid);
    }
    if (view_menu_ && view_menu_->show_debug_shapes) {
        ImGui::MenuItem("Debug Shapes", nullptr, view_menu_->show_debug_shapes);
    }
    if (view_menu_ && view_menu_->show_stats) {
        const auto chord = commands_.chord("view.toggle_stats");
        ImGui::MenuItem("Stats Overlay", chord ? to_string(*chord).c_str() : nullptr, view_menu_->show_stats);
    }
    ImGui::EndMenu();
}

void EditorUi::panel_menu_item(const char* id, const char* label) {
    if (Panel* p = panels_.find(id)) {
        ImGui::MenuItem(label ? label : p->title().c_str(), nullptr, p->open_flag());
    }
}

void EditorUi::draw_add_menu() {
    if (!ImGui::BeginMenu("Add")) {
        return;
    }
    command_menu_item("add.empty", "Empty");
    ImGui::Separator();
    if (ImGui::BeginMenu("Light")) {
        command_menu_item("add.light_directional", "Directional");
        command_menu_item("add.light_point", "Point");
        command_menu_item("add.light_spot", "Spot");
        ImGui::EndMenu();
    }
    command_menu_item("add.sky", "Sky");
    if (ImGui::BeginMenu("Visual Effects")) {
        command_menu_item("add.height_fog", "Exponential Height Fog");
        command_menu_item("add.fog_volume", "Local Fog Volume");
        command_menu_item("add.post_process", "Post Process (Bloom, Grading)");
        ImGui::EndMenu();
    }
    command_menu_item("add.reflection_probe", "Reflection Probe");
    command_menu_item("camera.add", "Camera at View");
    ImGui::Separator();
    if (ImGui::BeginMenu("Shape")) {
        for (const auto& [id, label] :
             {std::pair{"add.shape_plane", "Plane"}, std::pair{"add.shape_cube", "Cube"},
              std::pair{"add.shape_sphere", "Sphere"}, std::pair{"add.shape_cylinder", "Cylinder"},
              std::pair{"add.shape_cone", "Cone"}, std::pair{"add.shape_torus", "Torus"},
              std::pair{"add.shape_capsule", "Capsule"}, std::pair{"add.shape_stairs", "Stairs"}}) {
            command_menu_item(id, label);
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (Panel* terrain = panels_.find(panel_ids::kTerrain); terrain && ImGui::MenuItem("Terrain...")) {
        terrain->set_open(true);
    }
    if (ImGui::BeginMenu("Scatter")) {
        command_menu_item("add.scatter_trees", "Trees");
        command_menu_item("add.scatter_grass", "Grass");
        command_menu_item("add.scatter_rocks", "Rocks");
        command_menu_item("add.scatter_selected", "Selected Meshes");
        ImGui::EndMenu();
    }
    command_menu_item("file.import", "Model (glTF)...");
    ImGui::EndMenu();
}

void EditorUi::draw_scene_menu() {
    if (!ImGui::BeginMenu("Scene")) {
        return;
    }
    command_menu_item("selection.all");
    command_menu_item("selection.none");
    ImGui::Separator();
    command_menu_item("prefab.create");
    command_menu_item("assets.rescan");
    ImGui::Separator();
    panel_menu_item(panel_ids::kOutliner);
    panel_menu_item(panel_ids::kLayers);
    panel_menu_item(panel_ids::kInspector);
    panel_menu_item(panel_ids::kAssetBrowser);
    ImGui::EndMenu();
}

void EditorUi::draw_tools_menu() {
    if (!ImGui::BeginMenu("Tools")) {
        return;
    }
    command_menu_item("tool.translate", "Move");
    command_menu_item("tool.rotate", "Rotate");
    command_menu_item("tool.scale", "Scale");
    command_menu_item("tool.toggle_space");
    ImGui::Separator();
    command_menu_item("tool.measure", "Measure");
    command_menu_item("tool.sculpt", "Terrain Sculpt");
    if (ImGui::BeginMenu("Modeling")) {
        command_menu_item("modeling.select_faces", "Select Faces");
        command_menu_item("modeling.select_all_faces", "Select All Faces");
        ImGui::Separator();
        command_menu_item("modeling.extrude", "Extrude Faces");
        command_menu_item("modeling.inset", "Inset Faces");
        command_menu_item("modeling.delete_faces", "Delete Faces");
        command_menu_item("modeling.flip", "Flip Normals");
        ImGui::Separator();
        command_menu_item("modeling.subdivide", "Subdivide (Smooth)");
        if (ImGui::BeginMenu("Mirror")) {
            command_menu_item("modeling.mirror_x", "X");
            command_menu_item("modeling.mirror_y", "Y");
            command_menu_item("modeling.mirror_z", "Z");
            ImGui::EndMenu();
        }
        command_menu_item("modeling.merge_vertices", "Merge Vertices");
        if (ImGui::BeginMenu("Origin")) {
            command_menu_item("modeling.origin_centre", "To Centre");
            command_menu_item("modeling.origin_bottom", "To Bottom");
            ImGui::EndMenu();
        }
        command_menu_item("modeling.shade_flat", "Shade Flat");
        command_menu_item("modeling.shade_smooth", "Shade Smooth");
        ImGui::Separator();
        command_menu_item("modeling.make_editable", "Make Editable");
        panel_menu_item(panel_ids::kModeling, "Modeling Panel");
        ImGui::EndMenu();
    }
    ImGui::Separator();
    panel_menu_item(panel_ids::kTerrain);
    panel_menu_item(panel_ids::kScatter);
    panel_menu_item(panel_ids::kMaterialEditor);
    panel_menu_item(panel_ids::kPython);
    panel_menu_item(panel_ids::kConsole);
    ImGui::Separator();
    command_menu_item("editor.command_palette");
    ImGui::EndMenu();
}

void EditorUi::draw_render_menu() {
    if (!ImGui::BeginMenu("Render")) {
        return;
    }
    if (view_menu_ && view_menu_->render_settings) {
        draw_render_settings_menu(*view_menu_->render_settings);
        ImGui::Separator();
    }
    command_menu_item("render.bake_probes");
    command_menu_item("render.dump_graph");
    ImGui::Separator();
    panel_menu_item(panel_ids::kResidency);
    ImGui::EndMenu();
}

void EditorUi::draw_camera_menu() {
    if (!ImGui::BeginMenu("Camera")) {
        return;
    }
    command_menu_item("camera.add", "Add Camera at View");
    ImGui::Separator();
    command_menu_item("camera.pilot", "Pilot Selected Camera");
    command_menu_item("camera.eject", "Eject");
    command_menu_item("camera.snap_to_view", "Snap Selected Camera to View");
    command_menu_item("camera.toggle_pin", "Pin/Unpin Camera Preview");
    ImGui::Separator();
    draw_bookmarks_menu();
    ImGui::EndMenu();
}

void EditorUi::draw_render_settings_menu(render::raster::RenderSettings& settings) {
    using render::raster::ExposureMode;
    using render::raster::Tonemapper;
    using render::raster::ViewMode;
    if (ImGui::BeginMenu("Viewport Mode")) {
        for (const auto& [mode, label] :
             {std::pair{ViewMode::Lit, "Lit"}, std::pair{ViewMode::Unlit, "Unlit"},
              std::pair{ViewMode::Wireframe, "Wireframe"}, std::pair{ViewMode::Normals, "Normals"},
              std::pair{ViewMode::Cascades, "Shadow Cascades"}}) {
            if (ImGui::MenuItem(label, nullptr, settings.mode == mode)) {
                settings.mode = mode;
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tonemapper")) {
        for (const auto& [tonemapper, label] : {std::pair{Tonemapper::AgX, "AgX"}, std::pair{Tonemapper::Aces, "ACES"},
                                                std::pair{Tonemapper::None, "None (clamp)"}}) {
            if (ImGui::MenuItem(label, nullptr, settings.tonemapper == tonemapper)) {
                settings.tonemapper = tonemapper;
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Exposure")) {
        bool automatic = settings.exposure.mode == ExposureMode::Auto;
        if (ImGui::MenuItem("Automatic", nullptr, &automatic)) {
            settings.exposure.mode = automatic ? ExposureMode::Auto : ExposureMode::Manual;
        }
        ImGui::BeginDisabled(automatic);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderFloat("EV100", &settings.exposure.manual_ev100, -2.0f, 18.0f, "%.1f");
        ImGui::SetItemTooltip("Manual exposure: higher is darker (sunny day: 15, overcast: 12, indoors: 7)");
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderFloat("Compensation", &settings.exposure.compensation, -5.0f, 5.0f, "%.1f EV");
        ImGui::SetItemTooltip("Brighter (+) or darker (-); each step doubles or halves the light");
        ImGui::TextDisabled("Physical exposure needs a Sky entity.");
        ImGui::EndMenu();
    }
    draw_quality_menus(settings);
}

void EditorUi::draw_quality_menus(render::raster::RenderSettings& settings) {
    using render::raster::FogQuality;
    using render::raster::Scalability;
    bool changed = false;
    ImGui::Separator();
    // Unreal's viewport "Screen Percentage": render fewer pixels for speed, or more for quality.
    if (ImGui::BeginMenu("Screen Percentage")) {
        for (const float percent : {25.0f, 50.0f, 75.0f, 100.0f, 150.0f, 200.0f}) {
            char label[32];
            std::snprintf(label, sizeof(label), "%.0f %%%s", percent, percent == 100.0f ? " (native)" : "");
            if (ImGui::MenuItem(label, nullptr, settings.screen_percentage == percent)) {
                settings.screen_percentage = percent;
                changed = true;
            }
        }
        ImGui::Separator();
        ImGui::SetNextItemWidth(160.0f);
        changed |= ImGui::SliderFloat("##screen_percentage", &settings.screen_percentage,
                                      render::raster::kMinScreenPercentage, render::raster::kMaxScreenPercentage,
                                      "%.0f %%");
        ImGui::TextDisabled("Below 100 %% renders faster, above supersamples.");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Scalability")) {
        for (const auto& [level, label] :
             {std::pair{Scalability::Low, "Low (50 %, light fog, small shadows)"},
              std::pair{Scalability::Medium, "Medium (75 %)"}, std::pair{Scalability::High, "High (native)"},
              std::pair{Scalability::Epic, "Epic (fine fog, large shadows)"}}) {
            if (ImGui::MenuItem(label)) {
                std::uint32_t resolution = view_menu_ && view_menu_->shadow_settings
                                               ? view_menu_->shadow_settings->resolution
                                               : 2048u;
                render::raster::apply_scalability(level, settings, resolution);
                if (view_menu_ && view_menu_->shadow_settings) {
                    view_menu_->shadow_settings->resolution = resolution;
                }
                changed = true;
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Volumetric Fog")) {
        for (const auto& [quality, label] :
             {std::pair{FogQuality::Off, "Off (height fog only)"}, std::pair{FogQuality::Low, "Low"},
              std::pair{FogQuality::Medium, "Medium"}, std::pair{FogQuality::High, "High"},
              std::pair{FogQuality::Epic, "Epic"}}) {
            if (ImGui::MenuItem(label, nullptr, settings.fog_quality == quality)) {
                settings.fog_quality = quality;
                changed = true;
            }
        }
        ImGui::EndMenu();
    }
    changed |= ImGui::MenuItem("Post Processing", nullptr, &settings.post_process);
    if (changed && view_menu_ && view_menu_->render_settings_changed) {
        view_menu_->render_settings_changed();
    }
}

void EditorUi::save_bookmark() {
    if (!scene_editor_) {
        return;
    }
    scene::CameraBookmark view;
    view.target = camera_.target;
    view.distance = camera_.distance;
    view.yaw = camera_.yaw;
    view.pitch = camera_.pitch;
    view.orthographic = camera_.projection == Projection::Orthographic;
    const std::string name = scene_editor_->save_bookmark(view);
    if (!name.empty()) {
        F3D_LOG_INFO("Saved camera bookmark '{}'", name);
    }
}

void EditorUi::draw_bookmarks_menu() {
    if (!scene_editor_ || !ImGui::BeginMenu("Bookmarks")) {
        return;
    }
    command_menu_item("view.save_bookmark", "Save Current View");
    const std::vector<scene::CameraBookmark> bookmarks = scene_editor_->world().bookmarks().bookmarks();
    if (!bookmarks.empty()) {
        ImGui::Separator();
        for (const scene::CameraBookmark& bookmark : bookmarks) {
            if (ImGui::MenuItem(bookmark.name.c_str())) {
                scene_editor_->recall_bookmark(bookmark.name);
            }
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Delete")) {
            for (const scene::CameraBookmark& bookmark : bookmarks) {
                if (ImGui::MenuItem(bookmark.name.c_str())) {
                    scene_editor_->run("bookmark.delete", {{"name", bookmark.name}});
                }
            }
            ImGui::EndMenu();
        }
    }
    ImGui::EndMenu();
}

void EditorUi::draw() {
    F3D_ZONE;
    ImGuizmo::BeginFrame();
    draw_main_menu();
    const ImGuiID dockspace_id = ImGui::GetID("forge3d.dockspace");
    if (layout_requested_) {
        apply_default_layout(dockspace_id);
        layout_requested_ = false;
    }
    ImGui::DockSpaceOverViewport(dockspace_id, ImGui::GetMainViewport());
    panels_.draw_all();
    palette_.draw();
    if (show_demo_) {
        ImGui::ShowDemoWindow(&show_demo_);
    }
}

} // namespace f3d::editor
