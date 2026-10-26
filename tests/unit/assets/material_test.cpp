// SPDX-License-Identifier: Apache-2.0
// Materials (phases.md P4.2): the material asset, importers mapping glTF / OBJ
// materials onto it, submeshes through cooking, material resolution and ops.
#include <doctest/doctest.h>

#include "f3d/assets/asset_database.hpp"
#include "f3d/assets/cook.hpp"
#include "f3d/assets/material.hpp"
#include "f3d/assets/material_ops.hpp"
#include "f3d/assets/mesh_library.hpp"
#include "f3d/assets/pipeline.hpp"
#include "f3d/core/vfs.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json.hpp>

#include <cstring>
#include <filesystem>

using namespace f3d;
namespace fs = std::filesystem;

namespace {

fs::path data_dir() {
    return fs::path(F3D_TEST_DATA_DIR);
}

/// A mesh of two quads, the first with material 3, the second with material 1.
assets::MeshData two_material_mesh(std::uint32_t grid = 24) {
    assets::MeshData mesh;
    mesh.name = "two";
    // A grid (so it simplifies) split down the middle by material.
    for (std::uint32_t y = 0; y <= grid; ++y) {
        for (std::uint32_t x = 0; x <= grid; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(grid);
            const float fy = static_cast<float>(y) / static_cast<float>(grid);
            mesh.vertices.push_back({glm::vec3(fx, 0.01f * std::sin(fx * 9.0f) * std::cos(fy * 7.0f), fy),
                                     glm::vec3(0, 1, 0), 0xFFFFFFFFu});
        }
    }
    std::vector<std::uint32_t> materials;
    for (std::uint32_t y = 0; y < grid; ++y) {
        for (std::uint32_t x = 0; x < grid; ++x) {
            const std::uint32_t a = y * (grid + 1) + x;
            mesh.indices.insert(mesh.indices.end(), {a, a + grid + 1, a + 1, a + 1, a + grid + 1, a + grid + 2});
            const std::uint32_t m = x < grid / 2 ? 3u : 1u;
            materials.insert(materials.end(), {m, m});
        }
    }
    mesh.submeshes = assets::group_by_material(mesh, materials);
    for (const auto& v : mesh.vertices) {
        mesh.bounds.add(v.position);
    }
    return mesh;
}

} // namespace

TEST_CASE("materials: JSON and .f3dmat round trip; missing fields default") {
    assets::Material m;
    m.name = "Rusty";
    m.base_color = {0.5f, 0.25f, 0.1f, 1.0f};
    m.metallic = 0.8f;
    m.roughness = 0.35f;
    m.emissive = {1.0f, 0.5f, 0.0f};
    m.emissive_strength = 3.0f;
    m.alpha_mode = assets::AlphaMode::Mask;
    m.double_sided = true;
    m.base_color_texture = {core::Guid::generate(), "project://textures/rust.png"};
    m.orm_texture = {core::Guid{}, "#image/2"};
    m.orm_occlusion = true;
    const std::string text = assets::write_material(m);
    const auto back = assets::read_material(text);
    REQUIRE(back.has_value());
    CHECK(*back == m);

    const auto partial =
        assets::read_material(R"({"format":"forge3d.material","version":1,"material":{"roughness":0.9}})");
    REQUIRE(partial.has_value());
    CHECK(partial->roughness == doctest::Approx(0.9f));
    CHECK(partial->metallic == 0.0f);
    CHECK(partial->base_color == glm::vec4(1.0f));

    std::string error;
    CHECK_FALSE(assets::read_material("{}", &error).has_value());
    CHECK_FALSE(error.empty());
    CHECK_FALSE(assets::read_material(R"({"format":"forge3d.material","version":7})").has_value());
}

TEST_CASE("materials: sub-asset references rebase onto their model") {
    const core::Guid model = core::Guid::generate();
    const core::AssetRef image{core::Guid{}, "#image/1"};
    const core::AssetRef rebased = assets::rebase_sub_asset(image, {model, "project://m/a.glb#mesh/0"});
    CHECK(rebased.guid == model);
    CHECK(rebased.path == "project://m/a.glb#image/1");
    // Whole references are left alone.
    const core::AssetRef texture{core::Guid::generate(), "project://t.png"};
    CHECK(assets::rebase_sub_asset(texture, {model, "x.glb"}) == texture);
    CHECK(assets::asset_file(rebased) == "project://m/a.glb");
    CHECK(assets::sub_asset(rebased) == "image/1");
}

TEST_CASE("materials: glTF materials, images and submeshes are imported") {
    const auto box = assets::load_gltf_scene(data_dir() / "gltf" / "Box.glb");
    REQUIRE(box.scene.has_value());
    REQUIRE(box.scene->materials.size() == 1);
    const assets::Material& red = box.scene->materials[0];
    CHECK(red.base_color.r == doctest::Approx(0.8f));
    CHECK(red.base_color.g == doctest::Approx(0.0f));
    CHECK(red.base_color_texture.empty());
    REQUIRE(box.scene->meshes[0].submeshes.size() == 1);
    CHECK(box.scene->meshes[0].submeshes[0].material == 0);
    CHECK(box.scene->meshes[0].submeshes[0].index_count == 36);
    // Material colours are no longer baked into the vertices.
    for (const auto& v : box.scene->meshes[0].vertices) {
        CHECK(v.color == 0xFFFFFFFFu);
    }

    const auto fox = assets::load_gltf_scene(data_dir() / "gltf" / "Fox.glb");
    REQUIRE(fox.scene.has_value());
    REQUIRE(fox.scene->materials.size() == 1);
    REQUIRE(fox.scene->images.size() == 1);
    CHECK(fox.scene->materials[0].base_color_texture.path == "#image/0");
    CHECK(fox.scene->images[0].kind == assets::TextureKind::Color);
    CHECK_FALSE(fox.scene->images[0].bytes.empty()); // embedded in the .glb
    CHECK(fox.scene->images[0].extension == ".png");
    std::string error;
    const auto decoded = assets::decode_model_image(fox.scene->images[0], data_dir() / "gltf" / "Fox.glb", &error);
    INFO(error);
    REQUIRE(decoded.has_value());
    CHECK(decoded->width() > 0);
}

TEST_CASE("materials: OBJ materials map onto the material asset") {
    const auto cube = assets::load_obj_scene(data_dir() / "import" / "models" / "cube.obj");
    REQUIRE(cube.scene.has_value());
    REQUIRE(cube.scene->materials.size() == 1);
    const assets::Material& brick = cube.scene->materials[0];
    CHECK(brick.name == "Brick");
    CHECK(brick.base_color.r == doctest::Approx(0.8f));
    CHECK(brick.base_color.g == doctest::Approx(0.25f));
    CHECK(brick.base_color_texture.path == "#image/0");
    REQUIRE(cube.scene->images.size() == 1);
    CHECK(cube.scene->images[0].uri == "../textures/checker.png");
    REQUIRE(cube.scene->meshes[0].submeshes.size() == 1);
    CHECK(cube.scene->meshes[0].submeshes[0].material == 0);
    CHECK(assets::decode_model_image(cube.scene->images[0], data_dir() / "import" / "models" / "cube.obj"));
}

TEST_CASE("submeshes: grouping by material keeps every triangle") {
    assets::MeshData mesh = two_material_mesh(4);
    REQUIRE(mesh.submeshes.size() == 2);
    CHECK(mesh.submeshes[0].material == 1); // sorted by material
    CHECK(mesh.submeshes[1].material == 3);
    CHECK(mesh.submeshes[0].first_index == 0);
    CHECK(mesh.submeshes[0].index_count + mesh.submeshes[1].index_count == mesh.indices.size());
    CHECK(mesh.submeshes[1].first_index == mesh.submeshes[0].index_count);
    // Material 1 covers the right half (x >= 0.5).
    for (std::uint32_t i = 0; i < mesh.submeshes[0].index_count; ++i) {
        CHECK(mesh.vertices[mesh.indices[i]].position.x >= 0.5f - 1e-5f);
    }
    // No material at all: no submeshes (one implicit range).
    assets::MeshData plain = two_material_mesh(2);
    std::vector<std::uint32_t> none(plain.indices.size() / 3, assets::SubMesh::kNoMaterial);
    plain.submeshes = assets::group_by_material(plain, none);
    CHECK(plain.submeshes.empty());
    CHECK(assets::submeshes_of(plain).size() == 1);
}

TEST_CASE("submeshes: cooking keeps materials in every LOD and .f3dmesh v2 round-trips") {
    const assets::MeshData mesh = two_material_mesh(32);
    assets::MeshCookSettings settings;
    settings.lod_error = 0.2f;
    const assets::CookedMesh cooked = assets::cook_mesh(mesh, settings);
    REQUIRE(cooked.lods.size() >= 2);
    for (std::size_t lod = 0; lod < cooked.lods.size(); ++lod) {
        const auto& level = cooked.lods[lod];
        REQUIRE(level.submeshes.size() == 2);
        CHECK(level.submeshes[0].material == 1);
        CHECK(level.submeshes[1].material == 3);
        std::uint32_t sum = 0;
        for (const auto& s : level.submeshes) {
            CHECK(s.first_index >= level.first_index);
            CHECK(s.first_index + s.index_count <= level.first_index + level.index_count);
            sum += s.index_count;
        }
        CHECK(sum == level.index_count);
        // Relative ranges for renderers start at 0.
        CHECK(cooked.lod_submeshes(lod)[0].first_index == 0);
        // Each range keeps to its half of the mesh (materials don't swap triangles).
        const auto& right = level.submeshes[0];
        for (std::uint32_t i = 0; i < right.index_count; ++i) {
            CHECK(cooked.vertices[cooked.indices[right.first_index + i]].position.x >= 0.5f - 1e-4f);
        }
    }
    const auto bytes = assets::write_f3dmesh(cooked);
    std::string error;
    const auto back = assets::read_f3dmesh(bytes, &error);
    INFO(error);
    REQUIRE(back.has_value());
    REQUIRE(back->lods.size() == cooked.lods.size());
    for (std::size_t i = 0; i < cooked.lods.size(); ++i) {
        CHECK(back->lods[i].submeshes == cooked.lods[i].submeshes);
    }
    CHECK(back->to_mesh_data(1).submeshes == cooked.lod_submeshes(1));
}

TEST_CASE("submeshes: version 1 .f3dmesh files (no submesh table) still load") {
    assets::MeshData mesh = two_material_mesh(4);
    mesh.submeshes.clear();
    const assets::CookedMesh cooked = assets::cook_mesh(mesh, {});
    std::vector<std::uint8_t> v2 = assets::write_f3dmesh(cooked);
    // v1 = v2 without the per-LOD submesh counts (all zero here), version 1.
    const std::size_t header = 52 + cooked.name.size();
    const std::size_t tables = header + cooked.lods.size() * 12;
    std::vector<std::uint8_t> v1(v2.begin(), v2.begin() + static_cast<std::ptrdiff_t>(tables));
    v1.insert(v1.end(), v2.begin() + static_cast<std::ptrdiff_t>(tables + cooked.lods.size() * 4), v2.end());
    const std::uint32_t version = 1;
    std::memcpy(v1.data() + 4, &version, 4);
    std::string error;
    const auto back = assets::read_f3dmesh(v1, &error);
    INFO(error);
    REQUIRE(back.has_value());
    CHECK(back->indices == cooked.indices);
    CHECK(back->lods[0].submeshes.empty());
    CHECK(back->lod_submeshes(0).size() == 1);
}

TEST_CASE("materials: the mesh library resolves model materials, images and slots") {
    assets::MeshLibrary library;
    const std::string file = (data_dir() / "gltf" / "Fox.glb").generic_string();
    const core::AssetRef mesh_ref{core::Guid{}, file + "#mesh/0"};
    const auto mesh = library.get(mesh_ref);
    REQUIRE(mesh);
    const auto submeshes = assets::submeshes_of(*mesh);
    REQUIRE(submeshes.size() == 1);

    scene::MeshRenderer renderer;
    renderer.mesh = mesh_ref;
    const core::AssetRef slot = assets::MeshLibrary::slot_material(renderer, 0, submeshes[0]);
    CHECK(slot.path == file + "#material/0");
    const auto material = library.material(slot);
    REQUIRE(material);
    CHECK(material->base_color_texture.path == file + "#image/0"); // rebased onto the file
    assets::TextureKind kind = assets::TextureKind::Linear;
    const auto image = library.image(material->base_color_texture, &kind);
    REQUIRE(image.has_value());
    CHECK(kind == assets::TextureKind::Color);

    // An override replaces the default; an empty entry keeps it.
    library.add_material("mem#material", assets::Material{});
    renderer.materials = {{core::Guid{}, "mem#material"}};
    CHECK(assets::MeshLibrary::slot_material(renderer, 0, submeshes[0]).path == "mem#material");
    renderer.materials = {{}};
    CHECK(assets::MeshLibrary::slot_material(renderer, 0, submeshes[0]).path == file + "#material/0");
    CHECK(library.material({core::Guid{}, "mem#material"}));
    CHECK_FALSE(library.material({core::Guid{}, file + "#material/9"}));
    CHECK(assets::MeshLibrary::default_material(mesh_ref, assets::SubMesh::kNoMaterial).empty());
}

TEST_CASE("materials: imported models cook their materials and images") {
    const fs::path dir = fs::path(F3D_TEST_OUTPUT_DIR) / "material_import";
    fs::remove_all(dir);
    fs::create_directories(dir / "models");
    fs::copy(data_dir() / "import", dir, fs::copy_options::recursive);
    fs::copy_file(data_dir() / "gltf" / "Fox.glb", dir / "models" / "Fox.glb");
    core::vfs::FileSystem files;
    files.mount("project", std::make_shared<core::vfs::DirectoryBackend>(dir));
    assets::AssetDatabase db;
    db.rescan(files, "project://");
    for (const char* path : {"project://models/Fox.glb", "project://models/cube.obj"}) {
        const assets::AssetRecord* record = nullptr;
        for (const auto& r : db.records()) {
            if (r.path == path) {
                record = &r;
            }
        }
        REQUIRE(record);
        const assets::ImportOutcome outcome = assets::import_asset(files, *record);
        INFO(path, ": ", outcome.error);
        REQUIRE(outcome.ok);
        const auto model = assets::load_cooked_model(files, record->guid);
        REQUIRE(model.has_value());
        REQUIRE(model->materials.size() == 1);
        CHECK(model->materials[0].base_color_texture.path == "#image/0");
        CHECK(model->meshes[0].lods[0].submeshes.size() == 1);
        std::string error;
        const auto image =
            assets::load_cooked_texture(files, core::AssetRef{record->guid, std::string(path) + "#image/0"}, &error);
        INFO(error);
        REQUIRE(image.has_value());
        CHECK(image->srgb); // a base colour
        CHECK(image->mips.size() > 1);
    }
}

TEST_CASE("material ops: create and edit a material file, merge drags, undo and redo") {
    const fs::path dir = fs::path(F3D_TEST_OUTPUT_DIR) / "material_ops";
    fs::remove_all(dir);
    fs::create_directories(dir);
    core::vfs::FileSystem files;
    files.mount("project", std::make_shared<core::vfs::DirectoryBackend>(dir));
    scene::World world;
    ops::ChangeBus changes;
    std::vector<std::string> changed;
    changes.subscribe([&](const ops::ChangeEvent& e) {
        if (e.kind == ops::ChangeKind::AssetChanged) {
            changed.push_back(e.asset);
        }
    });
    ops::Context context{world, changes, &files};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    assets::register_material_ops(registry);
    const std::string path = "project://materials/steel.f3dmat";
    const auto load = [&] { return assets::read_material(*files.read_text(path)); };

    REQUIRE(stack.execute(
        registry.create("material.create", {{"path", path}, {"material", {{"name", "Steel"}, {"metallic", 1.0}}}})));
    CHECK(load()->metallic == 1.0f);
    CHECK(load()->roughness == 0.5f);
    CHECK(changed == std::vector<std::string>{path});
    // A second create on an existing file fails.
    CHECK_FALSE(stack.execute(registry.create("material.create", {{"path", path}})));

    // A drag: three edits, one history entry.
    for (const double r : {0.4, 0.3, 0.2}) {
        REQUIRE(stack.execute(
            registry.create("material.set_property", {{"path", path}, {"property", "roughness"}, {"value", r}}), true));
    }
    CHECK(load()->roughness == doctest::Approx(0.2f));
    REQUIRE(stack.execute(registry.create(
        "material.set_property", {{"path", path}, {"property", "base_color"}, {"value", {0.1, 0.2, 0.3, 1.0}}})));
    CHECK(load()->base_color == glm::vec4(0.1f, 0.2f, 0.3f, 1.0f));
    // Unknown fields and bad values fail without touching the file.
    CHECK_FALSE(stack.execute(
        registry.create("material.set_property", {{"path", path}, {"property", "shininess"}, {"value", 3}})));
    CHECK_FALSE(stack.execute(
        registry.create("material.set_property", {{"path", path}, {"property", "roughness"}, {"value", "rough"}})));

    stack.undo(); // base colour
    CHECK(load()->base_color == glm::vec4(1.0f));
    stack.undo(); // the whole drag
    CHECK(load()->roughness == doctest::Approx(0.5f));
    stack.undo(); // the file
    CHECK_FALSE(files.exists(path));
    stack.redo();
    stack.redo();
    CHECK(load()->roughness == doctest::Approx(0.2f));
    CHECK(load()->name == "Steel");

    // Delete takes the .meta with it; undo brings both back (the GUID survives).
    REQUIRE(files.write_text(path + ".meta", "{\"guid\": \"x\"}"));
    REQUIRE(stack.execute(registry.create("material.delete", {{"path", path}})));
    CHECK_FALSE(files.exists(path));
    CHECK_FALSE(files.exists(path + ".meta"));
    CHECK_FALSE(stack.execute(registry.create("material.delete", {{"path", path}})));
    stack.undo();
    CHECK(load()->name == "Steel");
    CHECK(*files.read_text(path + ".meta") == "{\"guid\": \"x\"}");
}
