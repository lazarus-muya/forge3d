// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/modeling_tool.hpp"

#include "f3d/core/debug_draw.hpp"
#include "f3d/core/log.hpp"
#include "f3d/editor/preferences.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/world.hpp"

#include <imgui.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>

namespace f3d::editor {
namespace {

/// Edges drawn as the mesh's wireframe at most (big meshes show only the selection).
constexpr std::size_t kMaxWireEdges = 40000;

bool ctrl_held() {
    return ImGui::GetCurrentContext() && ImGui::GetIO().KeyCtrl;
}

/// The character a key types into a number ("7", "keypad 7", "period", ...), or 0.
char number_char(const std::string& key) {
    std::string_view k = key;
    if (k.starts_with("keypad ")) {
        k.remove_prefix(7);
    }
    if (k.size() == 1 && ((k[0] >= '0' && k[0] <= '9') || k[0] == '.' || k[0] == '-')) {
        return k[0];
    }
    return k == "period" ? '.' : k == "minus" ? '-' : '\0';
}

std::optional<float> parse_number(const std::string& text) {
    float value = 0.0f;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc() && end == text.data() + text.size() ? std::optional<float>(value) : std::nullopt;
}

float snap(float value, float step) {
    return std::round(value / step) * step;
}

} // namespace

ModelingTool::ModelingTool(scene::World& world, ops::OpStack& stack, const ops::OpRegistry& registry)
    : world_(world), stack_(stack), registry_(registry) {}

void ModelingTool::set_target(const core::Guid& entity) {
    core::Guid next;
    const entt::entity e = entity.is_null() ? entt::entity{entt::null} : world_.find(entity);
    if (e != entt::null && world_.registry().all_of<modeling::EditableMesh>(e)) {
        next = entity;
    }
    if (next != target_) {
        end_modal(); // the preview belongs to the old target
        target_ = next;
        faces_.clear();
        hovered_.reset();
    }
    prune();
}

const modeling::EditableMesh* ModelingTool::mesh() const {
    if (target_.is_null()) {
        return nullptr;
    }
    const entt::entity e = world_.find(target_);
    return e == entt::null ? nullptr : world_.registry().try_get<modeling::EditableMesh>(e);
}

void ModelingTool::prune() {
    const modeling::EditableMesh* m = mesh();
    const std::size_t count = m ? m->face_count() : 0;
    std::erase_if(faces_, [&](std::uint32_t f) { return f >= count; });
    if (hovered_ && *hovered_ >= count) {
        hovered_.reset();
    }
}

void ModelingTool::select_all_faces() {
    const modeling::EditableMesh* m = mesh();
    faces_.resize(m ? m->face_count() : 0);
    std::iota(faces_.begin(), faces_.end(), 0u);
}

void ModelingTool::update(const glm::vec3& origin, const glm::vec3& direction, bool pressed, float) {
    const bool click = pressed && !was_pressed_;
    was_pressed_ = pressed;
    hovered_.reset();
    const modeling::EditableMesh* m = mesh();
    if (!m) {
        end_modal();
        return;
    }
    // The ray in the mesh's local space (directions keep their length: distances don't matter).
    const glm::mat4 inverse = glm::inverse(world_.world_matrix(world_.find(target_)));
    const glm::vec3 local_origin = glm::vec3(inverse * glm::vec4(origin, 1.0f));
    const glm::vec3 local_direction = glm::vec3(inverse * glm::vec4(direction, 0.0f));
    if (modal_ == Edit::LoopCut) {
        track_loop_cut(local_origin, local_direction);
        if (click && cut_edge_) {
            confirm();
        }
        return;
    }
    if (modal_ != Edit::None) {
        track(origin, direction);
        if (click) {
            confirm();
        }
        return;
    }
    if (const auto hit = modeling::raycast(*m, local_origin, local_direction)) {
        hovered_ = hit->face;
    }
    if (!click) {
        return;
    }
    const bool extend = ImGui::GetCurrentContext() && ImGui::GetIO().KeyShift;
    if (!hovered_) {
        if (!extend) {
            faces_.clear(); // a click on nothing deselects
        }
        return;
    }
    const auto it = std::find(faces_.begin(), faces_.end(), *hovered_);
    if (extend) {
        if (it == faces_.end()) {
            faces_.push_back(*hovered_);
        } else {
            faces_.erase(it);
        }
    } else {
        faces_.assign(1, *hovered_);
    }
}

void ModelingTool::leave() {
    hovered_.reset();
    was_pressed_ = false;
    if (modal_ == Edit::LoopCut) {
        cut_edge_.reset(); // nothing under the cursor; an extrude or inset keeps its value
    }
}

void ModelingTool::cancel() {
    if (modal_ != Edit::None) {
        end_modal();
    } else {
        faces_.clear();
    }
}

// --- Interactive edits --------------------------------------------------------------------------

bool ModelingTool::begin_extrude() {
    if (modal_ != Edit::None || !mesh()) {
        return false;
    }
    if (faces_.empty()) {
        F3D_LOG_WARN("Extrude: pick faces first (click them; Shift+click adds)");
        return false;
    }
    const modeling::EditableMesh& m = *mesh();
    const glm::mat4 world = world_.world_matrix(world_.find(target_));
    glm::vec3 centre(0.0f), normal(0.0f);
    for (const std::uint32_t f : faces_) {
        centre += modeling::face_centre(m, f);
        normal += modeling::face_normal(m, f);
    }
    centre /= static_cast<float>(faces_.size());
    normal = glm::length(normal) > 1e-6f ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 world_normal = glm::mat3(world) * normal;
    original_ = m;
    pivot_ = glm::vec3(world * glm::vec4(centre, 1.0f));
    axis_ = glm::length(world_normal) > 1e-8f ? glm::normalize(world_normal) : glm::vec3(0.0f, 1.0f, 0.0f);
    local_scale_ = 1.0f / std::max(glm::length(world_normal), 1e-6f);
    // The faces' size, so an inset started near the centre still has room to move.
    const std::vector<std::uint32_t> offsets = modeling::face_offsets(m);
    float sum = 0.0f;
    std::size_t count = 0;
    for (const std::uint32_t f : faces_) {
        for (std::uint32_t i = 0; i < m.face_sizes[f]; ++i) {
            const glm::vec3 p = glm::vec3(world * glm::vec4(m.positions[m.corners[offsets[f] + i]], 1.0f));
            sum += glm::distance(p, pivot_);
            ++count;
        }
    }
    span_ = std::max(count ? sum / static_cast<float>(count) : 1.0f, 1e-3f);
    reference_.reset();
    typed_.clear();
    modal_value_ = 0.0f;
    shown_ = false;
    modal_ = Edit::Extrude;
    return true;
}

bool ModelingTool::begin_inset() {
    if (!begin_extrude()) { // the same pivot, axis and size
        return false;
    }
    modal_ = Edit::Inset;
    return true;
}

bool ModelingTool::begin_loop_cut() {
    if (modal_ != Edit::None || !mesh()) {
        return false;
    }
    typed_.clear();
    cut_edge_.reset();
    modal_value_ = 0.5f;
    modal_ = Edit::LoopCut;
    return true;
}

void ModelingTool::track(const glm::vec3& origin, const glm::vec3& direction) {
    float measure = 0.0f;
    if (modal_ == Edit::Extrude) {
        // Where the cursor ray passes closest to the line through the pivot along the axis.
        const glm::vec3 w = pivot_ - origin;
        const float b = glm::dot(axis_, direction);
        const float c = glm::dot(direction, direction);
        const float denom = c - b * b;
        if (denom < 1e-6f * c) {
            return; // looking straight down the axis
        }
        measure = (b * glm::dot(direction, w) - c * glm::dot(axis_, w)) / denom;
    } else {
        // How far from the pivot the cursor is, in the faces' plane.
        const float denom = glm::dot(axis_, direction);
        if (std::abs(denom) < 1e-6f) {
            return;
        }
        const float s = glm::dot(pivot_ - origin, axis_) / denom;
        if (s <= 0.0f) {
            return;
        }
        measure = glm::distance(origin + direction * s, pivot_);
    }
    if (!reference_) {
        reference_ = modal_ == Edit::Inset ? std::max(measure, span_ * 0.5f) : measure;
    }
    float value = 0.0f;
    if (modal_ == Edit::Extrude) {
        value = (measure - *reference_) * local_scale_;
        value = ctrl_held() ? snap(value, 0.1f) : value;
    } else {
        value = std::clamp(1.0f - measure / *reference_, 0.0f, 0.99f);
        value = ctrl_held() ? snap(value, 0.05f) : value;
    }
    if (const auto typed = parse_number(typed_)) {
        value = modal_ == Edit::Inset ? std::clamp(*typed, 0.0f, 0.99f) : *typed;
    }
    if (shown_ && value == modal_value_) {
        return;
    }
    modal_value_ = value;
    show(modal_ == Edit::Extrude ? modeling::extrude_faces(original_, faces_, value)
                                 : modeling::inset_faces(original_, faces_, value));
}

void ModelingTool::track_loop_cut(const glm::vec3& local_origin, const glm::vec3& local_direction) {
    cut_edge_.reset();
    const modeling::EditableMesh* m = mesh();
    const auto hit = m ? modeling::raycast(*m, local_origin, local_direction) : std::nullopt;
    if (!hit) {
        return;
    }
    hovered_ = hit->face;
    const glm::vec3 point = local_origin + local_direction * hit->distance;
    const std::uint32_t offset = modeling::face_offsets(*m)[hit->face];
    const std::uint32_t size = m->face_sizes[hit->face];
    // The face's edge nearest the cursor: the cut crosses it.
    float best = std::numeric_limits<float>::max();
    for (std::uint32_t i = 0; i < size; ++i) {
        const std::uint32_t a = m->corners[offset + i];
        const std::uint32_t b = m->corners[offset + (i + 1) % size];
        const glm::vec3 ab = m->positions[b] - m->positions[a];
        const float length2 = glm::dot(ab, ab);
        const float t = length2 > 0.0f ? std::clamp(glm::dot(point - m->positions[a], ab) / length2, 0.0f, 1.0f) : 0.0f;
        const float d = glm::distance(point, m->positions[a] + ab * t);
        if (d < best && !modeling::edge_ring(*m, a, b).empty()) {
            best = d;
            cut_edge_ = std::pair{a, b};
            // Where along the edge: snaps to the middle near it, Ctrl to tenths.
            float factor = std::clamp(t, 0.05f, 0.95f);
            factor = std::abs(factor - 0.5f) < 0.08f ? 0.5f : factor;
            modal_value_ = ctrl_held() ? std::clamp(snap(factor, 0.1f), 0.1f, 0.9f) : factor;
        }
    }
}

void ModelingTool::show(const modeling::EditableMesh& preview) {
    const entt::entity e = target_.is_null() ? entt::entity{entt::null} : world_.find(target_);
    if (e != entt::null && world_.registry().all_of<modeling::EditableMesh>(e)) {
        world_.registry().replace<modeling::EditableMesh>(e, preview);
        shown_ = true;
    }
}

void ModelingTool::end_modal() {
    if (shown_) {
        show(original_);
    }
    shown_ = false;
    modal_ = Edit::None;
    typed_.clear();
    reference_.reset();
    cut_edge_.reset();
}

bool ModelingTool::confirm() {
    if (modal_ == Edit::None) {
        return false;
    }
    LastEdit edit;
    edit.kind = modal_;
    edit.faces = faces_;
    edit.value = modal_value_;
    const std::optional<float> typed = parse_number(typed_);
    if (modal_ == Edit::LoopCut) {
        if (!cut_edge_) {
            return false;
        }
        edit.a = cut_edge_->first;
        edit.b = cut_edge_->second;
        edit.cuts = loop_cuts;
    } else if (typed) {
        edit.value = modal_ == Edit::Inset ? std::clamp(*typed, 0.0f, 0.99f) : *typed;
    }
    end_modal(); // the operation starts from the original mesh
    if (edit.kind == Edit::Extrude) {
        extrude_distance = edit.value;
    } else if (edit.kind == Edit::Inset) {
        inset_amount = edit.value;
    }
    return run_edit(edit);
}

bool ModelingTool::key_pressed(const std::string& key, bool ctrl, bool shift, bool alt) {
    if (modal_ != Edit::None) {
        if (key == "return" || key == "keypad enter") {
            confirm();
        } else if (key == "backspace") {
            if (!typed_.empty()) {
                typed_.pop_back();
            }
        } else if (const char c = number_char(key)) {
            if (modal_ == Edit::LoopCut) {
                // Typing sets the number of cuts.
                if (c >= '0' && c <= '9') {
                    std::string next = typed_ + c;
                    if (next.size() > 2) {
                        next = std::string(1, c); // start over
                    }
                    typed_ = next;
                    loop_cuts = static_cast<std::uint32_t>(std::clamp(std::stoi(next), 1, 64));
                }
            } else if (c != '-' || typed_.empty()) {
                typed_.push_back(c);
            }
        }
        // Escape and the right button reach the viewport, which cancels. Every other key
        // is swallowed so no shortcut (undo, delete, ...) runs underneath the edit.
        return true;
    }
    if (target_.is_null() || alt || shift) {
        return false;
    }
    if (!ctrl && key == "e") {
        begin_extrude();
        return true;
    }
    if (!ctrl && key == "i") {
        begin_inset();
        return true;
    }
    if (ctrl && key == "r") {
        begin_loop_cut();
        return true;
    }
    return false;
}

bool ModelingTool::wheel(float notches) {
    if (modal_ != Edit::LoopCut || notches == 0.0f) {
        return false;
    }
    const int cuts = static_cast<int>(loop_cuts) + (notches > 0.0f ? 1 : -1);
    loop_cuts = static_cast<std::uint32_t>(std::clamp(cuts, 1, 64));
    typed_.clear();
    return true;
}

std::string ModelingTool::status() const {
    char text[256];
    const std::string typed = typed_.empty() ? std::string() : " [" + typed_ + "]";
    switch (modal_) {
    case Edit::Extrude:
        std::snprintf(text, sizeof(text),
                      "Extrude %.3f m%s   Move the mouse or type a value, Ctrl snaps. Click/Enter: confirm, "
                      "right-click/Esc: cancel",
                      modal_value_, typed.c_str());
        return text;
    case Edit::Inset:
        std::snprintf(text, sizeof(text),
                      "Inset %.2f%s   Move towards the centre or type a value (0-0.99), Ctrl snaps. Click/Enter: "
                      "confirm, right-click/Esc: cancel",
                      modal_value_, typed.c_str());
        return text;
    case Edit::LoopCut:
        if (!cut_edge_) {
            std::snprintf(text, sizeof(text), "Loop cut (%u)   Hover an edge between quads. Right-click/Esc: cancel",
                          loop_cuts);
        } else {
            std::snprintf(text, sizeof(text),
                          "Loop cut: %u cut%s at %.2f   Wheel or type for more cuts, Ctrl snaps. Click: cut, "
                          "right-click/Esc: cancel",
                          loop_cuts, loop_cuts == 1 ? "" : "s", modal_value_);
        }
        return text;
    case Edit::None:
        break;
    }
    if (target_.is_null()) {
        return "Modeling: select an editable mesh";
    }
    std::snprintf(text, sizeof(text), "%zu face%s picked   E: extrude   I: inset   Ctrl+R: loop cut   Del: delete",
                  faces_.size(), faces_.size() == 1 ? "" : "s");
    return text;
}

void ModelingTool::draw() const {
    const modeling::EditableMesh* m = mesh();
    if (!m || !modeling::is_valid(*m)) {
        return;
    }
    const glm::mat4 world = world_.world_matrix(world_.find(target_));
    const auto at = [&](const modeling::EditableMesh& mesh, std::uint32_t position) {
        return glm::vec3(world * glm::vec4(mesh.positions[position], 1.0f));
    };
    const std::vector<std::uint32_t> offsets = modeling::face_offsets(*m);
    const auto outline = [&](std::uint32_t face, std::uint32_t color, float lift) {
        const glm::vec3 n = glm::normalize(glm::mat3(world) * modeling::face_normal(*m, face)) * lift;
        const std::uint32_t size = m->face_sizes[face];
        for (std::uint32_t i = 0; i < size; ++i) {
            core::debug::line(at(*m, m->corners[offsets[face] + i]) + n,
                              at(*m, m->corners[offsets[face] + (i + 1) % size]) + n, color);
        }
    };
    // The wireframe, so faces can be told apart.
    if (m->corners.size() <= kMaxWireEdges) {
        const std::uint32_t wire = packed(current_theme_colors().edge);
        for (std::uint32_t f = 0; f < m->face_count(); ++f) {
            outline(f, wire, 0.0005f);
        }
    }
    const std::uint32_t selected = packed(current_theme_colors().face_selected);
    for (const std::uint32_t f : faces_) {
        if (f >= m->face_count()) {
            continue;
        }
        outline(f, selected, 0.002f);
        // A cross through the centre marks the face's fill.
        const std::uint32_t size = m->face_sizes[f];
        const glm::vec3 centre = glm::vec3(world * glm::vec4(modeling::face_centre(*m, f), 1.0f));
        for (std::uint32_t i = 0; i < size; ++i) {
            core::debug::line(centre, glm::mix(centre, at(*m, m->corners[offsets[f] + i]), 0.6f), selected);
        }
    }
    if (hovered_ && *hovered_ < m->face_count() && modal_ == Edit::None) {
        outline(*hovered_, packed(current_theme_colors().face_hovered), 0.003f);
    }
    // The loop cut under the cursor: the new edges it would add.
    if (modal_ == Edit::LoopCut && cut_edge_) {
        const modeling::EditableMesh cut =
            modeling::loop_cut(*m, cut_edge_->first, cut_edge_->second, loop_cuts, modal_value_);
        const auto old_count = static_cast<std::uint32_t>(m->positions.size());
        const std::vector<std::uint32_t> cut_offsets = modeling::face_offsets(cut);
        const std::uint32_t color = core::debug::rgba8(255, 220, 60);
        for (std::size_t f = 0; f < cut.face_count(); ++f) {
            const std::uint32_t size = cut.face_sizes[f];
            for (std::uint32_t i = 0; i < size; ++i) {
                const std::uint32_t a = cut.corners[cut_offsets[f] + i];
                const std::uint32_t b = cut.corners[cut_offsets[f] + (i + 1) % size];
                if (a >= old_count && b >= old_count) {
                    core::debug::line(at(cut, a), at(cut, b), color);
                }
            }
        }
    }
}

// --- Edits --------------------------------------------------------------------------------------

bool ModelingTool::run(const char* id, nlohmann::json params, bool keep_faces) {
    if (target_.is_null()) {
        F3D_LOG_WARN("Modeling: select an editable mesh first (Add > Shape, or Tools > Modeling > Make Editable)");
        return false;
    }
    params["entity"] = target_.to_string();
    auto op = registry_.create(id, params);
    if (!op || !stack_.execute(std::move(op))) {
        return false;
    }
    if (!keep_faces) {
        faces_.clear();
    }
    prune();
    return true;
}

bool ModelingTool::run_edit(const LastEdit& edit) {
    bool ok = false;
    switch (edit.kind) {
    case Edit::Extrude:
        ok = run("modeling.extrude", {{"faces", edit.faces}, {"distance", edit.value}}, true);
        break;
    case Edit::Inset:
        ok = run("modeling.inset", {{"faces", edit.faces}, {"amount", edit.value}}, true);
        break;
    case Edit::LoopCut:
        // The split faces are renumbered: the picked faces no longer mean the same.
        ok = run("modeling.loop_cut",
                 {{"a", edit.a}, {"b", edit.b}, {"cuts", edit.cuts}, {"factor", edit.value}}, false);
        break;
    case Edit::None:
        break;
    }
    if (ok) {
        last_ = edit;
        last_state_ = stack_.state_id();
        last_target_ = target_;
    }
    return ok;
}

const ModelingTool::LastEdit* ModelingTool::last_edit() const {
    if (!last_ || last_target_ != target_ || target_.is_null() || stack_.state_id() != last_state_) {
        return nullptr;
    }
    return &*last_;
}

bool ModelingTool::adjust_last(float value, std::uint32_t cuts) {
    const LastEdit* last = last_edit();
    if (!last || modal_ != Edit::None) {
        return false;
    }
    LastEdit edit = *last;
    edit.value = value;
    edit.cuts = std::clamp(cuts, 1u, 64u);
    const std::vector<std::uint32_t> faces = faces_;
    if (!stack_.undo()) {
        return false;
    }
    faces_ = edit.faces;
    prune();
    if (run_edit(edit)) {
        return true;
    }
    stack_.redo(); // the new value failed: keep the edit as it was
    faces_ = faces;
    last_state_ = stack_.state_id();
    return false;
}

bool ModelingTool::set_shape(const modeling::ShapeParams& params) {
    return run("modeling.set_shape",
               {{"shape", std::string(modeling::to_string(params.shape))},
                {"size", {params.size.x, params.size.y, params.size.z}},
                {"segments_a", params.segments_a},
                {"segments_b", params.segments_b}},
               false);
}

bool ModelingTool::extrude(float distance) {
    if (faces_.empty()) {
        F3D_LOG_WARN("Extrude: pick faces first (Tools > Modeling > Select Faces, then click them)");
        return false;
    }
    return run_edit({Edit::Extrude, faces_, distance});
}

bool ModelingTool::inset(float amount) {
    if (faces_.empty()) {
        F3D_LOG_WARN("Inset: pick faces first (Tools > Modeling > Select Faces, then click them)");
        return false;
    }
    return run_edit({Edit::Inset, faces_, amount});
}

bool ModelingTool::loop_cut(std::uint32_t a, std::uint32_t b, std::uint32_t cuts, float factor) {
    return run_edit({Edit::LoopCut, {}, factor, cuts, a, b});
}

bool ModelingTool::delete_faces() {
    if (faces_.empty()) {
        return false;
    }
    return run("modeling.delete_faces", {{"faces", faces_}}, false);
}

bool ModelingTool::flip() {
    return run("modeling.flip", {{"faces", faces_}}, true);
}

bool ModelingTool::subdivide(std::uint32_t levels) {
    return run("modeling.subdivide", {{"levels", levels}}, false);
}

bool ModelingTool::mirror(int axis) {
    return run("modeling.mirror", {{"axis", axis}, {"weld", std::max(merge_distance, 1e-5f)}}, false);
}

bool ModelingTool::merge_vertices(float distance) {
    return run("modeling.merge_vertices", {{"distance", distance}}, false);
}

bool ModelingTool::set_origin(const char* mode) {
    return run("modeling.set_origin", {{"mode", mode}}, true);
}

bool ModelingTool::set_smoothing(float degrees) {
    if (target_.is_null()) {
        return false;
    }
    auto op = registry_.create("entity.set_property", {{"entity", target_.to_string()},
                                                       {"path", "EditableMesh.smoothing_angle"},
                                                       {"value", degrees}});
    return op && stack_.execute(std::move(op));
}

} // namespace f3d::editor
