// SPDX-License-Identifier: Apache-2.0
// The SQLite asset index and model placement from cooked assets (phases.md P3.3).
#include <doctest/doctest.h>

#include "f3d/assets/asset_database.hpp"
#include "f3d/assets/asset_index.hpp"
#include "f3d/assets/mesh_library.hpp"
#include "f3d/assets/pipeline.hpp"
#include "f3d/core/vfs.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/script.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/world.hpp"

#include <filesystem>
#include <fstream>

using namespace f3d;
namespace fs = std::filesystem;

namespace {

struct Project {
    fs::path dir;
    core::vfs::FileSystem files;
    assets::AssetDatabase db;

    explicit Project(const char* name) : dir(fs::path(F3D_TEST_OUTPUT_DIR) / "asset_index" / name) {
        fs::remove_all(dir);
        fs::create_directories(dir);
        fs::copy(fs::path(F3D_TEST_DATA_DIR) / "import", dir, fs::copy_options::recursive);
        files.mount("project", std::make_shared<core::vfs::DirectoryBackend>(dir));
        db.rescan(files, "project://");
    }

    void import_all() {
        for (const assets::AssetRecord& r : db.records()) {
            REQUIRE_MESSAGE(assets::import_asset(files, r).ok, r.path);
        }
        db.rescan(files, "project://");
    }

    void tag(std::string_view path, std::vector<std::string> tags) {
        const std::string meta_path = std::string(path) + ".meta";
        auto meta = assets::parse_meta(*files.read_text(meta_path));
        REQUIRE(meta.has_value());
        meta->tags = std::move(tags);
        REQUIRE(files.write_text(meta_path, assets::meta_to_json(*meta)));
    }
};

std::vector<std::string> names(const std::vector<assets::AssetSummary>& results) {
    std::vector<std::string> out;
    for (const auto& r : results) {
        out.push_back(r.name);
    }
    return out;
}

} // namespace

TEST_CASE("asset index: incremental sync and search by text, type and tags") {
    Project p("search");
    auto index = assets::AssetIndex::open(":memory:");
    REQUIRE(index != nullptr);

    auto stats = index->sync(p.db);
    CHECK(stats.inserted == 8);
    CHECK(index->size() == 8);
    stats = index->sync(p.db); // nothing changed: nothing written
    CHECK(stats.unchanged == 8);
    CHECK(stats.inserted + stats.updated + stats.removed == 0);

    CHECK(names(index->search({"rock", "", {}})) == std::vector<std::string>{"rock_normal", "rock_roughness"});
    CHECK(names(index->search({"ROCK_N", "", {}})) == std::vector<std::string>{"rock_normal"}); // case-insensitive
    CHECK(index->search({"", "model", {}}).size() == 3);
    CHECK(index->search({"", "texture", {}}).size() == 5);
    CHECK(index->search({"models/", "", {}}).size() == 3); // matches paths too
    CHECK(index->search({"100%_", "", {}}).empty());       // LIKE wildcards are literal
    CHECK(index->search({"", "", {}, 2}).size() == 2);     // limit

    // Tags come from the .meta files; changing them updates only that row.
    p.tag("project://textures/checker.png", {"debug", "pattern"});
    p.tag("project://models/cube.obj", {"debug"});
    p.db.rescan(p.files, "project://");
    stats = index->sync(p.db);
    CHECK(stats.updated == 2);
    CHECK(stats.unchanged == 6);
    CHECK(index->tags() == std::vector<std::string>{"debug", "pattern"});
    CHECK(names(index->search({"", "", {"debug"}})) == std::vector<std::string>{"checker", "cube"});
    CHECK(names(index->search({"", "", {"debug", "pattern"}})) == std::vector<std::string>{"checker"});
    CHECK(names(index->search({"", "model", {"debug"}})) == std::vector<std::string>{"cube"});

    // Removing a source removes its row.
    fs::remove(p.dir / "textures" / "photo.jpg");
    p.db.rescan(p.files, "project://");
    stats = index->sync(p.db);
    CHECK(stats.removed == 1);
    CHECK(index->size() == 7);
}

TEST_CASE("asset index: dependencies found at import, and imported state") {
    Project p("dependencies");
    auto index = assets::AssetIndex::open(":memory:");
    REQUIRE(index != nullptr);
    index->sync(p.db);
    CHECK(index->search({"cube", "model", {}})[0].stale); // not imported yet

    p.import_all();
    index->sync(p.db);
    const assets::AssetRecord* cube = p.db.find_path("project://models/cube.obj");
    const assets::AssetRecord* checker = p.db.find_path("project://textures/checker.png");
    REQUIRE(cube != nullptr);
    REQUIRE(checker != nullptr);
    // cube.obj -> cube.mtl (not an asset) and map_Kd ../textures/checker.png.
    CHECK(cube->dependencies ==
          std::vector<std::string>{"project://models/cube.mtl", "project://textures/checker.png"});
    CHECK(index->dependencies(cube->guid) == std::vector<core::Guid>{checker->guid});
    CHECK(index->dependents(checker->guid) == std::vector<core::Guid>{cube->guid});
    CHECK_FALSE(index->search({"cube", "model", {}})[0].stale);
}

TEST_CASE("asset index: persists, seeds the next scan and records thumbnails") {
    Project p("persist");
    const fs::path file = p.dir / ".forge3d" / "index.sqlite";
    {
        auto index = assets::AssetIndex::open(file);
        REQUIRE(index != nullptr);
        index->sync(p.db);
        index->set_thumbnail(p.db.records()[0].guid, {"project://.forge3d/thumbnails/x.png", "fnv1a64:1"});
    }
    auto index = assets::AssetIndex::open(file);
    REQUIRE(index != nullptr);
    CHECK(index->size() == 8);
    const auto thumbnail = index->thumbnail(p.db.records()[0].guid);
    REQUIRE(thumbnail.has_value());
    CHECK(thumbnail->source_hash == "fnv1a64:1");
    CHECK_FALSE(index->thumbnail(core::Guid::generate()).has_value());

    // A new session seeded from the index re-hashes nothing (files unchanged).
    assets::AssetDatabase next;
    next.seed(index->load_records());
    CHECK(next.size() == 8);
    const assets::ScanReport report = next.rescan(p.files, "project://");
    CHECK(report.hashed == 0);
    CHECK(report.metas_created == 0);
    CHECK(next.find_path("project://models/cube.obj")->guid == p.db.find_path("project://models/cube.obj")->guid);
}

TEST_CASE("placement: a cooked model is instanced through operations and resolves by GUID") {
    Project p("placement");
    p.import_all();
    const assets::AssetRecord* fbx = p.db.find_path("project://models/cube_ascii.fbx");
    REQUIRE(fbx != nullptr);
    const auto model = assets::load_cooked_model(p.files, fbx->guid);
    REQUIRE(model.has_value());

    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    ops::register_builtin_ops(registry);
    core::Guid root;
    const auto script = assets::model_instance_ops(
        fbx->name(), model->nodes,
        [&](std::size_t mesh) { return core::AssetRef{fbx->guid, fbx->path + "#mesh/" + std::to_string(mesh)}; }, {},
        glm::vec3(5.0f, 0.0f, -2.0f), root);
    REQUIRE(ops::run_script(stack, registry, script, "Place").ok);
    CHECK(stack.undo_count() == 1);
    const entt::entity root_e = world.find(root);
    REQUIRE((root_e != entt::null));
    CHECK(world.transform(root_e).translation == glm::vec3(5.0f, 0.0f, -2.0f));

    // The MeshRenderer references the asset by GUID; a resolver turns it into the cooked mesh.
    assets::MeshLibrary meshes;
    int resolved = 0;
    meshes.set_resolver([&](const core::AssetRef& ref) -> std::shared_ptr<const assets::MeshData> {
        ++resolved;
        CHECK(ref.guid == fbx->guid);
        return std::make_shared<const assets::MeshData>(model->meshes[0].to_mesh_data(0));
    });
    std::size_t renderers = 0;
    for (const entt::entity e : world.subtree(root_e)) {
        if (const auto* mr = world.registry().try_get<scene::MeshRenderer>(e)) {
            ++renderers;
            CHECK(mr->mesh.guid == fbx->guid);
            const auto mesh = meshes.get(mr->mesh);
            REQUIRE(mesh != nullptr);
            CHECK(mesh->indices.size() == 36);
        }
    }
    CHECK(renderers == 1);
    (void)meshes.get(world.registry().get<scene::MeshRenderer>(world.subtree(root_e).back()).mesh);
    CHECK(resolved == 1); // cached after the first request
    meshes.invalidate(fbx->guid);
    (void)meshes.get(world.registry().get<scene::MeshRenderer>(world.subtree(root_e).back()).mesh);
    CHECK(resolved == 2); // re-resolved after a re-import

    // Placed instances sit on top of their bounds' world position (cube: 2 m tall, node at y = 1).
    const core::Bounds bounds = assets::world_bounds(world, meshes, root_e);
    CHECK(bounds.min.y == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(bounds.center().x == doctest::Approx(5.0f).epsilon(0.01));
}
