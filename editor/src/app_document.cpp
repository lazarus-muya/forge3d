// SPDX-License-Identifier: Apache-2.0
// The App's document handling (phases.md P2.6): New/Open/Import/Save, the
// unsaved-changes and crash-recovery prompts, autosave and the window title.
#include "app.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/core/time.hpp"
#include "f3d/editor/project_import.hpp"
#include "f3d/io/forge_file.hpp"
#include "f3d/ops/script.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <system_error>

namespace f3d::editor {
namespace {

std::filesystem::path utf8_path(const char* text) {
    const std::string utf8(text); // SDL paths are UTF-8
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

bool has_extension(const std::filesystem::path& path, std::string_view extension) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == extension;
}

} // namespace

scene::CameraBookmark App::camera_view() const {
    scene::CameraBookmark view;
    view.target = camera_.target;
    view.distance = camera_.distance;
    view.yaw = camera_.yaw;
    view.pitch = camera_.pitch;
    view.orthographic = camera_.projection == Projection::Orthographic;
    return view;
}

void App::set_camera_view(const scene::CameraBookmark& view) {
    camera_.target = view.target;
    camera_.distance = std::max(view.distance, EditorCamera::kMinDistance);
    camera_.yaw = view.yaw;
    camera_.pitch = view.pitch;
    camera_.projection = view.orthographic ? Projection::Orthographic : Projection::Perspective;
}

void App::scene_replaced(const std::optional<scene::CameraBookmark>& camera) {
    transform_tool_.cancel();
    camera_actors_.eject(camera_);
    if (!camera_actors_.pinned().is_null()) {
        camera_actors_.toggle_pin(camera_actors_.pinned());
    }
    measure_.clear();
    pending_pick_.reset();
    scene_bounds_ = assets::world_bounds(world_, meshes_);
    if (scene_bounds_.empty()) {
        scene_bounds_.add(glm::vec3(-5.0f, 0.0f, -5.0f));
        scene_bounds_.add(glm::vec3(5.0f, 2.0f, 5.0f));
    }
    if (camera) {
        set_camera_view(*camera);
    } else {
        frame_scene();
    }
}

// --- File dialogs -------------------------------------------------------------------------------

void App::show_file_dialog(FileDialog kind) {
    static const SDL_DialogFileFilter kOpenFilters[] = {{"forge3d projects and models", "forge;gltf;glb;fbx;obj"},
                                                        {"forge3d projects", "forge"},
                                                        {"Models (glTF, FBX, OBJ)", "gltf;glb;fbx;obj"}};
    static const SDL_DialogFileFilter kModelFilters[] = {{"Models (glTF, FBX, OBJ)", "gltf;glb;fbx;obj"}};
    static const SDL_DialogFileFilter kProjectFilters[] = {{"forge3d projects", "forge"}};
    const auto callback = [](void* user, const char* const* files, int) {
        auto* request = static_cast<DialogRequest*>(user);
        if (files && files[0]) {
            std::scoped_lock lock(request->app->pending_dialog_mutex_);
            request->app->pending_dialog_ = DialogResult{request->kind, utf8_path(files[0])};
        } else if (request->kind == FileDialog::SaveAs) {
            std::scoped_lock lock(request->app->pending_dialog_mutex_);
            request->app->pending_dialog_ = DialogResult{FileDialog::SaveAs, {}}; // cancelled
        }
    };
    dialog_request_ = {this, kind};
    switch (kind) {
    case FileDialog::Open:
        SDL_ShowOpenFileDialog(callback, &dialog_request_, window_, kOpenFilters, 3, nullptr, false);
        break;
    case FileDialog::Import:
        SDL_ShowOpenFileDialog(callback, &dialog_request_, window_, kModelFilters, 1, nullptr, false);
        break;
    case FileDialog::SaveAs: {
        const std::string start = document_.untitled() ? std::string() : document_.path().string();
        SDL_ShowSaveFileDialog(callback, &dialog_request_, window_, kProjectFilters, 1,
                               start.empty() ? nullptr : start.c_str());
        break;
    }
    }
}

void App::handle_dialog_result(const DialogResult& result) {
    switch (result.kind) {
    case FileDialog::Open:
        request(PendingAction::Open, result.path);
        break;
    case FileDialog::Import:
        import_model(result.path, false);
        break;
    case FileDialog::SaveAs: {
        if (result.path.empty()) {
            continue_after_save_ = false; // cancelled: so is whatever was waiting for the save
            pending_action_ = PendingAction::None;
            return;
        }
        std::filesystem::path path = result.path;
        if (!has_extension(path, ".forge")) {
            path += ".forge";
        }
        if (save_project(path) && continue_after_save_) {
            continue_after_save_ = false;
            perform(pending_action_, pending_path_);
        }
        break;
    }
    }
}

// --- Actions ------------------------------------------------------------------------------------

void App::request(PendingAction action, std::filesystem::path path) {
    if (document_.dirty() && !is_automated_run()) {
        pending_action_ = action;
        pending_path_ = std::move(path);
        show_unsaved_prompt_ = true;
        return;
    }
    perform(action, path);
}

void App::perform(PendingAction action, const std::filesystem::path& path) {
    pending_action_ = PendingAction::None;
    switch (action) {
    case PendingAction::None:
        break;
    case PendingAction::New:
        new_scene();
        break;
    case PendingAction::Open:
        if (has_extension(path, ".forge")) {
            open_project(path);
        } else {
            import_model(path, true);
        }
        break;
    case PendingAction::Quit:
        quit_confirmed_ = true;
        break;
    }
}

void App::save() {
    if (document_.untitled()) {
        show_file_dialog(FileDialog::SaveAs);
    } else {
        save_project(document_.path());
    }
}

std::filesystem::path App::startup_scene_path() const {
    // Next to preferences.json (none for automated runs).
    return preferences_path_.empty() ? std::filesystem::path() : preferences_path_.parent_path() / "startup.forge";
}

void App::new_scene() {
    untitled_imports_.clear();
    const std::filesystem::path startup = startup_scene_path();
    std::error_code ec;
    if (!startup.empty() && std::filesystem::exists(startup, ec)) {
        std::optional<scene::CameraBookmark> camera;
        const io::ForgeResult r = document_.start_from(startup, &camera);
        if (r.ok) {
            scene_replaced(camera);
            F3D_LOG_INFO("New scene from the startup scene ({} entities)", world_.size());
            return;
        }
        F3D_LOG_WARN("Ignoring the startup scene {}: {}", startup.string(), r.error);
    }
    document_.reset();
    scene_replaced(std::nullopt);
    F3D_LOG_INFO("New scene");
}

void App::save_startup_scene() {
    const std::filesystem::path startup = startup_scene_path();
    if (startup.empty()) {
        F3D_LOG_WARN("Save Startup Scene: there is no user settings directory");
        return;
    }
    // Written directly (not through Document::save): the open document keeps its own path.
    io::ForgeSaveOptions options;
    options.editor = Document::editor_state(camera_view());
    const io::ForgeResult r = io::save_forge(startup, world_, options);
    if (!r.ok) {
        F3D_LOG_ERROR("Could not save the startup scene: {}", r.error);
        return;
    }
    save_render_preferences(); // the viewport's render settings are defaults too
    F3D_LOG_INFO("Saved the startup scene ({} entities): File > New starts from it", world_.size());
}

void App::reset_startup_scene() {
    const std::filesystem::path startup = startup_scene_path();
    std::error_code ec;
    if (!startup.empty() && std::filesystem::remove(startup, ec)) {
        std::filesystem::remove(io::autosave_path(startup), ec);
        F3D_LOG_INFO("Removed the startup scene: new scenes start empty");
    } else {
        F3D_LOG_INFO("There is no startup scene: new scenes start empty");
    }
}

bool App::save_project(const std::filesystem::path& path) {
    F3D_ZONE;
    const io::ForgeResult r = document_.save(path, camera_view());
    if (!r.ok) {
        F3D_LOG_ERROR("Could not save: {}", r.error);
        return false;
    }
    copy_untitled_imports(path.parent_path());
    document_.restart_autosave_clock(core::seconds_since_process_start());
    F3D_LOG_INFO("Saved {} ({} entities)", path.string(), world_.size());
    return true;
}

bool App::open_project(const std::filesystem::path& path) {
    const std::filesystem::path autosave = io::autosave_path(path);
    if (!is_automated_run() && io::autosave_is_newer(path, autosave)) {
        recovery_ = Recovery{path, autosave};
        return true; // the recovery prompt decides
    }
    return load_project(path, false);
}

bool App::load_project(const std::filesystem::path& path, bool from_autosave) {
    F3D_ZONE;
    std::optional<scene::CameraBookmark> camera;
    const std::filesystem::path autosave = path.empty() ? document_.autosave_target() : io::autosave_path(path);
    const io::ForgeResult r =
        from_autosave ? document_.recover(path, autosave, &camera) : document_.open(path, &camera);
    if (!r.ok) {
        F3D_LOG_ERROR("Could not open: {}", r.error);
        scene_replaced(std::nullopt);
        return false;
    }
    document_.restart_autosave_clock(core::seconds_since_process_start());
    untitled_imports_.clear();
    scene_replaced(camera);
    F3D_LOG_INFO("{} {} ({} entities, format v{})", from_autosave ? "Recovered" : "Opened",
                 (from_autosave ? autosave : path).string(), world_.size(), r.loaded_version);
    return true;
}

bool App::import_model(const std::filesystem::path& source, bool as_new_scene, std::optional<glm::vec3> position) {
    F3D_ZONE;
    std::string error;
    auto gltf = meshes_.load_file(source, &error);
    if (!gltf) {
        F3D_LOG_ERROR("Could not import {}: {}", source.string(), error);
        return false;
    }
    if (as_new_scene) {
        document_.reset();
        untitled_imports_.clear();
    }
    // The asset browser lists the project directory: bring the model into it. Automated
    // runs never write into the directories they open.
    std::filesystem::path path = source;
    if (!is_automated_run()) {
        const std::filesystem::path project_dir = document_.untitled() ? std::filesystem::path()
                                                                        : document_.path().parent_path();
        if (project_dir.empty()) {
            untitled_imports_.emplace_back(source, gltf->dependencies); // copied on the first save
        } else if (!is_inside(source, project_dir)) {
            std::vector<std::string> warnings;
            if (const auto copy = copy_model_into_project(source, gltf->dependencies, project_dir, &error, &warnings)) {
                for (const std::string& warning : warnings) {
                    F3D_LOG_WARN("Import {}: {}", source.filename().string(), warning);
                }
                if (auto copied = meshes_.load_file(*copy, &error)) {
                    path = *copy;
                    gltf = std::move(copied);
                    F3D_LOG_INFO("Copied {} into the project: {}", source.filename().string(), path.string());
                    start_asset_scan();
                } else {
                    F3D_LOG_WARN("Could not load the project copy of {} ({}); using the original", source.string(),
                                 error);
                }
            } else {
                F3D_LOG_WARN("Could not copy {} into the project ({}); it won't be in the asset browser",
                             source.string(), error);
            }
        }
    }
    // The import is a script of entity operations: one undo step, nothing written behind the history.
    core::Guid root;
    const nlohmann::json script = assets::gltf_import_ops(*gltf, path, {}, root);
    op_stack_.begin_transaction("Import " + gltf->name);
    const ops::ScriptResult result = ops::run_script(op_stack_, op_registry_, script);
    if (!result.ok) {
        op_stack_.abort_transaction();
        F3D_LOG_ERROR("Could not import {}: {}", path.string(), result.error);
        return false;
    }
    if (position && !as_new_scene &&
        !scene_editor_.run("entity.set_property", {{"entity", root.to_string()},
                                                   {"path", "Transform.translation"},
                                                   {"value", {position->x, position->y, position->z}}})) {
        op_stack_.abort_transaction();
        return false;
    }
    core::Guid select = root;
    if (as_new_scene) {
        // Start with the first entity that draws a mesh selected, so the inspector shows something.
        for (const entt::entity e : world_.depth_first()) {
            if (world_.registry().all_of<scene::MeshRenderer>(e)) {
                select = world_.guid(e);
                break;
            }
        }
    }
    scene_editor_.set_selection({select}, select);
    op_stack_.end_transaction();

    if (as_new_scene) {
        op_stack_.clear(); // a new scene made from the model: nothing to undo, nothing unsaved
        document_.mark_clean();
        scene_replaced(std::nullopt);
    } else {
        const core::Bounds bounds = assets::world_bounds(world_, meshes_, world_.find(root));
        scene_bounds_.add(bounds);
        if (!bounds.empty() && !position) { // a dropped model lands where the user is looking
            camera_.frame(bounds);
        }
    }
    const entt::entity root_entity = world_.find(root);
    F3D_LOG_INFO("Imported {} as {} entities", path.filename().string(),
                 root_entity == entt::null ? std::size_t{0} : world_.subtree(root_entity).size());
    return true;
}

void App::copy_untitled_imports(const std::filesystem::path& project_dir) {
    if (untitled_imports_.empty()) {
        return;
    }
    for (const auto& [model, dependencies] : untitled_imports_) {
        if (is_inside(model, project_dir)) {
            continue; // already part of the project
        }
        std::string error;
        std::vector<std::string> warnings;
        if (const auto copy = copy_model_into_project(model, dependencies, project_dir, &error, &warnings)) {
            for (const std::string& warning : warnings) {
                F3D_LOG_WARN("Import {}: {}", model.filename().string(), warning);
            }
            F3D_LOG_INFO("Copied {} into the project: {}", model.filename().string(), copy->string());
        } else {
            F3D_LOG_WARN("Could not copy {} into the project: {}", model.string(), error);
        }
    }
    untitled_imports_.clear();
    if (project_dir == mounted_project_dir_) {
        start_asset_scan(); // otherwise mounting the new project scans it
    }
}

// --- Files dropped from the system ------------------------------------------------------------

void App::handle_file_drop(const char* file, const glm::vec2& window_position) {
    const std::filesystem::path path = utf8_path(file);
    // ImGui coordinates are the window's, or the desktop's with multi-viewports.
    glm::vec2 screen = window_position;
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        int x = 0, y = 0;
        SDL_GetWindowPosition(window_, &x, &y);
        screen += glm::vec2(static_cast<float>(x), static_cast<float>(y));
    }
    glm::vec3 origin(0.0f), direction(0.0f);
    const bool on_viewport = ui_ && ui_->viewport().ray_at(screen, origin, direction);

    if (has_extension(path, ".forge")) {
        request(PendingAction::Open, path);
        return;
    }
    const std::string_view importer = assets::importer_for(path.string());
    if (importer == "gltf" || importer == "fbx" || importer == "obj") {
        // Into the scene where it was dropped (else at the 3D cursor), one undo step.
        const std::optional<glm::vec3> at = on_viewport ? surface_point(origin, direction) : std::nullopt;
        import_model(path, false, at.value_or(transform_tool_.cursor));
        return;
    }
    if (importer.empty()) {
        F3D_LOG_WARN("Can't use {}: drop a project (.forge), a model (glTF, FBX, OBJ), an image, a material "
                     "(.f3dmat) or a prefab (.f3dprefab)",
                     path.filename().string());
        return;
    }
    if (mounted_project_dir_.empty()) {
        F3D_LOG_WARN("Save the scene first: dropped files are copied into its project ({})",
                     path.filename().string());
        return;
    }
    // A texture, material or prefab: into the project (Imported/<name>/), then the asset scan lists it.
    std::filesystem::path copy = path;
    if (!is_inside(path, mounted_project_dir_)) {
        std::string error;
        const auto copied = copy_model_into_project(path, {}, mounted_project_dir_, &error);
        if (!copied) {
            F3D_LOG_ERROR("Could not copy {} into the project: {}", path.string(), error);
            return;
        }
        copy = *copied;
    }
    const std::optional<std::string> virtual_path = vfs_.to_virtual(copy);
    if (!virtual_path) {
        return;
    }
    F3D_LOG_INFO("Added {} to the project{}", *virtual_path,
                 on_viewport && importer != "texture_hdr" ? "; it is applied when imported" : "");
    pending_drops_.push_back({*virtual_path,
                              on_viewport ? std::optional(std::pair{origin, direction}) : std::nullopt, 0.0});
    start_asset_scan();
}

void App::update_pending_drops(double dt_seconds) {
    std::erase_if(pending_drops_, [&](PendingDrop& drop) {
        drop.age += dt_seconds;
        const assets::AssetRecord* record = asset_db_.find_path(drop.path);
        if (!record) {
            if (drop.age > 30.0) {
                F3D_LOG_WARN("{} did not show up in the asset browser", drop.path);
                return true;
            }
            return false;
        }
        if (drop.ray) {
            const auto& [origin, direction] = *drop.ray;
            if (record->type() == "texture" && record->importer == "texture") {
                apply_texture(record->guid, origin, direction);
            } else if (record->type() == "material") {
                apply_material(record->guid, origin, direction);
            } else if (record->type() == "prefab") {
                place_asset(record->guid, surface_point(origin, direction).value_or(transform_tool_.cursor));
            }
        }
        return true;
    });
}

// --- Per frame ----------------------------------------------------------------------------------

void App::update_document() {
    std::optional<DialogResult> dialog;
    {
        std::scoped_lock lock(pending_dialog_mutex_);
        dialog.swap(pending_dialog_);
    }
    if (dialog) {
        handle_dialog_result(*dialog);
    }
    update_landscape_dialogs();
    const double now = core::seconds_since_process_start();
    if (!is_automated_run() && document_.autosave_due(now, preferences_.autosave_minutes)) {
        const io::ForgeResult r = document_.autosave(now, camera_view());
        if (r.ok) {
            F3D_LOG_INFO("Autosaved to {}", document_.autosave_target().string());
        } else {
            F3D_LOG_WARN("Autosave failed: {}", r.error);
        }
    }
    update_project_mount();
    const std::string title = "forge3d - " + document_.title();
    if (title != window_title_) {
        window_title_ = title;
        SDL_SetWindowTitle(window_, title.c_str());
    }
}

void App::draw_prompts() {
    if (show_unsaved_prompt_) {
        ImGui::OpenPopup("Unsaved changes");
        show_unsaved_prompt_ = false;
    }
    if (recovery_ && !ImGui::IsPopupOpen("Recover autosave")) {
        ImGui::OpenPopup("Recover autosave");
    }
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const char* what = pending_action_ == PendingAction::Quit  ? "quitting"
                           : pending_action_ == PendingAction::New ? "starting a new scene"
                                                                   : "opening another file";
        ImGui::Text("Save the changes to %s before %s?", document_.title().c_str(), what);
        ImGui::Spacing();
        if (ImGui::Button("Save")) {
            ImGui::CloseCurrentPopup();
            if (document_.untitled()) {
                continue_after_save_ = true;
                show_file_dialog(FileDialog::SaveAs);
            } else if (save_project(document_.path())) {
                perform(pending_action_, pending_path_);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save")) {
            ImGui::CloseCurrentPopup();
            perform(pending_action_, pending_path_);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
            pending_action_ = PendingAction::None;
        }
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Recover autosave", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const Recovery recovery = recovery_.value_or(Recovery{});
        const std::string name =
            recovery.project.empty() ? std::string("an unsaved scene") : recovery.project.filename().string();
        ImGui::Text("An autosave of %s is newer than the last save.", name.c_str());
        ImGui::TextDisabled("%s", recovery.autosave.string().c_str());
        ImGui::Spacing();
        if (ImGui::Button("Recover")) {
            ImGui::CloseCurrentPopup();
            recovery_.reset();
            load_project(recovery.project, true);
        }
        ImGui::SameLine();
        if (ImGui::Button(recovery.project.empty() ? "Discard" : "Open the saved file")) {
            ImGui::CloseCurrentPopup();
            recovery_.reset();
            std::error_code ec;
            std::filesystem::remove(recovery.autosave, ec);
            if (!recovery.project.empty()) {
                load_project(recovery.project, false);
            }
        }
        ImGui::EndPopup();
    }
}

void App::update_project_mount() {
    const std::filesystem::path dir = document_.untitled() ? std::filesystem::path() : document_.path().parent_path();
    if (dir == mounted_project_dir_) {
        return;
    }
    mounted_project_dir_ = dir;
    scan_token_.cancel();
    thumbnails_->clear();
    source_watcher_.clear();
    cooked_models_.clear();
    residency_->clear();
    asset_db_.clear();
    if (dir.empty()) {
        vfs_.unmount("project");
    } else {
        vfs_.mount("project", std::make_shared<core::vfs::DirectoryBackend>(dir));
        F3D_LOG_INFO("project:// = {}", dir.string());
    }
    // Automated runs never write .meta files (or an index) into the directories they
    // open, unless --scan-assets asks for it.
    if (!is_automated_run()) {
        open_asset_index(dir);
        start_asset_scan();
    } else if (options_.scan_assets) {
        open_asset_index(dir);
        scan_assets_now();
    } else {
        open_asset_index({});
    }
}

void App::start_asset_scan() {
    scan_token_.cancel(); // a newer scan replaces a running one
    scan_token_ = core::jobs::CancellationToken{};
    if (!vfs_.mounted("project")) {
        asset_db_.clear();
        return;
    }
    // The worker reads a copy of the current database (to reuse unchanged hashes);
    // the result is applied on the main thread.
    auto previous = std::make_shared<assets::AssetDatabase>(asset_db_);
    jobs_.submit(
        core::jobs::Priority::Background, scan_token_,
        [this, previous](const core::jobs::CancellationToken&) {
            return assets::AssetDatabase::scan(vfs_, "project://", previous.get());
        },
        [this](assets::ScanResult result) {
            const assets::ScanReport& r = result.report;
            F3D_LOG_INFO("Assets: {} sources, {} new .meta, {} hashed, {} orphaned .meta", r.sources, r.metas_created,
                         r.hashed, r.orphaned.size());
            for (const std::string& orphan : r.orphaned) {
                F3D_LOG_WARN("Orphaned asset metadata (source missing): {}", orphan);
            }
            for (const std::string& path : r.regenerated) {
                F3D_LOG_WARN("New asset identity for {} (duplicate or invalid .meta)", path);
            }
            for (const std::string& error : r.errors) {
                F3D_LOG_ERROR("Assets: {}", error);
            }
            asset_db_.apply(std::move(result));
            if (asset_index_) {
                const assets::AssetIndex::SyncStats sync = asset_index_->sync(asset_db_);
                F3D_LOG_DEBUG("Asset index: {} new, {} updated, {} removed", sync.inserted, sync.updated, sync.removed);
            }
            // New or changed sources are imported in the background.
            if (const std::size_t queued = import_queue_.enqueue_stale(asset_db_); queued > 0) {
                F3D_LOG_INFO("Importing {} assets", queued);
            }
        });
}

void App::check_untitled_recovery() {
    if (is_automated_run() || document_.autosave_target().empty()) {
        return;
    }
    std::error_code ec;
    if (std::filesystem::exists(document_.autosave_target(), ec)) {
        recovery_ = Recovery{{}, document_.autosave_target()};
    }
}

} // namespace f3d::editor
