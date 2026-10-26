// SPDX-License-Identifier: Apache-2.0
#include "modeling_panel.hpp"

#include "f3d/editor/modeling_tool.hpp"
#include "f3d/editor/viewport_tool.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <utility>

namespace f3d::editor {
namespace {

using modeling::Shape;

constexpr std::array<std::pair<Shape, const char*>, 8> kShapes = {{
    {Shape::Plane, "Plane"},
    {Shape::Cube, "Cube"},
    {Shape::Sphere, "Sphere"},
    {Shape::Cylinder, "Cylinder"},
    {Shape::Cone, "Cone"},
    {Shape::Torus, "Torus"},
    {Shape::Capsule, "Capsule"},
    {Shape::Stairs, "Stairs"},
}};

const char* shape_name(Shape shape) {
    for (const auto& [value, name] : kShapes) {
        if (value == shape) {
            return name;
        }
    }
    return "Mesh";
}

/// Labels of the two segment counts, per shape (null: unused).
std::pair<const char*, const char*> segment_labels(Shape shape) {
    switch (shape) {
    case Shape::Plane:
        return {"Divisions X", "Divisions Z"};
    case Shape::Cube:
        return {"Divisions", nullptr};
    case Shape::Sphere:
        return {"Segments", "Rings"};
    case Shape::Cylinder:
    case Shape::Cone:
        return {"Sides", "Height segments"};
    case Shape::Torus:
        return {"Segments", "Tube segments"};
    case Shape::Capsule:
        return {"Sides", "Cap rings"};
    case Shape::Stairs:
        return {"Steps", nullptr};
    case Shape::None:
        break;
    }
    return {nullptr, nullptr};
}

} // namespace

ModelingPanel::ModelingPanel(ModelingTool* tool, LandscapeTools* tools, ModelingActions actions)
    : Panel(panel_ids::kModeling, "Modeling"), tool_(tool), tools_(tools), actions_(std::move(actions)) {
    set_open(false); // Tools > Modeling, the command palette, or a new shape
}

void ModelingPanel::draw_contents() {
    if (!tool_) {
        return;
    }
    ImGui::SeparatorText("Add shape");
    if (actions_.add_shape) {
        for (std::size_t i = 0; i < kShapes.size(); ++i) {
            if (i % 4 != 0) {
                ImGui::SameLine();
            }
            if (ImGui::Button(kShapes[i].second, ImVec2(ImGui::GetContentRegionAvail().x > 320.0f ? 72.0f : 0.0f, 0.0f))) {
                actions_.add_shape(kShapes[i].first);
            }
        }
    }
    const modeling::EditableMesh* mesh = tool_->mesh();
    if (!mesh) {
        ImGui::Spacing();
        ImGui::TextWrapped("Select a shape to edit it. To edit an imported model, select it and use Make Editable.");
        if (actions_.make_editable && ImGui::Button("Make Editable")) {
            actions_.make_editable();
        }
        return;
    }
    if (mesh->shape != Shape::None) {
        draw_shape(*mesh);
    }
    draw_faces(*mesh);
    draw_mesh_tools(*mesh);
}

void ModelingPanel::draw_shape(const modeling::EditableMesh& mesh) {
    // Follow the mesh (undo, another object) unless an edit is waiting to be applied.
    if (editing_target_ != tool_->target() || !shape_pending_) {
        editing_ = mesh.shape_params();
        editing_target_ = tool_->target();
        shape_pending_ = false;
    }
    ImGui::SeparatorText(shape_name(mesh.shape));
    ImGui::TextDisabled("Parameters stay editable until the first edit.");
    bool changed = false;
    // Size, with the axes each shape uses.
    switch (editing_.shape) {
    case Shape::Plane: {
        float xz[2] = {editing_.size.x, editing_.size.z};
        changed |= ImGui::DragFloat2("Size (X, Z)", xz, 0.01f, 0.01f, 1000.0f, "%.2f m");
        editing_.size.x = xz[0];
        editing_.size.z = xz[1];
        break;
    }
    case Shape::Torus: {
        changed |= ImGui::DragFloat("Outer diameter", &editing_.size.x, 0.01f, 0.02f, 1000.0f, "%.2f m");
        changed |= ImGui::DragFloat("Tube diameter", &editing_.size.y, 0.01f, 0.01f, 1000.0f, "%.2f m");
        break;
    }
    case Shape::Stairs:
        changed |= ImGui::DragFloat3("Width, height, run", &editing_.size.x, 0.01f, 0.01f, 1000.0f, "%.2f m");
        break;
    default:
        changed |= ImGui::DragFloat3("Size", &editing_.size.x, 0.01f, 0.01f, 1000.0f, "%.2f m");
        break;
    }
    const auto [label_a, label_b] = segment_labels(editing_.shape);
    if (label_a) {
        int a = static_cast<int>(editing_.segments_a);
        changed |= ImGui::SliderInt(label_a, &a, 1, 128);
        editing_.segments_a = static_cast<std::uint32_t>(std::max(a, 1));
    }
    if (label_b) {
        int b = static_cast<int>(editing_.segments_b);
        changed |= ImGui::SliderInt(label_b, &b, 1, 64);
        editing_.segments_b = static_cast<std::uint32_t>(std::max(b, 1));
    }
    // Applied when the drag or typing ends: one undo step per change.
    shape_pending_ = shape_pending_ || changed;
    if (shape_pending_ && !ImGui::IsAnyItemActive()) {
        shape_pending_ = false;
        if (!(editing_ == mesh.shape_params())) {
            tool_->set_shape(editing_);
        }
    }
}

void ModelingPanel::draw_faces(const modeling::EditableMesh& mesh) {
    ImGui::SeparatorText("Faces");
    const bool active = tools_ && tools_->active() == tool_;
    bool select = active || tool_->enabled;
    if (ImGui::Checkbox("Select faces in the viewport", &select) && tools_) {
        tools_->activate(select ? tool_ : nullptr);
    }
    ImGui::TextDisabled("Click a face; Shift+click adds or removes. Esc leaves.");
    ImGui::TextDisabled("In the viewport: E extrude, I inset, Ctrl+R loop cut (follow the mouse;\n"
                        "type a value, click to confirm, right-click to cancel).");
    ImGui::Text("%zu of %zu faces selected, %zu vertices", tool_->faces().size(), mesh.face_count(),
                mesh.positions.size());
    if (ImGui::Button("Select All")) {
        tool_->select_all_faces();
    }
    ImGui::SameLine();
    if (ImGui::Button("Select None")) {
        tool_->clear_faces();
    }
    const bool none = tool_->faces().empty();
    ImGui::BeginDisabled(none);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat("##extrude", &tool_->extrude_distance, 0.01f, -100.0f, 100.0f, "%.2f m");
    ImGui::SameLine();
    if (ImGui::Button("Extrude")) {
        tool_->extrude(tool_->extrude_distance);
    }
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("##inset", &tool_->inset_amount, 0.01f, 0.95f, "%.2f");
    ImGui::SameLine();
    if (ImGui::Button("Inset")) {
        tool_->inset(tool_->inset_amount);
    }
    if (ImGui::Button("Delete Faces")) {
        tool_->delete_faces();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(none ? "Flip All Normals" : "Flip Normals")) {
        tool_->flip();
    }
    int cuts = static_cast<int>(tool_->loop_cuts);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderInt("##cuts", &cuts, 1, 16, "%d cuts")) {
        tool_->loop_cuts = static_cast<std::uint32_t>(cuts);
    }
    ImGui::SameLine();
    if (ImGui::Button("Loop Cut")) {
        if (tools_) {
            tools_->activate(tool_); // the cut is placed in the viewport
        }
        tool_->begin_loop_cut();
    }
    ImGui::SetItemTooltip("Hover an edge in the viewport and click (Ctrl+R there)");
    draw_adjust_last();
}

void ModelingPanel::draw_adjust_last() {
    const ModelingTool::LastEdit* last = tool_->last_edit();
    if (!last) {
        return;
    }
    // Blender's "Adjust Last Operation": the edit is undone and run again with the new value.
    ImGui::SeparatorText("Adjust last operation");
    const ModelingTool::LastEdit edit = *last;
    float value = edit.value;
    int cuts = static_cast<int>(edit.cuts);
    bool changed = false;
    switch (edit.kind) {
    case ModelingTool::Edit::Extrude:
        changed = ImGui::DragFloat("Extrude distance", &value, 0.005f, -100.0f, 100.0f, "%.3f m");
        break;
    case ModelingTool::Edit::Inset:
        changed = ImGui::SliderFloat("Inset amount", &value, 0.0f, 0.99f, "%.3f");
        break;
    case ModelingTool::Edit::LoopCut:
        changed = ImGui::SliderInt("Number of cuts", &cuts, 1, 64);
        ImGui::BeginDisabled(cuts > 1);
        changed |= ImGui::SliderFloat("Position", &value, 0.01f, 0.99f, "%.2f");
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Where a single cut sits along the ring (several cuts are spaced evenly)");
        break;
    case ModelingTool::Edit::None:
        break;
    }
    if (changed) {
        tool_->adjust_last(value, static_cast<std::uint32_t>(std::max(cuts, 1)));
    }
}

void ModelingPanel::draw_mesh_tools(const modeling::EditableMesh& mesh) {
    ImGui::SeparatorText("Mesh");
    int levels = static_cast<int>(tool_->subdivide_levels);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderInt("##levels", &levels, 1, 3)) {
        tool_->subdivide_levels = static_cast<std::uint32_t>(levels);
    }
    ImGui::SameLine();
    if (ImGui::Button("Subdivide (smooth)")) {
        tool_->subdivide(tool_->subdivide_levels);
    }
    ImGui::TextUnformatted("Mirror");
    for (int axis = 0; axis < 3; ++axis) {
        ImGui::SameLine();
        const char* labels[] = {"X##mirror", "Y##mirror", "Z##mirror"};
        if (ImGui::Button(labels[axis])) {
            tool_->mirror(axis);
        }
    }
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat("##merge", &tool_->merge_distance, 0.0005f, 0.0f, 10.0f, "%.4f m");
    ImGui::SameLine();
    if (ImGui::Button("Merge Vertices")) {
        tool_->merge_vertices(tool_->merge_distance);
    }
    ImGui::TextUnformatted("Origin");
    ImGui::SameLine();
    if (ImGui::Button("Centre")) {
        tool_->set_origin("centre");
    }
    ImGui::SameLine();
    if (ImGui::Button("Bottom")) {
        tool_->set_origin("bottom");
    }
    if (!ImGui::IsAnyItemActive()) {
        smoothing_ = mesh.smoothing_angle;
    }
    ImGui::SliderFloat("Smoothing angle", &smoothing_, 0.0f, 180.0f, "%.0f deg");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        tool_->set_smoothing(smoothing_);
    }
    if (ImGui::Button("Flat Shading")) {
        tool_->set_smoothing(0.0f);
    }
    ImGui::SameLine();
    if (ImGui::Button("Smooth Shading")) {
        tool_->set_smoothing(180.0f);
    }
}

} // namespace f3d::editor
