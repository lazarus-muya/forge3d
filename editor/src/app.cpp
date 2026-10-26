// SPDX-License-Identifier: Apache-2.0
#include "app.hpp"

#include "f3d/terrain/terrain.hpp"
#include "f3d/terrain/erosion.hpp"
#include "f3d/terrain/terrain_ops.hpp"

#include "f3d/assets/material_ops.hpp"
#include "f3d/core/debug_draw.hpp"
#include "f3d/core/image.hpp"
#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/core/reflect.hpp"
#include "f3d/core/system.hpp"
#include "f3d/core/time.hpp"
#include "f3d/editor/frame_constants.hpp"
#include "f3d/editor/selection_tools.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/script.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/modeling/editable_mesh.hpp"
#include "f3d/modeling/modeling_ops.hpp"
#include "f3d/world/scatter.hpp"
#include "f3d/world/spline.hpp"
#include "f3d/world/world_ops.hpp"
#include "platform.hpp"
#include "ui/imgui_layer.hpp"

#include <SDL3/SDL.h>
#include <fmt/ranges.h>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <utility>

namespace f3d::editor {
namespace {

namespace rhi = render::rhi;

constexpr const char* kDefaultNavigationPreset = "blender";
constexpr float kCameraPickRadius = 14.0f; ///< logical pixels around a camera body that select it

/// Per-user directory (with trailing separator), or empty.
std::string user_pref_dir() {
    char* pref = SDL_GetPrefPath("forge3d", "forge3d");
    if (!pref) {
        return {};
    }
    std::string path(pref);
    SDL_free(pref);
    return path;
}

/// Per-user layout file, or empty for headless/automated runs (deterministic layout).
std::string layout_ini_path(const LaunchOptions& options) {
    if (options.hidden || !options.capture.empty() || !options.bench.empty() || options.frames >= 0 ||
        !options.replay.empty()) {
        return {};
    }
    const std::string dir = user_pref_dir();
    return dir.empty() ? std::string{} : dir + "imgui.ini";
}

Modifiers modifiers_from(SDL_Keymod mod) {
    return {(mod & SDL_KMOD_CTRL) != 0, (mod & SDL_KMOD_SHIFT) != 0, (mod & SDL_KMOD_ALT) != 0};
}

MouseButton button_from(Uint8 button) {
    switch (button) {
    case SDL_BUTTON_LEFT:
        return MouseButton::Left;
    case SDL_BUTTON_MIDDLE:
        return MouseButton::Middle;
    case SDL_BUTTON_RIGHT:
        return MouseButton::Right;
    default:
        return MouseButton::None;
    }
}

std::string key_name(SDL_Keycode key) {
    std::string name = SDL_GetKeyName(key);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
}

/// Automated runs use default preferences and never write the user's file.
bool is_automated(const LaunchOptions& o) {
    return o.hidden || o.frames >= 0 || !o.capture.empty() || !o.bench.empty() || !o.replay.empty();
}

} // namespace

void App::load_user_preferences() {
    if (is_automated(options_)) {
        return; // deterministic: defaults only
    }
    const std::string dir = user_pref_dir();
    if (dir.empty()) {
        return;
    }
    preferences_path_ = std::filesystem::path(dir) / "preferences.json";
    std::error_code ec;
    if (!std::filesystem::exists(preferences_path_, ec)) {
        return;
    }
    auto loaded = load_preferences(preferences_path_);
    if (!loaded.preferences) {
        F3D_LOG_WARN("Ignoring preferences {}: {}", preferences_path_.string(), loaded.error);
        return;
    }
    preferences_ = std::move(*loaded.preferences);
    if (loaded.loaded_version < Preferences::kVersion) {
        F3D_LOG_INFO("Migrated preferences from version {} to {}", loaded.loaded_version, Preferences::kVersion);
        save_user_preferences();
    }
}

void App::save_user_preferences() {
    if (preferences_path_.empty()) {
        return;
    }
    preferences_.keymap = commands_.keymap_overrides();
    if (!save_preferences(preferences_path_, preferences_)) {
        F3D_LOG_ERROR("Could not save preferences to {}", preferences_path_.string());
    }
}

void App::apply_preferences() {
    imgui_->set_theme(preferences_.theme, preferences_.colors);
    set_current_theme_colors(preferences_.colors);
    imgui_->set_scale(preferences_.ui_scale > 0.0f ? preferences_.ui_scale : content_scale_);
    if (residency_) {
        residency_->set_budget(vram_budget_bytes());
    }
    if (!replay_ && preferences_.navigation_preset != view_menu_.current_preset &&
        navigation_presets_.count(preferences_.navigation_preset)) {
        select_navigation_preset(preferences_.navigation_preset);
    }
    if (viewport_renderer_) {
        render::raster::RenderSettings& settings = viewport_renderer_->settings();
        settings.screen_percentage = preferences_.screen_percentage;
        settings.fog_quality =
            render::raster::parse_fog_quality(preferences_.fog_quality).value_or(render::raster::FogQuality::High);
        settings.post_process = preferences_.post_process;
        viewport_renderer_->shadow_settings().resolution = static_cast<std::uint32_t>(preferences_.shadow_resolution);
    }
}

void App::save_render_preferences() {
    if (!viewport_renderer_) {
        return;
    }
    const render::raster::RenderSettings& settings = viewport_renderer_->settings();
    preferences_.screen_percentage = settings.screen_percentage;
    preferences_.fog_quality = std::string(render::raster::to_string(settings.fog_quality));
    preferences_.post_process = settings.post_process;
    preferences_.shadow_resolution = static_cast<int>(viewport_renderer_->shadow_settings().resolution);
    save_user_preferences();
}

std::uint64_t App::vram_budget_bytes() const {
    const std::uint64_t mb =
        options_.vram_cap_mb ? *options_.vram_cap_mb : static_cast<std::uint64_t>(preferences_.vram_budget_mb);
    return mb << 20;
}

bool App::is_automated_run() const {
    return is_automated(options_);
}

App::App(LaunchOptions options) : options_(std::move(options)), console_(std::make_shared<ConsoleModel>()) {
    if (is_automated(options_)) {
        core::disable_error_dialogs(); // a hidden run must fail, not wait on an invisible dialog
    }
    core::log::add_sink(console_); // before anything logs, so the console panel has the full history
    landscape_tools_.add(&sculpt_tool_);
    landscape_tools_.add(&paint_tool_);
    landscape_tools_.add(&foliage_tool_);
    landscape_tools_.add(&spline_tool_);
    landscape_tools_.add(&water_tool_);
    landscape_tools_.add(&modeling_tool_);
    scene_bounds_.add(glm::vec3(-5.0f, 0.0f, -5.0f));
    scene_bounds_.add(glm::vec3(5.0f, 2.0f, 5.0f));
}

App::~App() {
    shutdown();
    core::log::remove_sink(console_);
}

void App::discover_navigation_presets() {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(input_preset_dir(), ec)) {
        if (entry.path().extension() == ".json") {
            navigation_presets_[entry.path().stem().string()] = entry.path();
        }
    }
    // A user-defined map, e.g. a copy of blender.json with changed bindings.
    const std::string pref = user_pref_dir();
    if (!pref.empty()) {
        const std::filesystem::path custom = std::filesystem::path(pref) / "input" / "custom.json";
        if (std::filesystem::exists(custom, ec)) {
            navigation_presets_["custom"] = custom;
        }
    }
    view_menu_.navigation_presets.clear();
    for (const auto& [name, path] : navigation_presets_) {
        view_menu_.navigation_presets.push_back(name);
    }
}

bool App::select_navigation_preset(const std::string& name) {
    const auto it = navigation_presets_.find(name);
    if (it == navigation_presets_.end()) {
        F3D_LOG_ERROR("Navigation preset '{}' not found in {}", name, input_preset_dir().string());
        return false;
    }
    auto loaded = load_input_map(it->second);
    if (!loaded.map) {
        F3D_LOG_ERROR("Navigation preset '{}': {}", name, loaded.error);
        return false;
    }
    navigation_.set_input_map(std::move(*loaded.map));
    view_menu_.current_preset = name;
    F3D_LOG_INFO("Navigation preset: {} ({} bindings)", navigation_.input_map().name,
                 navigation_.input_map().bindings.size());
    return true;
}

void App::frame_scene() {
    camera_.frame(scene_bounds_);
}

void App::frame_selection() {
    const std::vector<core::Guid> targets = scene_editor_.selected_roots();
    if (targets.empty()) {
        frame_scene();
        return;
    }
    core::Bounds bounds;
    for (const core::Guid& guid : targets) {
        const entt::entity e = world_.find(guid);
        bounds.add(assets::world_bounds(world_, meshes_, e));
        if (bounds.empty()) {
            const glm::vec3 origin(world_.world_matrix(e)[3]);
            bounds.add(origin - glm::vec3(1.0f));
            bounds.add(origin + glm::vec3(1.0f));
        }
    }
    camera_.frame(bounds);
}

void App::process_pick(const ViewportPick& pick) {
    F3D_ZONE;
    const render::raster::View view = ui_->viewport().view();
    std::vector<entt::entity> hits;
    std::vector<core::Guid> picked;
    if (pick.box) {
        hits = viewport_renderer_->pick_region(view, {&world_, &meshes_}, pick.x0, pick.y0, pick.x1, pick.y1);
        picked = camera_actors_.pick_region(view, static_cast<float>(pick.x0), static_cast<float>(pick.y0),
                                            static_cast<float>(pick.x1), static_cast<float>(pick.y1));
    } else if (const core::Guid camera =
                   camera_actors_.pick(view, static_cast<float>(pick.x0) + 0.5f, static_cast<float>(pick.y0) + 0.5f,
                                       kCameraPickRadius * content_scale_);
               !camera.is_null()) {
        picked.push_back(camera); // camera bodies are not in the ID buffer
    } else if (const entt::entity e = viewport_renderer_->pick(view, {&world_, &meshes_}, pick.x0, pick.y0);
               e != entt::null) {
        hits.push_back(e);
    }
    for (const entt::entity e : hits) {
        if (world_.valid(e) && std::find(picked.begin(), picked.end(), world_.guid(e)) == picked.end()) {
            picked.push_back(world_.guid(e));
        }
    }
    const scene::Selection& current = world_.selection();
    SelectionResult result = combine_selection(current.entities(), current.active(), picked, pick.mode);
    scene_editor_.set_selection(std::move(result.entities), result.active);
}

std::optional<glm::vec3> App::snap_point(const glm::vec3& origin, const glm::vec3& direction, float max_angle) {
    // The entities being dragged (and their children) must not snap onto themselves.
    const std::vector<core::Guid> targets = transform_tool_.targets();
    std::vector<entt::entity> moving;
    for (const core::Guid& guid : targets) {
        moving.push_back(world_.find(guid));
    }
    const auto is_moving = [&](entt::entity e) {
        for (const entt::entity m : moving) {
            if (world_.is_ancestor_or_self(m, e)) {
                return true;
            }
        }
        return false;
    };
    snap_bvh_.build(world_, meshes_, is_moving);
    const assets::SceneHit hit = transform_tool_.snap.target == SnapTarget::Vertex
                                     ? snap_bvh_.nearest_vertex(origin, direction, max_angle)
                                     : snap_bvh_.raycast(origin, direction);
    if (transform_tool_.snap.target != SnapTarget::Vertex) {
        // Terrain surfaces (heightfield raycast) count too, unless a terrain is the one moving.
        const std::optional<terrain::TerrainHit> ground = terrain::raycast(world_, origin, direction);
        if (ground && !is_moving(ground->entity) && (!hit.hit || ground->distance < hit.distance)) {
            return ground->point;
        }
    }
    if (!hit.hit) {
        return std::nullopt;
    }
    return hit.point;
}

std::optional<glm::vec3> App::surface_point(const glm::vec3& origin, const glm::vec3& direction) {
    snap_bvh_.build(world_, meshes_);
    const assets::SceneHit hit = snap_bvh_.raycast(origin, direction);
    const std::optional<terrain::TerrainHit> ground = terrain::raycast(world_, origin, direction);
    if (ground && (!hit.hit || ground->distance < hit.distance)) {
        return ground->point;
    }
    if (hit.hit) {
        return hit.point;
    }
    if (std::abs(direction.y) > 1e-6f) {
        const float t = -origin.y / direction.y; // the ground plane
        if (t > 0.0f) {
            return origin + direction * t;
        }
    }
    return std::nullopt;
}

void App::apply_view_request() {
    std::optional<scene::CameraBookmark>& request = world_.view_request();
    if (request) {
        set_camera_view(*request);
        request.reset();
    }
}

void App::draw_selection_overlay() {
    const scene::Selection& selection = world_.selection();
    modeling_tool_.set_target(selection.active()); // the mesh the Modeling panel edits
    for (const core::Guid& guid : selection.entities()) {
        const entt::entity e = world_.find(guid);
        if (e == entt::null) {
            continue;
        }
        const ThemeColors& theme = current_theme_colors();
        const std::uint32_t color = packed(guid == selection.active() ? theme.active : theme.selected);
        const core::Bounds bounds = assets::world_bounds(world_, meshes_, e);
        if (!bounds.empty()) {
            core::debug::box(bounds.min, bounds.max, color);
        } else {
            const glm::vec3 o(world_.world_matrix(e)[3]);
            core::debug::line(o - glm::vec3(0.2f, 0, 0), o + glm::vec3(0.2f, 0, 0), color);
            core::debug::line(o - glm::vec3(0, 0.2f, 0), o + glm::vec3(0, 0.2f, 0), color);
            core::debug::line(o - glm::vec3(0, 0, 0.2f), o + glm::vec3(0, 0, 0.2f), color);
        }
    }
    camera_actors_.draw_gizmos();
    if (measuring_) {
        measure_.draw();
    }
    if (ViewportTool* tool = landscape_tools_.sync()) {
        tool->draw();
    }
    // The 3D cursor.
    const glm::vec3 c = transform_tool_.cursor;
    const float r = 0.25f;
    core::debug::line(c - glm::vec3(r, 0, 0), c + glm::vec3(r, 0, 0), core::debug::kRed);
    core::debug::line(c - glm::vec3(0, r, 0), c + glm::vec3(0, r, 0), core::debug::kGreen);
    core::debug::line(c - glm::vec3(0, 0, r), c + glm::vec3(0, 0, r), core::debug::kBlue);
}

void App::register_edit_commands() {
    const auto chord = [](const char* text) { return parse_key_chord(text); };
    commands_.add({"edit.delete", "Delete",
                   [this] {
                       // A landscape tool may take it (the spline tool deletes its selected point).
                       if (ViewportTool* tool = landscape_tools_.active(); tool && tool->erase()) {
                           return;
                       }
                       scene_editor_.delete_selection();
                   },
                   chord("Delete")});
    commands_.add({"edit.duplicate", "Duplicate", [this] { scene_editor_.duplicate_selection(); }, chord("Ctrl+D")});
    commands_.add({"selection.all", "Select All", [this] { scene_editor_.select_all(); }, chord("Ctrl+A")});
    commands_.add({"selection.none", "Deselect All", [this] { scene_editor_.set_selection({}, {}); }, chord("Alt+A")});
    commands_.add({"tool.measure", "Measure Tool",
                   [this] {
                       measuring_ = !measuring_;
                       if (!measuring_) {
                           measure_.clear();
                       }
                   },
                   chord("M")});
    commands_.add(
        {"tool.translate", "Move Tool", [this] { transform_tool_.operation = GizmoOperation::Translate; }, chord("W")});
    commands_.add(
        {"tool.rotate", "Rotate Tool", [this] { transform_tool_.operation = GizmoOperation::Rotate; }, chord("E")});
    commands_.add(
        {"tool.scale", "Scale Tool", [this] { transform_tool_.operation = GizmoOperation::Scale; }, chord("R")});
    commands_.add({"tool.toggle_space", "Toggle Local/World Space",
                   [this] {
                       transform_tool_.space =
                           transform_tool_.space == GizmoSpace::World ? GizmoSpace::Local : GizmoSpace::World;
                   },
                   std::nullopt});
}

void App::start_flight(FlightMode mode) {
    if (!ui_ || replay_ || navigation_.flight() != FlightMode::None) {
        return;
    }
    if (ViewportTool* tool = landscape_tools_.active(); tool && tool->busy()) {
        tool->cancel();
    }
    transform_tool_.cancel();
    navigation_.begin_flight(mode, camera_);
    F3D_LOG_INFO("{} mode: WASD move, Q/E down/up, Shift faster, wheel speed, Tab {}; click keeps the view, "
                 "right-click or Esc goes back",
                 mode == FlightMode::Walk ? "Walk" : "Fly", mode == FlightMode::Walk ? "flies" : "walks");
}

void App::register_camera_commands() {
    commands_.add({"view.walk", "Walk Navigation (WASD)", [this] { start_flight(FlightMode::Walk); },
                   parse_key_chord("Shift+`")});
    commands_.add({"view.fly", "Fly Navigation (WASD)", [this] { start_flight(FlightMode::Fly); },
                   parse_key_chord("Shift+F")});
    commands_.add({"camera.add", "Add Camera at View", [this] { camera_actors_.create_at_view(camera_); },
                   std::nullopt});
    commands_.add({"camera.pilot", "Pilot Selected Camera",
                   [this] {
                       if (!camera_actors_.pilot(camera_actors_.selected_camera(), camera_)) {
                           F3D_LOG_INFO("Pilot: select a camera first");
                       }
                   },
                   std::nullopt});
    commands_.add({"camera.eject", "Eject (Stop Piloting)", [this] { camera_actors_.eject(camera_); }, std::nullopt});
    commands_.add({"camera.snap_to_view", "Snap Selected Camera to View",
                   [this] { camera_actors_.snap_to_view(camera_actors_.selected_camera(), camera_); }, std::nullopt});
    commands_.add({"camera.toggle_pin", "Pin/Unpin Camera Preview",
                   [this] {
                       const core::Guid selected = camera_actors_.selected_camera();
                       camera_actors_.toggle_pin(selected.is_null() ? camera_actors_.pinned() : selected);
                   },
                   std::nullopt});
}

core::Guid App::add_entity(const std::string& name, const char* component, const nlohmann::json& properties) {
    // One undo step: create at the 3D cursor, add the component, set its properties, select it.
    const core::Guid guid = core::Guid::generate();
    const std::string id = guid.to_string();
    const glm::vec3 c = transform_tool_.cursor;
    scene_editor_.stack().begin_transaction("Add " + name);
    bool ok = scene_editor_.run("entity.create", {{"name", name}, {"parent", nullptr}, {"guid", id}});
    ok = ok && scene_editor_.run("entity.set_property",
                                 {{"entity", id}, {"path", "Transform.translation"}, {"value", {c.x, c.y, c.z}}});
    if (component) {
        ok = ok && scene_editor_.run("entity.add_component", {{"entity", id}, {"component", component}});
    }
    for (const auto& [path, value] : properties.items()) {
        ok = ok && scene_editor_.run("entity.set_property", {{"entity", id}, {"path", path}, {"value", value}});
    }
    if (!ok) {
        scene_editor_.stack().abort_transaction();
        return {};
    }
    scene_editor_.set_selection({guid}, guid);
    scene_editor_.stack().end_transaction();
    return guid;
}

void App::register_add_commands() {
    // Directional and spot lights point down -Z; tilt them 45 degrees towards the ground.
    const glm::quat tilt = glm::angleAxis(glm::radians(-45.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    const nlohmann::json tilted = {tilt.w, tilt.x, tilt.y, tilt.z};
    const auto light = [tilted](scene::LightType type, bool aimed) {
        nlohmann::json props = {{"Light.type", static_cast<int>(type)}};
        if (aimed) {
            props["Transform.rotation"] = tilted;
        }
        return props;
    };
    commands_.add({"add.empty", "Add Empty", [this] { add_entity("Empty", nullptr, nlohmann::json::object()); },
                   parse_key_chord("Shift+A")});
    commands_.add({"add.light_directional", "Add Directional Light",
                   [this, light] { add_entity("Directional Light", "Light", light(scene::LightType::Directional, true)); },
                   std::nullopt});
    commands_.add({"add.light_point", "Add Point Light",
                   [this, light] { add_entity("Point Light", "Light", light(scene::LightType::Point, false)); },
                   std::nullopt});
    commands_.add({"add.light_spot", "Add Spot Light",
                   [this, light] { add_entity("Spot Light", "Light", light(scene::LightType::Spot, true)); },
                   std::nullopt});
    commands_.add({"add.sky", "Add Sky", [this] { add_entity("Sky", "Sky", nlohmann::json::object()); },
                   std::nullopt});
    commands_.add({"add.reflection_probe", "Add Reflection Probe",
                   [this] { add_entity("Reflection Probe", "ReflectionProbe", nlohmann::json::object()); },
                   std::nullopt});
    commands_.add({"tool.sculpt", "Toggle Terrain Sculpt", [this] { landscape_tools_.toggle(&sculpt_tool_); },
                   std::nullopt});
    for (const world::ScatterPreset& preset : world::scatter_presets()) {
        const std::string id = preset.id;
        commands_.add({"add.scatter_" + id, std::string("Add Scatter: ") + preset.name, [this, id] { add_scatter(id); },
                       std::nullopt});
    }
    commands_.add({"add.scatter_selected", "Add Scatter: Selected Meshes", [this] { add_scatter("selected"); },
                   std::nullopt});
}

core::Guid App::add_scatter(const std::string& preset) {
    world::Scatter scatter;
    std::string name = "Scatter";
    bool fit_terrain = true;
    if (preset == "selected") {
        for (const core::Guid& root : scene_editor_.selected_roots()) {
            for (const entt::entity e : world_.subtree(world_.find(root))) {
                const auto* renderer = world_.registry().try_get<scene::MeshRenderer>(e);
                if (renderer && !renderer->mesh.empty() &&
                    std::find(scatter.meshes.begin(), scatter.meshes.end(), renderer->mesh) == scatter.meshes.end()) {
                    scatter.meshes.push_back(renderer->mesh);
                }
            }
        }
        if (scatter.meshes.empty()) {
            F3D_LOG_WARN("Add Scatter: select objects with meshes first (or pick Trees, Grass or Rocks)");
            return {};
        }
        scatter.density = 0.01f;
        scatter.min_spacing = 4.0f;
        scatter.slope_max = 35.0f;
    } else {
        const auto& presets = world::scatter_presets();
        const auto it = std::find_if(presets.begin(), presets.end(),
                                     [&](const world::ScatterPreset& p) { return preset == p.id; });
        if (it == presets.end()) {
            F3D_LOG_ERROR("Add Scatter: unknown preset '{}'", preset);
            return {};
        }
        scatter = it->scatter;
        name = it->name;
        fit_terrain = it->fit_terrain;
    }

    // The surface: the selected terrain, else the one under the 3D cursor, else the first.
    entt::registry& registry = world_.registry();
    const glm::vec3 cursor = transform_tool_.cursor;
    entt::entity surface = world_.find(world_.selection().active());
    if (surface != entt::null && !registry.all_of<terrain::Terrain>(surface)) {
        surface = entt::null;
    }
    for (const entt::entity t : registry.view<terrain::Terrain>()) {
        const glm::vec3 origin = terrain::terrain_origin(world_, t);
        const float half = registry.get<terrain::Terrain>(t).size * 0.5f;
        if (surface == entt::null && std::abs(cursor.x - origin.x) <= half && std::abs(cursor.z - origin.z) <= half) {
            surface = t;
        }
    }
    if (const auto terrains = registry.view<terrain::Terrain>(); surface == entt::null && !terrains.empty()) {
        surface = terrains.front();
    }
    nlohmann::json properties = nlohmann::json::object();
    if (surface != entt::null) {
        scatter.terrain = world_.guid(surface);
        if (fit_terrain) {
            const glm::vec3 origin = terrain::terrain_origin(world_, surface);
            const float size = registry.get<terrain::Terrain>(surface).size;
            scatter.area = {size, size};
            properties["Transform.translation"] = {origin.x, origin.y, origin.z};
        }
    }
    nlohmann::json fields;
    core::reflect::value_to_json(fields, entt::forward_as_meta(scatter));
    for (const auto& [field, value] : fields.items()) {
        properties["Scatter." + field] = value;
    }
    const core::Guid guid = add_entity(name, "Scatter", properties);
    if (!guid.is_null() && ui_) {
        if (Panel* panel = ui_->panels().find(panel_ids::kScatter)) {
            panel->set_open(true);
        }
    }
    return guid;
}

bool App::capture_scene(scene::World& world, const render::raster::CaptureRequest& request, std::string* error) {
    if (!device_ || !shaders_ || !viewport_renderer_) {
        *error = "no GPU device";
        return false;
    }
    if (!capturer_) {
        capturer_ = std::make_unique<render::raster::Capturer>(*device_, *shaders_);
        capturer_->engine().set_residency(residency_.get());
    }
    capturer_->engine().settings().tonemapper = viewport_renderer_->settings().tonemapper;
    capturer_->engine().settings().exposure = viewport_renderer_->settings().exposure;
    render::raster::View current;
    current.view = camera_.view_matrix();
    current.projection = camera_.projection_matrix(16.0f / 9.0f);
    current.camera_position = camera_.position();
    current.orthographic = camera_.projection == Projection::Orthographic;
    current.near_plane = camera_.near_plane;
    current.far_plane = camera_.far_plane;
    return capturer_->capture(world, meshes_, request, &current, error);
}

void App::start_rpc() {
    if (!options_.rpc_port && !preferences_.rpc_enabled) {
        return; // off by default
    }
    api::RpcServerOptions options;
    options.port = options_.rpc_port && *options_.rpc_port != 0xFFFF
                       ? *options_.rpc_port
                       : static_cast<std::uint16_t>(preferences_.rpc_port);
    options.token_file = options_.rpc_token_file;
    if (options.token_file.empty()) {
        if (const std::string dir = user_pref_dir(); !dir.empty()) {
            options.token_file = std::filesystem::path(dir) / "rpc.json";
        }
    }
    api::RpcServices services;
    services.host = &api_host_;
    services.changes = &changes_;
    services.search_assets = [this](const assets::AssetQuery& query) {
        return asset_index_ptr_ ? asset_index_ptr_->search(query) : std::vector<assets::AssetSummary>{};
    };
    services.capture = [this](scene::World& world, const render::raster::CaptureRequest& request, std::string* error) {
        return capture_scene(world, request, error);
    };
    rpc_ = std::make_unique<api::RpcServer>(std::move(services), std::move(options));
    std::string error;
    if (!rpc_->start(&error)) {
        F3D_LOG_ERROR("JSON-RPC server not started: {}", error);
        rpc_.reset();
    }
}

PythonActions App::python_actions() {
    PythonActions actions;
    actions.enabled = [this] { return preferences_.python_enabled; };
#if defined(F3D_WITH_PYTHON)
    actions.built = true;
    const auto interpreter = [this]() -> python::Interpreter* {
        if (!preferences_.python_enabled) {
            return nullptr;
        }
        if (!python_) {
            api::RpcServices services;
            services.host = &api_host_;
            services.changes = &changes_;
            services.search_assets = [this](const assets::AssetQuery& query) {
                return asset_index_ptr_ ? asset_index_ptr_->search(query) : std::vector<assets::AssetSummary>{};
            };
            services.capture = [this](scene::World& world, const render::raster::CaptureRequest& request,
                                      std::string* error) { return capture_scene(world, request, error); };
            python_ = std::make_unique<python::Interpreter>(std::move(services));
            std::string error;
            if (!python_->start(&error)) {
                F3D_LOG_ERROR("{}", error);
                python_.reset();
                return nullptr;
            }
        }
        return python_.get();
    };
    actions.run_code = [interpreter](const std::string& code) {
        python::Interpreter* py = interpreter();
        if (!py) {
            return PythonOutput{false, "Python is off or failed to start (see the console)"};
        }
        const python::RunResult r = py->run_string(code);
        return PythonOutput{r.ok, r.output + r.error};
    };
    actions.run_file = [interpreter](const std::string& path) {
        python::Interpreter* py = interpreter();
        if (!py) {
            return PythonOutput{false, "Python is off or failed to start (see the console)"};
        }
        const python::RunResult r = py->run_file(path, true);
        return PythonOutput{r.ok, r.output + r.error};
    };
#endif
    return actions;
}

bool App::init() {
    F3D_ZONE;
    scene::register_types();                 // reflection for the inspector (P2.2)
    assets::register_material_types();       // materials (P4.2)
    ops::register_builtin_ops(op_registry_); // entity, selection, layer and bookmark operations
    assets::register_material_ops(op_registry_);
    terrain::register_terrain_ops(op_registry_); // terrain.create (P6.1)
    world::register_world_ops(op_registry_);     // Scatter, WaterBody, Spline; scatter.paint_stroke
    modeling::register_modeling_ops(op_registry_); // EditableMesh, shapes and mesh edits
    for (auto& [path, mesh] : world::builtin_spline_meshes()) {
        meshes_.add(path, std::move(mesh)); // fence posts and rails
    }
    for (auto& [path, mesh] : world::builtin_scatter_meshes()) {
        meshes_.add(path, std::move(mesh)); // what the scatter presets place
    }
    // viewport.capture (P4.4): an offscreen renderer of its own, the editor camera as
    // the default view.
    render::raster::register_capture_op(op_registry_,
                                        [this](scene::World& world, const render::raster::CaptureRequest& request,
                                               std::string* error) { return capture_scene(world, request, error); });
    // Plugins add operations (phases.md P5.1): plugins/ next to the editor.
    plugins_.load_directory(api::default_plugin_dir());
    // Material files rewritten by operations (edits, undo): drop the cached copies.
    changes_.subscribe([this](const ops::ChangeEvent& event) {
        if (event.kind == ops::ChangeKind::AssetChanged) {
            meshes_.invalidate_materials();
        }
    });
#if defined(__linux__)
    // Diligent's Vulkan swapchain needs an X11 window (XWayland on Wayland desktops).
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        F3D_LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    sdl_initialized_ = true;

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (options_.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    window_ = SDL_CreateWindow("forge3d", 1600, 900, flags);
    if (!window_) {
        F3D_LOG_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }
    content_scale_ = SDL_GetWindowDisplayScale(window_);
    F3D_LOG_INFO("Video driver: {}, display scale {:.2f}", SDL_GetCurrentVideoDriver(), content_scale_);

    rhi::DeviceDesc desc;
    desc.backend = options_.rhi.value_or(rhi::default_backend());
    desc.allow_fallback = !options_.rhi.has_value(); // an explicit --rhi must not silently change backend
    desc.adapter_index = options_.adapter_index;
    desc.adapter_preference = options_.adapter_preference;
    desc.validation = options_.validation;
    auto created = rhi::Device::create(desc);
    if (!created.device) {
        F3D_LOG_ERROR("Could not create a GPU device: {}", created.error);
        return false;
    }
    device_ = std::move(created.device);

    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    swapchain_ = device_->create_swapchain(native_window(window_), static_cast<std::uint32_t>(width),
                                           static_cast<std::uint32_t>(height));
    if (!swapchain_) {
        return false;
    }
    shaders_ = std::make_unique<rhi::ShaderLibrary>(*device_, rhi::default_shader_library_desc());
    viewport_renderer_ = std::make_unique<render::raster::RasterEngine>(*device_, *shaders_);
    // Project assets stream into VRAM by on-screen size, under the budget (P3.5).
    render::raster::ResidencySources residency_sources;
    residency_sources.mesh = [this](const core::AssetRef& ref) { return cooked_mesh(ref); };
    residency_sources.texture = [this](const core::AssetRef& ref) -> std::shared_ptr<const assets::CookedTexture> {
        std::string error;
        auto texture = assets::load_cooked_texture(vfs_, ref, &error);
        if (!texture) {
            F3D_LOG_DEBUG("Cooked texture {} ({}): {}", ref.path, ref.guid.to_string(), error);
            return nullptr;
        }
        return std::make_shared<const assets::CookedTexture>(std::move(*texture));
    };
    residency_ = std::make_unique<render::raster::ResidencyManager>(*device_, std::move(residency_sources));
    viewport_renderer_->set_residency(residency_.get());
    thumbnail_renderer_ = std::make_unique<render::raster::ThumbnailRenderer>(*device_, *shaders_);
    thumbnails_ = std::make_unique<ThumbnailCache>(*device_, thumbnail_renderer_.get(), vfs_, jobs_);
    // MeshRenderers that reference project assets (by GUID) draw their cooked meshes.
    meshes_.set_resolver([this](const core::AssetRef& ref) { return resolve_mesh(ref); });
    meshes_.set_material_resolver([this](const core::AssetRef& ref) { return resolve_material(ref); });
    // Images by reference (terrain layer textures): cooked texture assets, or VFS files.
    meshes_.set_image_resolver([this](const core::AssetRef& ref) -> std::optional<assets::SourceImage> {
        if (!ref.guid.is_null()) {
            if (auto cooked = assets::load_cooked_texture(vfs_, ref)) {
                return assets::SourceImage{assets::decode_mip(*cooked, 0), {}};
            }
        }
        if (ref.path.find("://") != std::string::npos) {
            if (const auto bytes = vfs_.read(ref.path)) {
                std::string extension = std::filesystem::path(ref.path).extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return assets::decode_texture_source(*bytes, extension);
            }
        }
        return std::nullopt;
    });
    material_preview_ = std::make_unique<render::raster::MaterialPreview>(*device_, *shaders_);
    material_preview_->set_residency(residency_.get());

    // Navigation presets; a replay may pick its own.
    discover_navigation_presets();
    if (!options_.replay.empty()) {
        auto loaded = load_replay(options_.replay);
        if (!loaded.replay) {
            F3D_LOG_ERROR("--replay {}: {}", options_.replay, loaded.error);
            return false;
        }
        replay_ = std::move(loaded.replay);
        if (!replay_->sculpt.empty()) {
            std::string error;
            if (!sculpt_tool_.apply_json(nlohmann::json::parse(replay_->sculpt), &error)) {
                F3D_LOG_ERROR("--replay {}: sculpt: {}", options_.replay, error);
                return false;
            }
            landscape_tools_.activate(&sculpt_tool_);
            sculpt_tool_.scripted = true;
        }
        F3D_LOG_INFO("Replaying {} input events from {}", replay_->events.size(), options_.replay);
    }
    load_user_preferences();
    std::string preset = replay_ && !replay_->preset.empty() ? replay_->preset : preferences_.navigation_preset;
    if (!navigation_presets_.count(preset)) {
        preset = kDefaultNavigationPreset;
    }
    if (!select_navigation_preset(preset)) {
        return false;
    }
    navigation_.on_action = [this](std::string_view action) {
        if (action == "view.frame_selection") {
            frame_selection();
        } else if (!commands_.execute(action)) {
            F3D_LOG_DEBUG("Unhandled navigation action {}", action);
        }
    };
    // Walk mode stands on whatever is below the eyes: terrain, meshes or the ground plane.
    navigation_.ground_height = [this](const glm::vec3& eye) -> std::optional<float> {
        const std::optional<glm::vec3> below = surface_point(eye, glm::vec3(0.0f, -1.0f, 0.0f));
        return below ? std::optional<float>(below->y) : std::nullopt;
    };
    view_menu_.select_preset = [this](const std::string& name) { select_navigation_preset(name); };
    view_menu_.frame_scene = [this] { frame_selection(); };
    view_menu_.show_grid = &viewport_renderer_->grid().visible;
    view_menu_.show_debug_shapes = &show_debug_shapes_;
    view_menu_.show_stats = &show_stats_;
    view_menu_.render_settings = &viewport_renderer_->settings();
    view_menu_.shadow_settings = &viewport_renderer_->shadow_settings();
    view_menu_.render_settings_changed = [this] { save_render_preferences(); };
    if (const char* env = std::getenv("F3D_DEBUG_SHAPES"); env && *env == '1') {
        show_debug_shapes_ = true;
    }

    ImGuiLayerDesc ui_desc;
    ui_desc.window = window_;
    ui_desc.back_buffer_format = swapchain_->format();
    ui_desc.viewports = !options_.hidden; // a hidden main window can't host detached windows
    ui_desc.ini_path = layout_ini_path(options_);
    ui_desc.scale = content_scale_;
    imgui_ = std::make_unique<ImGuiLayer>(*device_, ui_desc);

    commands_.add({"file.new", "New Scene", [this] { request(PendingAction::New); }, parse_key_chord("Ctrl+N")});
    commands_.add({"file.open", "Open...", [this] { show_file_dialog(FileDialog::Open); }, parse_key_chord("Ctrl+O")});
    commands_.add(
        {"file.import", "Import glTF...", [this] { show_file_dialog(FileDialog::Import); }, parse_key_chord("Ctrl+I")});
    commands_.add({"file.save", "Save", [this] { save(); }, parse_key_chord("Ctrl+S")});
    commands_.add({"file.save_startup", "Save Startup Scene (Defaults for New Scenes)", [this] { save_startup_scene(); },
                   std::nullopt});
    commands_.add({"file.reset_startup", "Reset Startup Scene", [this] { reset_startup_scene(); }, std::nullopt});
    commands_.add({"assets.rescan", "Rescan Project Assets", [this] { start_asset_scan(); }, std::nullopt});
    commands_.add(
        {"prefab.create", "Create Prefab from Selection", [this] { create_prefab_from_selection(); }, std::nullopt});
    commands_.add({"file.save_as", "Save As...", [this] { show_file_dialog(FileDialog::SaveAs); },
                   parse_key_chord("Ctrl+Shift+S")});
    commands_.add({"render.dump_graph", "Dump Render Graph (Graphviz)",
                   [this] {
                       const std::filesystem::path path = std::filesystem::absolute("forge3d-render-graph.dot");
                       std::ofstream file(path);
                       file << viewport_renderer_->graphviz();
                       if (file) {
                           F3D_LOG_INFO("Render graph written to {}", path.string());
                       } else {
                           F3D_LOG_ERROR("Could not write {}", path.string());
                       }
                   },
                   std::nullopt});
    commands_.add({"view.toggle_stats", "Toggle Viewport Stats", [this] { show_stats_ = !show_stats_; }, std::nullopt});
    commands_.add({"render.bake_probes", "Rebake Reflection Probes", [this] { viewport_renderer_->rebake_probes(); },
                   std::nullopt});
    commands_.add({"material.new", "New Material",
                   [this] {
                       create_material({});
                       if (Panel* library = ui_ ? ui_->panels().find(panel_ids::kMaterialLibrary) : nullptr) {
                           library->set_open(true);
                       }
                   },
                   std::nullopt});
    register_edit_commands();
    register_camera_commands();
    register_add_commands();
    register_modeling_commands();
    EditorUiDesc editor_desc;
    editor_desc.device = device_.get();
    editor_desc.python = python_actions();
    editor_desc.renderer = viewport_renderer_.get();
    editor_desc.meshes = &meshes_;
    editor_desc.material_preview = material_preview_.get();
    editor_desc.material_actions.make_editable_copy = [this](const core::Guid& entity, std::size_t slot) {
        make_editable_material(entity, slot);
    };
    editor_desc.material_actions.assign_slot = [this](const core::Guid& entity, std::size_t slot,
                                                      const core::Guid& material) {
        assign_material_slot(entity, slot, material);
    };
    editor_desc.material_library_actions.create = [this](const core::AssetRef& copy_from) {
        return create_material(copy_from);
    };
    editor_desc.material_library_actions.assign_to_selection = [this](const core::Guid& material) {
        assign_material_to_selection(material);
    };
    editor_desc.material_library_actions.remove = [this](const core::Guid& material) { delete_material(material); };
    editor_desc.asset_actions.assign_material = [this](const core::Guid& material) {
        assign_material_to_selection(material);
    };
    editor_desc.camera = &camera_;
    editor_desc.world = &world_;
    editor_desc.asset_db = &asset_db_;
    editor_desc.import_queue = &import_queue_;
    editor_desc.residency = residency_.get();
    editor_desc.asset_index = &asset_index_ptr_;
    editor_desc.thumbnails = thumbnails_.get();
    editor_desc.asset_actions.place = [this](const core::Guid& guid) { place_asset(guid, transform_tool_.cursor); };
    editor_desc.asset_actions.reimport = [this](const core::Guid& guid) {
        if (const assets::AssetRecord* record = asset_db_.find(guid)) {
            import_queue_.enqueue(*record, true);
        }
    };
    editor_desc.asset_actions.set_tags = [this](const core::Guid& guid, const std::vector<std::string>& tags) {
        set_asset_tags(guid, tags);
    };
    editor_desc.asset_actions.rescan = [this] { start_asset_scan(); };
    editor_desc.viewport_tools.on_asset_drop = [this](const core::Guid& guid, const glm::vec3& origin,
                                                      const glm::vec3& direction) {
        if (const assets::AssetRecord* record = asset_db_.find(guid); record && record->type() == "texture") {
            apply_texture(guid, origin, direction);
            return;
        }
        if (const assets::AssetRecord* record = asset_db_.find(guid); record && record->type() == "material") {
            apply_material(guid, origin, direction);
            return;
        }
        const std::optional<glm::vec3> point = surface_point(origin, direction);
        place_asset(guid, point.value_or(origin + direction * 10.0f));
    };
    import_queue_.on_finished = [this](const ImportQueue::Item& item) {
        if (item.state == ImportQueue::State::Done) {
            asset_imported(item.guid);
        }
        if (import_queue_.active() == 0) {
            start_asset_scan(); // refresh the import state of every asset
        }
    };
    editor_desc.scene_editor = &scene_editor_;
    editor_desc.op_stack = &op_stack_;
    editor_desc.on_property_edit = [this](const core::Guid& entity, const std::string& path,
                                          const nlohmann::json& value, bool continuing) {
        // Inspector edits go through entity.set_property, so they are undoable; the
        // edits of one drag (or one text field) merge into one history entry.
        scene_editor_.run("entity.set_property", {{"entity", entity.to_string()}, {"path", path}, {"value", value}},
                          continuing);
    };
    editor_desc.viewport_tools.transform = &transform_tool_;
    editor_desc.viewport_tools.on_pick = [this](const ViewportPick& pick) { pending_pick_ = pick; };
    editor_desc.viewport_tools.snap_point = [this](const glm::vec3& origin, const glm::vec3& direction,
                                                   float max_angle) {
        return snap_point(origin, direction, max_angle);
    };
    editor_desc.viewport_tools.surface_point = [this](const glm::vec3& origin, const glm::vec3& direction) {
        return surface_point(origin, direction);
    };
    editor_desc.viewport_tools.landscape = &landscape_tools_;
    editor_desc.viewport_tools.cameras = &camera_actors_;
    editor_desc.viewport_tools.navigation = &navigation_;
    editor_desc.viewport_tools.preview_renderer = [this]() -> render::raster::RasterEngine* {
        if (!preview_renderer_) {
            preview_renderer_ = std::make_unique<render::raster::RasterEngine>(*device_, *shaders_);
            preview_renderer_->set_residency(residency_.get());
            preview_renderer_->grid().visible = false; // what the camera sees, without editor helpers
        }
        // The same look as the viewport, but always lit.
        preview_renderer_->settings().tonemapper = viewport_renderer_->settings().tonemapper;
        preview_renderer_->settings().exposure = viewport_renderer_->settings().exposure;
        return preview_renderer_.get();
    };
    editor_desc.viewport_tools.pen_pressure = &pen_pressure_;
    editor_desc.landscape.sculpt = &sculpt_tool_;
    editor_desc.landscape.paint = &paint_tool_;
    editor_desc.landscape.foliage = &foliage_tool_;
    editor_desc.landscape.spline = &spline_tool_;
    editor_desc.landscape.water = &water_tool_;
    editor_desc.landscape.tools = &landscape_tools_;
    editor_desc.terrain_actions.pick_images = [this](std::function<void(std::vector<std::string>)> done) {
        show_image_dialog(std::move(done));
    };
    editor_desc.terrain_actions.add_scatter = [this](const std::string& preset) { add_scatter(preset); };
    editor_desc.scatter_actions.create = [this](const std::string& preset) { add_scatter(preset); };
    editor_desc.modeling_tool = &modeling_tool_;
    editor_desc.modeling_actions.add_shape = [this](modeling::Shape shape) { add_shape(shape); };
    editor_desc.modeling_actions.make_editable = [this] { make_editable(); };
    editor_desc.terrain_actions.erode = [this](const terrain::ErosionSettings& settings) { start_erosion(settings); };
    editor_desc.terrain_actions.cancel_erosion = [this] {
        if (erosion_) {
            erosion_->token.cancel();
            erosion_.reset();
        }
    };
    editor_desc.terrain_actions.erosion_progress = [this]() -> std::optional<float> {
        return erosion_ ? std::optional<float>(erosion_->progress.load()) : std::nullopt;
    };
    editor_desc.viewport_tools.measure = &measure_;
    editor_desc.viewport_tools.measuring = &measuring_;
    editor_desc.console = console_;
    editor_desc.commands = &commands_;
    editor_desc.preferences = &preferences_;
    editor_desc.view_menu = &view_menu_;
    editor_desc.on_preferences_changed = [this](bool save) {
        apply_preferences();
        if (save) {
            save_user_preferences();
        }
    };
    editor_desc.on_keymap_changed = [this] { save_user_preferences(); };
    editor_desc.build_default_layout = !imgui_->loaded_saved_layout();
    ui_ = std::make_unique<EditorUi>(editor_desc);
    view_menu_.select_preset = [this](const std::string& name) {
        if (select_navigation_preset(name)) {
            preferences_.navigation_preset = name;
            save_user_preferences();
        }
    };
    commands_.apply_keymap_overrides(preferences_.keymap); // after every command is registered
    for (const auto& group : commands_.conflicts()) {
        F3D_LOG_WARN("Keymap conflict: {} share {}", fmt::join(group, ", "), to_string(*commands_.chord(group[0])));
    }
    apply_preferences();

    vfs_.mount("engine", std::make_shared<core::vfs::DirectoryBackend>(input_preset_dir().parent_path(), false));
    if (const std::string pref = user_pref_dir(); !pref.empty() && !is_automated_run()) {
        document_.set_untitled_autosave_dir(std::filesystem::path(pref) / "autosave");
    }
    if (!options_.open.empty()) {
        const std::filesystem::path path(options_.open);
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!(ext == ".forge" ? open_project(path) : import_model(path, true))) {
            return false;
        }
    } else {
        std::error_code ec;
        if (const std::filesystem::path startup = startup_scene_path();
            !startup.empty() && std::filesystem::exists(startup, ec)) {
            new_scene(); // the user's defaults (sky, lights, fog, camera, ...)
        }
        check_untitled_recovery();
    }
    if (!options_.scene.empty()) {
        const ops::ScriptResult result = ops::run_script_file(op_stack_, op_registry_, options_.scene);
        if (!result.ok) {
            F3D_LOG_ERROR("--scene {}: {}", options_.scene, result.error);
            return false;
        }
        F3D_LOG_INFO("Ran {} operations from {}", result.executed, options_.scene);
        apply_view_request();
    }
    start_rpc();
    return true;
}

void App::shutdown() {
    scan_token_.cancel();
    if (erosion_) {
        erosion_->token.cancel();
    }
    watch_token_.cancel();
    import_queue_.cancel_all();
    jobs_.wait_idle();
    thumbnails_.reset(); // GPU textures: before the device
    thumbnail_renderer_.reset();
    asset_index_ptr_ = nullptr;
    asset_index_.reset();
    if (device_) {
        device_->context().wait_idle();
    }
    ui_.reset();
    imgui_.reset();
    material_preview_.reset();
    rpc_.reset(); // before what its calls use
#if defined(F3D_WITH_PYTHON)
    python_.reset();
#endif
    capturer_.reset();
    viewport_renderer_.reset();
    residency_.reset(); // GPU buffers and textures: before the device
    shaders_.reset();
    swapchain_.reset();
    device_.reset();
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    if (sdl_initialized_) {
        SDL_Quit();
        sdl_initialized_ = false;
    }
}

void App::route_navigation_input(const SDL_Event& event) {
    // Navigation starts over the viewport image; a drag keeps going outside it until released.
    const bool over_viewport = ui_ && ui_->viewport().hovered();
    const bool typing = ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;
    const bool flying = navigation_.flight() != FlightMode::None;
    InputEvent in;
    switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (!over_viewport && !flying) {
            return;
        }
        in.type = InputEventType::MouseDown;
        in.button = button_from(event.button.button);
        in.position = {event.button.x, event.button.y};
        in.modifiers = modifiers_from(SDL_GetModState());
        if (flying) {
            navigation_.handle(in, camera_); // a click keeps the view, a right-click goes back
            return;
        }
        if (landscape_tools_.active() && in.button == MouseButton::Left && !in.modifiers.alt) {
            return; // a landscape tool's drag (Alt+drag still navigates)
        }
        if (in.button == MouseButton::Right && ui_->viewport().cancellable()) {
            return; // the right button cancels what is in progress (the viewport does it)
        }
        if (navigation_.handle(in, camera_)) {
            SDL_CaptureMouse(true);
        }
        return;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        in.type = InputEventType::MouseUp;
        in.button = button_from(event.button.button);
        in.position = {event.button.x, event.button.y};
        in.modifiers = modifiers_from(SDL_GetModState());
        if (navigation_.handle(in, camera_) && !navigation_.dragging()) {
            SDL_CaptureMouse(false);
        }
        return;
    case SDL_EVENT_MOUSE_MOTION:
        if (!navigation_.dragging() && !flying) {
            return;
        }
        in.type = InputEventType::MouseMove;
        in.position = {event.motion.x, event.motion.y};
        in.motion = {event.motion.xrel, event.motion.yrel};
        in.modifiers = modifiers_from(SDL_GetModState());
        navigation_.handle(in, camera_);
        return;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!over_viewport && !flying) {
            return;
        }
        if (const ViewportTool* tool = landscape_tools_.active(); tool && tool->busy() && !flying) {
            return; // the tool takes the wheel (e.g. the number of loop cuts)
        }
        in.type = InputEventType::Wheel;
        in.wheel = event.wheel.y;
        in.modifiers = modifiers_from(SDL_GetModState());
        navigation_.handle(in, camera_);
        return;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if ((typing && !flying) || (!over_viewport && !navigation_.dragging() && !flying) ||
            (event.key.repeat && event.type == SDL_EVENT_KEY_DOWN)) {
            return;
        }
        in.type = event.type == SDL_EVENT_KEY_DOWN ? InputEventType::KeyDown : InputEventType::KeyUp;
        in.key = key_name(event.key.key);
        in.modifiers = modifiers_from(event.key.mod);
        navigation_.handle(in, camera_);
        return;
    default:
        return;
    }
}

bool App::handle_shortcut(const SDL_Event& event) {
    if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat || !ui_) {
        return false;
    }
    const KeyChord chord{key_name(event.key.key), modifiers_from(event.key.mod)};
    if (is_modifier_key(chord.key)) {
        return false;
    }
    if (ui_->keymap().capturing()) {
        ui_->keymap().capture(chord);
        return true;
    }
    if (ImGui::GetIO().WantTextInput) {
        return false; // typing into a text field
    }
    if (navigation_.dragging() || navigation_.flight() != FlightMode::None) {
        return false; // e.g. Unreal's RMB + WASD fly mode, or walk / fly mode
    }
    // The active viewport tool first: modeling's E / I / Ctrl+R, and values typed during a
    // modal edit (which swallows every other key until it ends).
    if (ViewportTool* tool = landscape_tools_.active();
        tool && !tool->scripted && (tool->busy() || ui_->viewport().hovered()) &&
        tool->key_pressed(chord.key, chord.modifiers.ctrl, chord.modifiers.shift, chord.modifiers.alt)) {
        return true;
    }
    // Blender's Tab over the viewport: in and out of face editing ("edit mode").
    if (chord.key == "tab" && chord.modifiers == Modifiers{} && ui_->viewport().hovered()) {
        if (landscape_tools_.active() == &modeling_tool_) {
            landscape_tools_.activate(nullptr);
            return true;
        }
        if (!modeling_tool_.target().is_null()) {
            commands_.execute("modeling.select_faces");
            return true;
        }
        if (const entt::entity e = world_.find(world_.selection().active());
            e != entt::null && world_.registry().all_of<scene::MeshRenderer>(e)) {
            F3D_LOG_INFO("Tab: make the mesh editable first (Tools > Modeling > Make Editable)");
            return true;
        }
    }
    if (const Command* command = commands_.find_by_chord(chord)) {
        commands_.execute(command->id);
        return true;
    }
    return false;
}

bool App::pump_events() {
    F3D_ZONE;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // In walk / fly mode the mouse and keys steer the camera only (the hidden cursor
        // must not click the UI).
        const bool input = event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                           event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL ||
                           event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP;
        if (imgui_ && !(input && navigation_.flight() != FlightMode::None)) {
            imgui_->process_event(event);
        }
        const bool shortcut = handle_shortcut(event);
        if (!replay_ && !shortcut) {
            route_navigation_input(event);
        }
        switch (event.type) {
        case SDL_EVENT_PEN_AXIS:
            if (event.paxis.axis == SDL_PEN_AXIS_PRESSURE) {
                pen_pressure_ = std::clamp(event.paxis.value, 0.0f, 1.0f);
            }
            break;
        case SDL_EVENT_PEN_PROXIMITY_OUT:
            pen_pressure_ = 1.0f; // back to the mouse
            break;
        case SDL_EVENT_DROP_FILE:
            if (event.drop.data && event.drop.windowID == SDL_GetWindowID(window_)) {
                handle_file_drop(event.drop.data, {event.drop.x, event.drop.y});
            }
            break;
        case SDL_EVENT_QUIT:
            request(PendingAction::Quit);
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (event.window.windowID == SDL_GetWindowID(window_)) {
                request(PendingAction::Quit); // detached ImGui windows are closed by ImGui itself
            }
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            if (event.window.windowID == SDL_GetWindowID(window_)) {
                minimized_ = true;
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (event.window.windowID == SDL_GetWindowID(window_)) {
                navigation_.end_flight(true, camera_); // alt-tabbed away: keep where it got to
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            // Back from another application: pick up assets added or removed meanwhile
            // (edits to known sources are caught by the source watcher already).
            if (event.window.windowID == SDL_GetWindowID(window_) && !is_automated_run()) {
                start_asset_scan();
            }
            break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
            if (event.window.windowID == SDL_GetWindowID(window_)) {
                minimized_ = false;
            }
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            if (swapchain_ && event.window.windowID == SDL_GetWindowID(window_)) {
                swapchain_->resize(static_cast<std::uint32_t>(event.window.data1),
                                   static_cast<std::uint32_t>(event.window.data2));
            }
            break;
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            if (event.window.windowID == SDL_GetWindowID(window_)) {
                content_scale_ = SDL_GetWindowDisplayScale(window_);
                if (preferences_.ui_scale == 0.0f) { // automatic scale follows the display
                    imgui_->set_scale(content_scale_);
                }
                F3D_LOG_INFO("Display scale changed to {:.2f}", content_scale_);
            }
            break;
        default:
            break;
        }
    }
    if (ui_ && ui_->quit_requested()) {
        ui_->clear_quit_request();
        request(PendingAction::Quit);
    }
    return !quit_confirmed_;
}

void App::deliver_replay_events() {
    if (!replay_) {
        return;
    }
    navigation_.set_viewport_size(replay_->viewport);
    while (next_replay_event_ < replay_->events.size() && replay_->events[next_replay_event_].frame <= frame_index_) {
        const InputEvent& event = replay_->events[next_replay_event_].event;
        ++next_replay_event_;
        if (sculpt_tool_.scripted) {
            // The left button and the cursor drive the sculpt tool.
            const bool left = event.button == MouseButton::Left;
            if (event.type == InputEventType::MouseDown && left) {
                replay_sculpt_pressed_ = true;
                replay_sculpt_position_ = event.position;
                continue;
            }
            if (event.type == InputEventType::MouseUp && left) {
                replay_sculpt_pressed_ = false;
                replay_sculpt_position_ = event.position;
                continue;
            }
            if (event.type == InputEventType::MouseMove) {
                replay_sculpt_position_ = event.position;
                if (replay_sculpt_pressed_) {
                    continue;
                }
            }
        }
        navigation_.handle(event, camera_);
    }
}

void App::start_erosion(const terrain::ErosionSettings& settings) {
    if (erosion_) {
        return; // one at a time
    }
    // The selected terrain, else the first one in the scene.
    entt::entity target = world_.find(world_.selection().active());
    if (target == entt::null || !world_.registry().all_of<terrain::Terrain>(target)) {
        const auto terrains = world_.registry().view<terrain::Terrain>();
        target = terrains.empty() ? entt::null : terrains.front();
    }
    const std::shared_ptr<terrain::Heightfield> hf =
        target == entt::null ? nullptr : terrain::heightfield_of(world_, target);
    if (!hf) {
        F3D_LOG_WARN("Erosion: no terrain in the scene");
        return;
    }
    const terrain::Terrain t = world_.registry().get<terrain::Terrain>(target);
    const core::Guid guid = world_.guid(target);
    const std::uint64_t revision = hf->revision();
    const std::uint32_t samples = hf->samples();
    const float spacing = hf->spacing();
    auto job = std::make_shared<ErosionJob>();
    erosion_ = job;
    F3D_LOG_INFO("Erosion: {} iterations on {} ({}x{} samples)", settings.iterations, world_.name(target), samples,
                 samples);
    jobs_.submit(
        core::jobs::Priority::Background, job->token,
        [job, settings, t, samples, spacing, heights = hf->heights()](const core::jobs::CancellationToken& token) mutable {
            bool done = false;
            try {
                done = terrain::erode(heights, samples, samples, spacing, settings, t.min_height, t.max_height,
                                      [&](float fraction) {
                                          job->progress = fraction;
                                          return !token.cancelled();
                                      });
            } catch (const std::bad_alloc&) {
                F3D_LOG_ERROR("Erosion: not enough memory");
            }
            return done ? std::optional<std::vector<float>>(std::move(heights)) : std::nullopt;
        },
        [this, job, guid, settings, revision](std::optional<std::vector<float>> result) {
            if (erosion_ == job) {
                erosion_.reset();
            }
            if (result) {
                // If the heights changed meanwhile, the operation erodes again (synchronously).
                op_stack_.execute(terrain::make_erode_op(guid, settings, std::move(*result), revision));
            }
        });
}

void App::update_scripted_sculpt() {
    if (!sculpt_tool_.scripted || !replay_ || !ui_) {
        return;
    }
    const ViewportPanel& viewport = ui_->viewport();
    if (viewport.width() == 0 || viewport.height() == 0) {
        return;
    }
    // Replay positions are logical pixels of the recorded viewport size.
    const glm::vec2 scale = glm::vec2(static_cast<float>(viewport.width()), static_cast<float>(viewport.height())) /
                            glm::max(replay_->viewport, glm::vec2(1.0f));
    glm::vec3 origin, direction;
    viewport.view().pixel_ray(replay_sculpt_position_.x * scale.x, replay_sculpt_position_.y * scale.y, origin,
                              direction);
    sculpt_tool_.update(origin, direction, replay_sculpt_pressed_, 1.0f);
}

void App::update(double dt_seconds) {
    F3D_ZONE;
    update_document();
    jobs_.run_continuations();
    import_queue_.update();
    update_pending_drops(dt_seconds);
    watch_asset_sources(dt_seconds);
    thumbnails_->update();
    if (pending_pick_) {
        const ViewportPick pick = *pending_pick_;
        pending_pick_.reset();
        process_pick(pick);
    }
    apply_view_request();
    shader_poll_timer_ += dt_seconds;
    if (shaders_->hot_reload_enabled() && shader_poll_timer_ >= 0.5) {
        shader_poll_timer_ = 0.0;
        shaders_->poll();
    }
    if (!replay_) {
        navigation_.set_viewport_size(ui_->viewport().logical_size());
    }
    navigation_.update(static_cast<float>(dt_seconds), camera_);
    camera_actors_.update(camera_); // a piloted camera follows the view (and its preview with it)
    // Walk / fly mode looks with the mouse: hide and hold the cursor while it runs.
    if (const bool relative = navigation_.flight() != FlightMode::None;
        window_ && relative != SDL_GetWindowRelativeMouseMode(window_)) {
        SDL_SetWindowRelativeMouseMode(window_, relative);
    }
    update_scripted_sculpt();

    draw_selection_overlay();
    if (show_debug_shapes_) {
        core::debug::box(scene_bounds_.min, scene_bounds_.max, core::debug::kYellow);
        core::debug::sphere(glm::vec3(0.0f, 1.0f, 0.0f), 1.0f, core::debug::kWhite);
        core::debug::line(glm::vec3(0.0f), glm::vec3(0.0f, 3.0f, 0.0f), core::debug::kGreen);
    }
    ui_->viewport().set_stats_toggle(&show_stats_);
    if (options_.viewport_size) {
        ui_->viewport().set_forced_size(options_.viewport_size->first, options_.viewport_size->second);
    }
    if (navigation_.flight() != FlightMode::None) {
        ui_->viewport().set_overlay_text(
            std::string(navigation_.flight() == FlightMode::Walk ? "Walk" : "Fly") +
            fmt::format(" (speed x{:.2f}) · WASD move · Q/E down/up · Shift faster · wheel speed · Tab {} · "
                        "click keeps · right-click/Esc cancels",
                        navigation_.speed_scale(), navigation_.flight() == FlightMode::Walk ? "fly" : "walk"));
    } else {
        ui_->viewport().set_overlay_text(navigation_.input_map().name + " · " +
                                         (camera_.projection == Projection::Orthographic ? "Orthographic"
                                                                                         : "Perspective"));
    }
}

void App::capture(const rhi::Texture& source) {
    const core::Image image = device_->context().read_texture(source);
    if (!image.empty() && core::write_png(options_.capture, image)) {
        F3D_LOG_INFO("Captured {}x{} {} to {}", image.width, image.height, options_.capture_ui ? "window" : "viewport",
                     options_.capture);
    } else {
        F3D_LOG_ERROR("Could not capture to {}", options_.capture);
        capture_failed_ = true;
    }
}

void App::render(bool last_frame) {
    F3D_ZONE;
    rhi::CommandContext& ctx = device_->context();

    // 1. Build the UI (panels decide their sizes, e.g. the viewport target).
    imgui_->begin_frame(swapchain_->width(), swapchain_->height());
    ui_->draw();
    draw_prompts();

    // 2. Render the scene into the viewport target.
    ui_->viewport().render(ctx);

    // 3. Draw the UI into the back buffer.
    const rhi::Texture back_buffer = swapchain_->back_buffer();
    {
        rhi::ScopedGpuTimer timer(ctx, "frame");
        ctx.set_render_target(back_buffer);
        ctx.clear_color(back_buffer, kWindowClearColor);
        imgui_->render(ctx, back_buffer);
    }

    if (last_frame && !options_.capture.empty()) {
        capture(options_.capture_ui ? back_buffer : ui_->viewport().display_target());
    }
    imgui_->render_platform_windows();
    swapchain_->present(options_.vsync);
    residency_->end_frame(); // settle what the next frame has resident
}

void App::finish_bench() {
    BenchReport report;
    report.backend = std::string(rhi::to_string(device_->backend()));
    report.adapter = device_->adapter().name;
    report.width = swapchain_->width();
    report.height = swapchain_->height();
    report.viewport_width = ui_ ? ui_->viewport().width() : 0;
    report.viewport_height = ui_ ? ui_->viewport().height() : 0;
    report.vsync = options_.vsync;
    report.frames = stats_.frames();
    report.time_to_first_frame_ms = time_to_first_frame_ms_;
    report.cpu_frame_ms = stats_.cpu_summary();
    report.gpu_pass_ms = stats_.gpu_averages();
    const auto ram = core::process_memory();
    report.ram_bytes = ram.working_set;
    report.ram_peak_bytes = ram.peak_working_set;
    const auto vram = device_->memory_usage();
    report.vram_bytes = vram.vram_used;
    report.vram_budget_bytes = vram.vram_budget;
    const auto& streamed = residency_->stats();
    report.residency =
        BenchReport::Residency{streamed.budget, streamed.used, streamed.peak, streamed.meshes, streamed.textures};
    BenchReport::Camera cam;
    cam.position = camera_.position();
    cam.target = camera_.target;
    cam.yaw = camera_.yaw;
    cam.pitch = camera_.pitch;
    cam.distance = camera_.distance;
    cam.orthographic = camera_.projection == Projection::Orthographic;
    cam.navigation_preset = view_menu_.current_preset;
    report.camera = cam;
    report.terrain = BenchReport::Terrain{viewport_renderer_->last_terrain_nodes(), sculpt_tool_.strokes()};
    if (write_bench_report(options_.bench, report)) {
        F3D_LOG_INFO("Bench: {:.3f} ms avg, {:.3f} ms p95 over {} frames -> {}", report.cpu_frame_ms.average,
                     report.cpu_frame_ms.p95, report.cpu_frame_ms.count, options_.bench);
    } else {
        F3D_LOG_ERROR("Could not write bench report {}", options_.bench);
    }
}

int App::run() {
    if (!init()) {
        return 1;
    }

    using clock = std::chrono::steady_clock;
    auto previous = clock::now();
    while (true) {
        F3D_ZONE_N("Frame");
        if (!pump_events()) {
            break;
        }
        if (rpc_) {
            rpc_->poll(); // RPC calls run here, on the main thread (also while minimised)
        }
        if (minimized_) {
            SDL_WaitEventTimeout(nullptr, 100); // nothing to render; don't spin
            previous = clock::now();
            continue;
        }
        const auto now = clock::now();
        // Replays use a fixed time step so continuous navigation (fly mode) is deterministic.
        const double dt = replay_ ? 1.0 / 60.0 : std::chrono::duration<double>(now - previous).count();
        previous = now;

        const bool last = options_.frames >= 0 && frame_index_ + 1 >= static_cast<std::uint64_t>(options_.frames);
        deliver_replay_events();
        update(dt);
        render(last);
        ++frame_index_;
        if (frame_index_ == 1) {
            time_to_first_frame_ms_ = core::seconds_since_process_start() * 1000.0;
            F3D_LOG_INFO("Time to first frame: {:.0f} ms", time_to_first_frame_ms_);
        }
        stats_.add_frame(std::chrono::duration<double, std::milli>(clock::now() - now).count(), device_->gpu_timings());
        F3D_FRAME_MARK;
        if (last || options_.frames == 0) {
            break;
        }
    }

    if (replay_ && next_replay_event_ < replay_->events.size()) {
        F3D_LOG_WARN("Replay stopped with {} events not delivered (increase --frames)",
                     replay_->events.size() - next_replay_event_);
    }
    if (!options_.bench.empty()) {
        finish_bench();
    }
    F3D_LOG_INFO("Exiting after {} frames", frame_index_);
    return capture_failed_ ? 1 : 0;
}

} // namespace f3d::editor
