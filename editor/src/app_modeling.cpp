// SPDX-License-Identifier: Apache-2.0
// Modeling and atmosphere commands of the App: Add > Shape, Add > Visual Effects,
// Tools > Modeling (modeling_tool.hpp does the editing).

#include "app.hpp"

#include "f3d/core/log.hpp"
#include "f3d/modeling/editable_mesh.hpp"
#include "ui/modeling_panel.hpp"

#include <cctype>

namespace f3d::editor {

core::Guid App::add_shape(modeling::Shape shape) {
    std::string name(modeling::to_string(shape));
    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    const modeling::ShapeParams params = modeling::default_shape(shape);
    // One undo step: the entity (at the 3D cursor, selected) and its shape.
    scene_editor_.stack().begin_transaction("Add " + name);
    const core::Guid guid = add_entity(name, nullptr, nlohmann::json::object());
    const bool ok = !guid.is_null() &&
                    scene_editor_.run("modeling.set_shape", {{"entity", guid.to_string()},
                                                            {"shape", std::string(modeling::to_string(shape))},
                                                            {"size", {params.size.x, params.size.y, params.size.z}},
                                                            {"segments_a", params.segments_a},
                                                            {"segments_b", params.segments_b}});
    if (!ok) {
        scene_editor_.stack().abort_transaction();
        return {};
    }
    scene_editor_.stack().end_transaction();
    if (ui_) {
        if (Panel* panel = ui_->panels().find(panel_ids::kModeling)) {
            panel->set_open(true);
        }
    }
    return guid;
}

void App::make_editable() {
    const core::Guid guid = world_.selection().active();
    const entt::entity e = guid.is_null() ? entt::entity{entt::null} : world_.find(guid);
    if (e == entt::null) {
        F3D_LOG_WARN("Make Editable: select an object with a mesh first");
        return;
    }
    if (world_.registry().all_of<modeling::EditableMesh>(e)) {
        F3D_LOG_INFO("Make Editable: '{}' is already editable", world_.registry().get<scene::Name>(e).value);
        return;
    }
    const auto* renderer = world_.registry().try_get<scene::MeshRenderer>(e);
    const auto source = renderer ? meshes_.get(renderer->mesh) : nullptr;
    if (!source || source->indices.empty()) {
        F3D_LOG_WARN("Make Editable: the selected object has no loaded mesh");
        return;
    }
    const modeling::EditableMesh mesh = modeling::from_triangles(*source);
    nlohmann::json positions = nlohmann::json::array();
    for (const glm::vec3& p : mesh.positions) {
        positions.push_back({p.x, p.y, p.z});
    }
    const std::string id = guid.to_string();
    // The renderer points at the editable copy in the same step, so undo restores the model.
    scene_editor_.stack().begin_transaction("Make Editable");
    const bool ok =
        scene_editor_.run("entity.set_property", {{"entity", id},
                                                  {"path", "MeshRenderer.mesh"},
                                                  {"value", {{"path", modeling::mesh_path(world_, e)}}}}) &&
        scene_editor_.run("modeling.set_mesh",
                          {{"entity", id}, {"positions", positions}, {"face_sizes", mesh.face_sizes},
                           {"corners", mesh.corners}});
    if (!ok) {
        scene_editor_.stack().abort_transaction();
        return;
    }
    scene_editor_.stack().end_transaction();
    F3D_LOG_INFO("Made '{}' editable: {} faces, {} vertices", world_.registry().get<scene::Name>(e).value,
                 mesh.face_count(), mesh.positions.size());
}

void App::register_modeling_commands() {
    for (const auto shape : {modeling::Shape::Plane, modeling::Shape::Cube, modeling::Shape::Sphere,
                             modeling::Shape::Cylinder, modeling::Shape::Cone, modeling::Shape::Torus,
                             modeling::Shape::Capsule, modeling::Shape::Stairs}) {
        std::string name(modeling::to_string(shape));
        std::string label = name;
        label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
        commands_.add({"add.shape_" + name, "Add Shape: " + label, [this, shape] { add_shape(shape); },
                       std::nullopt});
    }
    // Atmosphere and post-processing (Add > Visual Effects).
    commands_.add({"add.height_fog", "Add Exponential Height Fog",
                   [this] { add_entity("Exponential Height Fog", "HeightFog", nlohmann::json::object()); },
                   std::nullopt});
    commands_.add({"add.fog_volume", "Add Local Fog Volume",
                   [this] { add_entity("Local Fog Volume", "FogVolume", nlohmann::json::object()); }, std::nullopt});
    commands_.add({"add.post_process", "Add Post Process",
                   [this] { add_entity("Post Process", "PostProcess", nlohmann::json::object()); }, std::nullopt});

    // Tools > Modeling.
    ModelingTool& tool = modeling_tool_;
    const auto add = [this](const char* id, const char* name, std::function<void()> fn) {
        commands_.add({id, name, std::move(fn), std::nullopt});
    };
    add("modeling.select_faces", "Modeling: Select Faces", [this] {
        landscape_tools_.toggle(&modeling_tool_);
        if (ui_) {
            if (Panel* panel = ui_->panels().find(panel_ids::kModeling)) {
                panel->set_open(true);
            }
        }
    });
    add("modeling.select_all_faces", "Modeling: Select All Faces", [&tool] { tool.select_all_faces(); });
    add("modeling.extrude", "Modeling: Extrude Faces", [&tool] { tool.extrude(tool.extrude_distance); });
    add("modeling.inset", "Modeling: Inset Faces", [&tool] { tool.inset(tool.inset_amount); });
    add("modeling.delete_faces", "Modeling: Delete Faces", [&tool] { tool.delete_faces(); });
    add("modeling.flip", "Modeling: Flip Normals", [&tool] { tool.flip(); });
    add("modeling.subdivide", "Modeling: Subdivide", [&tool] { tool.subdivide(tool.subdivide_levels); });
    add("modeling.mirror_x", "Modeling: Mirror X", [&tool] { tool.mirror(0); });
    add("modeling.mirror_y", "Modeling: Mirror Y", [&tool] { tool.mirror(1); });
    add("modeling.mirror_z", "Modeling: Mirror Z", [&tool] { tool.mirror(2); });
    add("modeling.merge_vertices", "Modeling: Merge Vertices", [&tool] { tool.merge_vertices(tool.merge_distance); });
    add("modeling.origin_centre", "Modeling: Origin to Centre", [&tool] { tool.set_origin("centre"); });
    add("modeling.origin_bottom", "Modeling: Origin to Bottom", [&tool] { tool.set_origin("bottom"); });
    add("modeling.shade_flat", "Modeling: Shade Flat", [&tool] { tool.set_smoothing(0.0f); });
    add("modeling.shade_smooth", "Modeling: Shade Smooth", [&tool] { tool.set_smoothing(180.0f); });
    add("modeling.make_editable", "Modeling: Make Editable", [this] { make_editable(); });
}

} // namespace f3d::editor
