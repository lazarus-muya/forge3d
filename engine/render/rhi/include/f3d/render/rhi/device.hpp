// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/render/rhi/command_context.hpp"
#include "f3d/render/rhi/resources.hpp"
#include "f3d/render/rhi/swapchain.hpp"
#include "f3d/render/rhi/types.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace f3d::render::rhi {

/// The GPU device: creates resources and owns the immediate CommandContext.
class Device {
public:
    struct CreateResult {
        std::unique_ptr<Device> device;
        std::string error; ///< why creation failed (all attempted backends)
    };

    /// Creates a device for desc.backend; falls back to the other backend if
    /// that fails and desc.allow_fallback is set.
    [[nodiscard]] static CreateResult create(const DeviceDesc& desc);

    /// Adapters the backend can use (empty if the backend isn't available).
    [[nodiscard]] static std::vector<AdapterInfo> enumerate_adapters(Backend backend);

    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] Backend backend() const noexcept;
    [[nodiscard]] const AdapterInfo& adapter() const noexcept;
    [[nodiscard]] const DeviceCaps& caps() const noexcept;
    [[nodiscard]] MemoryUsage memory_usage() const;

    [[nodiscard]] Buffer create_buffer(const BufferDesc& desc, const void* initial_data = nullptr);
    [[nodiscard]] Texture create_texture(const TextureDesc& desc);
    /// An immutable texture initialised from `data` (mip 0, rows `row_pitch` bytes apart).
    [[nodiscard]] Texture create_texture(const TextureDesc& desc, const void* data, std::size_t row_pitch);
    /// An immutable texture with every level given (`levels.size()` mips; for block
    /// formats `row_pitch` is the bytes of one row of 4x4 blocks).
    [[nodiscard]] Texture create_texture(const TextureDesc& desc, std::span<const TextureLevelData> levels);
    /// From precompiled bytecode (DXIL for D3D12, SPIR-V for Vulkan), entry point `main`.
    [[nodiscard]] Shader create_shader(ShaderStage stage, std::span<const std::byte> bytecode, std::string_view name);
    /// Compiles HLSL at run time (tests and tools; the editor uses precompiled shaders).
    [[nodiscard]] Shader create_shader_from_source(ShaderStage stage, std::string_view hlsl, std::string_view name,
                                                   std::string_view entry_point = "main");
    [[nodiscard]] Pipeline create_graphics_pipeline(const GraphicsPipelineDesc& desc);
    [[nodiscard]] Pipeline create_compute_pipeline(const ComputePipelineDesc& desc);
    [[nodiscard]] std::unique_ptr<Swapchain> create_swapchain(const NativeWindow& window, std::uint32_t width,
                                                              std::uint32_t height,
                                                              Format format = Format::RGBA8UnormSrgb);

    [[nodiscard]] CommandContext& context() noexcept;

    /// Latest resolved GPU timer results in milliseconds, by timer name.
    [[nodiscard]] std::vector<std::pair<std::string, double>> gpu_timings() const;

    /// Backend device (internal use by f3d_render_rhi).
    [[nodiscard]] void* native() const noexcept;

    struct Impl;
    [[nodiscard]] Impl& impl() noexcept { return *impl_; }

private:
    explicit Device(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
    std::unique_ptr<CommandContext> context_;
};

} // namespace f3d::render::rhi
