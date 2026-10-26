// SPDX-License-Identifier: Apache-2.0
// Importers and cooking (phases.md P3.2): the corpus in tests/data/import/ plus
// the glTF samples, imported into a scratch project and reloaded from the cache.
#include <doctest/doctest.h>

#include "f3d/assets/asset_database.hpp"
#include "f3d/assets/cook.hpp"
#include "f3d/assets/mesh.hpp"
#include "f3d/assets/pipeline.hpp"
#include "f3d/assets/texture.hpp"
#include "f3d/core/vfs.hpp"

#include <cmath>
#include <filesystem>
#include <map>

using namespace f3d;
namespace fs = std::filesystem;

namespace {

fs::path data_dir() {
    return fs::path(F3D_TEST_DATA_DIR);
}

/// A scratch project holding a copy of the corpus.
struct CorpusProject {
    fs::path dir = fs::path(F3D_TEST_OUTPUT_DIR) / "import_corpus";
    core::vfs::FileSystem files;
    assets::AssetDatabase db;

    CorpusProject() {
        fs::remove_all(dir);
        fs::create_directories(dir);
        fs::copy(data_dir() / "import", dir, fs::copy_options::recursive);
        fs::copy_file(data_dir() / "gltf" / "Box.glb", dir / "models" / "Box.glb");
        fs::copy_file(data_dir() / "gltf" / "Fox.glb", dir / "models" / "Fox.glb");
        files.mount("project", std::make_shared<core::vfs::DirectoryBackend>(dir));
        db.rescan(files, "project://");
    }
};

/// Peak signal-to-noise ratio of two RGBA8 images over RGB (dB).
double psnr(const core::Image& a, const core::Image& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
        if (i % 4 == 3) {
            continue;
        }
        const double d = static_cast<double>(a.rgba[i]) - static_cast<double>(b.rgba[i]);
        sum += d * d;
        ++n;
    }
    const double mse = sum / static_cast<double>(n);
    return mse == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

} // namespace

TEST_CASE("pipeline: every file of the import corpus imports, and the cooked outputs reload") {
    CorpusProject project;
    REQUIRE(project.db.size() == 10); // 5 models (OBJ x2, FBX, glTF x2) + 5 textures (cube.mtl isn't an asset)
    std::map<std::string, std::string> importers;
    for (const assets::AssetRecord& record : project.db.records()) {
        importers[record.path] = record.importer;
        INFO(record.path);
        const assets::ImportOutcome outcome = assets::import_asset(project.files, record);
        CHECK_MESSAGE(outcome.ok, outcome.error);
        CHECK_FALSE(outcome.outputs.empty());
        if (record.importer == "texture" || record.importer == "texture_hdr") {
            std::string error;
            const auto texture = assets::load_cooked_texture(project.files, record.guid, &error);
            REQUIRE_MESSAGE(texture.has_value(), error);
            CHECK(texture->width() > 0);
        } else {
            std::string error;
            const auto model = assets::load_cooked_model(project.files, record.guid, &error);
            REQUIRE_MESSAGE(model.has_value(), error);
            CHECK_FALSE(model->meshes.empty());
            for (const assets::CookedMesh& mesh : model->meshes) {
                CHECK(mesh.triangle_count(0) > 0);
                CHECK_FALSE(mesh.bounds.empty());
            }
        }
    }
    CHECK(importers["project://models/cube_ascii.fbx"] == "fbx");
    CHECK(importers["project://models/cube.obj"] == "obj");
    CHECK(importers["project://textures/sky.exr"] == "texture_hdr");

    // A rescan sees every asset as imported (the .meta records the source hash).
    project.db.rescan(project.files, "project://");
    for (const assets::AssetRecord& record : project.db.records()) {
        CHECK_MESSAGE(!record.stale(), record.path);
        CHECK(record.settings.is_object());
        CHECK_FALSE(record.settings.empty()); // defaults written back for the user to edit
    }
}

TEST_CASE("pipeline: model importers agree on a 2 m cube") {
    // OBJ (tinyobjloader) and FBX (ufbx, converted from metres-in-file to Y-up metres).
    for (const char* file : {"cube.obj", "cube_ascii.fbx"}) {
        INFO(file);
        const assets::ModelLoadResult r = assets::load_model(data_dir() / "import" / "models" / file);
        REQUIRE_MESSAGE(r.scene.has_value(), r.error);
        REQUIRE(r.scene->meshes.size() == 1);
        const assets::MeshData& mesh = r.scene->meshes[0];
        CHECK(mesh.indices.size() == 36);  // 6 quads -> 12 triangles
        CHECK(mesh.vertices.size() == 24); // 4 per face: normals/UVs differ per face
        CHECK(mesh.uvs.size() == mesh.vertices.size());
        const glm::vec3 size = mesh.bounds.extent();
        CHECK(size.x == doctest::Approx(2.0f).epsilon(0.01));
        CHECK(size.y == doctest::Approx(2.0f).epsilon(0.01));
        CHECK(size.z == doctest::Approx(2.0f).epsilon(0.01));
        for (const assets::MeshVertex& v : mesh.vertices) {
            CHECK(glm::length(v.normal) == doctest::Approx(1.0f).epsilon(0.001));
        }
    }
    const assets::ModelLoadResult fbx = assets::load_model(data_dir() / "import" / "models" / "cube_ascii.fbx");
    REQUIRE(fbx.scene.has_value());
    CHECK(fbx.scene->nodes[0].name == "Cube");
    CHECK(fbx.scene->nodes[0].translation.y == doctest::Approx(1.0f)); // Lcl Translation 0,1,0 (metres)
    CHECK_FALSE(assets::load_model(data_dir() / "import" / "textures" / "checker.png").scene.has_value());
}

TEST_CASE("pipeline: LOD triangle counts shrink as expected") {
    const assets::ModelLoadResult r = assets::load_model(data_dir() / "import" / "models" / "height_field.obj");
    REQUIRE(r.scene.has_value());
    const std::uint32_t source_triangles = static_cast<std::uint32_t>(r.scene->meshes[0].indices.size() / 3);
    CHECK(source_triangles == 2 * 64 * 64);

    assets::MeshCookSettings settings; // 4 levels, ratio 0.5
    const assets::CookedMesh cooked = assets::cook_mesh(r.scene->meshes[0], settings);
    REQUIRE(cooked.lods.size() >= 3);
    CHECK(cooked.triangle_count(0) == source_triangles);
    for (std::size_t lod = 1; lod < cooked.lods.size(); ++lod) {
        INFO("LOD ", lod, ": ", cooked.triangle_count(lod), " triangles");
        CHECK(cooked.triangle_count(lod) < cooked.triangle_count(lod - 1));
        // Each level aims for half of the previous one; allow the simplifier some slack.
        CHECK(cooked.triangle_count(lod) <= cooked.triangle_count(lod - 1) * 6 / 10);
        CHECK(cooked.lods[lod].error <= settings.lod_error + 1e-4f);
    }

    // Round trip through the file format.
    const auto bytes = assets::write_f3dmesh(cooked);
    std::string error;
    const auto loaded = assets::read_f3dmesh(bytes, &error);
    REQUIRE_MESSAGE(loaded.has_value(), error);
    CHECK(loaded->lods.size() == cooked.lods.size());
    CHECK(loaded->indices == cooked.indices);
    CHECK(loaded->vertices.size() == cooked.vertices.size());
    CHECK(loaded->to_mesh_data(1).indices.size() == cooked.lods[1].index_count);
    auto damaged = bytes;
    damaged.resize(damaged.size() - 4);
    CHECK_FALSE(assets::read_f3dmesh(damaged).has_value());

    // A single LOD when asked, and tiny meshes aren't simplified.
    settings.max_lods = 1;
    CHECK(assets::cook_mesh(r.scene->meshes[0], settings).lods.size() == 1);
    const auto box = assets::load_model(data_dir() / "gltf" / "Box.glb");
    REQUIRE(box.scene.has_value());
    CHECK(assets::cook_mesh(box.scene->meshes[0], {}).lods.size() == 1);
}

TEST_CASE("pipeline: xatlas generates UVs inside [0, 1]") {
    const auto r = assets::load_model(data_dir() / "gltf" / "Fox.glb");
    REQUIRE(r.scene.has_value());
    assets::MeshData mesh = r.scene->meshes[0];
    const std::size_t triangles = mesh.indices.size() / 3;
    std::string error;
    REQUIRE_MESSAGE(assets::generate_uvs(mesh, &error), error);
    CHECK(mesh.indices.size() / 3 == triangles);
    REQUIRE(mesh.uvs.size() == mesh.vertices.size());
    for (const glm::vec2& uv : mesh.uvs) {
        CHECK(uv.x >= 0.0f);
        CHECK(uv.x <= 1.0f);
        CHECK(uv.y >= 0.0f);
        CHECK(uv.y <= 1.0f);
    }
}

TEST_CASE("pipeline: textures get the right format, mips and quality") {
    const auto load = [](const char* name) {
        const fs::path dir = data_dir() / "import" / "textures";
        const auto bytes = core::vfs::DirectoryBackend(dir, false).read(name);
        REQUIRE(bytes.has_value());
        std::string error;
        auto image = assets::decode_texture_source(*bytes, fs::path(name).extension().string(), &error);
        REQUIRE_MESSAGE(image.has_value(), error);
        return std::make_pair(std::move(*image), assets::TextureSettings::defaults_for(name));
    };
    /// Round-trips through the file format and decodes mip 0.
    const auto reload = [](const assets::CookedTexture& t) {
        std::string error;
        const auto loaded = assets::read_f3dtex(assets::write_f3dtex(t), &error);
        REQUIRE_MESSAGE(loaded.has_value(), error);
        CHECK(loaded->format == t.format);
        CHECK(loaded->mips.size() == t.mips.size());
        return assets::decode_mip(*loaded, 0);
    };

    SUBCASE("colour: BC7 sRGB, a full mip chain, faithful to the source") {
        auto [image, settings] = load("checker.png");
        CHECK(settings.kind == assets::TextureKind::Color);
        const assets::CookedTexture t = assets::cook_texture(image, settings);
        CHECK(t.format == assets::TextureFormat::BC7);
        CHECK(t.srgb);
        CHECK(t.mips.size() == 7); // 64 .. 1
        CHECK(t.mips.back().width == 1);
        CHECK(t.mips[0].data.size() == 16u * 16u * 16u); // 16 x 16 blocks of 16 bytes
        CHECK(psnr(reload(t), image.ldr) > 35.0);
    }
    SUBCASE("colour, non power of two JPEG: partial blocks and odd mips") {
        auto [image, settings] = load("photo.jpg");
        const assets::CookedTexture t = assets::cook_texture(image, settings);
        CHECK(t.width() == 100);
        CHECK(t.height() == 60);
        CHECK(t.mips[1].width == 50);
        CHECK(t.mips[2].height == 15);
        CHECK(psnr(reload(t), image.ldr) > 35.0);
    }
    SUBCASE("normal map: BC5, linear, unit normals after decoding") {
        auto [image, settings] = load("rock_normal.png");
        CHECK(settings.kind == assets::TextureKind::Normal);
        const assets::CookedTexture t = assets::cook_texture(image, settings);
        CHECK(t.format == assets::TextureFormat::BC5);
        CHECK_FALSE(t.srgb);
        const core::Image decoded = reload(t);
        double max_error = 0.0; // in X/Y, compared to the source
        for (std::size_t i = 0; i < decoded.rgba.size(); i += 4) {
            for (int c = 0; c < 2; ++c) {
                max_error = std::max(max_error, std::abs(decoded.rgba[i + c] - image.ldr.rgba[i + c]) / 255.0);
            }
        }
        CHECK(max_error < 0.05);
    }
    SUBCASE("grayscale: BC4") {
        auto [image, settings] = load("rock_roughness.png");
        CHECK(settings.kind == assets::TextureKind::Grayscale);
        const assets::CookedTexture t = assets::cook_texture(image, settings);
        CHECK(t.format == assets::TextureFormat::BC4);
        CHECK(psnr(reload(t), image.ldr) > 35.0);
    }
    SUBCASE("HDR EXR: RGBA16F keeps values above 1") {
        auto [image, settings] = load("sky.exr");
        CHECK(settings.kind == assets::TextureKind::Hdr);
        REQUIRE(image.is_hdr());
        CHECK(image.hdr.width == 16);
        CHECK(image.hdr.height == 8);
        CHECK(image.hdr.rgba[(7 * 16 + 15) * 4] > 3.0f); // red ramps up to ~4
        CHECK(image.hdr.rgba[2] == doctest::Approx(1.5f));
        const assets::CookedTexture t = assets::cook_texture(image, settings);
        CHECK(t.format == assets::TextureFormat::RGBA16F);
        CHECK(t.mips[0].data.size() == 16u * 8u * 8u);
        reload(t);
    }
    SUBCASE("settings: explicit compression and no mips") {
        auto [image, settings] = load("checker.png");
        settings.compression = "bc1";
        settings.mipmaps = false;
        const assets::CookedTexture t = assets::cook_texture(image, settings);
        CHECK(t.format == assets::TextureFormat::BC1);
        CHECK(t.mips.size() == 1);
        settings.compression = "none";
        CHECK(assets::cook_texture(image, settings).format == assets::TextureFormat::RGBA8);
        const auto json = settings.to_json();
        const auto back = assets::TextureSettings::from_json(json, {});
        CHECK(back.compression == "none");
        CHECK_FALSE(back.mipmaps);
    }
    CHECK_FALSE(assets::read_f3dtex(std::vector<std::uint8_t>{1, 2, 3}).has_value());
}
