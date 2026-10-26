// SPDX-License-Identifier: Apache-2.0
// The .forge container and project files (phases.md P2.6).
#include <doctest/doctest.h>

#include "f3d/core/vfs.hpp"
#include "f3d/io/archive.hpp"
#include "f3d/io/archive_backend.hpp"
#include "f3d/io/forge_file.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/script.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/layers.hpp"
#include "f3d/scene/snapshot.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace f3d;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

fs::path out_dir() {
    const fs::path dir = fs::path(F3D_TEST_OUTPUT_DIR) / "io";
    fs::create_directories(dir);
    return dir;
}

io::Bytes bytes(std::string_view text) {
    return io::Bytes(text.begin(), text.end());
}

/// A scene with hierarchy, a layer and a bookmark, built through operations.
struct SampleScene {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;

    SampleScene() {
        ops::register_builtin_ops(registry);
        const auto r =
            ops::run_script_file(stack, registry, fs::path(F3D_TEST_DATA_DIR) / "scenes" / "outliner_layers.json");
        REQUIRE(r.ok);
    }
};

void write_archive(const fs::path& path, const std::function<void(io::ArchiveWriter&)>& fill) {
    std::FILE* f = io::open_file(path, "wb");
    REQUIRE(f != nullptr);
    io::ArchiveWriter writer(f);
    fill(writer);
    REQUIRE(writer.finish());
    std::fclose(f);
}

} // namespace

TEST_CASE("io: crc32 matches the standard check value") {
    CHECK(io::crc32(bytes("123456789")) == 0xCBF43926u);
    CHECK(io::crc32(bytes("")) == 0u);
}

TEST_CASE("io: archive entries round-trip, stored and zstd") {
    const fs::path path = out_dir() / "roundtrip.zip";
    std::string big(100000, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<char>("abcabcabd"[i % 9]);
    }
    io::Bytes binary(256);
    for (std::size_t i = 0; i < binary.size(); ++i) {
        binary[i] = static_cast<std::uint8_t>(i);
    }
    write_archive(path, [&](io::ArchiveWriter& w) {
        CHECK(w.add("a.txt", std::string_view("hello"), io::Compression::Store));
        CHECK(w.add("big.txt", big, io::Compression::Zstd));
        CHECK(w.add("dir/binary.bin", binary, io::Compression::Zstd)); // incompressible: stored
        CHECK(w.add("empty", std::string_view(""), io::Compression::Zstd));
        CHECK_FALSE(w.add("a.txt", std::string_view("again"), io::Compression::Store)); // duplicate
    });

    io::ArchiveReader reader(path);
    REQUIRE(reader.ok());
    REQUIRE(reader.entries().size() == 4);
    CHECK(reader.find("big.txt")->compression == io::Compression::Zstd);
    CHECK(reader.find("big.txt")->stored_size < big.size() / 10);
    CHECK(reader.find("dir/binary.bin")->compression == io::Compression::Store);
    CHECK(*reader.read_text("a.txt") == "hello");
    CHECK(*reader.read_text("big.txt") == big);
    CHECK(*reader.read("dir/binary.bin") == binary);
    CHECK(reader.read_text("empty")->empty());
    CHECK_FALSE(reader.read("missing").has_value());
}

TEST_CASE("io: corruption is detected") {
    const fs::path path = out_dir() / "corrupt.zip";
    write_archive(path, [](io::ArchiveWriter& w) {
        CHECK(w.add("data.txt", std::string_view("0123456789"), io::Compression::Store));
    });
    {
        // Flip a payload byte (local header 30 bytes + 8-byte name).
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(30 + 8 + 3);
        f.put('X');
    }
    io::ArchiveReader reader(path);
    REQUIRE(reader.ok());
    CHECK_FALSE(reader.read("data.txt").has_value());
    CHECK(reader.error().find("checksum") != std::string::npos);

    std::ofstream(out_dir() / "not_a_zip.forge") << "plain text";
    CHECK_FALSE(io::ArchiveReader(out_dir() / "not_a_zip.forge").ok());
}

TEST_CASE("forge: save then load reproduces the scene, editor state and chunks") {
    SampleScene sample;
    const fs::path path = out_dir() / "roundtrip.forge";
    io::ForgeSaveOptions options;
    options.editor = {{"camera", {{"distance", 7.5}}}};
    options.chunks["terrain/tile_0_0.bin"] = io::Bytes(4096, 7);
    const io::ForgeResult saved = io::save_forge(path, sample.world, options);
    REQUIRE_MESSAGE(saved.ok, saved.error);
    CHECK_FALSE(fs::exists(path.string() + ".tmp"));

    scene::World loaded;
    const io::ForgeResult r = io::load_forge(path, loaded);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(r.loaded_version == io::kForgeFormatVersion);
    CHECK(scene::snapshot_world(loaded) == scene::snapshot_world(sample.world));
    CHECK(r.editor["camera"]["distance"] == 7.5);

    io::ForgeDocument document;
    REQUIRE(io::read_forge(path, document).ok);
    CHECK(document.chunks.at("terrain/tile_0_0.bin") == io::Bytes(4096, 7));
    CHECK(document.manifest["app_version"].is_string());
    CHECK(document.manifest["chunks"].size() == 3);

    // Saving the same scene twice gives the same bytes (fixed timestamps, sorted JSON).
    const fs::path again = out_dir() / "roundtrip_again.forge";
    REQUIRE(io::save_forge(again, sample.world, options).ok);
    CHECK(fs::file_size(again) == fs::file_size(path));
}

TEST_CASE("forge: a save that dies before the rename leaves the existing file intact") {
    SampleScene sample;
    const fs::path path = out_dir() / "atomic.forge";
    REQUIRE(io::save_forge(path, sample.world).ok);
    const json before = [&] {
        scene::World w;
        REQUIRE(io::load_forge(path, w).ok);
        return scene::snapshot_world(w);
    }();

    // Change the scene, then "crash" mid-save.
    REQUIRE(sample.stack.execute(sample.registry.create("entity.create", {{"name", "Unsaved"}})));
    io::ForgeSaveOptions options;
    options.fail_before_rename = true;
    const io::ForgeResult r = io::save_forge(path, sample.world, options);
    CHECK_FALSE(r.ok);
    CHECK(fs::exists(path.string() + ".tmp")); // the half-done save is left beside it

    scene::World w;
    REQUIRE(io::load_forge(path, w).ok);
    CHECK(scene::snapshot_world(w) == before);

    // The next successful save replaces the leftover temporary file.
    REQUIRE(io::save_forge(path, sample.world).ok);
    CHECK_FALSE(fs::exists(path.string() + ".tmp"));
    REQUIRE(io::load_forge(path, w).ok);
    CHECK(w.size() == sample.world.size());
}

TEST_CASE("forge: migrations run in order; newer files are refused") {
    // A synthetic version-0 file whose scene keeps entities under "entities".
    const fs::path path = out_dir() / "v0.forge";
    write_archive(path, [](io::ArchiveWriter& w) {
        CHECK(w.add("scene.json", std::string_view(R"({"entities":[]})"), io::Compression::Zstd));
        const json manifest = {{"format", io::kForgeFormatName},
                               {"format_version", 0},
                               {"chunks", {{{"name", "scene.json"}, {"size", 15}}}}};
        CHECK(w.add("manifest.json", manifest.dump(), io::Compression::Store));
    });

    scene::World world;
    io::ForgeResult r = io::load_forge(path, world); // the real chain has no v0 step
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("no migration from format version 0") != std::string::npos);

    io::MigrationChain chain;
    int calls = 0;
    chain.add(0, [&](io::ForgeDocument& doc, std::string&) {
        ++calls;
        doc.scene["roots"] = doc.scene["entities"];
        doc.scene.erase("entities");
        return true;
    });
    r = io::load_forge(path, world, chain);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(r.loaded_version == 0);
    CHECK(calls == 1);

    io::ForgeDocument doc;
    doc.version = 0;
    io::MigrationChain failing;
    failing.add(0, [](io::ForgeDocument&, std::string& error) {
        error = "boom";
        return false;
    });
    std::string error;
    CHECK_FALSE(failing.migrate(doc, 1, error));
    CHECK(error.find("boom") != std::string::npos);

    doc.version = 7;
    CHECK_FALSE(chain.migrate(doc, 1, error));
    CHECK(error.find("newer") != std::string::npos);
}

TEST_CASE("forge: the checked-in v1 fixture loads") {
    scene::World world;
    const io::ForgeResult r = io::load_forge(fs::path(F3D_TEST_DATA_DIR) / "forge" / "v1_basic.forge", world);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(r.loaded_version == 1);
    // tests/data/forge/README.md lists what the fixture contains.
    CHECK(world.size() == 7);
    CHECK(world.layers().size() == 3);
    CHECK(world.bookmarks().find("Overview") != nullptr);
    const entt::entity rock_b = world.find(*core::Guid::parse("00000000-0000-4000-8000-000000000003"));
    REQUIRE((rock_b != entt::null));
    CHECK(world.name(rock_b) == "Rock B");
    CHECK(world.transform(rock_b).translation == glm::vec3(2.0f, 0.0f, 1.0f));
    CHECK_FALSE(scene::is_pickable(world, rock_b)); // its parent's layer is locked
}

TEST_CASE("forge: autosave paths and recovery check") {
    CHECK(io::autosave_path("C:/p/scene.forge").filename() == "scene.autosave.forge");
    const fs::path project = out_dir() / "recover.forge";
    const fs::path autosave = io::autosave_path(project);
    fs::remove(project);
    fs::remove(autosave);
    CHECK_FALSE(io::autosave_is_newer(project, autosave));
    scene::World world;
    REQUIRE(io::save_forge(project, world).ok);
    REQUIRE(io::save_forge(autosave, world).ok);
    fs::last_write_time(autosave, fs::last_write_time(project) + std::chrono::seconds(5));
    CHECK(io::autosave_is_newer(project, autosave));
    fs::last_write_time(autosave, fs::last_write_time(project) - std::chrono::seconds(5));
    CHECK_FALSE(io::autosave_is_newer(project, autosave));
}

TEST_CASE("forge: a .forge archive mounts as a read-only VFS backend") {
    SampleScene sample;
    const fs::path path = out_dir() / "mounted.forge";
    io::ForgeSaveOptions options;
    options.chunks["terrain/tile_0_0.bin"] = io::Bytes(16, 1);
    options.chunks["terrain/tile_0_1.bin"] = io::Bytes(16, 2);
    REQUIRE(io::save_forge(path, sample.world, options).ok);

    auto backend = std::make_shared<io::ArchiveBackend>(path);
    REQUIRE(backend->ok());
    core::vfs::FileSystem files;
    files.mount("archive", backend);
    CHECK(files.exists("archive://scene.json"));
    CHECK(files.exists("archive://chunks/terrain"));
    CHECK(*files.read("archive://chunks/terrain/tile_0_1.bin") == io::Bytes(16, 2));
    const json scene = json::parse(*files.read_text("archive://scene.json"));
    CHECK(scene == scene::snapshot_world(sample.world));
    CHECK_FALSE(files.write_text("archive://new.txt", "read-only"));

    const auto top = files.list("archive://");
    REQUIRE(top.size() == 3); // chunks/, manifest.json, scene.json
    CHECK(top[0].path == "archive://chunks");
    CHECK(top[0].directory);
    const auto tiles = files.list("archive://chunks/terrain");
    REQUIRE(tiles.size() == 2);
    CHECK(tiles[1].path == "archive://chunks/terrain/tile_0_1.bin");
    CHECK(files.list("archive://", true).size() == 6); // 4 entries + 2 directories
}
