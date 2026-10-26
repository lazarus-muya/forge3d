// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/core/reflect.hpp"
#include "f3d/io/forge_file.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/builtin_textures.hpp"
#include "f3d/terrain/layers.hpp"
#include "f3d/terrain/terrain.hpp"
#include "f3d/terrain/terrain_ops.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <random>

using namespace f3d;
using namespace f3d::terrain;

namespace {

struct Session {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    core::Guid terrain = *core::Guid::parse("00000000-0000-4000-8000-00000000d001");

    Session() {
        scene::register_types();
        ops::register_builtin_ops(registry);
        register_terrain_ops(registry);
        stack.set_limits(100000, 0);
        REQUIRE(stack.execute(registry.create(
            "terrain.create",
            {{"guid", terrain.to_string()}, {"tiles", 2}, {"size", 512.0f}, {"max_height", 200.0f}, {"seed", 3}})));
    }
    bool run(const char* id, const nlohmann::json& params) { return stack.execute(registry.create(id, params)); }
    [[nodiscard]] std::shared_ptr<Heightfield> heights() { return heightfield_of(world, world.find(terrain)); }
    [[nodiscard]] Terrain& component() { return world.registry().get<Terrain>(world.find(terrain)); }
    [[nodiscard]] std::vector<std::uint8_t> splat_bytes() {
        const Splatmap* splat = heights()->splat();
        std::vector<std::uint8_t> out;
        for (std::uint32_t m = 0; splat && m < splat->maps(); ++m) {
            out.insert(out.end(), splat->map(m), splat->map(m) + splat->map_bytes());
        }
        return out;
    }
};

std::string layer_names(const Terrain& t) {
    std::string out;
    for (const TerrainLayer& l : t.layers) {
        out += l.name + ";";
    }
    return out;
}

} // namespace

TEST_CASE("layers: texture slots and sets are recognised by file name") {
    CHECK(guess_texture_slot("Rock030_2K_Color.jpg") == LayerTextureSlot::Albedo);
    CHECK(guess_texture_slot("Rock030_2K_NormalGL.jpg") == LayerTextureSlot::Normal);
    CHECK(guess_texture_slot("Rock030_2K_Roughness.jpg") == LayerTextureSlot::Roughness);
    CHECK(guess_texture_slot("Rock030_2K_AmbientOcclusion.jpg") == LayerTextureSlot::Ao);
    CHECK(guess_texture_slot("Rock030_2K_Displacement.jpg") == LayerTextureSlot::Height);
    CHECK(guess_texture_slot("rock_face_diff_2k.jpg") == LayerTextureSlot::Albedo);
    CHECK(guess_texture_slot("grass_basecolor.png") == LayerTextureSlot::Albedo);
    CHECK_FALSE(guess_texture_slot("photo.png").has_value());

    // ambientCG style: the colour map finds its set, and prefers the OpenGL normal map.
    const std::vector<std::string> folder = {
        "C:/tex/Rock030_2K_Color.jpg",     "C:/tex/Rock030_2K_NormalDX.jpg", "C:/tex/Rock030_2K_NormalGL.jpg",
        "C:/tex/Rock030_2K_Roughness.jpg", "C:/tex/Rock030_2K_AmbientOcclusion.jpg",
        "C:/tex/Rock030_2K_Displacement.jpg", "C:/tex/Moss_2K_Color.jpg", "C:/other/Rock030_2K_Roughness.jpg"};
    auto set = guess_texture_set("C:/tex/Rock030_2K_Color.jpg", folder);
    CHECK(set[0] == "C:/tex/Rock030_2K_Color.jpg");
    CHECK(set[1] == "C:/tex/Rock030_2K_NormalGL.jpg");
    CHECK(set[2] == "C:/tex/Rock030_2K_Roughness.jpg");
    CHECK(set[3] == "C:/tex/Rock030_2K_AmbientOcclusion.jpg");
    CHECK(set[4] == "C:/tex/Rock030_2K_Displacement.jpg");

    // Poly Haven style ("nor_gl", the resolution after the slot word).
    const std::vector<std::string> haven = {"/t/rock_face_diff_2k.jpg", "/t/rock_face_nor_gl_2k.jpg",
                                            "/t/rock_face_rough_2k.jpg", "/t/rock_face_disp_2k.png"};
    set = guess_texture_set("/t/rock_face_diff_2k.jpg", haven);
    CHECK(set[1] == "/t/rock_face_nor_gl_2k.jpg");
    CHECK(set[2] == "/t/rock_face_rough_2k.jpg");
    CHECK(set[3].empty());
    CHECK(set[4] == "/t/rock_face_disp_2k.png");

    // Poly Haven's packed ARM map fills roughness and AO when there are no separate maps.
    CHECK(is_packed_orm("/t/rock_face_arm_2k.jpg"));
    CHECK_FALSE(is_packed_orm("/t/rock_face_rough_2k.jpg"));
    CHECK_FALSE(guess_texture_slot("/t/rock_face_arm_2k.jpg").has_value());
    const std::vector<std::string> packed = {"/t/rock_face_diff_2k.jpg", "/t/rock_face_nor_gl_2k.jpg",
                                             "/t/rock_face_arm_2k.jpg", "/t/rock_face_ao_2k.jpg"};
    set = guess_texture_set("/t/rock_face_diff_2k.jpg", packed);
    CHECK(set[2] == "/t/rock_face_arm_2k.jpg");
    CHECK(set[3] == "/t/rock_face_ao_2k.jpg"); // the separate map wins

    CHECK(texture_set_name("/t/aerial_rocks_02_diff_2k.jpg") == "Aerial rocks 02");
    CHECK(texture_set_name("C:/tex/Rock030_2K_Color.jpg") == "Rock030");
    CHECK(texture_set_name("grass_basecolor_4096.png") == "Grass");
    CHECK(texture_set_name("diff_2k.png") == "Layer");
}

TEST_CASE("layers: built-in surfaces are generated, cached and tile") {
    REQUIRE(builtin_surfaces().size() >= 10);
    const TerrainLayer grass = builtin_layer("grass");
    CHECK(grass.name == "Grass");
    CHECK(grass.albedo_texture.path == builtin_texture_path("grass", LayerTextureSlot::Albedo));
    const auto albedo = builtin_texture(grass.albedo_texture.path);
    REQUIRE(albedo.has_value());
    CHECK(albedo->width == kBuiltinTextureSize);
    CHECK(albedo->rgba.size() == static_cast<std::size_t>(kBuiltinTextureSize) * kBuiltinTextureSize * 4);
    // Cached and deterministic.
    CHECK(builtin_texture(grass.albedo_texture.path)->rgba == albedo->rgba);
    CHECK_FALSE(builtin_texture("builtin:terrain/grass/ao").has_value()); // no AO maps
    CHECK_FALSE(builtin_texture("builtin:terrain/lava/albedo").has_value());
    CHECK_FALSE(builtin_texture("C:/grass.png").has_value());

    // Tileable: the step across the wrap-around edge is like any other step.
    const auto height = builtin_texture(builtin_texture_path("rock", LayerTextureSlot::Height));
    REQUIRE(height.has_value());
    const std::uint32_t n = height->width;
    double inner = 0.0, seam = 0.0;
    for (std::uint32_t y = 0; y < n; ++y) {
        const auto at = [&](std::uint32_t x) { return static_cast<double>(height->rgba[(y * n + x) * 4]); };
        inner += std::abs(at(n / 2) - at(n / 2 + 1));
        seam += std::abs(at(n - 1) - at(0));
    }
    CHECK(seam < inner * 2.0 + n);
}

TEST_CASE("layers: resampling keeps the size and wraps") {
    core::Image image;
    image.width = image.height = 64;
    image.rgba.assign(64 * 64 * 4, 0);
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        image.rgba[i] = 200;
        image.rgba[i + 3] = 255;
    }
    const auto down = resample_tileable(image, 16);
    CHECK(down.size() == 16u * 16u * 4u);
    CHECK(down[0] == 200);
    const auto up = resample_tileable(image, 128);
    CHECK(up.size() == 128u * 128u * 4u);
    CHECK(up[(77 * 128 + 5) * 4] == 200);
    CHECK(resample_tileable(image, 64) == image.rgba);
}

TEST_CASE("layers: presets give textured layers with rules") {
    for (const LandscapePreset& preset : landscape_presets()) {
        const auto layers = landscape_preset_layers(preset.id, 0.0f, 300.0f);
        REQUIRE(layers.size() >= 3);
        CHECK_FALSE(layers[0].rule.enabled); // the base
        CHECK(layers[1].rule.enabled);
        for (const TerrainLayer& l : layers) {
            CHECK_FALSE(l.albedo_texture.empty());
        }
    }
    CHECK(landscape_preset_layers("moon", 0.0f, 1.0f).empty());
    const auto alpine = landscape_preset_layers("alpine", 100.0f, 1100.0f);
    CHECK(alpine.back().name == "Snow");
    CHECK(alpine.back().rule.height_min == doctest::Approx(820.0f));
}

TEST_CASE("layers: 50 random paint strokes undo and redo exactly") {
    Session s;
    const std::vector<std::uint8_t> original = s.splat_bytes();
    REQUIRE_FALSE(original.empty()); // a new terrain has baked rules
    std::mt19937 rng(17);
    std::uniform_real_distribution<float> pos(-300.0f, 300.0f);
    for (int i = 0; i < 50; ++i) {
        nlohmann::json points = nlohmann::json::array();
        for (int p = 0; p < 4; ++p) {
            points.push_back({pos(rng), pos(rng), 0.5f + 0.5f * static_cast<float>(p % 2)});
        }
        REQUIRE(s.run("terrain.paint_stroke", {{"terrain", s.terrain.to_string()},
                                               {"layer", static_cast<int>(rng() % 3)},
                                               {"erase", rng() % 4 == 0},
                                               {"radius", 5.0f + static_cast<float>(rng() % 40)},
                                               {"strength", 0.7f},
                                               {"points", points}}));
    }
    const std::vector<std::uint8_t> painted = s.splat_bytes();
    CHECK(painted != original);
    for (int i = 0; i < 50; ++i) {
        REQUIRE(s.stack.undo());
    }
    CHECK(s.splat_bytes() == original);
    for (int i = 0; i < 50; ++i) {
        REQUIRE(s.stack.redo());
    }
    CHECK(s.splat_bytes() == painted);
}

TEST_CASE("layers: set_layers remaps the painted weights and undoes") {
    Session s;
    const std::string before_names = layer_names(s.component());
    const std::vector<std::uint8_t> before = s.splat_bytes();
    // Swap rock (1) and snow (2): each texel's weights swap with them.
    std::vector<TerrainLayer> next = s.component().layers;
    std::swap(next[1], next[2]);
    nlohmann::json value;
    core::reflect::value_to_json(value, entt::forward_as_meta(next));
    REQUIRE(s.run("terrain.set_layers",
                  {{"terrain", s.terrain.to_string()}, {"layers", value}, {"source", {0, 2, 1}}}));
    CHECK(layer_names(s.component()) == "Grass;Snow;Rock;");
    const Splatmap& splat = *s.heights()->splat();
    std::size_t checked = 0;
    for (std::uint32_t z = 0; z < splat.resolution(); z += 7) {
        for (std::uint32_t x = 0; x < splat.resolution(); x += 7) {
            const std::size_t i = (static_cast<std::size_t>(z) * splat.resolution() + x) * 4;
            CHECK(splat.weight(x, z, 1) == before[i + 2]);
            CHECK(splat.weight(x, z, 2) == before[i + 1]);
            ++checked;
        }
    }
    CHECK(checked > 100);
    // Removing a layer gives its weight to the rest; every texel still sums to 255.
    std::vector<TerrainLayer> fewer = {next[0], next[2]};
    core::reflect::value_to_json(value, entt::forward_as_meta(fewer));
    REQUIRE(s.run("terrain.set_layers", {{"terrain", s.terrain.to_string()}, {"layers", value}, {"source", {0, 2}}}));
    CHECK(s.component().layers.size() == 2);
    for (std::uint32_t z = 0; z < splat.resolution(); z += 13) {
        for (std::uint32_t x = 0; x < splat.resolution(); x += 13) {
            int sum = 0;
            for (std::uint32_t l = 0; l < 4; ++l) {
                sum += s.heights()->splat()->weight(x, z, l);
            }
            CHECK(sum == 255);
        }
    }
    REQUIRE(s.stack.undo());
    REQUIRE(s.stack.undo());
    CHECK(layer_names(s.component()) == before_names);
    CHECK(s.splat_bytes() == before);
    // Bad input is refused.
    CHECK_FALSE(s.run("terrain.set_layers", {{"terrain", s.terrain.to_string()}, {"layers", nlohmann::json::array()}}));
}

TEST_CASE("layers: auto material replaces the layers and bakes them; undo restores") {
    Session s;
    const std::string before_names = layer_names(s.component());
    const std::vector<std::uint8_t> before = s.splat_bytes();
    REQUIRE(s.run("terrain.auto_material", {{"terrain", s.terrain.to_string()}, {"preset", "alpine"}}));
    CHECK(layer_names(s.component()) == "Meadow;Rock;Cliff;Snow;");
    const std::vector<std::uint8_t> alpine = s.splat_bytes();
    CHECK(alpine != before);
    REQUIRE(s.stack.undo());
    CHECK(layer_names(s.component()) == before_names);
    CHECK(s.splat_bytes() == before);
    REQUIRE(s.stack.redo());
    CHECK(s.splat_bytes() == alpine);
    CHECK_FALSE(s.run("terrain.auto_material", {{"terrain", s.terrain.to_string()}, {"preset", "moon"}}));
}

TEST_CASE("layers: terrain heights and weights are saved in .forge chunks") {
    Session s;
    REQUIRE(s.run("terrain.paint_stroke", {{"terrain", s.terrain.to_string()},
                                           {"layer", 2},
                                           {"radius", 30.0f},
                                           {"strength", 1.0f},
                                           {"points", {{10.0f, 20.0f, 1.0f}}}}));
    const std::string hash = height_hash(*s.heights());
    const std::vector<std::uint8_t> weights = s.splat_bytes();
    const auto path = std::filesystem::temp_directory_path() / "f3d_layers_test.forge";
    REQUIRE(io::save_forge(path, s.world).ok);

    scene::World loaded;
    REQUIRE(io::load_forge(path, loaded).ok);
    const entt::entity e = loaded.find(s.terrain);
    REQUIRE(loaded.valid(e));
    const auto hf = heightfield_of(loaded, e);
    REQUIRE(hf != nullptr);
    CHECK(height_hash(*hf) == hash);
    REQUIRE(hf->splat() != nullptr);
    std::vector<std::uint8_t> loaded_weights;
    for (std::uint32_t m = 0; m < hf->splat()->maps(); ++m) {
        loaded_weights.insert(loaded_weights.end(), hf->splat()->map(m), hf->splat()->map(m) + hf->splat()->map_bytes());
    }
    CHECK(loaded_weights == weights);
    CHECK(loaded.registry().get<Terrain>(e).layers.size() == 3);
    std::filesystem::remove(path);
}
