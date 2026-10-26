// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/core/guid.hpp"
#include "f3d/core/reflect.hpp"
#include "f3d/core/reflect_json.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/reflect.hpp"

#include <entt/meta/meta.hpp>
#include <nlohmann/json.hpp>

using namespace f3d;

namespace {

/// Serialises, deserialises and re-serialises; the two JSON documents must match.
template <class T>
nlohmann::json round_trip(const T& value) {
    scene::register_types();
    const nlohmann::json first = core::reflect::to_json(value);
    const T restored = core::reflect::from_json<T>(first);
    CHECK(core::reflect::to_json(restored) == first);
    return first;
}

} // namespace

TEST_CASE("reflect: every scene component round-trips through JSON unchanged") {
    CHECK(round_trip(scene::Name{"Ground"})["value"] == "Ground");

    scene::Transform transform;
    transform.translation = {1.0f, -2.5f, 3.25f};
    transform.rotation = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f)));
    transform.scale = {0.5f, 2.0f, 4.0f};
    const auto tj = round_trip(transform);
    CHECK(tj["translation"] == nlohmann::json::array({1.0f, -2.5f, 3.25f}));
    CHECK(tj["scale"] == nlohmann::json::array({0.5f, 2.0f, 4.0f}));
    CHECK(tj["rotation"].size() == 4);

    scene::MeshRenderer renderer;
    renderer.mesh.path = "models/rock.glb#mesh/2";
    renderer.mesh.guid = core::Guid{0x1122334455667788ull, 0x99aabbccddeeff00ull};
    renderer.visible = false;
    const auto rj = round_trip(renderer);
    CHECK(rj["mesh"]["path"] == "models/rock.glb#mesh/2");
    CHECK(rj["mesh"]["guid"] == "11223344-5566-7788-99aa-bbccddeeff00");
    CHECK(rj["visible"] == false);

    scene::Light light;
    light.type = scene::LightType::Spot;
    light.color = {0.9f, 0.8f, 0.7f};
    light.intensity = 1234.5f;
    light.range = 42.0f;
    light.inner_cone = 0.3f;
    light.outer_cone = 0.6f;
    light.cast_shadows = false;
    const auto lj = round_trip(light);
    CHECK(lj["type"] == static_cast<int>(scene::LightType::Spot));
    CHECK(lj["cast_shadows"] == false);

    scene::Camera camera;
    camera.projection = scene::CameraProjection::Orthographic;
    camera.fov_y = 1.1f;
    camera.ortho_height = 7.5f;
    camera.near_plane = 0.1f;
    camera.far_plane = 1234.0f;
    camera.aspect_ratio = 2.35f;
    const auto cj = round_trip(camera);
    CHECK(cj["projection"] == static_cast<int>(scene::CameraProjection::Orthographic));
    CHECK(cj["ortho_height"] == 7.5f);
    CHECK(cj["aspect_ratio"] == 2.35f);

    scene::Sky sky;
    sky.time_of_day = 17.25f;
    sky.year = 2031;
    sky.month = 2;
    sky.day = 9;
    sky.latitude = -33.5f;
    sky.longitude = 151.2f;
    sky.utc_offset = 10.0f;
    sky.north_offset = 15.0f;
    sky.haze = 2.5f;
    sky.ground_albedo = {0.1f, 0.2f, 0.3f};
    sky.moon = false;
    sky.shadows = false;
    sky.exposure_compensation = -1.5f;
    const auto sj = round_trip(sky);
    CHECK(sj["time_of_day"] == 17.25f);
    CHECK(sj["month"] == 2);
    CHECK(sj["moon"] == false);

    scene::ReflectionProbe probe;
    probe.radius = 25.0f;
    probe.resolution = 256;
    const auto pj = round_trip(probe);
    CHECK(pj["radius"] == 25.0f);
    CHECK(pj["resolution"] == 256);
}

TEST_CASE("reflect: enums round-trip by value") {
    scene::register_types();
    CHECK(core::reflect::from_json<scene::LightType>(nlohmann::json(2)) == scene::LightType::Spot);
    CHECK(core::reflect::from_json<scene::CameraProjection>(nlohmann::json(1)) ==
          scene::CameraProjection::Orthographic);
    CHECK(core::reflect::to_json(scene::LightType::Point) == nlohmann::json(1));
}

TEST_CASE("reflect: field metadata is attached") {
    scene::register_types();
    const entt::meta_type light = entt::resolve<scene::Light>();
    const entt::meta_data range = light.data(entt::hashed_string{"range"}.value());
    REQUIRE(static_cast<bool>(range));
    const core::reflect::FieldMeta* meta = core::reflect::field_meta(range);
    REQUIRE(meta != nullptr);
    CHECK(meta->unit == "m");
    CHECK(meta->has_range);

    const entt::meta_data intensity = light.data(entt::hashed_string{"intensity"}.value());
    REQUIRE(static_cast<bool>(intensity));
    CHECK(core::reflect::field_meta(intensity) != nullptr);
}

TEST_CASE("reflect: unsupported types are rejected") {
    scene::register_types();
    int value = 0;
    nlohmann::json out;
    CHECK_THROWS_AS(core::reflect::value_to_json(out, entt::forward_as_meta(&value)), std::runtime_error);
}
