// SPDX-License-Identifier: Apache-2.0
// The editor's import queue (phases.md P3.2).
#include <doctest/doctest.h>

#include "f3d/assets/asset_database.hpp"
#include "f3d/assets/pipeline.hpp"
#include "f3d/core/jobs.hpp"
#include "f3d/core/vfs.hpp"
#include "f3d/editor/import_queue.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace f3d;
namespace fs = std::filesystem;
using State = editor::ImportQueue::State;

namespace {

struct Project {
    fs::path dir = fs::path(F3D_TEST_OUTPUT_DIR) / "import_queue";
    core::vfs::FileSystem files;
    assets::AssetDatabase db;
    core::jobs::JobSystem jobs{1, 2};
    editor::ImportQueue queue{jobs, files};

    Project() {
        fs::remove_all(dir);
        fs::create_directories(dir);
        const fs::path data = fs::path(F3D_TEST_DATA_DIR);
        fs::copy_file(data / "import" / "textures" / "checker.png", dir / "checker.png");
        fs::copy_file(data / "import" / "models" / "cube.obj", dir / "cube.obj");
        fs::copy_file(data / "import" / "models" / "cube.mtl", dir / "cube.mtl");
        std::ofstream(dir / "broken.png", std::ios::binary) << "not a png";
        files.mount("project", std::make_shared<core::vfs::DirectoryBackend>(dir));
        db.rescan(files, "project://");
    }

    void drain() {
        for (int i = 0; i < 1000 && queue.active() > 0; ++i) {
            jobs.wait_idle();
            jobs.run_continuations();
            queue.update();
        }
    }

    const editor::ImportQueue::Item& item(std::string_view path) {
        for (const auto& i : queue.items()) {
            if (i.path == path) {
                return i;
            }
        }
        FAIL("no item for ", path);
        return queue.items().front();
    }
};

} // namespace

TEST_CASE("import queue: imports stale assets in the background and reports failures") {
    Project p;
    std::vector<std::string> finished;
    p.queue.on_finished = [&](const editor::ImportQueue::Item& item) { finished.push_back(item.path); };

    CHECK(p.queue.enqueue_stale(p.db) == 3);
    CHECK(p.queue.enqueue_stale(p.db) == 0); // already queued
    CHECK(p.queue.active() == 3);
    p.drain();
    CHECK(p.queue.active() == 0);
    CHECK(finished.size() == 3);
    CHECK(p.item("project://checker.png").state == State::Done);
    CHECK(p.item("project://checker.png").progress == doctest::Approx(1.0f));
    CHECK(p.item("project://cube.obj").state == State::Done);
    const auto& broken = p.item("project://broken.png");
    CHECK(broken.state == State::Failed);
    CHECK(broken.error.find("broken.png") != std::string::npos);

    // After a rescan only the broken file is stale, and it isn't retried until it changes...
    p.db.rescan(p.files, "project://");
    CHECK(p.queue.enqueue_stale(p.db) == 0);
    // ...or the user forces it.
    CHECK(p.queue.enqueue(*p.db.find_path("project://broken.png"), true));
    p.drain();

    // Fixing the file makes it importable again.
    fs::copy_file(fs::path(F3D_TEST_DATA_DIR) / "import" / "textures" / "checker.png", p.dir / "broken.png",
                  fs::copy_options::overwrite_existing);
    p.db.rescan(p.files, "project://");
    CHECK(p.queue.enqueue_stale(p.db) == 1);
    p.drain();
    CHECK(p.queue.items().back().state == State::Done);
    CHECK(assets::load_cooked_texture(p.files, p.db.find_path("project://broken.png")->guid).has_value());

    p.queue.clear_finished();
    CHECK(p.queue.items().empty());
}

TEST_CASE("import queue: cancel_all drops pending imports") {
    Project p;
    // Block the background pool so the imports stay queued.
    std::atomic<bool> release{false};
    for (int i = 0; i < 2; ++i) {
        p.jobs.submit(core::jobs::Priority::Background, {}, [&](const core::jobs::CancellationToken&) {
            while (!release) {
                std::this_thread::yield();
            }
        });
    }
    int finished = 0;
    p.queue.on_finished = [&](const editor::ImportQueue::Item&) { ++finished; };
    CHECK(p.queue.enqueue_stale(p.db) == 3);
    p.queue.cancel_all();
    release = true;
    p.jobs.wait_idle();
    p.jobs.run_continuations();
    CHECK(finished == 0);
    for (const auto& item : p.queue.items()) {
        CHECK(item.state == State::Cancelled);
    }
    // Nothing was cooked.
    CHECK_FALSE(assets::load_cooked_texture(p.files, p.db.find_path("project://checker.png")->guid).has_value());
}
