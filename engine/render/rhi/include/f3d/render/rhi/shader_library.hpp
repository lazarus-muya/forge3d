// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/core/file_watcher.hpp"
#include "f3d/render/rhi/resources.hpp"
#include "f3d/render/rhi/types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace f3d::render::rhi {

class Device;

/// Shader naming: `shaders/<name>.<vs|ps|cs>.hlsl` is compiled at build time by
/// DXC into `<binary_dir>/<name>.<vs|ps|cs>.dxil` (D3D12) and `.spv` (Vulkan),
/// entry point `main`. Load shaders by `<name>.<stage>`, e.g. "fullscreen.vs".
struct ShaderLibraryDesc {
    std::filesystem::path binary_dir; ///< compiled shaders
    /// Hot reload (debug builds): sources are watched and recompiled with the
    /// same DXC and flags as the build. Leave empty to disable.
    std::filesystem::path source_dir;
    std::filesystem::path dxc;
};

/// Build-time configuration baked into f3d_render_rhi (cmake/F3DShaders.cmake).
[[nodiscard]] ShaderLibraryDesc default_shader_library_desc();

/// Stage from a shader name suffix: "fullscreen.vs" -> Vertex.
[[nodiscard]] std::optional<ShaderStage> stage_from_name(std::string_view name) noexcept;

class ShaderLibrary {
public:
    ShaderLibrary(Device& device, ShaderLibraryDesc desc);

    /// Loads "<name>.<stage>" for the device's backend. Empty Shader on failure (logged).
    [[nodiscard]] Shader load(std::string_view name);

    /// Number of successful reloads so far; pipelines rebuild when it changes.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    /// Hot reload: recompiles shaders whose sources (or any .hlsli include) changed.
    /// Returns the reloaded shader names. No-op when hot reload is disabled.
    std::vector<std::string> poll();

    [[nodiscard]] bool hot_reload_enabled() const noexcept { return !desc_.source_dir.empty() && !desc_.dxc.empty(); }

    /// Compiles one HLSL file with the build's DXC flags. Returns DXC's output on failure.
    [[nodiscard]] static std::optional<std::string> compile(const std::filesystem::path& dxc,
                                                            const std::filesystem::path& source,
                                                            const std::filesystem::path& output, ShaderStage stage,
                                                            Backend backend);

private:
    [[nodiscard]] std::filesystem::path binary_path(std::string_view name) const;
    void watch_sources();

    Device& device_;
    ShaderLibraryDesc desc_;
    core::FileWatcher watcher_;
    std::vector<std::string> loaded_;
    std::uint64_t generation_ = 0;
};

} // namespace f3d::render::rhi
