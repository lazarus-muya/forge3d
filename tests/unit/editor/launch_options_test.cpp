// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/editor/launch_options.hpp"

#include <string_view>
#include <vector>

using f3d::editor::parse_launch_options;

namespace {
f3d::editor::LaunchParseResult parse(std::vector<std::string_view> args) {
    return parse_launch_options(args);
}
} // namespace

TEST_CASE("launch options: defaults") {
    const auto r = parse({});
    REQUIRE(r.ok());
    CHECK(r.options.frames == -1);
    CHECK_FALSE(r.options.hidden);
    CHECK_FALSE(r.options.help);
}

TEST_CASE("launch options: --frames and --hidden") {
    auto r = parse({"--frames", "60", "--hidden"});
    REQUIRE(r.ok());
    CHECK(r.options.frames == 60);
    CHECK(r.options.hidden);

    r = parse({"--frames=120"});
    REQUIRE(r.ok());
    CHECK(r.options.frames == 120);
}

TEST_CASE("launch options: errors") {
    CHECK_FALSE(parse({"--frames"}).ok());
    CHECK_FALSE(parse({"--frames", "abc"}).ok());
    CHECK_FALSE(parse({"--frames", "-3"}).ok());
    CHECK_FALSE(parse({"--frames=10x"}).ok());
    CHECK_FALSE(parse({"--hidden=yes"}).ok());
    const auto r = parse({"--bogus"});
    CHECK_FALSE(r.ok());
    CHECK(r.error == "unknown argument: --bogus");
}

TEST_CASE("launch options: help") {
    CHECK(parse({"-h"}).options.help);
    CHECK(parse({"--help"}).options.help);
}

TEST_CASE("launch options: rendering flags") {
    auto r = parse({"--rhi=vulkan", "--adapter", "integrated", "--no-vsync", "--validation", "--capture", "out.png",
                    "--bench=b.json"});
    REQUIRE(r.ok());
    CHECK(r.options.rhi == f3d::render::rhi::Backend::Vulkan);
    CHECK(r.options.adapter_preference == f3d::render::rhi::AdapterPreference::LowPower);
    CHECK_FALSE(r.options.adapter_index.has_value());
    CHECK_FALSE(r.options.vsync);
    CHECK(r.options.validation);
    CHECK(r.options.capture == "out.png");
    CHECK(r.options.bench == "b.json");

    r = parse({"--adapter=1", "--rhi", "d3d12"});
    REQUIRE(r.ok());
    CHECK(r.options.adapter_index == 1u);
    CHECK(r.options.rhi == f3d::render::rhi::Backend::D3D12);

    CHECK_FALSE(parse({"--rhi=metal"}).ok());
    CHECK_FALSE(parse({"--adapter=fast"}).ok());
    CHECK_FALSE(parse({"--capture"}).ok());
    CHECK_FALSE(parse({"--no-vsync=1"}).ok());
}

TEST_CASE("launch options: --vram-cap") {
    CHECK_FALSE(parse({}).options.vram_cap_mb.has_value());
    auto r = parse({"--vram-cap", "512"});
    REQUIRE(r.ok());
    CHECK(r.options.vram_cap_mb == 512u);
    CHECK_FALSE(parse({"--vram-cap"}).ok());
    CHECK_FALSE(parse({"--vram-cap=0"}).ok());
    CHECK_FALSE(parse({"--vram-cap=lots"}).ok());
}

TEST_CASE("launch options: --viewport and a software adapter") {
    CHECK_FALSE(parse({}).options.viewport_size.has_value());
    auto r = parse({"--viewport", "1920x1080", "--adapter=software"});
    REQUIRE(r.ok());
    REQUIRE(r.options.viewport_size.has_value());
    CHECK(r.options.viewport_size->first == 1920u);
    CHECK(r.options.viewport_size->second == 1080u);
    CHECK(r.options.adapter_preference == f3d::render::rhi::AdapterPreference::Software);
    CHECK_FALSE(r.options.adapter_index.has_value());
    CHECK_FALSE(parse({"--viewport=1920"}).ok());
    CHECK_FALSE(parse({"--viewport=0x1080"}).ok());
    CHECK_FALSE(parse({"--viewport=widex1080"}).ok());
}

TEST_CASE("launch options: --rpc") {
    CHECK_FALSE(parse({}).options.rpc_port.has_value()); // off by default
    auto r = parse({"--rpc"});
    REQUIRE(r.ok());
    CHECK(r.options.rpc_port == 0xFFFF); // the preference's port
    r = parse({"--rpc=0", "--rpc-token-file", "t.json"});
    REQUIRE(r.ok());
    CHECK(r.options.rpc_port == 0);
    CHECK(r.options.rpc_token_file == "t.json");
    CHECK(parse({"--rpc=9000"}).options.rpc_port == 9000);
    CHECK_FALSE(parse({"--rpc=port"}).ok());
    CHECK_FALSE(parse({"--rpc-token-file"}).ok());
}
