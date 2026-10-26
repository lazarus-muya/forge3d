// SPDX-License-Identifier: Apache-2.0
#include "f3d/scene/world.hpp"

#include "f3d/core/assert.hpp"
#include "f3d/core/profile.hpp"

#include <utility>

namespace f3d::scene {

glm::mat4 Transform::local_matrix() const noexcept {
    glm::mat4 m = glm::mat4_cast(rotation);
    m[0] *= scale.x;
    m[1] *= scale.y;
    m[2] *= scale.z;
    m[3] = glm::vec4(translation, 1.0f);
    return m;
}

void decompose(const glm::mat4& m, glm::vec3& translation, glm::quat& rotation, glm::vec3& scale) {
    translation = glm::vec3(m[3]);
    glm::vec3 cols[3] = {glm::vec3(m[0]), glm::vec3(m[1]), glm::vec3(m[2])};
    scale = {glm::length(cols[0]), glm::length(cols[1]), glm::length(cols[2])};
    if (glm::determinant(glm::mat3(m)) < 0.0f) {
        scale.x = -scale.x; // a mirror: put it on X so the rest is a proper rotation
    }
    for (int i = 0; i < 3; ++i) {
        if (scale[i] != 0.0f) {
            cols[i] /= scale[i];
        }
    }
    rotation = glm::normalize(glm::quat_cast(glm::mat3(cols[0], cols[1], cols[2])));
}

World::World() {
    registry_.on_destroy<core::Guid>().connect<&World::on_guid_destroyed>(this);
}

World::~World() {
    registry_.on_destroy<core::Guid>().disconnect(this);
}

void World::on_guid_destroyed(entt::registry& registry, entt::entity e) {
    guid_map_.erase(registry.get<core::Guid>(e));
}

bool World::valid(entt::entity e) const noexcept {
    return e != entt::null && registry_.valid(e) && registry_.all_of<core::Guid>(e);
}

entt::entity World::create(std::string_view name, entt::entity parent, core::Guid guid, std::size_t index) {
    if (guid.is_null()) {
        guid = core::Guid::generate();
    }
    if (guid_map_.contains(guid) || (parent != entt::null && !valid(parent))) {
        return entt::null;
    }
    const entt::entity e = registry_.create();
    registry_.emplace<Name>(e, std::string(name));
    registry_.emplace<core::Guid>(e, guid);
    registry_.emplace<Transform>(e);
    registry_.emplace<Hierarchy>(e);
    guid_map_.emplace(guid, e);
    link(e, parent, index);
    transforms_dirty_ = true;
    return e;
}

void World::destroy(entt::entity e) {
    if (!valid(e)) {
        return;
    }
    const auto doomed = subtree(e);
    unlink(e);
    // Children first: the subtree is pre-order, so walk it backwards.
    for (auto it = doomed.rbegin(); it != doomed.rend(); ++it) {
        registry_.destroy(*it);
    }
}

void World::clear() {
    registry_.clear();
    guid_map_.clear();
    selection_.clear();
    layers_.clear();
    bookmarks_.clear();
    view_request_.reset();
    first_root_ = last_root_ = entt::null;
    root_count_ = 0;
    transforms_dirty_ = false;
}

entt::entity World::find(const core::Guid& guid) const {
    const auto it = guid_map_.find(guid);
    return it == guid_map_.end() ? entt::null : it->second;
}

const core::Guid& World::guid(entt::entity e) const {
    return registry_.get<core::Guid>(e);
}

const std::string& World::name(entt::entity e) const {
    return registry_.get<Name>(e).value;
}

void World::set_name(entt::entity e, std::string name) {
    registry_.get<Name>(e).value = std::move(name);
}

entt::entity World::parent(entt::entity e) const {
    return hierarchy(e).parent;
}

std::vector<entt::entity> World::children(entt::entity e) const {
    std::vector<entt::entity> out;
    out.reserve(child_count(e));
    for (entt::entity c = e == entt::null ? first_root_ : hierarchy(e).first_child; c != entt::null;
         c = hierarchy(c).next_sibling) {
        out.push_back(c);
    }
    return out;
}

std::size_t World::child_count(entt::entity e) const {
    return e == entt::null ? root_count_ : hierarchy(e).child_count;
}

std::size_t World::sibling_index(entt::entity e) const {
    std::size_t index = 0;
    for (entt::entity s = hierarchy(e).prev_sibling; s != entt::null; s = hierarchy(s).prev_sibling) {
        ++index;
    }
    return index;
}

bool World::is_ancestor_or_self(entt::entity ancestor, entt::entity e) const {
    for (entt::entity p = e; p != entt::null; p = hierarchy(p).parent) {
        if (p == ancestor) {
            return true;
        }
    }
    return false;
}

std::size_t World::depth(entt::entity e) const {
    std::size_t d = 0;
    for (entt::entity p = hierarchy(e).parent; p != entt::null; p = hierarchy(p).parent) {
        ++d;
    }
    return d;
}

void World::link(entt::entity e, entt::entity parent, std::size_t index) {
    Hierarchy& h = hierarchy(e);
    F3D_ASSERT(h.parent == entt::null && h.prev_sibling == entt::null && h.next_sibling == entt::null,
               "entity is already linked");
    h.parent = parent;
    entt::entity& first = parent == entt::null ? first_root_ : hierarchy(parent).first_child;
    entt::entity& last = parent == entt::null ? last_root_ : hierarchy(parent).last_child;
    std::uint32_t& count = parent == entt::null ? root_count_ : hierarchy(parent).child_count;

    // Find the sibling to insert before (null: append).
    entt::entity next = entt::null;
    if (index < count) {
        next = first;
        for (std::size_t i = 0; i < index; ++i) {
            next = hierarchy(next).next_sibling;
        }
    }
    const entt::entity prev = next == entt::null ? last : hierarchy(next).prev_sibling;
    h.prev_sibling = prev;
    h.next_sibling = next;
    (prev == entt::null ? first : hierarchy(prev).next_sibling) = e;
    (next == entt::null ? last : hierarchy(next).prev_sibling) = e;
    ++count;
}

void World::unlink(entt::entity e) {
    Hierarchy& h = hierarchy(e);
    entt::entity& first = h.parent == entt::null ? first_root_ : hierarchy(h.parent).first_child;
    entt::entity& last = h.parent == entt::null ? last_root_ : hierarchy(h.parent).last_child;
    std::uint32_t& count = h.parent == entt::null ? root_count_ : hierarchy(h.parent).child_count;
    (h.prev_sibling == entt::null ? first : hierarchy(h.prev_sibling).next_sibling) = h.next_sibling;
    (h.next_sibling == entt::null ? last : hierarchy(h.next_sibling).prev_sibling) = h.prev_sibling;
    --count;
    h.parent = h.prev_sibling = h.next_sibling = entt::null;
}

bool World::set_parent(entt::entity e, entt::entity parent, std::size_t index, bool keep_world) {
    if (!valid(e) || (parent != entt::null && (!valid(parent) || is_ancestor_or_self(e, parent)))) {
        return false;
    }
    const glm::mat4 old_world = keep_world ? world_matrix(e) : glm::mat4(1.0f);
    unlink(e);
    link(e, parent, index);
    if (keep_world) {
        const glm::mat4 parent_world = parent == entt::null ? glm::mat4(1.0f) : world_matrix(parent);
        Transform& t = registry_.get<Transform>(e);
        decompose(glm::inverse(parent_world) * old_world, t.translation, t.rotation, t.scale);
    }
    mark_dirty(e);
    return true;
}

std::vector<entt::entity> World::subtree(entt::entity e) const {
    std::vector<entt::entity> out;
    std::vector<entt::entity> stack{e};
    while (!stack.empty()) {
        const entt::entity n = stack.back();
        stack.pop_back();
        out.push_back(n);
        // Push children in reverse so the first child is visited first.
        for (entt::entity c = hierarchy(n).last_child; c != entt::null; c = hierarchy(c).prev_sibling) {
            stack.push_back(c);
        }
    }
    return out;
}

std::vector<entt::entity> World::depth_first() const {
    std::vector<entt::entity> out;
    out.reserve(size());
    for (entt::entity r = first_root_; r != entt::null; r = hierarchy(r).next_sibling) {
        const auto sub = subtree(r);
        out.insert(out.end(), sub.begin(), sub.end());
    }
    return out;
}

const Transform& World::transform(entt::entity e) const {
    return registry_.get<Transform>(e);
}

void World::set_local(entt::entity e, const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale) {
    Transform& t = registry_.get<Transform>(e);
    t.translation = translation;
    t.rotation = rotation;
    t.scale = scale;
    mark_dirty(e);
}

void World::set_world_matrix(entt::entity e, const glm::mat4& world) {
    const entt::entity p = parent(e);
    const glm::mat4 parent_world = p == entt::null ? glm::mat4(1.0f) : world_matrix(p);
    Transform& t = registry_.get<Transform>(e);
    decompose(glm::inverse(parent_world) * world, t.translation, t.rotation, t.scale);
    mark_dirty(e);
}

void World::mark_dirty(entt::entity e) {
    registry_.get<Transform>(e).dirty = true;
    transforms_dirty_ = true;
}

void World::update_transforms() {
    if (!transforms_dirty_) {
        return;
    }
    F3D_ZONE;
    // Iterative pre-order walk: an entity is recomputed if it or any ancestor changed.
    struct Item {
        entt::entity e;
        bool parent_changed;
    };
    std::vector<Item> stack;
    for (entt::entity r = last_root_; r != entt::null; r = hierarchy(r).prev_sibling) {
        stack.push_back({r, false});
    }
    while (!stack.empty()) {
        const Item item = stack.back();
        stack.pop_back();
        Transform& t = registry_.get<Transform>(item.e);
        const bool changed = item.parent_changed || t.dirty;
        if (changed) {
            const entt::entity p = hierarchy(item.e).parent;
            t.world = p == entt::null ? t.local_matrix() : registry_.get<Transform>(p).world * t.local_matrix();
            t.dirty = false;
        }
        for (entt::entity c = hierarchy(item.e).last_child; c != entt::null; c = hierarchy(c).prev_sibling) {
            stack.push_back({c, changed});
        }
    }
    transforms_dirty_ = false;
}

const glm::mat4& World::world_matrix(entt::entity e) {
    update_transforms();
    return registry_.get<Transform>(e).world;
}

} // namespace f3d::scene
