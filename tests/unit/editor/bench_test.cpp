// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/editor/bench.hpp"

#include <string>

using f3d::editor::BenchReport;
using f3d::editor::FrameStats;

TEST_CASE("bench: frame stats skip warm-up frames and average GPU passes") {
    FrameStats stats(2);
    stats.add_frame(100.0, {{"frame", 50.0}}); // warm-up
    stats.add_frame(100.0, {});                // warm-up
    stats.add_frame(10.0, {{"frame", 4.0}});
    stats.add_frame(20.0, {{"frame", 6.0}, {"ui", 1.0}});
    CHECK(stats.frames() == 4);
    const auto cpu = stats.cpu_summary();
    CHECK(cpu.count == 2);
    CHECK(cpu.average == doctest::Approx(15.0));
    const auto gpu = stats.gpu_averages();
    REQUIRE(gpu.size() == 2);
    CHECK(gpu[0].first == "frame");
    CHECK(gpu[0].second == doctest::Approx(5.0));
    CHECK(gpu[1].first == "ui");
}

TEST_CASE("bench: JSON report contains every field") {
    BenchReport r;
    r.backend = "d3d12";
    r.adapter = R"(GPU "X")";
    r.width = 1920;
    r.height = 1080;
    r.frames = 120;
    r.cpu_frame_ms.average = 1.5;
    r.cpu_frame_ms.count = 110;
    r.gpu_pass_ms = {{"frame", 0.25}};
    r.ram_bytes = 100 * 1024 * 1024;
    const std::string json = f3d::editor::to_json(r);
    CHECK(json.find(R"("backend": "d3d12")") != std::string::npos);
    CHECK(json.find(R"("adapter": "GPU \"X\"")") != std::string::npos); // escaped quotes
    CHECK(json.find(R"("resolution": [1920, 1080])") != std::string::npos);
    CHECK(json.find(R"("avg": 1.5000)") != std::string::npos);
    CHECK(json.find(R"("frame": 0.2500)") != std::string::npos);
    CHECK(json.find(R"("working_set": 100.0)") != std::string::npos);
    CHECK(json.find(R"("vram_mb")") != std::string::npos);
    CHECK(json.find(R"("residency")") == std::string::npos); // optional

    r.residency = BenchReport::Residency{512ull << 20, 256ull << 20, 300ull << 20, 3, 200};
    const std::string streamed = f3d::editor::to_json(r);
    CHECK(
        streamed.find(
            R"("residency": { "budget_mb": 512.0, "used_mb": 256.0, "peak_mb": 300.0, "meshes": 3, "textures": 200 })") !=
        std::string::npos);
}
