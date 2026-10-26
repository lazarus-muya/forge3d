// SPDX-License-Identifier: Apache-2.0
#include "terrain_panel.hpp"

#include "asset_browser.hpp"
#include "f3d/core/reflect.hpp"
#include "f3d/editor/foliage_tool.hpp"
#include "f3d/editor/paint_tool.hpp"
#include "f3d/editor/scene_editor.hpp"
#include "f3d/editor/sculpt_tool.hpp"
#include "f3d/editor/spline_tool.hpp"
#include "f3d/editor/water_tool.hpp"
#include "f3d/editor/viewport_tool.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/builtin_textures.hpp"
#include "f3d/terrain/layers.hpp"
#include "reflect_widgets.hpp"
#include "f3d/world/scatter.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/world/spline.hpp"
#include "f3d/world/water.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace f3d::editor {
namespace {

constexpr std::array<const char*, 8> kTools = {"Raise", "Lower",  "Smooth", "Flatten",
                                               "Terrace", "Noise", "Stamp",  "Erode"};
constexpr std::array<const char*, 4> kFalloffs = {"Smooth", "Linear", "Sphere", "Constant"};
constexpr std::array<const char*, 3> kErosionModes = {"Hydraulic", "Thermal", "Both"};

template <class E, std::size_t N>
void enum_combo(const char* label, E& value, const std::array<const char*, N>& names) {
    int index = static_cast<int>(value);
    if (ImGui::Combo(label, &index, names.data(), static_cast<int>(N))) {
        value = static_cast<E>(index);
    }
}

/// The file name of a texture reference, for display.
std::string ref_label(const core::AssetRef& ref) {
    if (ref.empty()) {
        return "(none)";
    }
    if (ref.path.starts_with(terrain::kBuiltinTexturePrefix)) {
        return "built-in " + ref.path.substr(terrain::kBuiltinTexturePrefix.size());
    }
    if (!ref.path.empty()) {
        const std::size_t slash = ref.path.find_last_of("/\\");
        return slash == std::string::npos ? ref.path : ref.path.substr(slash + 1);
    }
    return "asset " + ref.guid.to_string().substr(0, 8);
}

/// The images next to `path` (same folder), for finding the rest of a texture set.
std::vector<std::string> siblings_of(const std::string& path) {
    std::vector<std::string> out;
    std::error_code ec;
    const std::filesystem::path folder = std::filesystem::path(path).parent_path();
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (entry.is_regular_file(ec)) {
            out.push_back(entry.path().generic_string());
        }
    }
    return out;
}

/// Continuing an edit of the widget just drawn (not its first frame).
bool continuing() {
    return !ImGui::IsItemActivated();
}

} // namespace

TerrainPanel::TerrainPanel(SceneEditor* scene_editor, LandscapeToolSet tools, TerrainActions actions)
    : Panel(panel_ids::kTerrain, "Landscape"), scene_editor_(scene_editor), tools_(tools),
      actions_(std::move(actions)) {
    set_open(false); // Window menu
}

entt::entity TerrainPanel::target_terrain() const {
    if (!scene_editor_) {
        return entt::null;
    }
    scene::World& world = scene_editor_->world();
    const core::Guid active = world.selection().active();
    if (!active.is_null()) {
        const entt::entity e = world.find(active);
        if (e != entt::null && world.registry().all_of<terrain::Terrain>(e)) {
            return e;
        }
    }
    const auto terrains = world.registry().view<terrain::Terrain>();
    return terrains.begin() == terrains.end() ? entt::null : *terrains.begin(); // the first one
}

void TerrainPanel::tool_switch(const char* label, ViewportTool* tool, const char* tooltip) {
    if (!tool) {
        return;
    }
    const bool on = tool->enabled;
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.62f, 0.2f, 1.0f));
    }
    const std::string text = std::string(on ? "Stop " : "") + label;
    if (ImGui::Button(text.c_str(), ImVec2(-1.0f, 0.0f))) {
        if (tools_.tools) {
            tools_.tools->toggle(tool);
        } else {
            tool->enabled = !tool->enabled;
        }
    }
    if (on) {
        ImGui::PopStyleColor(2);
    }
    ImGui::SetItemTooltip("%s", tooltip);
}

void TerrainPanel::draw_contents() {
    if (!scene_editor_) {
        return;
    }
    const entt::entity terrain = target_terrain();
    scene::World& world = scene_editor_->world();
    if (terrain != entt::null) {
        ImGui::TextDisabled("Terrain: %s", world.name(terrain).c_str());
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "No terrain yet: create one below.");
    }
    if (!ImGui::BeginTabBar("##landscape_tabs")) {
        return;
    }
    if (ImGui::BeginTabItem("Sculpt")) {
        draw_sculpt_tab();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Paint")) {
        if (terrain != entt::null) {
            draw_paint_tab(terrain);
        } else {
            ImGui::TextDisabled("Create a terrain first (Sculpt tab).");
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Foliage")) {
        draw_foliage_tab(terrain);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Water")) {
        draw_water_tab(terrain);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Splines")) {
        draw_spline_tab(terrain);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

// --- Sculpt ------------------------------------------------------------------------------

void TerrainPanel::draw_sculpt_tab() {
    draw_create();
    ImGui::Separator();
    draw_brush();
    ImGui::Separator();
    draw_erosion();
}

void TerrainPanel::draw_erosion() {
    if (!ImGui::CollapsingHeader("Erosion") || !actions_.erode) {
        return;
    }
    const std::optional<float> running = actions_.erosion_progress ? actions_.erosion_progress() : std::nullopt;
    ImGui::BeginDisabled(running.has_value());
    enum_combo("Mode", erosion_.mode, kErosionModes);
    int iterations = static_cast<int>(erosion_.iterations);
    if (ImGui::SliderInt("Iterations", &iterations, 1, 2000, "%d", ImGuiSliderFlags_Logarithmic)) {
        erosion_.iterations = static_cast<std::uint32_t>(std::max(iterations, 1));
    }
    int seed = static_cast<int>(erosion_.seed);
    if (ImGui::InputInt("Seed##erosion", &seed)) {
        erosion_.seed = static_cast<std::uint32_t>(std::max(seed, 0));
    }
    if (erosion_.mode != terrain::ErosionMode::Thermal) {
        ImGui::SliderFloat("Rain", &erosion_.rain, 0.0f, 0.1f, "%.3f");
        ImGui::SliderFloat("Evaporation", &erosion_.evaporation, 0.0f, 0.2f, "%.3f");
        ImGui::SliderFloat("Capacity", &erosion_.capacity, 0.0f, 4.0f, "%.2f");
        ImGui::SliderFloat("Dissolving", &erosion_.dissolving, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Deposition", &erosion_.deposition, 0.0f, 1.0f, "%.2f");
    }
    if (erosion_.mode != terrain::ErosionMode::Hydraulic) {
        ImGui::SliderFloat("Talus angle", &erosion_.talus_angle, 5.0f, 80.0f, "%.1f deg");
        ImGui::SliderFloat("Thermal rate", &erosion_.thermal_rate, 0.0f, 1.0f, "%.2f");
    }
    if (ImGui::Button("Erode")) {
        actions_.erode(erosion_);
    }
    ImGui::SetItemTooltip("Erodes the selected terrain (or the first one) in the background; one undo step");
    ImGui::EndDisabled();
    if (running) {
        ImGui::ProgressBar(*running, ImVec2(-1.0f, 0.0f));
        if (ImGui::Button("Cancel") && actions_.cancel_erosion) {
            actions_.cancel_erosion();
        }
    }
}

void TerrainPanel::draw_create() {
    if (!ImGui::CollapsingHeader("New terrain", target_terrain() == entt::null ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        return;
    }
    ImGui::SliderInt("Tiles per side", &tiles_, 1, 32);
    ImGui::SetItemTooltip("256 quads per tile: 32 tiles = 8192 x 8192");
    ImGui::DragFloat("Size (m)", &size_, 8.0f, 16.0f, 1.0e5f, "%.0f");
    ImGui::DragFloat("Max height (m)", &max_height_, 1.0f, 1.0f, 1.0e4f, "%.0f");
    ImGui::InputInt("Seed", &seed_);
    if (ImGui::Button("Create terrain") && scene_editor_) {
        scene_editor_->run("terrain.create", {{"name", "Terrain"},
                                              {"tiles", std::clamp(tiles_, 1, 64)},
                                              {"size", std::max(size_, 1.0f)},
                                              {"max_height", std::max(max_height_, 1.0f)},
                                              {"seed", std::max(seed_, 0)}});
    }
    ImGui::SetItemTooltip("Noise hills with grass, rock and snow layers; sculpt and paint from here");
}

void TerrainPanel::draw_brush() {
    if (!tools_.sculpt) {
        return;
    }
    terrain::Brush& brush = tools_.sculpt->brush;
    tool_switch("Sculpt", tools_.sculpt, "Left-drag on a terrain to sculpt; Escape cancels the stroke; Alt+drag navigates");
    enum_combo("Tool", brush.tool, kTools);
    ImGui::SliderFloat("Radius (m)", &brush.radius, 0.5f, 1000.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Strength", &brush.strength, 0.0f, 1.0f, "%.2f");
    enum_combo("Falloff", brush.falloff, kFalloffs);
    ImGui::SliderFloat("Spacing", &brush.spacing, 0.05f, 1.0f, "%.2f");
    ImGui::SetItemTooltip("Distance between dabs, as a fraction of the radius");
    switch (brush.tool) {
    case terrain::BrushTool::Flatten:
        ImGui::Checkbox("Fixed height", &brush.use_target);
        ImGui::SetItemTooltip("Off: flatten to the height where the stroke starts");
        if (brush.use_target) {
            ImGui::DragFloat("Height (m)", &brush.target_height, 0.5f);
        }
        break;
    case terrain::BrushTool::Terrace:
        ImGui::DragFloat("Step (m)", &brush.terrace_step, 0.25f, 0.1f, 1000.0f, "%.2f");
        break;
    case terrain::BrushTool::Noise: {
        ImGui::DragFloat("Scale (m)", &brush.noise_scale, 0.5f, 0.5f, 1000.0f, "%.1f");
        int seed = static_cast<int>(brush.noise_seed);
        if (ImGui::InputInt("Seed##noise", &seed)) {
            brush.noise_seed = static_cast<std::uint32_t>(std::max(seed, 0));
        }
        break;
    }
    case terrain::BrushTool::Stamp: {
        if (std::strcmp(stamp_path_, brush.stamp.c_str()) != 0 && !ImGui::IsAnyItemActive()) {
            std::snprintf(stamp_path_, sizeof(stamp_path_), "%s", brush.stamp.c_str());
        }
        if (ImGui::InputText("Heightmap", stamp_path_, sizeof(stamp_path_))) {
            brush.stamp = stamp_path_;
        }
        ImGui::SetItemTooltip("An image (red channel = height), e.g. project://stamps/crater.png");
        if (actions_.pick_images) {
            ImGui::SameLine();
            if (ImGui::SmallButton("...##stamp")) {
                SculptTool* sculpt = tools_.sculpt;
                actions_.pick_images([sculpt](std::vector<std::string> paths) { sculpt->brush.stamp = paths.front(); });
            }
        }
        ImGui::DragFloat("Stamp height (m)", &brush.stamp_height, 0.5f, -1000.0f, 1000.0f, "%.1f");
        float degrees = brush.stamp_rotation * 57.29578f;
        if (ImGui::SliderFloat("Rotation", &degrees, -180.0f, 180.0f, "%.0f deg")) {
            brush.stamp_rotation = degrees / 57.29578f;
        }
        break;
    }
    default:
        break;
    }
    if (tools_.sculpt->enabled) {
        ImGui::TextDisabled("Pen pressure scales the strength.");
    }
}

// --- Paint -------------------------------------------------------------------------------

void TerrainPanel::set_layers(entt::entity terrain, const std::vector<terrain::TerrainLayer>& layers, bool merge) {
    nlohmann::json value;
    core::reflect::value_to_json(value, entt::forward_as_meta(layers));
    const std::string guid = scene_editor_->world().guid(terrain).to_string();
    scene_editor_->run("terrain.set_layers", {{"terrain", guid}, {"layers", value}}, merge);
}

void TerrainPanel::draw_paint_tab(entt::entity terrain) {
    scene::World& world = scene_editor_->world();
    const std::string guid = world.guid(terrain).to_string();
    std::vector<terrain::TerrainLayer> layers = world.registry().get<terrain::Terrain>(terrain).layers;
    if (layers.empty()) {
        layers.emplace_back(); // what the renderer draws without layers
    }
    selected_layer_ = std::clamp(selected_layer_, 0, static_cast<int>(layers.size()) - 1);

    // One click: a whole layer set with rules, baked.
    if (ImGui::BeginCombo("##auto_material", "Auto material...")) {
        for (const terrain::LandscapePreset& preset : terrain::landscape_presets()) {
            if (ImGui::Selectable(preset.name)) {
                scene_editor_->run("terrain.auto_material", {{"terrain", guid}, {"preset", preset.id}});
                selected_layer_ = 0;
            }
            ImGui::SetItemTooltip("%s (replaces the layers and their painting)", preset.description);
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Replace the layers with a ready-made textured set that places itself by height and slope");
    ImGui::SameLine();
    if (ImGui::Button("Apply rules")) {
        scene_editor_->run("terrain.apply_rules", {{"terrain", guid}});
    }
    ImGui::SetItemTooltip("Lay the layers out by their height and slope rules (replaces painting; undoable)");

    ImGui::SeparatorText("Layers");
    draw_layer_list(terrain, layers);

    // The brush paints the selected layer.
    ImGui::SeparatorText("Paint brush");
    if (tools_.paint) {
        PaintTool& paint = *tools_.paint;
        paint.layer = static_cast<std::uint32_t>(selected_layer_);
        tool_switch("Paint layer", tools_.paint,
                    "Left-drag on the terrain to paint the selected layer; Escape cancels; Alt+drag navigates");
        ImGui::Checkbox("Erase", &paint.erase_layer);
        ImGui::SetItemTooltip("Take the selected layer away (the layers around it fill in)");
        ImGui::SliderFloat("Radius (m)##paint", &paint.brush.radius, 0.5f, 500.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Strength##paint", &paint.brush.strength, 0.0f, 1.0f, "%.2f");
        enum_combo("Falloff##paint", paint.brush.falloff, kFalloffs);
    }

    ImGui::SeparatorText("Selected layer");
    draw_layer_details(terrain, layers);
}

void TerrainPanel::draw_layer_list(entt::entity terrain, std::vector<terrain::TerrainLayer>& layers) {
    const auto count = static_cast<int>(layers.size());
    const float row = ImGui::GetFrameHeight();
    // Top layer first, as they stack.
    for (int i = count - 1; i >= 0; --i) {
        const terrain::TerrainLayer& layer = layers[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        const glm::vec3 c = layer.color;
        ImGui::ColorButton("##swatch", ImVec4(linear_to_srgb(c.r), linear_to_srgb(c.g), linear_to_srgb(c.b), 1.0f),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(row, row));
        ImGui::SameLine();
        std::string label = layer.name;
        if (!layer.albedo_texture.empty()) {
            label += "  [textured]";
        }
        if (layer.rule.enabled) {
            label += "  [rule]";
        }
        if (i == 0) {
            label += "  (base)";
        }
        if (ImGui::Selectable(label.c_str(), selected_layer_ == i, 0, ImVec2(0.0f, row))) {
            selected_layer_ = i;
        }
        ImGui::PopID();
    }

    // Add, remove, reorder (the painted weights follow).
    const auto source_identity = [&](std::size_t n) {
        std::vector<std::int32_t> source(n);
        for (std::size_t i = 0; i < n; ++i) {
            source[i] = i < layers.size() ? static_cast<std::int32_t>(i) : -1;
        }
        return source;
    };
    const auto run_edit = [&](const std::vector<terrain::TerrainLayer>& next, const std::vector<std::int32_t>& source) {
        nlohmann::json value;
        core::reflect::value_to_json(value, entt::forward_as_meta(next));
        nlohmann::json src = nlohmann::json::array();
        for (const std::int32_t s : source) {
            src.push_back(s);
        }
        scene_editor_->run("terrain.set_layers", {{"terrain", scene_editor_->world().guid(terrain).to_string()},
                                                  {"layers", value},
                                                  {"source", src}});
    };
    const bool full = layers.size() >= terrain::kMaxLayers;
    ImGui::BeginDisabled(full);
    if (ImGui::Button("+ Add layer")) {
        ImGui::OpenPopup("##add_layer");
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##add_layer")) {
        const auto add = [&](terrain::TerrainLayer layer) {
            std::vector<terrain::TerrainLayer> next = layers;
            next.push_back(std::move(layer));
            run_edit(next, source_identity(next.size()));
            selected_layer_ = static_cast<int>(next.size()) - 1;
        };
        ImGui::TextDisabled("Built-in surfaces");
        for (const terrain::BuiltinSurface& surface : terrain::builtin_surfaces()) {
            if (ImGui::Selectable(surface.name)) {
                add(terrain::builtin_layer(surface.id));
            }
        }
        ImGui::Separator();
        if (actions_.pick_images && ImGui::Selectable("From image files...")) {
            const std::size_t index = layers.size();
            std::vector<terrain::TerrainLayer> next = layers;
            terrain::TerrainLayer layer;
            layer.name = "Layer " + std::to_string(index + 1);
            next.push_back(layer);
            run_edit(next, source_identity(next.size()));
            selected_layer_ = static_cast<int>(index);
            actions_.pick_images([this, terrain, index](std::vector<std::string> paths) {
                assign_images(terrain, index, paths, std::nullopt);
            });
        }
        ImGui::SetItemTooltip("Pick a colour map (its normal, roughness, AO and height maps are found by name), "
                              "or several maps at once");
        if (ImGui::Selectable("Plain colour")) {
            terrain::TerrainLayer layer;
            layer.name = "Layer " + std::to_string(layers.size() + 1);
            add(layer);
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const auto sel = static_cast<std::size_t>(selected_layer_);
    ImGui::BeginDisabled(layers.size() <= 1);
    if (ImGui::Button("Remove")) {
        std::vector<terrain::TerrainLayer> next;
        std::vector<std::int32_t> source;
        for (std::size_t i = 0; i < layers.size(); ++i) {
            if (i != sel) {
                next.push_back(layers[i]);
                source.push_back(static_cast<std::int32_t>(i));
            }
        }
        run_edit(next, source);
        selected_layer_ = std::max(selected_layer_ - 1, 0);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    const auto move = [&](std::size_t from, std::size_t to) {
        std::vector<terrain::TerrainLayer> next = layers;
        std::swap(next[from], next[to]);
        std::vector<std::int32_t> source = source_identity(next.size());
        std::swap(source[from], source[to]);
        run_edit(next, source);
        selected_layer_ = static_cast<int>(to);
    };
    ImGui::BeginDisabled(sel + 1 >= layers.size());
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
        move(sel, sel + 1);
    }
    ImGui::SetItemTooltip("Move up (drawn over the layers below)");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(sel == 0);
    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
        move(sel, sel - 1);
    }
    ImGui::SetItemTooltip("Move down");
    ImGui::EndDisabled();
}

void TerrainPanel::draw_texture_slot(entt::entity terrain, std::vector<terrain::TerrainLayer>& layers,
                                     std::size_t index, terrain::LayerTextureSlot slot, const char* label) {
    terrain::TerrainLayer& layer = layers[index];
    core::AssetRef& ref = terrain::layer_texture(layer, slot);
    ImGui::PushID(static_cast<int>(slot));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
    const float clear = ref.empty() ? 0.0f : ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    if (ImGui::Button(ref_label(ref).c_str(), ImVec2(ImGui::GetContentRegionAvail().x - clear, 0.0f)) &&
        actions_.pick_images) {
        actions_.pick_images([this, terrain, index, slot](std::vector<std::string> paths) {
            assign_images(terrain, index, paths, slot);
        });
    }
    ImGui::SetItemTooltip("%s\nClick to pick an image, or drop a texture from the asset browser",
                          ref.path.empty() ? "No texture" : ref.path.c_str());
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            if (const auto asset = core::Guid::parse(static_cast<const char*>(payload->Data))) {
                ref = core::AssetRef{*asset, {}};
                if (slot == terrain::LayerTextureSlot::Albedo) {
                    layer.color = glm::vec3(1.0f);
                }
                set_layers(terrain, layers);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (!ref.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.0f))) {
            ref = {};
            set_layers(terrain, layers);
        }
        ImGui::SetItemTooltip("Remove this texture");
    }
    ImGui::PopID();
}

void TerrainPanel::assign_images(entt::entity terrain, std::size_t index, const std::vector<std::string>& paths,
                                 std::optional<terrain::LayerTextureSlot> slot) {
    scene::World& world = scene_editor_->world();
    if (!world.registry().valid(terrain) || !world.registry().all_of<terrain::Terrain>(terrain) || paths.empty()) {
        return;
    }
    std::vector<terrain::TerrainLayer> layers = world.registry().get<terrain::Terrain>(terrain).layers;
    if (index >= layers.size()) {
        return;
    }
    terrain::TerrainLayer& layer = layers[index];
    const auto set = [&](terrain::LayerTextureSlot s, const std::string& path) {
        terrain::layer_texture(layer, s) = core::AssetRef{core::Guid{}, path};
    };
    bool albedo = false;
    if (paths.size() == 1 && slot && *slot != terrain::LayerTextureSlot::Albedo) {
        set(*slot, paths.front());
    } else if (paths.size() == 1) {
        // A colour map: the rest of its set is found next to it by name.
        const auto found = terrain::guess_texture_set(paths.front(), siblings_of(paths.front()));
        const auto own = terrain::guess_texture_slot(paths.front());
        for (std::uint32_t s = 0; s < terrain::kLayerTextureSlots; ++s) {
            if (!found[s].empty()) {
                set(static_cast<terrain::LayerTextureSlot>(s), found[s]);
            }
        }
        if (!own || *own == terrain::LayerTextureSlot::Albedo) {
            set(terrain::LayerTextureSlot::Albedo, paths.front());
        }
        albedo = true;
    } else {
        // Several maps: each by its name; a packed ARM map fills roughness and AO unless
        // they come separately; unnamed ones become the colour map.
        std::string packed;
        for (const std::string& path : paths) {
            if (terrain::is_packed_orm(path)) {
                packed = path;
                continue;
            }
            const auto s = terrain::guess_texture_slot(path).value_or(terrain::LayerTextureSlot::Albedo);
            set(s, path);
            albedo = albedo || s == terrain::LayerTextureSlot::Albedo;
        }
        for (const auto s : {terrain::LayerTextureSlot::Roughness, terrain::LayerTextureSlot::Ao}) {
            const bool separate = std::any_of(paths.begin(), paths.end(), [&](const std::string& path) {
                return terrain::guess_texture_slot(path) == s;
            });
            if (!packed.empty() && !separate) {
                set(s, packed);
            }
        }
    }
    if (albedo) {
        layer.color = glm::vec3(1.0f); // the texture's own colour
        if (layer.name.starts_with("Layer")) {
            layer.name = terrain::texture_set_name(paths.front());
        }
        layer_name_for_ = -1;
    }
    set_layers(terrain, layers);
}

void TerrainPanel::draw_layer_details(entt::entity terrain, std::vector<terrain::TerrainLayer>& layers) {
    const auto index = static_cast<std::size_t>(selected_layer_);
    terrain::TerrainLayer& layer = layers[index];
    ImGui::PushID("layer_details");
    if (layer_name_for_ != selected_layer_ || !ImGui::IsAnyItemActive()) {
        if (layer_name_for_ != selected_layer_ || std::strcmp(layer_name_, layer.name.c_str()) != 0) {
            std::snprintf(layer_name_, sizeof(layer_name_), "%s", layer.name.c_str());
            layer_name_for_ = selected_layer_;
        }
    }
    if (ImGui::InputText("Name", layer_name_, sizeof(layer_name_), ImGuiInputTextFlags_EnterReturnsTrue) ||
        ImGui::IsItemDeactivatedAfterEdit()) {
        if (layer.name != layer_name_) {
            layer.name = layer_name_;
            set_layers(terrain, layers);
        }
    }

    // Textures.
    draw_texture_slot(terrain, layers, index, terrain::LayerTextureSlot::Albedo, "Colour");
    draw_texture_slot(terrain, layers, index, terrain::LayerTextureSlot::Normal, "Normal");
    draw_texture_slot(terrain, layers, index, terrain::LayerTextureSlot::Roughness, "Roughness");
    draw_texture_slot(terrain, layers, index, terrain::LayerTextureSlot::Ao, "AO");
    draw_texture_slot(terrain, layers, index, terrain::LayerTextureSlot::Height, "Height");
    if (ImGui::BeginCombo("##surface", "Use a built-in surface...")) {
        for (const terrain::BuiltinSurface& surface : terrain::builtin_surfaces()) {
            if (ImGui::Selectable(surface.name)) {
                terrain::TerrainLayer replacement = terrain::builtin_layer(surface.id);
                replacement.rule = layer.rule; // keep where it is
                layer = replacement;
                set_layers(terrain, layers);
            }
        }
        ImGui::EndCombo();
    }

    // Look. Slider drags merge into one undo step.
    if (edit_linear_color(layer.albedo_texture.empty() ? "Colour" : "Tint", &layer.color.x, 3)) {
        set_layers(terrain, layers, continuing());
    }
    if (ImGui::SliderFloat("Tiling (m)", &layer.tiling, 0.1f, 256.0f, "%.2f", ImGuiSliderFlags_Logarithmic)) {
        set_layers(terrain, layers, continuing());
    }
    ImGui::SetItemTooltip("Size of one texture repeat on the ground (Ctrl+click to type).\n"
                          "Scanned sets give their real size: Poly Haven's are usually 1-4 m.");
    if (ImGui::SliderFloat("Anti-tiling", &layer.anti_tiling, 0.0f, 1.0f, "%.2f")) {
        set_layers(terrain, layers, continuing());
    }
    ImGui::SetItemTooltip("Hides the repeating grid: blends shifted copies of the texture across the ground.\n"
                          "0: plain repeats (a seamless texture tiles exactly); 1: the most variation");
    if (ImGui::Checkbox("Triplanar (cliffs)", &layer.triplanar)) {
        set_layers(terrain, layers);
    }
    ImGui::SetItemTooltip("Project the textures from three sides so steep ground isn't stretched");
    if (layer.roughness_texture.empty() &&
        ImGui::SliderFloat("Roughness", &layer.roughness, 0.0f, 1.0f, "%.2f")) {
        set_layers(terrain, layers, continuing());
    }
    if (!layer.normal_texture.empty()) {
        if (ImGui::SliderFloat("Normal strength", &layer.normal_strength, 0.0f, 4.0f, "%.2f")) {
            set_layers(terrain, layers, continuing());
        }
        if (ImGui::Checkbox("DirectX normal map", &layer.normal_directx)) {
            set_layers(terrain, layers);
        }
        ImGui::SetItemTooltip("Tick if bumps look inverted (green channel pointing down)");
    }
    if (ImGui::SliderFloat("Border sharpness", &layer.height_contrast, 0.0f, 4.0f, "%.2f")) {
        set_layers(terrain, layers, continuing());
    }
    ImGui::SetItemTooltip("0: soft blends; higher: borders follow the stones and clumps of the height map");

    // Where it appears automatically.
    if (ImGui::TreeNodeEx("Placement rule", layer.rule.enabled ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        terrain::LayerRule& rule = layer.rule;
        if (ImGui::Checkbox("Place by height and slope", &rule.enabled)) {
            set_layers(terrain, layers);
        }
        ImGui::BeginDisabled(!rule.enabled);
        float heights[2] = {rule.height_min, rule.height_max};
        if (ImGui::DragFloat2("Height range (m)", heights, 0.5f, -1.0e4f, 1.0e4f, "%.1f")) {
            rule.height_min = std::min(heights[0], heights[1]);
            rule.height_max = std::max(heights[0], heights[1]);
            set_layers(terrain, layers, continuing());
        }
        float slopes[2] = {rule.slope_min, rule.slope_max};
        if (ImGui::DragFloat2("Slope range (deg)", slopes, 0.25f, 0.0f, 90.0f, "%.1f")) {
            rule.slope_min = std::clamp(std::min(slopes[0], slopes[1]), 0.0f, 90.0f);
            rule.slope_max = std::clamp(std::max(slopes[0], slopes[1]), 0.0f, 90.0f);
            set_layers(terrain, layers, continuing());
        }
        if (ImGui::SliderFloat("Blend", &rule.falloff, 0.0f, 64.0f, "%.1f")) {
            set_layers(terrain, layers, continuing());
        }
        ImGui::SetItemTooltip("Width of the soft edge: metres for height, degrees for slope");
        ImGui::EndDisabled();
        ImGui::TextDisabled("Rules take effect with Apply rules (top of this tab).");
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// --- Foliage, water, splines ---------------------------------------------------------------

void TerrainPanel::draw_foliage_tab(entt::entity terrain) {
    scene::World& world = scene_editor_->world();
    if (actions_.add_scatter) {
        ImGui::TextUnformatted("Add over the terrain");
        for (const auto& [id, label] : {std::pair{"trees", "Trees"}, std::pair{"grass", "Grass"},
                                        std::pair{"rocks", "Rocks"}, std::pair{"selected", "Selected meshes"}}) {
            if (ImGui::Button(label)) {
                actions_.add_scatter(id);
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();
    }

    // The scatters on this terrain: pick the one to paint.
    ImGui::SeparatorText("Foliage types");
    const core::Guid terrain_guid = terrain == entt::null ? core::Guid{} : world.guid(terrain);
    const core::Guid painting = tools_.foliage ? tools_.foliage->current_scatter() : core::Guid{};
    bool any = false;
    for (const entt::entity e : world.registry().view<world::Scatter>()) {
        const world::Scatter& scatter = world.registry().get<world::Scatter>(e);
        if (scatter.terrain != terrain_guid) {
            continue;
        }
        any = true;
        const core::Guid guid = world.guid(e);
        ImGui::PushID(guid.to_string().c_str());
        std::string label = world.name(e) + "  (" + std::to_string(world::scatter_instances(world, e).size()) + ")";
        if (scatter.paint_only) {
            label += "  [painted only]";
        }
        if (ImGui::Selectable(label.c_str(), guid == painting)) {
            scene_editor_->run("selection.set", {{"entities", {guid.to_string()}}});
            if (tools_.foliage) {
                tools_.foliage->target = guid;
            }
        }
        ImGui::PopID();
    }
    if (!any) {
        ImGui::TextDisabled("None yet: add trees, grass or rocks above.");
    }

    // Quick settings of the one being painted.
    const entt::entity active = painting.is_null() ? entt::null : world.find(painting);
    if (active != entt::null) {
        const world::Scatter settings = world.registry().get<world::Scatter>(active);
        const std::string id = painting.to_string();
        const auto set = [&](const char* field, const nlohmann::json& value, bool merge) {
            scene_editor_->run("entity.set_property",
                               {{"entity", id}, {"path", std::string("Scatter.") + field}, {"value", value}}, merge);
        };
        bool paint_only = settings.paint_only;
        if (ImGui::Checkbox("Painted only", &paint_only)) {
            set("paint_only", paint_only, false);
        }
        ImGui::SetItemTooltip("On: instances appear only where you paint. Off: everywhere the rules allow, "
                              "and the brush thins or removes them");
        float density = settings.density;
        if (ImGui::SliderFloat("Amount (/m2)", &density, 0.0005f, 5.0f, "%.4f", ImGuiSliderFlags_Logarithmic)) {
            set("density", density, continuing());
        }
        float spacing = settings.min_spacing;
        if (ImGui::SliderFloat("Spacing (m)", &spacing, 0.0f, 30.0f, "%.2f")) {
            set("min_spacing", spacing, continuing());
        }
        float scale[2] = {settings.scale_min, settings.scale_max};
        if (ImGui::DragFloat2("Size range", scale, 0.01f, 0.01f, 100.0f, "%.2f")) {
            set("scale_min", std::min(scale[0], scale[1]), continuing());
            set("scale_max", std::max(scale[0], scale[1]), true);
        }
        float slope = settings.slope_max;
        if (ImGui::SliderFloat("Steepest slope", &slope, 0.0f, 90.0f, "%.0f deg")) {
            set("slope_max", slope, continuing());
        }
        if (ImGui::Button("Reseed")) {
            set("seed", settings.seed + 1, false);
        }
        ImGui::SetItemTooltip("A new random layout with the same settings and painting");
    }

    ImGui::SeparatorText("Foliage brush");
    if (tools_.foliage) {
        FoliageTool& tool = *tools_.foliage;
        tool_switch("Paint foliage", tools_.foliage,
                    "Left-drag to add the selected foliage type; with Erase, to remove it. Escape cancels");
        ImGui::Checkbox("Erase##foliage", &tool.erase_instances);
        ImGui::SliderFloat("Radius (m)##foliage", &tool.brush.radius, 0.5f, 500.0f, "%.1f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Strength##foliage", &tool.brush.strength, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("How much each pass adds or removes (1: all at once)");
        enum_combo("Falloff##foliage", tool.brush.falloff, kFalloffs);
        if (painting.is_null()) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Pick a foliage type above to paint it.");
        }
    }
}

void TerrainPanel::draw_water_tab(entt::entity terrain) {
    scene::World& world = scene_editor_->world();
    if (WaterTool* tool = tools_.water) {
        tool_switch("Place lakes", tool, "Click the terrain: the basin around the click floods to the fill depth");
        ImGui::SliderFloat("Fill depth (m)", &tool->fill_depth, 0.1f, 100.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("How far above the clicked ground the water rises");
        if (tool->enabled) {
            ImGui::TextWrapped("Click a valley or hollow to fill it.");
        }
    }
    if (ImGui::Button("Add ocean", ImVec2(-1.0f, 0.0f))) {
        // At the terrain's base height (sea level), else y = 0.
        const glm::vec3 at = terrain != entt::null ? terrain::terrain_origin(world, terrain) : glm::vec3(0.0f);
        const core::Guid created = core::Guid::generate();
        if (scene_editor_->run("water.create",
                               {{"kind", "ocean"}, {"guid", created.to_string()}, {"position", {at.x, at.y, at.z}}})) {
            scene_editor_->set_selection({created}, created);
        }
    }
    ImGui::SetItemTooltip("An endless sea at the terrain's base height; raise it to flood the lowlands");

    ImGui::SeparatorText("Water bodies");
    const core::Guid active = world.selection().active();
    bool any = false;
    for (const entt::entity e : world.registry().view<world::WaterBody>()) {
        any = true;
        const core::Guid guid = world.guid(e);
        ImGui::PushID(guid.to_string().c_str());
        const world::WaterBody& water = world.registry().get<world::WaterBody>(e);
        const std::string label = world.name(e) + "  (" + std::string(world::to_string(water.kind)) + ", level " +
                                  std::to_string(static_cast<int>(std::lround(world::water_level(world, e)))) + " m)";
        if (ImGui::Selectable(label.c_str(), guid == active)) {
            scene_editor_->set_selection({guid}, guid);
        }
        ImGui::PopID();
    }
    if (!any) {
        ImGui::TextDisabled("None yet.");
    }
    const entt::entity e = active.is_null() ? entt::null : world.find(active);
    if (e == entt::null || !world.registry().all_of<world::WaterBody>(e)) {
        return;
    }

    // The selected water body.
    const world::WaterBody w = world.registry().get<world::WaterBody>(e);
    const std::string id = active.to_string();
    const auto set = [&](const char* field, const nlohmann::json& value, bool merge) {
        scene_editor_->run("entity.set_property",
                           {{"entity", id}, {"path", std::string("WaterBody.") + field}, {"value", value}}, merge);
    };
    ImGui::SeparatorText(world.name(e).c_str());
    // The level is the entity's height.
    const scene::Transform& transform = world.registry().get<scene::Transform>(e);
    const float level = world::water_level(world, e);
    float edited = level;
    if (ImGui::DragFloat("Level (m)", &edited, 0.05f, 0.0f, 0.0f, "%.2f") && edited != level) {
        const glm::vec3 t = transform.translation + glm::vec3(0.0f, edited - level, 0.0f);
        scene_editor_->run("entity.set_property",
                           {{"entity", id}, {"path", "Transform.translation"}, {"value", {t.x, t.y, t.z}}},
                           continuing());
    }
    ImGui::SetItemTooltip(w.kind == world::WaterKind::Lake ? "Raise to flood more of the basin" : "Sea level");

    // Look presets: a starting point, then tweak below.
    struct Look {
        const char* name;
        glm::vec3 shallow;
        glm::vec3 deep;
        float clarity;
        float roughness;
    };
    static const std::array<Look, 5> kLooks = {{
        {"Clear lake", {0.10f, 0.36f, 0.34f}, {0.010f, 0.040f, 0.055f}, 5.0f, 0.04f},
        {"Mountain lake", {0.08f, 0.40f, 0.45f}, {0.005f, 0.050f, 0.090f}, 12.0f, 0.02f},
        {"Tropical", {0.10f, 0.62f, 0.58f}, {0.010f, 0.120f, 0.200f}, 18.0f, 0.03f},
        {"Murky pond", {0.16f, 0.20f, 0.08f}, {0.020f, 0.030f, 0.010f}, 0.8f, 0.08f},
        {"Deep sea", {0.06f, 0.32f, 0.36f}, {0.004f, 0.022f, 0.050f}, 8.0f, 0.06f},
    }};
    if (ImGui::BeginCombo("Look", "Apply a preset...")) {
        for (const Look& look : kLooks) {
            if (ImGui::Selectable(look.name)) {
                scene_editor_->stack().begin_transaction(std::string("Water look: ") + look.name); // one undo step
                set("shallow_color", {look.shallow.x, look.shallow.y, look.shallow.z}, false);
                set("deep_color", {look.deep.x, look.deep.y, look.deep.z}, false);
                set("clarity", look.clarity, false);
                set("roughness", look.roughness, false);
                scene_editor_->stack().end_transaction();
            }
        }
        ImGui::EndCombo();
    }
    float shallow[3] = {w.shallow_color.x, w.shallow_color.y, w.shallow_color.z};
    if (edit_linear_color("Shallow colour", shallow, 3)) {
        set("shallow_color", {shallow[0], shallow[1], shallow[2]}, continuing());
    }
    float deep[3] = {w.deep_color.x, w.deep_color.y, w.deep_color.z};
    if (edit_linear_color("Deep colour", deep, 3)) {
        set("deep_color", {deep[0], deep[1], deep[2]}, continuing());
    }
    const auto slider = [&](const char* label, const char* field, float value, float lo, float hi, const char* format,
                            bool log, const char* tooltip) {
        if (ImGui::SliderFloat(label, &value, lo, hi, format, log ? ImGuiSliderFlags_Logarithmic : 0)) {
            set(field, value, continuing());
        }
        if (tooltip) {
            ImGui::SetItemTooltip("%s", tooltip);
        }
    };
    slider("Clarity (m)", "clarity", w.clarity, 0.1f, 100.0f, "%.1f", true,
           "How deep you can see before the bottom fades into the deep colour");
    ImGui::SeparatorText("Waves");
    slider("Height (m)##waves", "wave_height", w.wave_height, 0.0f, 10.0f, "%.2f", true, nullptr);
    slider("Length (m)##waves", "wave_length", w.wave_length, 0.5f, 300.0f, "%.1f", true, "Metres between crests");
    slider("Wind (deg)", "wind_direction", w.wind_direction, -180.0f, 180.0f, "%.0f", false,
           "The direction the waves travel");
    slider("Speed##waves", "wave_speed", w.wave_speed, 0.0f, 5.0f, "%.2f", false, nullptr);
    ImGui::SeparatorText("Surface");
    slider("Foam", "foam", w.foam, 0.0f, 2.0f, "%.2f", false, "Along shores and on crests");
    slider("Refraction", "refraction", w.refraction, 0.0f, 2.0f, "%.2f", false,
           "How much the bottom wobbles seen through the waves");
    slider("Roughness", "roughness", w.roughness, 0.0f, 1.0f, "%.3f", false, "0: mirror-like reflections");
    if (w.kind == world::WaterKind::Lake) {
        float extent[2] = {w.extent.x, w.extent.y};
        if (ImGui::DragFloat2("Max extent (m)", extent, 1.0f, 1.0f, 100000.0f, "%.0f")) {
            set("extent", {extent[0], extent[1]}, continuing());
        }
        ImGui::SetItemTooltip("The largest area the water may spread over (x, z), centred on the lake");
        if (!world::lake_surface(world, e)) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Dry: the ground under the lake is above its level.");
        }
    }
    bool visible = w.visible;
    if (ImGui::Checkbox("Visible", &visible)) {
        set("visible", visible, false);
    }
}

void TerrainPanel::draw_spline_tab(entt::entity terrain) {
    scene::World& world = scene_editor_->world();
    SplineTool* tool = tools_.spline;
    if (tool) {
        ImGui::TextUnformatted("Draw a new");
        constexpr std::array<std::pair<world::SplineKind, const char*>, 4> kKinds = {{
            {world::SplineKind::Road, "Road"},
            {world::SplineKind::Path, "Path"},
            {world::SplineKind::River, "River"},
            {world::SplineKind::Fence, "Fence"},
        }};
        for (const auto& [kind, label] : kKinds) {
            if (ImGui::Button(label)) {
                tool->start_new(kind);
                if (tools_.tools) {
                    tools_.tools->activate(tool);
                }
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();
        tool_switch("Edit splines", tool,
                    "Click the ground to add points, drag points to move them, Delete removes the selected point, "
                    "Escape lets go");
        if (tool->enabled) {
            if (tool->current_spline().is_null()) {
                ImGui::TextWrapped("Click the ground to start a %s.", std::string(world::to_string(tool->new_kind)).c_str());
            } else {
                ImGui::TextWrapped("Click to add points (near the line: in between), drag to move, Delete to remove.");
            }
        }
    }

    ImGui::SeparatorText("Splines");
    const core::Guid editing = tool ? tool->current_spline() : core::Guid{};
    bool any = false;
    for (const entt::entity e : world.registry().view<world::Spline>()) {
        any = true;
        const core::Guid guid = world.guid(e);
        ImGui::PushID(guid.to_string().c_str());
        const world::Spline& spline = world.registry().get<world::Spline>(e);
        const std::string label = world.name(e) + "  (" + std::to_string(spline.points.size()) + " points)";
        if (ImGui::Selectable(label.c_str(), guid == editing)) {
            scene_editor_->set_selection({guid}, guid);
        }
        ImGui::PopID();
    }
    if (!any) {
        ImGui::TextDisabled("None yet.");
    }
    const entt::entity active = editing.is_null() ? entt::null : world.find(editing);
    if (active == entt::null) {
        return;
    }

    // The selected spline.
    const world::Spline s = world.registry().get<world::Spline>(active);
    const std::string id = editing.to_string();
    const auto set = [&](const char* field, const nlohmann::json& value, bool merge) {
        scene_editor_->run("entity.set_property",
                           {{"entity", id}, {"path", std::string("Spline.") + field}, {"value", value}}, merge);
    };
    ImGui::SeparatorText(world.name(active).c_str());
    float width = s.width;
    if (ImGui::SliderFloat("Width (m)", &width, 0.2f, 60.0f, "%.1f", ImGuiSliderFlags_Logarithmic)) {
        set("width", width, continuing());
    }
    bool closed = s.closed;
    if (ImGui::Checkbox("Loop", &closed)) {
        set("closed", closed, false);
    }
    bool shape = s.shape_terrain;
    if (ImGui::Checkbox("Shape terrain", &shape)) {
        set("shape_terrain", shape, false);
    }
    ImGui::SetItemTooltip("Flatten the ground to the line (and carve the depth); undone when the spline goes");
    if (s.shape_terrain) {
        float falloff = s.falloff;
        if (ImGui::SliderFloat("Blend (m)", &falloff, 0.0f, 60.0f, "%.1f")) {
            set("falloff", falloff, continuing());
        }
        float depth = s.depth;
        if (ImGui::SliderFloat("Depth (m)", &depth, -10.0f, 20.0f, "%.2f")) {
            set("depth", depth, continuing());
        }
        ImGui::SetItemTooltip("Carved below the line in the middle (riverbeds, ditches)");
    }
    // Paint layer: one of the terrain's layers.
    if (terrain != entt::null) {
        const auto& layers = world.registry().get<terrain::Terrain>(terrain).layers;
        const std::string current = s.paint_layer >= 0 && static_cast<std::size_t>(s.paint_layer) < layers.size()
                                        ? layers[static_cast<std::size_t>(s.paint_layer)].name
                                        : std::string("None");
        if (ImGui::BeginCombo("Paint layer", current.c_str())) {
            if (ImGui::Selectable("None", s.paint_layer < 0)) {
                set("paint_layer", -1, false);
            }
            for (std::size_t i = 0; i < layers.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(layers[i].name.c_str(), s.paint_layer == static_cast<int>(i))) {
                    set("paint_layer", static_cast<int>(i), false);
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    // Surface ribbon.
    std::string surface_name = s.surface.empty() ? std::string("None") : s.surface;
    for (const terrain::BuiltinSurface& b : terrain::builtin_surfaces()) {
        if (s.surface == b.id) {
            surface_name = b.name;
        }
    }
    if (ImGui::BeginCombo("Surface", surface_name.c_str())) {
        if (ImGui::Selectable("None", s.surface.empty())) {
            set("surface", "", false);
        }
        for (const terrain::BuiltinSurface& b : terrain::builtin_surfaces()) {
            if (ImGui::Selectable(b.name, s.surface == b.id)) {
                set("surface", b.id, false);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("A paved strip laid along it (roads)");
    // Meshes along it.
    const auto ref_json = [](const core::AssetRef& m) {
        return nlohmann::json{{"guid", m.guid.is_null() ? std::string() : m.guid.to_string()}, {"path", m.path}};
    };
    const bool fence = std::any_of(s.meshes.begin(), s.meshes.end(),
                                   [](const core::AssetRef& m) { return m.path == world::kBuiltinFenceRail; });
    bool fence_on = fence;
    if (ImGui::Checkbox("Fence posts and rails", &fence_on)) {
        nlohmann::json meshes = nlohmann::json::array();
        if (fence_on) {
            meshes.push_back(ref_json({core::Guid{}, world::kBuiltinFencePost}));
            meshes.push_back(ref_json({core::Guid{}, world::kBuiltinFenceRail}));
        }
        set("meshes", meshes, false);
    }
    if (ImGui::BeginCombo("##along", "Place a scene object's mesh along it...")) {
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
                nlohmann::json meshes = nlohmann::json::array();
                for (const core::AssetRef& m : s.meshes) {
                    meshes.push_back(ref_json(m));
                }
                meshes.push_back(ref_json(renderer->mesh));
                set("meshes", meshes, false);
            }
            ImGui::PopID();
        }
        if (listed.empty()) {
            ImGui::TextDisabled("No objects with a mesh in the scene");
        }
        ImGui::EndCombo();
    }
    if (!s.meshes.empty()) {
        float spacing = s.mesh_spacing;
        if (ImGui::SliderFloat("Spacing (m)##along", &spacing, 0.5f, 100.0f, "%.1f", ImGuiSliderFlags_Logarithmic)) {
            set("mesh_spacing", spacing, continuing());
        }
        float offset = s.mesh_offset;
        if (ImGui::SliderFloat("Side offset (m)", &offset, -30.0f, 30.0f, "%.1f")) {
            set("mesh_offset", offset, continuing());
        }
        bool both = s.both_sides;
        if (ImGui::Checkbox("Both sides", &both)) {
            set("both_sides", both, false);
        }
    }
    bool clear = s.clear_scatter;
    if (ImGui::Checkbox("Keep foliage off", &clear)) {
        set("clear_scatter", clear, false);
    }
    if (s.kind == world::SplineKind::River) {
        float flow = s.flow_speed;
        if (ImGui::SliderFloat("Flow speed (m/s)", &flow, 0.0f, 8.0f, "%.2f")) {
            set("flow_speed", flow, continuing());
        }
    }

    // Point helpers.
    const std::vector<glm::vec3> points = world::spline_world_points(world, active);
    const glm::vec3 base(world.transform(active).world[3]);
    const auto set_points = [&](const std::vector<glm::vec3>& world_points) {
        nlohmann::json value = nlohmann::json::array();
        for (const glm::vec3& p : world_points) {
            const glm::vec3 local = p - base;
            value.push_back({local.x, local.y, local.z});
        }
        set("points", value, false);
    };
    if (ImGui::Button("Snap to ground")) {
        std::vector<glm::vec3> snapped = points;
        const entt::entity t = s.terrain.is_null() ? entt::null : world.find(s.terrain);
        if (const auto hf = terrain::heightfield_of(world, t)) { // the terrain's own ground, not the shaped one
            const glm::vec3 o = terrain::terrain_origin(world, t);
            for (glm::vec3& p : snapped) {
                p.y = hf->height_at(p.x - o.x, p.z - o.z) + o.y;
            }
            set_points(snapped);
        }
    }
    ImGui::SetItemTooltip("Put every point back on the terrain's own surface");
    if (s.kind == world::SplineKind::River) {
        ImGui::SameLine();
        if (ImGui::Button("Flow downhill")) {
            std::vector<glm::vec3> level = points;
            for (std::size_t i = 1; i < level.size(); ++i) {
                level[i].y = std::min(level[i].y, level[i - 1].y);
            }
            set_points(level);
        }
        ImGui::SetItemTooltip("Lower points so the water never runs uphill (from the first point to the last)");
    }
    if (ImGui::Button("Delete spline")) {
        scene_editor_->run("entity.delete", {{"entity", id}});
    }
}

} // namespace f3d::editor
