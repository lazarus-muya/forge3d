// SPDX-License-Identifier: Apache-2.0
// Asset identity: .meta sidecars and the GUID -> path database (phases.md P3.1).
#include <doctest/doctest.h>

#include "f3d/assets/asset_database.hpp"
#include "f3d/core/vfs.hpp"

#include <filesystem>
#include <fstream>

using namespace f3d;
namespace fs = std::filesystem;

namespace {

struct Project {
    fs::path dir;
    core::vfs::FileSystem files;
    assets::AssetDatabase db;

    explicit Project(const char* name) : dir(fs::path(F3D_TEST_OUTPUT_DIR) / "assets_db" / name) {
        fs::remove_all(dir);
        fs::create_directories(dir);
        files.mount("project", std::make_shared<core::vfs::DirectoryBackend>(dir));
    }

    void put(const fs::path& relative, const std::string& content) {
        fs::create_directories((dir / relative).parent_path());
        std::ofstream(dir / relative, std::ios::binary) << content;
    }
};

} // namespace

TEST_CASE("assets: meta files round-trip and reject bad input") {
    assets::AssetMeta meta;
    meta.guid = core::Guid::generate();
    meta.importer = "texture";
    meta.settings = {{"srgb", true}};
    meta.source_hash = "fnv1a64:0123456789abcdef";
    const auto parsed = assets::parse_meta(assets::meta_to_json(meta));
    REQUIRE(parsed.has_value());
    CHECK(parsed->guid == meta.guid);
    CHECK(parsed->importer == "texture");
    CHECK(parsed->settings["srgb"] == true);
    CHECK(parsed->source_hash == meta.source_hash);

    std::string error;
    CHECK_FALSE(assets::parse_meta("not json", &error).has_value());
    CHECK_FALSE(assets::parse_meta(R"({"format":"forge3d.meta","version":1,"guid":"zz"})", &error).has_value());
    CHECK(error.find("guid") != std::string::npos);
    CHECK_FALSE(assets::parse_meta(R"({"format":"other","version":1})").has_value());

    CHECK(assets::importer_for("project://a/Rock.GLB") == "gltf");
    CHECK(assets::importer_for("project://t.png") == "texture");
    CHECK(assets::importer_for("project://sky.exr") == "texture_hdr");
    CHECK(assets::importer_for("project://scene.forge").empty());
    CHECK(assets::importer_for("project://dir.with.dot/noext").empty());
    CHECK(assets::hash_source({}) == "fnv1a64:cbf29ce484222325");
}

TEST_CASE("assets: first sight creates .meta files; GUIDs resolve to paths") {
    Project p("first_sight");
    p.put("models/rock.glb", "rock bytes");
    p.put("textures/grass.png", "grass bytes");
    p.put("notes.txt", "not an asset");
    p.put(".forge3d/cache/ignored.png", "cache");

    const assets::ScanReport report = p.db.rescan(p.files, "project://");
    CHECK(report.sources == 2);
    CHECK(report.metas_created == 2);
    CHECK(report.errors.empty());
    CHECK(fs::exists(p.dir / "models" / "rock.glb.meta"));
    CHECK(fs::exists(p.dir / "textures" / "grass.png.meta"));
    CHECK_FALSE(fs::exists(p.dir / "notes.txt.meta"));
    CHECK_FALSE(fs::exists(p.dir / ".forge3d" / "cache" / "ignored.png.meta"));

    const assets::AssetRecord* rock = p.db.find_path("project://models/rock.glb");
    REQUIRE(rock != nullptr);
    CHECK(rock->importer == "gltf");
    CHECK(rock->stale()); // never imported
    CHECK(p.db.find(rock->guid) == rock);

    // A second scan keeps the GUIDs and creates nothing.
    const core::Guid guid = rock->guid;
    const assets::ScanReport again = p.db.rescan(p.files, "project://");
    CHECK(again.metas_created == 0);
    CHECK(again.hashed == 0); // unchanged files aren't re-read
    CHECK(p.db.find_path("project://models/rock.glb")->guid == guid);
}

TEST_CASE("assets: renaming or moving a source with its .meta keeps the GUID") {
    Project p("rename");
    p.put("models/rock.glb", "rock bytes");
    p.db.rescan(p.files, "project://");
    const core::Guid guid = p.db.find_path("project://models/rock.glb")->guid;

    fs::create_directories(p.dir / "props");
    fs::rename(p.dir / "models" / "rock.glb", p.dir / "props" / "boulder.glb");
    fs::rename(p.dir / "models" / "rock.glb.meta", p.dir / "props" / "boulder.glb.meta");
    const assets::ScanReport report = p.db.rescan(p.files, "project://");
    CHECK(report.metas_created == 0);
    CHECK(report.orphaned.empty());
    CHECK(p.db.find_path("project://models/rock.glb") == nullptr);
    REQUIRE(p.db.find(guid) != nullptr);
    CHECK(p.db.find(guid)->path == "project://props/boulder.glb");
}

TEST_CASE("assets: orphaned .meta files, copies and edits are reported") {
    Project p("orphans");
    p.put("a.png", "aaa");
    p.put("b.png", "bbb");
    p.db.rescan(p.files, "project://");
    const core::Guid a = p.db.find_path("project://a.png")->guid;

    SUBCASE("a .meta without its source is orphaned (and kept)") {
        fs::remove(p.dir / "b.png");
        const assets::ScanReport report = p.db.rescan(p.files, "project://");
        REQUIRE(report.orphaned.size() == 1);
        CHECK(report.orphaned[0] == "project://b.png.meta");
        CHECK(fs::exists(p.dir / "b.png.meta"));
        CHECK(p.db.size() == 1);
    }
    SUBCASE("a file copied with its .meta gets a new GUID; the original keeps its own") {
        fs::copy_file(p.dir / "a.png", p.dir / "a_copy.png");
        fs::copy_file(p.dir / "a.png.meta", p.dir / "a_copy.png.meta");
        const assets::ScanReport report = p.db.rescan(p.files, "project://");
        REQUIRE(report.regenerated.size() == 1);
        CHECK(report.regenerated[0] == "project://a_copy.png");
        CHECK(p.db.find_path("project://a.png")->guid == a);
        CHECK(p.db.find_path("project://a_copy.png")->guid != a);
        CHECK(assets::parse_meta(*p.files.read_text("project://a_copy.png.meta"))->guid ==
              p.db.find_path("project://a_copy.png")->guid);
    }
    SUBCASE("an edited source is re-hashed and stale; a corrupt .meta is regenerated") {
        const std::string before = p.db.find_path("project://a.png")->source_hash;
        p.put("a.png", "changed contents");
        p.put("b.png.meta", "{ broken");
        const assets::ScanReport report = p.db.rescan(p.files, "project://");
        CHECK(p.db.find_path("project://a.png")->source_hash != before);
        CHECK(p.db.find_path("project://a.png")->guid == a);
        CHECK(report.regenerated == std::vector<std::string>{"project://b.png"});
        CHECK(assets::parse_meta(*p.files.read_text("project://b.png.meta")).has_value());
    }
}
