// SPDX-License-Identifier: Apache-2.0
// Shader hot reload: editing a watched HLSL source recompiles it with the build's
// DXC flags and bumps the library generation; a broken edit keeps the old binary.
#include <doctest/doctest.h>

#include "f3d/core/log.hpp"
#include "f3d/render/rhi/device.hpp"
#include "f3d/render/rhi/shader_library.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
namespace rhi = f3d::render::rhi;

namespace {

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("rhi: shader hot reload recompiles edited sources") {
    const auto defaults = rhi::default_shader_library_desc();
    if (defaults.dxc.empty()) {
        MESSAGE("hot reload is only built into Debug configurations; skipping");
        return;
    }
    auto created = rhi::Device::create(rhi::DeviceDesc{});
    if (!created.device) {
        MESSAGE("no GPU device: ", created.error);
        return;
    }
    rhi::Device& device = *created.device;

    // Private copies of the sources and binaries, so the real build tree is untouched.
    const auto root = fs::temp_directory_path() / "forge3d-tests" / "hot-reload";
    fs::remove_all(root);
    fs::create_directories(root / "src");
    fs::create_directories(root / "bin");
    fs::copy_file(defaults.source_dir / "test_triangle.ps.hlsl", root / "src" / "test_triangle.ps.hlsl");
    fs::copy_file(defaults.source_dir / "common.hlsli", root / "src" / "common.hlsli");
    const char* ext = device.backend() == rhi::Backend::D3D12 ? ".dxil" : ".spv";
    const auto binary = root / "bin" / (std::string("test_triangle.ps") + ext);
    fs::copy_file(defaults.binary_dir / (std::string("test_triangle.ps") + ext), binary);

    rhi::ShaderLibrary library(device, {root / "bin", root / "src", defaults.dxc});
    REQUIRE(library.hot_reload_enabled());
    REQUIRE(library.load("test_triangle.ps"));
    CHECK(library.poll().empty());
    CHECK(library.generation() == 0);

    // A valid edit: recompiled, binary replaced, generation bumped, still loadable.
    const std::string original_binary = read(binary);
    {
        std::ofstream src(root / "src" / "test_triangle.ps.hlsl", std::ios::trunc);
        src << "struct PSInput { float4 Pos : SV_Position; float3 Color : COLOR0; };\n"
               "float4 main(PSInput input) : SV_Target { return float4(input.Color.bgr, 0.5); }\n";
    }
    const auto reloaded = library.poll();
    REQUIRE(reloaded.size() == 1);
    CHECK(reloaded[0] == "test_triangle.ps");
    CHECK(library.generation() == 1);
    CHECK(read(binary) != original_binary);
    CHECK(library.load("test_triangle.ps"));

    // A broken edit: logged, binary kept, generation unchanged.
    const std::string good_binary = read(binary);
    f3d::core::log::set_console_enabled(false);
    {
        std::ofstream src(root / "src" / "test_triangle.ps.hlsl", std::ios::trunc);
        src << "float4 main() : SV_Target { this is not hlsl }\n";
    }
    CHECK(library.poll().empty());
    f3d::core::log::set_console_enabled(true);
    CHECK(library.generation() == 1);
    CHECK(read(binary) == good_binary);
}
