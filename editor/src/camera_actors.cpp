// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/camera_actors.hpp"

#include "f3d/core/debug_draw.hpp"
#include "f3d/editor/preferences.hpp"
#include "f3d/editor/scene_editor.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace f3d::editor {

namespace {

constexpr std::uint32_t kCameraColor = core::debug::rgba8(170, 170, 180);
constexpr std::uint32_t kPinnedColor = core::debug::rgba8(110, 190, 255);
constexpr float kFrustumDepth = 1.5f; ///< metres of frustum drawn in front of each camera

glm::mat4 without_scale(const glm::mat4& m) {
    glm::mat4 out(1.0f);
    for (int axis = 0; axis < 3; ++axis) {
        const glm::vec3 v(m[axis]);
        const float length = glm::length(v);
        out[axis] = glm::vec4(length > 1e-8f ? v / length : glm::vec3(0.0f), 0.0f);
    }
    out[3] = m[3];
    return out;
}

/// Pixel position (y down) of a world point, or none when it is behind the view.
std::optional<glm::vec2> to_pixels(const render::View& view, const glm::vec3& p) {
    const glm::vec4 clip = view.view_projection() * glm::vec4(p, 1.0f);
    if (clip.w <= 1e-5f) {
        return std::nullopt;
    }
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    return glm::vec2((ndc.x * 0.5f + 0.5f) * static_cast<float>(view.width),
                     (0.5f - ndc.y * 0.5f) * static_cast<float>(view.height));
}

nlohmann::json json_of(const glm::vec3& v) {
    return {v.x, v.y, v.z};
}

nlohmann::json json_of(const glm::quat& q) {
    return {q.w, q.x, q.y, q.z};
}

} // namespace

render::View scene_camera_view(const glm::mat4& world, const scene::Camera& camera, std::uint32_t width,
                               std::uint32_t height) {
    render::View view;
    view.width = std::max(width, 1u);
    view.height = std::max(height, 1u);
    const glm::mat4 eye = without_scale(world);
    view.view = glm::inverse(eye);
    view.camera_position = glm::vec3(eye[3]);
    view.near_plane = std::max(camera.near_plane, 1e-4f);
    view.far_plane = std::max(camera.far_plane, view.near_plane * 2.0f);
    view.orthographic = camera.projection == scene::CameraProjection::Orthographic;
    const float aspect = static_cast<float>(view.width) / static_cast<float>(view.height);
    if (view.orthographic) {
        const float h = std::max(camera.ortho_height, 1e-3f) * 0.5f;
        const float w = h * aspect;
        view.projection = glm::orthoRH_ZO(-w, w, -h, h, view.near_plane, view.far_plane);
    } else {
        view.projection =
            glm::perspectiveRH_ZO(std::clamp(camera.fov_y, 0.01f, 3.13f), aspect, view.near_plane, view.far_plane);
    }
    return view;
}

void align_editor_camera(EditorCamera& camera, const glm::mat4& world) {
    const glm::mat4 eye = without_scale(world);
    const glm::vec3 position(eye[3]);
    const glm::vec3 forward = -glm::vec3(eye[2]);
    const glm::vec3 right(eye[0]);
    camera.pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    // Yaw from the look direction; straight up or down, from the right axis instead.
    camera.yaw = glm::length(glm::vec2(forward.x, forward.z)) > 1e-3f ? std::atan2(-forward.x, -forward.z)
                                                                      : std::atan2(-right.z, right.x);
    camera.target = position + camera.forward() * camera.distance;
}

glm::mat4 editor_camera_matrix(const EditorCamera& camera) {
    return glm::translate(glm::mat4(1.0f), camera.position()) * glm::mat4_cast(camera.orientation());
}

CameraActors::CameraActors(scene::World& world, SceneEditor& editor) : world_(world), editor_(editor) {}

bool CameraActors::is_camera(const core::Guid& entity) const {
    const entt::entity e = entity.is_null() ? entt::entity{entt::null} : world_.find(entity);
    return e != entt::null && world_.registry().all_of<scene::Camera>(e);
}

core::Guid CameraActors::selected_camera() const {
    const core::Guid& active = world_.selection().active();
    return is_camera(active) ? active : core::Guid{};
}

std::string CameraActors::name(const core::Guid& entity) const {
    const entt::entity e = entity.is_null() ? entt::entity{entt::null} : world_.find(entity);
    return e == entt::null ? std::string() : world_.name(e);
}

core::Guid CameraActors::preview_camera() const {
    const core::Guid& active = world_.selection().active();
    if (is_camera(active)) {
        return active;
    }
    return is_camera(pinned_) ? pinned_ : core::Guid{};
}

std::optional<render::View> CameraActors::view(const core::Guid& entity, std::uint32_t width,
                                               std::uint32_t height) const {
    if (!is_camera(entity)) {
        return std::nullopt;
    }
    const entt::entity e = world_.find(entity);
    return scene_camera_view(world_.world_matrix(e), world_.registry().get<scene::Camera>(e), width, height);
}

float CameraActors::aspect(const core::Guid& entity) const {
    if (!is_camera(entity)) {
        return 16.0f / 9.0f;
    }
    return std::clamp(world_.registry().get<scene::Camera>(world_.find(entity)).aspect_ratio, 0.1f, 10.0f);
}

void CameraActors::toggle_pin(const core::Guid& entity) {
    pinned_ = pinned_ == entity ? core::Guid{} : entity;
}

bool CameraActors::pilot(const core::Guid& entity, EditorCamera& camera) {
    if (!is_camera(entity)) {
        return false;
    }
    if (piloted_.is_null()) {
        saved_projection_ = camera.projection;
        saved_fov_ = camera.fov_y;
        saved_near_ = camera.near_plane;
        saved_far_ = camera.far_plane;
    }
    piloted_ = entity;
    const entt::entity e = world_.find(entity);
    const scene::Camera& lens = world_.registry().get<scene::Camera>(e);
    camera.projection =
        lens.projection == scene::CameraProjection::Orthographic ? Projection::Orthographic : Projection::Perspective;
    camera.fov_y = std::clamp(lens.fov_y, 0.01f, 3.13f);
    camera.near_plane = lens.near_plane;
    camera.far_plane = lens.far_plane;
    last_world_ = world_.world_matrix(e);
    align_editor_camera(camera, last_world_);
    last_view_ = camera.view_matrix();
    return true;
}

void CameraActors::eject(EditorCamera& camera) {
    if (piloted_.is_null()) {
        return;
    }
    if (pilot_moving_) {
        editor_.stack().end_transaction();
        pilot_moving_ = false;
    }
    piloted_ = {};
    camera.projection = saved_projection_;
    camera.fov_y = saved_fov_;
    camera.near_plane = saved_near_;
    camera.far_plane = saved_far_;
}

void CameraActors::update(EditorCamera& camera) {
    if (piloted_.is_null()) {
        return;
    }
    if (!is_camera(piloted_)) {
        eject(camera); // deleted, or its Camera component removed (undo)
        return;
    }
    const entt::entity e = world_.find(piloted_);
    const bool view_moved = camera.view_matrix() != last_view_;
    if (view_moved) {
        // One undo step per continuous move: the transaction stays open while the view moves.
        if (!pilot_moving_) {
            editor_.stack().begin_transaction("Pilot " + world_.name(e));
            pilot_moving_ = true;
        }
        write_pilot_transform(camera);
        last_world_ = world_.world_matrix(e);
        last_view_ = camera.view_matrix();
        return;
    }
    if (pilot_moving_) {
        editor_.stack().end_transaction();
        pilot_moving_ = false;
    }
    if (const glm::mat4& world = world_.world_matrix(e); world != last_world_) {
        // Moved by something else (gizmo, inspector, undo): the view follows.
        align_editor_camera(camera, world);
        last_world_ = world;
        last_view_ = camera.view_matrix();
    }
    // The lens may be edited in the inspector while piloting.
    const scene::Camera& lens = world_.registry().get<scene::Camera>(e);
    camera.fov_y = std::clamp(lens.fov_y, 0.01f, 3.13f);
    camera.near_plane = lens.near_plane;
    camera.far_plane = lens.far_plane;
}

void CameraActors::write_pilot_transform(const EditorCamera& camera) {
    const entt::entity e = world_.find(piloted_);
    const entt::entity parent = world_.parent(e);
    const glm::vec3 scale = world_.transform(e).scale;
    const glm::mat4 world = editor_camera_matrix(camera) * glm::scale(glm::mat4(1.0f), scale);
    const glm::mat4 parent_world = parent == entt::null ? glm::mat4(1.0f) : world_.world_matrix(parent);
    glm::vec3 translation, local_scale;
    glm::quat rotation;
    scene::decompose(glm::inverse(parent_world) * world, translation, rotation, local_scale);
    const std::string guid = piloted_.to_string();
    editor_.run("entity.set_property",
                {{"entity", guid}, {"path", "Transform.translation"}, {"value", json_of(translation)}}, true);
    editor_.run("entity.set_property", {{"entity", guid}, {"path", "Transform.rotation"}, {"value", json_of(rotation)}},
                true);
}

core::Guid CameraActors::create_at_view(const EditorCamera& camera) {
    int n = 1;
    const auto name_used = [&](const std::string& name) {
        for (const entt::entity e : world_.depth_first()) {
            if (world_.name(e) == name) {
                return true;
            }
        }
        return false;
    };
    while (name_used("Camera " + std::to_string(n))) {
        ++n;
    }
    const std::string name = "Camera " + std::to_string(n);
    const core::Guid guid = core::Guid::generate();
    glm::vec3 translation, scale;
    glm::quat rotation;
    scene::decompose(editor_camera_matrix(camera), translation, rotation, scale);

    editor_.stack().begin_transaction("Add " + name);
    bool ok = editor_.run("entity.create", {{"name", name}, {"parent", nullptr}, {"guid", guid.to_string()}});
    ok = ok && editor_.run("entity.add_component", {{"entity", guid.to_string()}, {"component", "Camera"}});
    ok = ok && editor_.run("entity.set_property", {{"entity", guid.to_string()},
                                                   {"path", "Transform.translation"},
                                                   {"value", json_of(translation)}});
    ok = ok && editor_.run("entity.set_property",
                           {{"entity", guid.to_string()}, {"path", "Transform.rotation"}, {"value", json_of(rotation)}});
    if (!ok) {
        editor_.stack().abort_transaction();
        return {};
    }
    editor_.set_selection({guid}, guid);
    editor_.stack().end_transaction();
    return guid;
}

bool CameraActors::snap_to_view(const core::Guid& entity, const EditorCamera& camera) {
    const entt::entity e = entity.is_null() ? entt::entity{entt::null} : world_.find(entity);
    if (e == entt::null) {
        return false;
    }
    const entt::entity parent = world_.parent(e);
    const glm::mat4 world = editor_camera_matrix(camera) * glm::scale(glm::mat4(1.0f), world_.transform(e).scale);
    const glm::mat4 parent_world = parent == entt::null ? glm::mat4(1.0f) : world_.world_matrix(parent);
    glm::vec3 translation, scale;
    glm::quat rotation;
    scene::decompose(glm::inverse(parent_world) * world, translation, rotation, scale);
    editor_.stack().begin_transaction("Snap " + world_.name(e) + " to View");
    editor_.run("entity.set_property",
                {{"entity", entity.to_string()}, {"path", "Transform.translation"}, {"value", json_of(translation)}});
    editor_.run("entity.set_property",
                {{"entity", entity.to_string()}, {"path", "Transform.rotation"}, {"value", json_of(rotation)}});
    return editor_.stack().end_transaction();
}

void CameraActors::draw_gizmos() {
    const scene::Selection& selection = world_.selection();
    const auto cameras = world_.registry().view<scene::Camera>();
    for (const entt::entity e : cameras) {
        const core::Guid& guid = world_.guid(e);
        if (guid == piloted_) {
            continue; // the viewport is looking through it
        }
        const scene::Camera& lens = cameras.get<scene::Camera>(e);
        const std::uint32_t color = selection.contains(guid) ? packed(current_theme_colors().active)
                                    : guid == pinned_       ? kPinnedColor
                                                            : kCameraColor;
        const glm::mat4 m = without_scale(world_.world_matrix(e));
        const auto at = [&](float x, float y, float z) { return glm::vec3(m * glm::vec4(x, y, z, 1.0f)); };

        // The body: a box behind the lens, a lens cone and a film reel on top (Unreal's camera mesh).
        core::debug::box(m * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.22f)),
                         glm::vec3(0.1f, 0.14f, 0.22f), color);
        const std::array<glm::vec3, 4> lens_ring{at(-0.09f, -0.09f, -0.16f), at(0.09f, -0.09f, -0.16f),
                                                 at(0.09f, 0.09f, -0.16f), at(-0.09f, 0.09f, -0.16f)};
        const std::array<glm::vec3, 4> lens_base{at(-0.05f, -0.05f, 0.0f), at(0.05f, -0.05f, 0.0f),
                                                 at(0.05f, 0.05f, 0.0f), at(-0.05f, 0.05f, 0.0f)};
        for (std::size_t i = 0; i < 4; ++i) {
            core::debug::line(lens_ring[i], lens_ring[(i + 1) % 4], color);
            core::debug::line(lens_base[i], lens_ring[i], color);
        }
        for (const float z : {0.12f, 0.34f}) {
            const glm::vec3 c = at(0.0f, 0.26f, z);
            constexpr int kSegments = 12;
            for (int i = 0; i < kSegments; ++i) {
                const float a0 = glm::two_pi<float>() * static_cast<float>(i) / kSegments;
                const float a1 = glm::two_pi<float>() * static_cast<float>(i + 1) / kSegments;
                core::debug::line(c + glm::vec3(m * glm::vec4(0.0f, std::sin(a0), std::cos(a0), 0.0f)) * 0.11f,
                                  c + glm::vec3(m * glm::vec4(0.0f, std::sin(a1), std::cos(a1), 0.0f)) * 0.11f, color);
            }
        }

        // The frustum, from the lens to kFrustumDepth (or the far plane), with an "up" marker.
        const float aspect = std::clamp(lens.aspect_ratio, 0.1f, 10.0f);
        const float depth = std::min(kFrustumDepth, std::max(lens.far_plane, 0.01f));
        float near_h, far_h, near_z = -0.16f;
        if (lens.projection == scene::CameraProjection::Orthographic) {
            near_h = far_h = std::max(lens.ortho_height, 1e-3f) * 0.5f;
        } else {
            const float t = std::tan(std::clamp(lens.fov_y, 0.01f, 3.13f) * 0.5f);
            near_h = t * -near_z;
            far_h = t * depth;
        }
        const auto rect = [&](float h, float z) {
            const float w = h * aspect;
            return std::array<glm::vec3, 4>{at(-w, -h, z), at(w, -h, z), at(w, h, z), at(-w, h, z)};
        };
        const auto near_rect = rect(near_h, near_z);
        const auto far_rect = rect(far_h, -depth);
        for (std::size_t i = 0; i < 4; ++i) {
            core::debug::line(far_rect[i], far_rect[(i + 1) % 4], color);
            core::debug::line(near_rect[i], far_rect[i], color);
        }
        const float w = far_h * aspect;
        core::debug::line(at(-w * 0.3f, far_h * 1.08f, -depth), at(0.0f, far_h * 1.35f, -depth), color);
        core::debug::line(at(0.0f, far_h * 1.35f, -depth), at(w * 0.3f, far_h * 1.08f, -depth), color);
        core::debug::line(at(w * 0.3f, far_h * 1.08f, -depth), at(-w * 0.3f, far_h * 1.08f, -depth), color);
    }
}

core::Guid CameraActors::pick(const render::View& view, float x, float y, float radius) const {
    core::Guid best;
    float best_distance = std::numeric_limits<float>::max();
    const auto cameras = world_.registry().view<scene::Camera>();
    for (const entt::entity e : cameras) {
        const core::Guid& guid = world_.guid(e);
        if (guid == piloted_) {
            continue;
        }
        const glm::mat4 m = without_scale(world_.world_matrix(e));
        // The body's centre and its lens: either counts as a hit.
        for (const glm::vec3 local : {glm::vec3(0.0f, 0.0f, 0.15f), glm::vec3(0.0f, 0.0f, -0.1f)}) {
            const std::optional<glm::vec2> p = to_pixels(view, glm::vec3(m * glm::vec4(local, 1.0f)));
            if (!p) {
                continue;
            }
            const float d = glm::distance(*p, glm::vec2(x, y));
            if (d <= radius && d < best_distance) {
                best_distance = d;
                best = guid;
            }
        }
    }
    return best;
}

std::vector<core::Guid> CameraActors::pick_region(const render::View& view, float x0, float y0, float x1,
                                                  float y1) const {
    std::vector<core::Guid> out;
    const auto cameras = world_.registry().view<scene::Camera>();
    for (const entt::entity e : cameras) {
        const core::Guid& guid = world_.guid(e);
        if (guid == piloted_) {
            continue;
        }
        const std::optional<glm::vec2> p = to_pixels(view, glm::vec3(world_.world_matrix(e)[3]));
        if (p && p->x >= x0 && p->x < x1 && p->y >= y0 && p->y < y1) {
            out.push_back(guid);
        }
    }
    return out;
}

} // namespace f3d::editor
