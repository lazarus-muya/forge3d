// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/scene/world.hpp"

#include <random>

using f3d::core::Guid;
using f3d::scene::Transform;
using f3d::scene::World;

namespace {

bool near(const glm::mat4& a, const glm::mat4& b, float eps = 1e-4f) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (std::abs(a[c][r] - b[c][r]) > eps * std::max(1.0f, std::abs(b[c][r]))) {
                return false;
            }
        }
    }
    return true;
}

std::vector<std::string> names(const World& w, const std::vector<entt::entity>& es) {
    std::vector<std::string> out;
    for (const auto e : es) {
        out.push_back(w.name(e));
    }
    return out;
}

} // namespace

TEST_CASE("world: every entity has the core components") {
    World w;
    const auto e = w.create("A");
    REQUIRE(w.valid(e));
    CHECK(w.registry().all_of<f3d::scene::Name, Guid, Transform, f3d::scene::Hierarchy>(e));
    CHECK(w.name(e) == "A");
    CHECK_FALSE(w.guid(e).is_null());
    CHECK(w.size() == 1);
}

TEST_CASE("world: GUID lookup follows create and destroy") {
    World w;
    const Guid g{1, 2};
    const auto a = w.create("A", entt::null, g);
    REQUIRE((a != entt::null));
    CHECK(w.find(g) == a);
    CHECK((w.create("dup", entt::null, g) == entt::null)); // GUIDs are unique

    const auto child = w.create("child", a);
    const auto grandchild = w.create("grandchild", child);
    const Guid gc = w.guid(grandchild);
    CHECK(w.find(gc) == grandchild);

    w.destroy(a); // destroys the subtree
    CHECK((w.find(g) == entt::null));
    CHECK((w.find(gc) == entt::null));
    CHECK(w.size() == 0);
    CHECK(w.roots().empty());

    // The GUID can be reused after its entity is gone (undo of a delete recreates it).
    const auto again = w.create("A again", entt::null, g);
    CHECK(w.find(g) == again);

    // Destroying through the registry directly keeps the map in sync too.
    w.registry().destroy(again);
    CHECK((w.find(g) == entt::null));
}

TEST_CASE("world: ordered children and roots") {
    World w;
    const auto p = w.create("P");
    const auto b = w.create("B", p);
    const auto d = w.create("D", p);
    w.create("A", p, {}, 0);
    w.create("C", p, {}, 2);
    CHECK(names(w, w.children(p)) == std::vector<std::string>{"A", "B", "C", "D"});
    CHECK(w.child_count(p) == 4);
    CHECK(w.sibling_index(d) == 3);

    // Reorder within the same parent: the index is taken after removal.
    CHECK(w.set_parent(d, p, 0));
    CHECK(names(w, w.children(p)) == std::vector<std::string>{"D", "A", "B", "C"});
    CHECK(w.set_parent(d, p, World::kAppend));
    CHECK(names(w, w.children(p)) == std::vector<std::string>{"A", "B", "C", "D"});

    // Move to the root chain at a position.
    w.create("R2");
    CHECK(w.set_parent(b, entt::null, 1));
    CHECK(names(w, w.roots()) == std::vector<std::string>{"P", "B", "R2"});
    CHECK((w.parent(b) == entt::null));
    CHECK(names(w, w.depth_first()) == std::vector<std::string>{"P", "A", "C", "D", "B", "R2"});
}

TEST_CASE("world: reparenting rejects cycles") {
    World w;
    const auto a = w.create("A");
    const auto b = w.create("B", a);
    const auto c = w.create("C", b);
    CHECK_FALSE(w.set_parent(a, c));
    CHECK_FALSE(w.set_parent(a, a));
    CHECK((w.parent(a) == entt::null));
    CHECK(w.parent(c) == b);
    CHECK(w.depth(c) == 2);
    CHECK(w.is_ancestor_or_self(a, c));
    CHECK_FALSE(w.is_ancestor_or_self(c, a));
}

TEST_CASE("world: deep hierarchy propagation") {
    World w;
    constexpr int kDepth = 2000; // deep enough that a recursive walk would be risky
    std::vector<entt::entity> chain;
    entt::entity parent = entt::null;
    for (int i = 0; i < kDepth; ++i) {
        parent = w.create("n" + std::to_string(i), parent);
        w.set_local(parent, glm::vec3(1.0f, 0.0f, 0.0f), glm::quat(1, 0, 0, 0), glm::vec3(1.0f));
        chain.push_back(parent);
    }
    const auto leaf = chain.back();
    CHECK(w.world_matrix(leaf)[3].x == doctest::Approx(kDepth));
    CHECK_FALSE(w.transforms_dirty());

    // Changing the root moves every descendant; untouched entities keep their cache.
    w.set_local(chain.front(), glm::vec3(0.0f, 5.0f, 0.0f), glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)),
                glm::vec3(2.0f));
    const glm::vec3 leaf_pos = glm::vec3(w.world_matrix(leaf)[3]);
    // Root at (0,5,0) rotated 90° about Y and scaled 2: the other kDepth-1 unit steps
    // along local +X become 2 * (kDepth-1) along world -Z.
    CHECK(leaf_pos.x == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(leaf_pos.y == doctest::Approx(5.0f));
    CHECK(leaf_pos.z == doctest::Approx(-2.0f * (kDepth - 1)).epsilon(1e-4));

    // A change in the middle only affects that subtree.
    const auto mid = chain[kDepth / 2];
    const glm::mat4 before_mid_parent = w.world_matrix(chain[kDepth / 2 - 1]);
    w.set_local(mid, glm::vec3(0.0f), glm::quat(1, 0, 0, 0), glm::vec3(1.0f));
    CHECK(near(w.world_matrix(chain[kDepth / 2 - 1]), before_mid_parent));
    CHECK(near(w.world_matrix(mid), before_mid_parent));
}

TEST_CASE("world: reparenting keeps the world transform") {
    World w;
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> pos(-10.0f, 10.0f);
    std::uniform_real_distribution<float> angle(-3.0f, 3.0f);
    std::uniform_real_distribution<float> scl(0.5f, 2.0f);
    const auto random_local = [&](entt::entity e) {
        const glm::vec3 axis = glm::normalize(glm::vec3(pos(rng), pos(rng), pos(rng)) + glm::vec3(0.01f));
        const float s = scl(rng); // uniform scale: reparenting under it never needs shear
        w.set_local(e, {pos(rng), pos(rng), pos(rng)}, glm::angleAxis(angle(rng), axis), glm::vec3(s));
    };
    std::vector<entt::entity> es;
    for (int i = 0; i < 40; ++i) {
        const entt::entity parent = es.empty() || i % 5 == 0 ? entt::null : es[rng() % es.size()];
        es.push_back(w.create("e" + std::to_string(i), parent));
        random_local(es.back());
    }
    for (int i = 0; i < 200; ++i) {
        const auto e = es[rng() % es.size()];
        const auto target = rng() % 4 == 0 ? entt::null : es[rng() % es.size()];
        const glm::mat4 before = w.world_matrix(e);
        const bool moved = w.set_parent(e, target);
        CHECK(moved == (target == entt::null || !w.is_ancestor_or_self(e, target)));
        CHECK(near(w.world_matrix(e), before, 2e-3f));
    }

    // Without keep_world the local transform is kept instead.
    const auto a = w.create("a");
    const auto b = w.create("b");
    w.set_local(a, glm::vec3(10, 0, 0), glm::quat(1, 0, 0, 0), glm::vec3(1));
    w.set_local(b, glm::vec3(0, 1, 0), glm::quat(1, 0, 0, 0), glm::vec3(1));
    w.set_parent(b, a, World::kAppend, false);
    CHECK(glm::vec3(w.world_matrix(b)[3]) == glm::vec3(10, 1, 0));
}

TEST_CASE("world: set_world_matrix and decompose") {
    World w;
    const auto p = w.create("p");
    const auto c = w.create("c", p);
    w.set_local(p, glm::vec3(3, 0, 0), glm::angleAxis(0.7f, glm::vec3(0, 0, 1)), glm::vec3(2));
    glm::mat4 target = glm::translate(glm::mat4(1.0f), glm::vec3(1, 2, 3)) *
                       glm::mat4_cast(glm::angleAxis(-0.4f, glm::normalize(glm::vec3(1, 1, 0))));
    w.set_world_matrix(c, target);
    CHECK(near(w.world_matrix(c), target));

    // Mirrors come back as a negative X scale.
    glm::vec3 t, s;
    glm::quat r;
    f3d::scene::decompose(glm::scale(glm::mat4(1.0f), glm::vec3(1, -1, 1)), t, r, s);
    CHECK(s.x * s.y * s.z < 0.0f);
}
