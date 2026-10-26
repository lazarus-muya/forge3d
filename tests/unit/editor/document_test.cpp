// SPDX-License-Identifier: Apache-2.0
// editor::Document: dirty tracking, save/open, autosave and recovery (P2.6).
#include <doctest/doctest.h>

#include "f3d/editor/document.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/snapshot.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>

using namespace f3d;
namespace fs = std::filesystem;

namespace {

fs::path out_dir() {
    const fs::path dir = fs::path(F3D_TEST_OUTPUT_DIR) / "document";
    fs::create_directories(dir);
    return dir;
}

struct Fixture {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    editor::Document doc{world, stack};

    Fixture() { ops::register_builtin_ops(registry); }

    void create(const char* name, bool merge = false) {
        REQUIRE(stack.execute(registry.create("entity.create", {{"name", name}}), merge));
    }
};

} // namespace

TEST_CASE("document: dirty tracking follows the history, including undo back to the save") {
    Fixture f;
    CHECK(f.doc.untitled());
    CHECK_FALSE(f.doc.dirty());
    CHECK(f.doc.title() == "Untitled");

    f.create("A");
    CHECK(f.doc.dirty());
    CHECK(f.doc.title() == "Untitled*");

    const fs::path path = out_dir() / "dirty.forge";
    REQUIRE(f.doc.save(path).ok);
    CHECK_FALSE(f.doc.dirty());
    CHECK(f.doc.title() == "dirty.forge");

    f.create("B");
    CHECK(f.doc.dirty());
    f.stack.undo();
    CHECK_FALSE(f.doc.dirty()); // back at the saved state
    f.stack.undo();
    CHECK(f.doc.dirty()); // before the saved state
    f.stack.redo();
    CHECK_FALSE(f.doc.dirty());

    // A merge into the saved entry is a change too.
    const core::Guid a = f.world.guid(f.world.roots()[0]);
    REQUIRE(f.stack.execute(f.registry.create("entity.set_property",
                                              {{"entity", a.to_string()}, {"path", "Name.value"}, {"value", "A1"}})));
    REQUIRE(f.doc.save(path).ok);
    REQUIRE(f.stack.execute(
        f.registry.create("entity.set_property", {{"entity", a.to_string()}, {"path", "Name.value"}, {"value", "A2"}}),
        true));
    CHECK(f.doc.dirty());

    // Transient operations change nothing.
    REQUIRE(f.doc.save(path).ok);
    REQUIRE(f.stack.execute(f.registry.create("bookmark.save", {{"name", "V"}})));
    REQUIRE(f.doc.save(path).ok);
    REQUIRE(f.stack.execute(f.registry.create("bookmark.recall", {{"name", "V"}})));
    CHECK_FALSE(f.doc.dirty());
}

TEST_CASE("document: open restores the scene and the editor camera; reset empties it") {
    Fixture f;
    f.create("A");
    scene::CameraBookmark camera;
    camera.target = {1, 2, 3};
    camera.distance = 42.0f;
    const fs::path path = out_dir() / "open.forge";
    REQUIRE(f.doc.save(path, camera).ok);
    const nlohmann::json saved = scene::snapshot_world(f.world);

    f.doc.reset();
    CHECK(f.world.size() == 0);
    CHECK(f.doc.untitled());
    CHECK_FALSE(f.doc.dirty());
    CHECK_FALSE(f.stack.can_undo());

    std::optional<scene::CameraBookmark> loaded_camera;
    REQUIRE(f.doc.open(path, &loaded_camera).ok);
    CHECK(scene::snapshot_world(f.world) == saved);
    CHECK(f.doc.path() == path);
    CHECK_FALSE(f.doc.dirty());
    REQUIRE(loaded_camera.has_value());
    CHECK(loaded_camera->distance == doctest::Approx(42.0f));
    CHECK(loaded_camera->target == glm::vec3(1, 2, 3));

    CHECK_FALSE(f.doc.open(out_dir() / "missing.forge").ok);
    CHECK(f.world.size() == 0);
    CHECK(f.doc.untitled());
}

TEST_CASE("document: autosave timing, recovery and cleanup") {
    Fixture f;
    const fs::path path = out_dir() / "autosave.forge";
    const fs::path autosave = io::autosave_path(path);
    fs::remove(autosave);
    REQUIRE(f.doc.save(path).ok);
    f.doc.restart_autosave_clock(100.0);

    CHECK_FALSE(f.doc.autosave_due(1000.0, 5)); // clean: nothing to autosave
    f.create("Unsaved");
    CHECK_FALSE(f.doc.autosave_due(100.0 + 299.0, 5)); // interval not reached
    CHECK(f.doc.autosave_due(100.0 + 300.0, 5));
    CHECK_FALSE(f.doc.autosave_due(100.0 + 300.0, 0)); // off
    REQUIRE(f.doc.autosave(400.0).ok);
    CHECK(fs::exists(autosave));
    CHECK(f.doc.dirty());                        // an autosave is not a save
    CHECK_FALSE(f.doc.autosave_due(10000.0, 5)); // nothing new since the autosave
    f.create("More");
    CHECK_FALSE(f.doc.autosave_due(400.0 + 100.0, 5));
    CHECK(f.doc.autosave_due(400.0 + 300.0, 5));
    REQUIRE(f.doc.autosave(700.0).ok);
    const nlohmann::json latest = scene::snapshot_world(f.world);

    // "Crash": a fresh document recovers the autosave in place of the project.
    fs::last_write_time(autosave, fs::last_write_time(path) + std::chrono::seconds(10));
    CHECK(io::autosave_is_newer(path, autosave));
    Fixture g;
    REQUIRE(g.doc.recover(path, autosave).ok);
    CHECK(scene::snapshot_world(g.world) == latest);
    CHECK(g.doc.path() == path);
    CHECK(g.doc.dirty()); // must be saved deliberately
    REQUIRE(g.doc.save(path).ok);
    CHECK_FALSE(fs::exists(autosave)); // a real save removes the stale autosave

    // Untitled scenes autosave into the configured directory.
    Fixture u;
    CHECK(u.doc.autosave_target().empty());
    u.doc.set_untitled_autosave_dir(out_dir() / "untitled");
    CHECK(u.doc.autosave_target().filename() == "untitled.autosave.forge");
    u.create("X");
    REQUIRE(u.doc.autosave(1.0).ok);
    CHECK(fs::exists(u.doc.autosave_target()));
}

TEST_CASE("document: a new scene from the startup template is untitled, clean and leaves the template alone") {
    Fixture f;
    f.create("Sky");
    f.create("Sun");
    const fs::path startup = out_dir() / "startup.forge";
    scene::CameraBookmark view;
    view.distance = 42.0f;
    REQUIRE(f.doc.save(startup, view).ok);
    const auto written = fs::last_write_time(startup);

    Fixture g;
    std::optional<scene::CameraBookmark> camera;
    REQUIRE(g.doc.start_from(startup, &camera).ok);
    CHECK(g.world.size() == 2);
    CHECK(g.doc.untitled());
    CHECK_FALSE(g.doc.dirty());
    CHECK_FALSE(g.stack.can_undo());
    REQUIRE(camera);
    CHECK(camera->distance == 42.0f);
    g.create("Tree");
    CHECK(g.doc.dirty());
    CHECK(fs::last_write_time(startup) == written);

    // A missing template: an empty untitled scene.
    Fixture h;
    h.create("Old");
    CHECK_FALSE(h.doc.start_from(out_dir() / "missing.forge").ok);
    CHECK(h.world.size() == 0);
    CHECK(h.doc.untitled());
    CHECK_FALSE(h.doc.dirty());
}
