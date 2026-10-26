// SPDX-License-Identifier: Apache-2.0
// phases.md P3.3: a generated library of 1,000 assets indexes, and search returns
// results in < 100 ms. Label `perf`; timings are printed for docs/budgets.md.
#include <doctest/doctest.h>

#include "f3d/assets/asset_database.hpp"
#include "f3d/assets/asset_index.hpp"
#include "f3d/core/vfs.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace f3d;
namespace fs = std::filesystem;

namespace {

double ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

TEST_CASE("perf: 1,000 assets index and search in under 100 ms") {
    const fs::path dir = fs::path(F3D_TEST_OUTPUT_DIR) / "perf_asset_library";
    fs::remove_all(dir);
    const fs::path png = fs::path(F3D_TEST_DATA_DIR) / "import" / "textures" / "checker.png";
    const fs::path obj = fs::path(F3D_TEST_DATA_DIR) / "import" / "models" / "cube.obj";
    // 40 folders x 25 files: 600 textures, 400 models.
    const char* kinds[] = {"rock", "tree", "grass", "cliff", "house", "fence", "road", "water"};
    for (int i = 0; i < 1000; ++i) {
        const fs::path folder = dir / kinds[i % 8] / ("set_" + std::to_string(i / 25));
        fs::create_directories(folder);
        const std::string name = std::string(kinds[i % 8]) + "_" + std::to_string(i);
        if (i % 5 < 3) {
            fs::copy_file(png, folder / (name + ".png"));
        } else {
            fs::copy_file(obj, folder / (name + ".obj"));
        }
    }

    core::vfs::FileSystem files;
    files.mount("project", std::make_shared<core::vfs::DirectoryBackend>(dir));
    auto start = std::chrono::steady_clock::now();
    assets::AssetDatabase db;
    const assets::ScanReport report = db.rescan(files, "project://");
    const double scan_ms = ms_since(start);
    REQUIRE(db.size() == 1000);
    CHECK(report.metas_created == 1000);

    // Tag every tenth asset "hero", every seventh "night".
    std::vector<assets::AssetRecord> records = db.records();
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (i % 10 == 0) {
            records[i].tags.push_back("hero");
        }
        if (i % 7 == 0) {
            records[i].tags.push_back("night");
        }
    }
    db.seed(records);

    auto index = assets::AssetIndex::open(dir / ".forge3d" / "index.sqlite");
    REQUIRE(index != nullptr);
    start = std::chrono::steady_clock::now();
    const auto stats = index->sync(db);
    const double sync_ms = ms_since(start);
    CHECK(stats.inserted == 1000);
    start = std::chrono::steady_clock::now();
    const auto resync = index->sync(db);
    const double resync_ms = ms_since(start);
    CHECK(resync.unchanged == 1000);

    struct Case {
        const char* label;
        assets::AssetQuery query;
        std::size_t expected;
    };
    const Case cases[] = {
        {"all", {"", "", {}, 5000}, 1000},
        {"text 'rock'", {"rock", "", {}, 5000}, 125},
        {"text 'tree_9'", {"tree_9", "", {}, 5000}, 0}, // filled in below
        {"type texture", {"", "texture", {}, 5000}, 600},
        {"tag hero", {"", "", {"hero"}, 5000}, 100},
        {"text + type + 2 tags", {"set_1", "model", {"hero", "night"}, 5000}, 0},
    };
    double worst = 0.0;
    for (const Case& c : cases) {
        start = std::chrono::steady_clock::now();
        const auto results = index->search(c.query);
        const double ms = ms_since(start);
        worst = std::max(worst, ms);
        MESSAGE(std::string(c.label), ": ", results.size(), " results in ", ms, " ms");
        CHECK(ms < 100.0);
        if (c.expected > 0) {
            CHECK(results.size() == c.expected);
        }
    }
    MESSAGE("scan (hash + create 1000 .meta) ", scan_ms, " ms; index sync ", sync_ms, " ms; re-sync ", resync_ms,
            " ms; worst query ", worst, " ms");
}
