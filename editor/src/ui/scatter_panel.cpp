// SPDX-License-Identifier: Apache-2.0
#include "scatter_panel.hpp"

#include "f3d/editor/foliage_tool.hpp"
#include "f3d/editor/scene_editor.hpp"
#include "f3d/editor/viewport_tool.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/terrain.hpp"
#include "f3d/world/scatter.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace f3d::editor {
namespace {

nlohmann::json ref_json(const core::AssetRef& ref) {
    return {{"guid", ref.guid.is_null() ? std::string() : ref.guid.to_string()}, {"path", ref.path}};
}

nlohmann::json meshes_json(const std::vector<core::AssetRef>& meshes) {
    nlohmann::json out = nlohmann::json::array();
    for (const core::AssetRef& ref : meshes) {
        out.push_back(ref_json(ref));
    }
    return out;
}

constexpr std::array<std::pair<const char*, const char*>, 4> kBuiltins = {{
    {"Pine", world::kBuiltinPine},
    {"Broadleaf tree", world::kBuiltinBroadleaf},
    {"Grass tuft", world::kBuiltinGrass},
    {"Rock", world::kBuiltinRock},
}};

/// A short label for a mesh reference: the built-in's name, else the path's tail.
std::string mesh_label(const core::AssetRef& ref) {
    for (const auto& [name, path] : kBuiltins) {
        if (ref.path == path) {
            return std::string(name) + " (built-in)";
        }
    }
    const std::size_t slash = ref.path.find_last_of("/\\");
    return slash == std::string::npos ? ref.path : ref.path.substr(slash + 1);
}

} // namespace

ScatterPanel::ScatterPanel(SceneEditor* scene_editor, ScatterActions actions, FoliageTool* foliage,
                           LandscapeTools* tools)
    : Panel(panel_ids::kScatter, "Scatter"), scene_editor_(scene_editor), actions_(std::move(actions)),
      foliage_(foliage), tools_(tools) {
    set_open(false); // Window menu, the command palette, or a new scatter
}

void ScatterPanel::draw_contents() {
    if (!scene_editor_) {
        return;
    }
    draw_create();
    ImGui::Separator();
    draw_active();
}

void ScatterPanel::draw_create() {
    if (!actions_.create) {
        return;
    }
    ImGui::TextUnformatted("New scatter");
    for (const world::ScatterPreset& preset : world::scatter_presets()) {
        if (ImGui::Button(preset.name)) {
            actions_.create(preset.id);
        }
        ImGui::SetItemTooltip("Adds a ready-made %s scatter using the built-in meshes", preset.name);
        ImGui::SameLine();
    }
    if (ImGui::Button("Selected meshes")) {
        actions_.create("selected");
    }
    ImGui::SetItemTooltip("Scatters the meshes of the selected objects over the terrain");
}

void ScatterPanel::draw_active() {
    scene::World& world = scene_editor_->world();
    const core::Guid guid = world.selection().active();
    const entt::entity e = guid.is_null() ? entt::null : world.find(guid);
    const auto* scatter = e == entt::null ? nullptr : world.registry().try_get<world::Scatter>(e);
    if (!scatter) {
        ImGui::TextDisabled("Select a scatter to edit it (more settings in the Inspector).");
        return;
    }
    const std::string id = guid.to_string();
    const auto set = [&](const char* field, const nlohmann::json& value) {
        scene_editor_->run("entity.set_property",
                           {{"entity", id}, {"path", std::string("Scatter.") + field}, {"value", value}});
    };
    const world::Scatter settings = *scatter; // edits below may replace the component
    ImGui::Text("%s: %zu instances", world.name(e).c_str(), world::scatter_instances(world, e).size());
    if (scatter->meshes.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Add a mesh to scatter.");
    }

    // Surface: a terrain or flat ground.
    std::vector<std::pair<core::Guid, std::string>> terrains;
    for (const entt::entity t : world.registry().view<terrain::Terrain>()) {
        terrains.emplace_back(world.guid(t), world.name(t));
    }
    const entt::entity surface = settings.terrain.is_null() ? entt::null : world.find(settings.terrain);
    const std::string current = surface == entt::null ? "Flat ground" : world.name(surface);
    const bool surface_open = ImGui::BeginCombo("Surface", current.c_str());
    ImGui::SetItemTooltip("What the instances stand on: a terrain, or flat ground at the scatter's height");
    if (surface_open) {
        if (ImGui::Selectable("Flat ground", surface == entt::null)) {
            set("terrain", nullptr);
        }
        for (const auto& [terrain_guid, name] : terrains) {
            ImGui::PushID(terrain_guid.to_string().c_str());
            if (ImGui::Selectable(name.c_str(), terrain_guid == settings.terrain)) {
                set("terrain", terrain_guid.to_string());
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (surface != entt::null) {
        if (ImGui::Button("Fit to terrain")) {
            const glm::vec3 origin = terrain::terrain_origin(world, surface);
            const float size = world.registry().get<terrain::Terrain>(surface).size;
            scene_editor_->stack().begin_transaction("Fit scatter to terrain");
            const bool ok = scene_editor_->run("entity.set_property", {{"entity", id},
                                                                       {"path", "Transform.translation"},
                                                                       {"value", {origin.x, origin.y, origin.z}}}) &&
                            scene_editor_->run("entity.set_property",
                                               {{"entity", id}, {"path", "Scatter.area"}, {"value", {size, size}}});
            if (ok) {
                scene_editor_->stack().end_transaction();
            } else {
                scene_editor_->stack().abort_transaction();
            }
        }
        ImGui::SetItemTooltip("Centre the scatter on the terrain and cover all of it");
        ImGui::SameLine();
    }
    if (ImGui::Button("Reseed")) {
        set("seed", settings.seed + 1);
    }
    ImGui::SetItemTooltip("A new random layout with the same settings");
    draw_paint(guid, settings.paint_only);

    // Meshes.
    ImGui::SeparatorText("Meshes");
    ImGui::TextDisabled("Each instance uses one of these, picked at random.");
    std::vector<core::AssetRef> meshes = settings.meshes;
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton("x")) {
            std::vector<core::AssetRef> rest = meshes;
            rest.erase(rest.begin() + static_cast<std::ptrdiff_t>(i));
            set("meshes", meshes_json(rest));
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(mesh_label(meshes[i]).c_str());
        ImGui::SetItemTooltip("%s", meshes[i].path.c_str());
        ImGui::PopID();
    }
    const auto add_mesh = [&](const core::AssetRef& ref) {
        if (!ref.empty() && std::find(meshes.begin(), meshes.end(), ref) == meshes.end()) {
            meshes.push_back(ref);
            set("meshes", meshes_json(meshes));
        }
    };
    const bool builtin_open = ImGui::BeginCombo("##add_builtin", "Add built-in mesh...");
    ImGui::SetItemTooltip("Add a placeholder tree, grass or rock mesh to the random mix");
    if (builtin_open) {
        for (const auto& [name, path] : kBuiltins) {
            if (ImGui::Selectable(name)) {
                add_mesh({core::Guid{}, path});
            }
        }
        ImGui::EndCombo();
    }
    const bool scene_open = ImGui::BeginCombo("##add_scene", "Add mesh of a scene object...");
    ImGui::SetItemTooltip("Add the mesh of an object already in the scene to the random mix");
    if (scene_open) {
        std::vector<core::AssetRef> listed;
        for (const entt::entity other : world.depth_first()) {
            const auto* renderer = world.registry().try_get<scene::MeshRenderer>(other);
            if (!renderer || renderer->mesh.empty() ||
                std::find(listed.begin(), listed.end(), renderer->mesh) != listed.end()) {
                continue;
            }
            listed.push_back(renderer->mesh);
            ImGui::PushID(static_cast<int>(listed.size()));
            if (ImGui::Selectable(world.name(other).c_str())) {
                add_mesh(renderer->mesh);
            }
            ImGui::SetItemTooltip("%s", renderer->mesh.path.c_str());
            ImGui::PopID();
        }
        if (listed.empty()) {
            ImGui::TextDisabled("No objects with a mesh (place a model first)");
        }
        ImGui::EndCombo();
    }
}

void ScatterPanel::draw_paint(const core::Guid& scatter, bool paint_only) {
    if (!foliage_ || !tools_) {
        return;
    }
    ImGui::SeparatorText("Paint");
    FoliageTool& tool = *foliage_;
    bool painting = tool.enabled && tool.current_scatter() == scatter;
    if (ImGui::Checkbox("Paint in the viewport", &painting)) {
        tool.target = scatter;
        tools_->activate(painting ? foliage_ : nullptr);
    }
    ImGui::SetItemTooltip("Left-drag over the ground to add instances (Erase: remove them). Right-click or "
                          "Escape cancels a stroke; Escape again stops painting");
    bool only = paint_only;
    if (ImGui::Checkbox("Painted only", &only)) {
        scene_editor_->run("entity.set_property",
                           {{"entity", scatter.to_string()}, {"path", "Scatter.paint_only"}, {"value", only}});
    }
    ImGui::SetItemTooltip("On: instances appear only where you paint. Off: everywhere the rules allow, and the "
                          "brush thins or removes them");
    ImGui::Checkbox("Erase", &tool.erase_instances);
    ImGui::SliderFloat("Brush radius (m)", &tool.brush.radius, 0.5f, 500.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Brush strength", &tool.brush.strength, 0.0f, 1.0f, "%.2f");
    if (painting && paint_only && !tool.erase_instances) {
        ImGui::TextDisabled("Drag over the ground in the viewport.");
    }
}

} // namespace f3d::editor
