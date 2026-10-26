// SPDX-License-Identifier: Apache-2.0
#include "f3d/assets/raycast.hpp"

#include "f3d/assets/mesh_library.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/core/snap.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/layers.hpp"
#include "f3d/scene/world.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace f3d::assets {
namespace {

constexpr std::uint32_t kLeafSize = 4;

[[nodiscard]] glm::vec3 transform_point(const glm::mat4& m, const glm::vec3& p) {
    return glm::vec3(m * glm::vec4(p, 1.0f));
}

} // namespace

MeshHit ray_mesh(const glm::vec3& origin, const glm::vec3& direction, const MeshData& mesh, float t_max) {
    MeshHit result;
    float best = t_max;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const glm::vec3& v0 = mesh.vertices[mesh.indices[i]].position;
        const glm::vec3& v1 = mesh.vertices[mesh.indices[i + 1]].position;
        const glm::vec3& v2 = mesh.vertices[mesh.indices[i + 2]].position;
        float t = 0.0f;
        float u = 0.0f;
        float v = 0.0f;
        if (core::ray_triangle(origin, direction, v0, v1, v2, t, u, v) && t < best) {
            best = t;
            result.hit = true;
            result.t = t;
            // Face normal (flat shading); the caller can smooth it if needed.
            result.normal = glm::normalize(glm::cross(v1 - v0, v2 - v0));
        }
    }
    return result;
}

void SceneBvh::build(scene::World& world, MeshLibrary& meshes, const EntityFilter& exclude) {
    F3D_ZONE;
    items_.clear();
    nodes_.clear();
    world.update_transforms();
    const auto& registry = world.registry();
    for (const entt::entity e : world.depth_first()) {
        const auto* renderer = registry.try_get<scene::MeshRenderer>(e);
        if (!renderer || !renderer->visible || !scene::is_visible(world, e) || (exclude && exclude(e))) {
            continue;
        }
        auto mesh = meshes.get(renderer->mesh);
        if (!mesh || mesh->indices.empty()) {
            continue;
        }
        Item item;
        item.entity = e;
        item.world = registry.get<scene::Transform>(e).world;
        item.inverse = glm::inverse(item.world);
        item.bounds = mesh->bounds.transformed(item.world);
        item.mesh = std::move(mesh);
        items_.push_back(std::move(item));
    }
    if (!items_.empty()) {
        nodes_.reserve(items_.size() * 2);
        nodes_.emplace_back();
        build_node(0, 0, static_cast<std::uint32_t>(items_.size()));
    }
}

void SceneBvh::build_node(std::uint32_t index, std::uint32_t first, std::uint32_t count) {
    core::Bounds bounds;
    core::Bounds centres;
    for (std::uint32_t i = first; i < first + count; ++i) {
        bounds.add(items_[i].bounds);
        centres.add(items_[i].bounds.center());
    }
    nodes_[index].bounds = bounds;
    if (count <= kLeafSize) {
        nodes_[index].first = first;
        nodes_[index].count = count;
        return;
    }
    // Median split along the longest axis of the centres.
    const glm::vec3 extent = centres.extent();
    const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
    const std::uint32_t half = count / 2;
    std::nth_element(
        items_.begin() + first, items_.begin() + first + half, items_.begin() + first + count,
        [axis](const Item& a, const Item& b) { return a.bounds.center()[axis] < b.bounds.center()[axis]; });
    // Children are allocated as a pair: right = left + 1.
    const auto left = static_cast<std::uint32_t>(nodes_.size());
    nodes_.emplace_back();
    nodes_.emplace_back();
    nodes_[index].first = left;
    nodes_[index].count = 0;
    build_node(left, first, half);
    build_node(left + 1, first + half, count - half);
}

SceneHit SceneBvh::raycast(const glm::vec3& origin, const glm::vec3& direction, const EntityFilter& skip) const {
    F3D_ZONE;
    SceneHit result;
    last_tested_ = 0;
    if (nodes_.empty() || glm::dot(direction, direction) == 0.0f) {
        return result;
    }
    const glm::vec3 dir = glm::normalize(direction);
    float best = std::numeric_limits<float>::max();
    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
        const Node& node = nodes_[stack.back()];
        stack.pop_back();
        float t_node = 0.0f;
        if (!core::ray_aabb(origin, dir, node.bounds, best, t_node)) {
            continue;
        }
        if (node.count == 0) {
            stack.push_back(node.first);
            stack.push_back(node.first + 1);
            continue;
        }
        for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
            const Item& item = items_[i];
            float t_item = 0.0f;
            if ((skip && skip(item.entity)) || !core::ray_aabb(origin, dir, item.bounds, best, t_item)) {
                continue;
            }
            ++last_tested_;
            // Test in mesh space; t is then in mesh-space units, so measure the hit in world space.
            const glm::vec3 local_origin = transform_point(item.inverse, origin);
            const glm::vec3 local_dir = glm::vec3(item.inverse * glm::vec4(dir, 0.0f));
            const MeshHit hit = ray_mesh(local_origin, local_dir, *item.mesh);
            if (!hit.hit) {
                continue;
            }
            const glm::vec3 point = transform_point(item.world, local_origin + local_dir * hit.t);
            const float distance = glm::distance(origin, point);
            if (distance < best) {
                best = distance;
                result.hit = true;
                result.entity = item.entity;
                result.point = point;
                result.distance = distance;
                result.normal = glm::normalize(glm::vec3(glm::transpose(item.inverse) * glm::vec4(hit.normal, 0.0f)));
            }
        }
    }
    return result;
}

SceneHit SceneBvh::nearest_vertex(const glm::vec3& origin, const glm::vec3& direction, float max_angle,
                                  const EntityFilter& skip) const {
    F3D_ZONE;
    SceneHit result;
    if (items_.empty() || glm::dot(direction, direction) == 0.0f) {
        return result;
    }
    const glm::vec3 dir = glm::normalize(direction);
    const float max_tan = std::tan(std::clamp(max_angle, 0.0f, 1.5f));
    float best = max_tan; // tangent of the angle between the ray and the vertex
    for (const Item& item : items_) {
        if (skip && skip(item.entity)) {
            continue;
        }
        // Cone vs bounding sphere: skip meshes entirely outside the cone.
        const glm::vec3 centre = item.bounds.center();
        const float radius = glm::length(item.bounds.extent()) * 0.5f;
        float t_centre = 0.0f;
        const float d_centre = core::ray_point_distance(origin, dir, centre, t_centre);
        if (d_centre - radius > max_tan * (t_centre + radius)) {
            continue;
        }
        for (const MeshVertex& v : item.mesh->vertices) {
            const glm::vec3 p = transform_point(item.world, v.position);
            float t = 0.0f;
            const float d = core::ray_point_distance(origin, dir, p, t);
            if (t <= 1e-6f) {
                continue; // behind the camera
            }
            const float tan_angle = d / t;
            if (tan_angle < best || (result.hit && tan_angle == best && t < result.distance)) {
                best = tan_angle;
                result.hit = true;
                result.entity = item.entity;
                result.point = p;
                result.distance = t;
                result.normal = glm::normalize(glm::vec3(glm::transpose(item.inverse) * glm::vec4(v.normal, 0.0f)));
            }
        }
    }
    return result;
}

SceneHit raycast_scene(scene::World& world, MeshLibrary& meshes, const glm::vec3& origin, const glm::vec3& direction) {
    SceneBvh bvh;
    bvh.build(world, meshes);
    return bvh.raycast(origin, direction);
}

} // namespace f3d::assets
