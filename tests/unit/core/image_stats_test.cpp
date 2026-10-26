// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/core/file_watcher.hpp"
#include "f3d/core/image.hpp"
#include "f3d/core/stats.hpp"
#include "f3d/core/system.hpp"

#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace {
fs::path temp_dir(const char* name) {
    const auto dir = fs::temp_directory_path() / "forge3d-tests" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}
} // namespace

TEST_CASE("image: PNG round-trip and pixel access") {
    f3d::core::Image img;
    img.width = 3;
    img.height = 2;
    img.rgba.resize(3 * 2 * 4);
    for (std::size_t i = 0; i < img.rgba.size(); ++i) {
        img.rgba[i] = static_cast<std::uint8_t>(i * 10);
    }
    const auto path = temp_dir("image") / "roundtrip.png";
    REQUIRE(f3d::core::write_png(path, img));
    const auto loaded = f3d::core::read_png(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->width == 3);
    CHECK(loaded->height == 2);
    CHECK(loaded->rgba == img.rgba);
    CHECK(loaded->pixel(2, 1) == std::array<std::uint8_t, 4>{200, 210, 220, 230});
    CHECK(loaded->pixel(9, 9) == std::array<std::uint8_t, 4>{0, 0, 0, 0}); // out of range
    CHECK_FALSE(f3d::core::write_png(path, f3d::core::Image{}));
    CHECK_FALSE(f3d::core::read_png(path.parent_path() / "missing.png").has_value());
}

TEST_CASE("image: sRGB encoding") {
    CHECK(f3d::core::linear_to_srgb(0.0f) == doctest::Approx(0.0f));
    CHECK(f3d::core::linear_to_srgb(1.0f) == doctest::Approx(1.0f));
    CHECK(f3d::core::linear_to_srgb(0.5f) == doctest::Approx(0.7354f).epsilon(0.001));
    CHECK(f3d::core::linear_to_srgb(0.002f) == doctest::Approx(0.02584f).epsilon(0.001)); // linear segment
}

TEST_CASE("stats: summary and nearest-rank percentiles") {
    std::vector<double> samples;
    for (int i = 1; i <= 100; ++i) {
        samples.push_back(static_cast<double>(i));
    }
    const auto s = f3d::core::summarize(samples);
    CHECK(s.count == 100);
    CHECK(s.average == doctest::Approx(50.5));
    CHECK(s.min == 1.0);
    CHECK(s.max == 100.0);
    CHECK(s.p50 == 50.0);
    CHECK(s.p95 == 95.0);
    CHECK(s.p99 == 99.0);
    CHECK(f3d::core::percentile(std::vector<double>{3.0, 1.0, 2.0}, 100.0) == 3.0);
    CHECK(f3d::core::summarize({}).count == 0);
}

TEST_CASE("file watcher: reports modification, creation and deletion once") {
    const auto dir = temp_dir("watcher");
    const auto a = dir / "a.txt";
    const auto b = dir / "b.txt";
    std::ofstream(a) << "one";

    f3d::core::FileWatcher watcher;
    watcher.watch(a);
    watcher.watch(b); // does not exist yet
    watcher.watch(a); // duplicate is ignored
    CHECK(watcher.size() == 2);
    CHECK(watcher.poll().empty());

    std::ofstream(a) << "two, longer";
    auto changed = watcher.poll();
    REQUIRE(changed.size() == 1);
    CHECK(changed[0] == a);
    CHECK(watcher.poll().empty());

    std::ofstream(b) << "new";
    changed = watcher.poll();
    REQUIRE(changed.size() == 1);
    CHECK(changed[0] == b);

    fs::remove(a);
    changed = watcher.poll();
    REQUIRE(changed.size() == 1);
    CHECK(changed[0] == a);

    watcher.unwatch(b);
    CHECK(watcher.size() == 1);
}

TEST_CASE("system: process memory and executable dir") {
    const auto mem = f3d::core::process_memory();
    CHECK(mem.working_set > 0);
    CHECK(mem.peak_working_set >= mem.working_set);
    CHECK(fs::is_directory(f3d::core::executable_dir()));
}
