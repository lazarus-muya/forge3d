// SPDX-License-Identifier: Apache-2.0
// VFS and the job system (phases.md P3.1).
#include <doctest/doctest.h>

#include "f3d/core/jobs.hpp"
#include "f3d/core/vfs.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace f3d::core;
namespace fs = std::filesystem;

namespace {

fs::path fresh_dir(const char* name) {
    const fs::path dir = fs::path(F3D_TEST_OUTPUT_DIR) / "vfs" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("vfs: paths are parsed, normalised and kept inside their mount") {
    auto p = vfs::parse("project://models/./rocks//big.glb");
    REQUIRE(p.has_value());
    CHECK(p->mount == "project");
    CHECK(p->relative == "models/rocks/big.glb");
    CHECK(vfs::parse("project://a\\b\\c.png")->relative == "a/b/c.png");
    CHECK(vfs::parse("project://a/../b.png")->relative == "b.png");
    CHECK(vfs::parse("project://")->relative.empty());
    CHECK_FALSE(vfs::parse("project://../escape.txt").has_value());
    CHECK_FALSE(vfs::parse("project://a/../../escape.txt").has_value());
    CHECK_FALSE(vfs::parse("project://C:/abs.txt").has_value());
    CHECK_FALSE(vfs::parse("no-scheme/file.txt").has_value());
    CHECK_FALSE(vfs::parse("://x").has_value());
    CHECK(vfs::join("engine", "shaders/a.hlsl") == "engine://shaders/a.hlsl");
}

TEST_CASE("vfs: directory mounts read, write, list and map native paths") {
    const fs::path project = fresh_dir("project");
    const fs::path engine = fresh_dir("engine");
    std::ofstream(engine / "readme.txt") << "engine data";

    vfs::FileSystem files;
    files.mount("project", std::make_shared<vfs::DirectoryBackend>(project));
    files.mount("engine", std::make_shared<vfs::DirectoryBackend>(engine, false));
    CHECK(files.mounted("project"));

    CHECK(files.write_text("project://textures/grass.txt", "green"));
    CHECK(fs::exists(project / "textures" / "grass.txt"));
    CHECK(*files.read_text("project://textures/grass.txt") == "green");
    CHECK(files.exists("project://textures"));
    CHECK(*files.read_text("engine://readme.txt") == "engine data");
    CHECK_FALSE(files.write_text("engine://new.txt", "read-only mount"));
    CHECK_FALSE(files.read("unmounted://x").has_value());
    CHECK_FALSE(files.read("project://../outside.txt").has_value());

    files.write_text("project://a.txt", "a");
    const auto top = files.list("project://");
    REQUIRE(top.size() == 2);
    CHECK(top[0].path == "project://a.txt");
    CHECK(top[1].path == "project://textures");
    CHECK(top[1].directory);
    const auto all = files.list("project://", true);
    CHECK(all.size() == 3);
    CHECK(all[0].modified != 0);

    CHECK(files.native_path("project://a.txt") == project / "a.txt");
    CHECK(files.to_virtual(project / "textures" / "grass.txt") == "project://textures/grass.txt");
    CHECK_FALSE(files.to_virtual(fs::temp_directory_path() / "elsewhere.txt").has_value());

    CHECK(files.remove("project://a.txt"));
    CHECK_FALSE(files.exists("project://a.txt"));
    files.unmount("project");
    CHECK_FALSE(files.exists("project://textures/grass.txt"));
}

TEST_CASE("jobs: work runs on workers, continuations on the calling thread") {
    jobs::JobSystem jobs(2, 1);
    const auto main_thread = std::this_thread::get_id();
    std::atomic<bool> worker_was_other_thread{false};
    int result = 0;
    bool continuation_on_main = false;
    jobs.submit(
        jobs::Priority::Interactive, {},
        [&](const jobs::CancellationToken&) {
            worker_was_other_thread = std::this_thread::get_id() != main_thread;
            return 21 * 2;
        },
        [&](int value) {
            result = value;
            continuation_on_main = std::this_thread::get_id() == main_thread;
        });
    jobs.wait_idle();
    CHECK(result == 0); // not before run_continuations
    CHECK(jobs.run_continuations() == 1);
    CHECK(result == 42);
    CHECK(continuation_on_main);
    CHECK(worker_was_other_thread);
}

TEST_CASE("jobs: a cancelled job never runs its continuation") {
    jobs::JobSystem jobs(1, 1);
    std::atomic<int> started{0};
    bool continued = false;

    SUBCASE("cancelled before it starts: the work doesn't run either") {
        // Block the only background worker so the second job waits in the queue.
        std::atomic<bool> release{false};
        jobs.submit(jobs::Priority::Background, {}, [&](const jobs::CancellationToken&) {
            while (!release) {
                std::this_thread::yield();
            }
        });
        jobs::CancellationToken token;
        jobs.submit(
            jobs::Priority::Background, token,
            [&](const jobs::CancellationToken&) {
                ++started;
                return 1;
            },
            [&](int) { continued = true; });
        token.cancel();
        release = true;
        jobs.wait_idle();
        jobs.run_continuations();
        CHECK(started == 0);
        CHECK_FALSE(continued);
    }
    SUBCASE("cancelled while running: the work can stop early, the continuation is dropped") {
        jobs::CancellationToken token;
        std::atomic<bool> stopped_early{false};
        jobs.submit(
            jobs::Priority::Background, token,
            [&](const jobs::CancellationToken& t) {
                ++started;
                while (!t.cancelled()) {
                    std::this_thread::yield();
                }
                stopped_early = true;
                return 1;
            },
            [&](int) { continued = true; });
        while (started == 0) {
            std::this_thread::yield();
        }
        token.cancel();
        jobs.wait_idle();
        CHECK(jobs.run_continuations() == 0);
        CHECK(stopped_early);
        CHECK_FALSE(continued);
    }
    SUBCASE("cancelled after finishing but before the main thread ran the continuation") {
        jobs::CancellationToken token;
        jobs.submit(
            jobs::Priority::Interactive, token, [](const jobs::CancellationToken&) { return 1; },
            [&](int) { continued = true; });
        jobs.wait_idle();
        token.cancel();
        CHECK(jobs.run_continuations() == 0);
        CHECK_FALSE(continued);
    }
}

TEST_CASE("jobs: interactive work doesn't wait behind background work") {
    jobs::JobSystem jobs(1, 1);
    std::atomic<bool> release{false};
    jobs.submit(jobs::Priority::Background, {}, [&](const jobs::CancellationToken&) {
        while (!release) {
            std::this_thread::yield();
        }
    });
    std::atomic<bool> interactive_done{false};
    jobs.submit(jobs::Priority::Interactive, {}, [&](const jobs::CancellationToken&) { interactive_done = true; });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!interactive_done && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    CHECK(interactive_done); // while the background worker is still busy
    release = true;
    jobs.wait_idle();
    CHECK(jobs.pending() == 0);
}

TEST_CASE("jobs: many jobs, one continuation each") {
    jobs::JobSystem jobs;
    int sum = 0;
    for (int i = 1; i <= 200; ++i) {
        jobs.submit(
            i % 2 ? jobs::Priority::Interactive : jobs::Priority::Background, {},
            [i](const jobs::CancellationToken&) { return i; }, [&](int v) { sum += v; });
    }
    jobs.wait_idle();
    CHECK(jobs.run_continuations() == 200);
    CHECK(sum == 200 * 201 / 2);
}
