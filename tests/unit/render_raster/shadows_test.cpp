// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/render/raster/shadows.hpp"

#include <cmath>

using namespace f3d;
using namespace f3d::render::raster;

namespace {

render::View make_view(glm::vec3 eye, glm::vec3 target) {
    render::View view;
    view.camera_position = eye;
    view.view = glm::lookAtRH(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
    view.width = 1600;
    view.height = 900;
    view.near_plane = 0.1f;
    view.far_plane = 1000.0f;
    view.projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 1600.0f / 900.0f, view.near_plane, view.far_plane);
    return view;
}

bool same(const glm::mat4& a, const glm::mat4& b) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (a[c][r] != b[c][r]) {
                return false;
            }
        }
    }
    return true;
}

const glm::vec3 kSun = glm::normalize(glm::vec3(0.4f, 0.7f, 0.3f));

} // namespace

TEST_CASE("shadows: cascade splits run from near to far and blend linear and logarithmic") {
    const auto log = cascade_splits(0.1f, 100.0f, 1.0f);
    const auto lin = cascade_splits(0.1f, 100.0f, 0.0f);
    const auto mixed = cascade_splits(0.1f, 100.0f, 0.75f);
    CHECK(mixed[0] == doctest::Approx(0.1f));
    CHECK(mixed[kShadowCascades] == doctest::Approx(100.0f));
    for (std::uint32_t i = 1; i <= kShadowCascades; ++i) {
        CHECK(mixed[i] > mixed[i - 1]);
        CHECK(log[i] <= mixed[i] + 1e-4f);
        CHECK(mixed[i] <= lin[i] + 1e-4f);
    }
    CHECK(lin[2] == doctest::Approx(50.05f));
    CHECK(log[2] == doctest::Approx(std::sqrt(0.1f * 100.0f)));
}

TEST_CASE("shadows: every cascade's sphere holds its frustum slice") {
    const render::View view = make_view({3.0f, 2.0f, 8.0f}, {0.0f, 0.5f, 0.0f});
    ShadowSettings settings;
    const ShadowCascades cascades = compute_cascades(view, kSun, settings);
    const auto splits = cascade_splits(view.near_plane, settings.max_distance, settings.split_lambda);
    const glm::mat4 inverse = glm::inverse(view.view_projection());
    for (std::uint32_t i = 0; i < kShadowCascades; ++i) {
        CHECK(cascades.split_far[i] == doctest::Approx(splits[i + 1]));
        const glm::vec3 centre(cascades.spheres[i]);
        const float radius = cascades.spheres[i].w;
        // The slice's corners: the far-plane corners scaled to the slice depths.
        for (const float depth : {splits[i], splits[i + 1]}) {
            for (const float x : {-1.0f, 1.0f}) {
                for (const float y : {-1.0f, 1.0f}) {
                    const glm::vec4 far_point = inverse * glm::vec4(x, y, 1.0f, 1.0f);
                    const glm::vec3 dir = glm::vec3(far_point) / far_point.w - view.camera_position;
                    const glm::vec3 forward = -glm::vec3(glm::inverse(view.view)[2]);
                    const glm::vec3 corner = view.camera_position + dir * (depth / glm::dot(dir, forward));
                    // Snapping moves the centre by up to half a texel per axis.
                    CHECK(glm::distance(corner, centre) <= radius + cascades.texel_size[i] + 1e-3f);
                }
            }
        }
        // The sphere's centre projects to the middle of the cascade.
        const glm::vec4 clip = cascades.view_projection[i] * glm::vec4(centre, 1.0f);
        CHECK(std::abs(clip.x / clip.w) < 1e-3f);
        CHECK(std::abs(clip.y / clip.w) < 1e-3f);
        CHECK(cascades.texel_size[i] == doctest::Approx(2.0f * radius / settings.resolution));
    }
    CHECK(cascades.texel_size[0] < cascades.texel_size[kShadowCascades - 1]);
}

TEST_CASE("shadows: a sub-texel camera move doesn't shift the shadow matrix") {
    ShadowSettings settings;
    const glm::vec3 target(0.0f, 0.5f, 0.0f);
    const glm::vec3 eye(3.0f, 2.0f, 8.0f);
    const ShadowCascades base = compute_cascades(make_view(eye, target), kSun, settings);

    // Put the camera where cascade 0's centre sits exactly on a snapped position, then
    // move it by 0.3 texel along each light-space axis: every matrix stays the same.
    const glm::mat4 rotation = light_rotation(kSun);
    const glm::mat3 to_world = glm::transpose(glm::mat3(rotation));
    const float texel = base.texel_size[0];
    const auto centre_ls = [&](const ShadowCascades& c) {
        return glm::vec3(rotation * glm::vec4(glm::vec3(c.spheres[0]), 1.0f));
    };
    // The unsnapped centre moves with the camera; find the offset to the snapped one.
    const glm::vec3 forward = glm::normalize(target - eye);
    const auto splits = cascade_splits(0.1f, settings.max_distance, settings.split_lambda);
    const float tan_y = std::tan(glm::radians(30.0f));
    const float tan_x = tan_y * 1600.0f / 900.0f;
    const float k2 = tan_x * tan_x + tan_y * tan_y;
    const float depth = std::min(0.5f * (splits[0] + splits[1]) * (1.0f + k2), splits[1]);
    const glm::vec3 raw = glm::vec3(rotation * glm::vec4(eye + forward * depth, 1.0f));
    const glm::vec3 aligned_eye = eye + to_world * (centre_ls(base) - raw);
    const ShadowCascades aligned = compute_cascades(make_view(aligned_eye, aligned_eye + forward), kSun, settings);

    for (const glm::vec3 axis : {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)}) {
        for (const float amount : {0.3f, -0.3f}) {
            const glm::vec3 moved_eye = aligned_eye + to_world * (axis * texel * amount);
            const ShadowCascades moved = compute_cascades(make_view(moved_eye, moved_eye + forward), kSun, settings);
            CHECK(same(moved.view_projection[0], aligned.view_projection[0]));
        }
    }
    // A move of several texels shifts it by whole texels.
    const glm::vec3 far_eye = aligned_eye + to_world * glm::vec3(texel * 3.0f, 0.0f, 0.0f);
    const ShadowCascades shifted = compute_cascades(make_view(far_eye, far_eye + forward), kSun, settings);
    const float dx = (centre_ls(shifted).x - centre_ls(aligned).x) / texel;
    CHECK(dx == doctest::Approx(3.0f).epsilon(1e-3));
}

TEST_CASE("shadows: turning the camera keeps every cascade's size") {
    ShadowSettings settings;
    const glm::vec3 eye(0.0f, 2.0f, 0.0f);
    const ShadowCascades a = compute_cascades(make_view(eye, eye + glm::vec3(1, -0.2f, 0)), kSun, settings);
    const ShadowCascades b = compute_cascades(make_view(eye, eye + glm::vec3(-0.3f, 0.1f, -1)), kSun, settings);
    for (std::uint32_t i = 0; i < kShadowCascades; ++i) {
        CHECK(a.spheres[i].w == b.spheres[i].w);
        CHECK(a.texel_size[i] == b.texel_size[i]);
    }
}
