// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/core/debug_draw.hpp"

namespace debug = f3d::core::debug;

TEST_CASE("debug draw: shapes produce line-list vertices and are taken once") {
    (void)debug::take_lines(); // start clean
    debug::line(glm::vec3(0), glm::vec3(1, 0, 0), debug::kRed);
    CHECK(debug::pending_vertex_count() == 2);
    debug::box(glm::vec3(-1), glm::vec3(1), debug::kWhite); // 12 edges
    CHECK(debug::pending_vertex_count() == 2 + 24);
    debug::sphere(glm::vec3(0), 2.0f, debug::kBlue, 16); // 3 circles x 16 segments
    CHECK(debug::pending_vertex_count() == 2 + 24 + 96);

    const auto lines = debug::take_lines();
    CHECK(lines.size() == 122);
    CHECK(lines[0].color == debug::kRed);
    CHECK(lines[1].position.x == doctest::Approx(1.0f));
    for (std::size_t i = 26; i < lines.size(); ++i) {
        CHECK(glm::length(lines[i].position) == doctest::Approx(2.0f)); // on the sphere
    }
    CHECK(debug::pending_vertex_count() == 0);
}

TEST_CASE("debug draw: colour packing") {
    CHECK(debug::rgba8(1, 2, 3, 4) == 0x04030201u);
    CHECK(debug::rgba(1.0f, 0.0f, 0.0f) == debug::rgba8(255, 0, 0, 255));
    CHECK(debug::rgba(2.0f, -1.0f, 0.5f, 1.0f) == debug::rgba8(255, 0, 128, 255));
}
