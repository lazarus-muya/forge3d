// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/core/image.hpp"
#include "f3d/render/rhi/resources.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

namespace f3d::render::rhi {

class Device;

/// One texture state change (CommandContext::transition).
struct TextureTransition {
    Texture texture;
    ResourceState state = ResourceState::ShaderResource;
};

/// Records and submits GPU work (wraps the immediate context). Resource state
/// transitions are handled automatically.
class CommandContext {
public:
    CommandContext(const CommandContext&) = delete;
    CommandContext& operator=(const CommandContext&) = delete;

    /// Binds colour targets (and optionally a depth target) and sets the
    /// viewport to the first target's full size.
    void set_render_targets(std::span<const Texture> colors, const Texture& depth = {});
    void set_render_target(const Texture& color, const Texture& depth = {});
    /// Render-target / depth-stencil views (one slice and mip of an array or cube).
    /// The viewport covers the first view.
    void set_render_targets(std::span<const TextureView> colors, const TextureView& depth = {});
    void set_render_target(const TextureView& color, const TextureView& depth = {});
    void set_viewport(float x, float y, float width, float height);
    void clear_color(const Texture& target, const std::array<float, 4>& rgba);
    /// Clears an integer-format target (e.g. R32_UINT) to a single value.
    void clear_color_uint(const Texture& target, std::uint32_t value);
    void clear_depth(const Texture& target, float depth = 1.0f);
    void clear_color(const TextureView& target, const std::array<float, 4>& rgba);
    void clear_depth(const TextureView& target, float depth = 1.0f);

    /// Writes a rectangle of mip 0 of a (Default-usage) texture: `width` x `height`
    /// texels at (x, y), rows `row_pitch` bytes apart; `slice`: the array slice.
    void update_texture(const Texture& texture, std::uint32_t x, std::uint32_t y, std::uint32_t width,
                        std::uint32_t height, const void* data, std::size_t row_pitch, std::uint32_t slice = 0);

    /// Fills mips 1.. of a texture created with TextureDesc::generate_mips from mip 0.
    /// Unbinds the render targets.
    void generate_mips(const Texture& texture);

    void set_pipeline(const Pipeline& pipeline);
    void commit(const Bindings& bindings);
    void set_vertex_buffer(const Buffer& buffer, std::uint64_t offset = 0);
    /// Binds `buffers` to vertex slots 0, 1, ... (offsets 0).
    void set_vertex_buffers(std::initializer_list<const Buffer*> buffers);
    void set_index_buffer(const Buffer& buffer, std::uint64_t offset = 0);

    void draw(std::uint32_t vertex_count, std::uint32_t instance_count = 1, std::uint32_t first_vertex = 0);
    void draw_indexed(std::uint32_t index_count, IndexType type, std::uint32_t instance_count = 1,
                      std::uint32_t first_index = 0, std::int32_t base_vertex = 0);
    void dispatch(std::uint32_t x, std::uint32_t y = 1, std::uint32_t z = 1);

    /// Moves textures into the given states (from whatever state they are in), with
    /// the barriers that needs. UnorderedAccess -> UnorderedAccess is a UAV barrier.
    /// Other calls transition automatically too; the render graph does it explicitly
    /// so every pass starts with its resources ready.
    void transition(std::span<const TextureTransition> transitions);
    /// The state the backend currently tracks for `texture`.
    [[nodiscard]] ResourceState state(const Texture& texture) const;

    /// Writes `size` bytes at `offset`. Dynamic buffers are rewritten whole
    /// (offset must be 0); default buffers are updated in place.
    void update_buffer(const Buffer& buffer, const void* data, std::size_t size, std::uint64_t offset = 0);

    /// Copies an 8-bit RGBA/BGRA texture (mip 0) to the CPU as RGBA8. Waits for
    /// the GPU. Empty image if the format isn't supported.
    [[nodiscard]] core::Image read_texture(const Texture& texture);

    /// Copies a single-channel 32-bit texture (R32_UINT / R32_FLOAT, mip 0) to the
    /// CPU as raw uint32 values, row-major. Waits for the GPU. Empty if unsupported.
    [[nodiscard]] std::vector<std::uint32_t> read_texture_uint32(const Texture& texture);

    /// Copies one mip of one slice of an RGBA16F / RGBA32F texture to the CPU as
    /// floats (4 per texel, row-major). Waits for the GPU. Empty if unsupported.
    [[nodiscard]] std::vector<float> read_texture_float(const Texture& texture, std::uint32_t mip = 0,
                                                        std::uint32_t slice = 0);

    /// Copies a buffer's contents to the CPU. Waits for the GPU. Tests and tools.
    [[nodiscard]] std::vector<std::uint8_t> read_buffer(const Buffer& buffer);

    /// GPU timer around a pass; results appear in Device::gpu_timings() a few frames later.
    void begin_timer(std::string_view name);
    void end_timer(std::string_view name);

    void flush();
    void wait_idle();

    /// The backend context (internal use by f3d_render_rhi).
    [[nodiscard]] void* native() const noexcept { return context_; }

private:
    friend class Device;
    CommandContext(Device& device, void* context) noexcept : device_(device), context_(context) {}

    Device& device_;
    void* context_ = nullptr; // backend immediate context, owned by Device
};

/// RAII GPU timer.
class ScopedGpuTimer {
public:
    ScopedGpuTimer(CommandContext& ctx, std::string_view name) : ctx_(ctx), name_(name) { ctx_.begin_timer(name_); }
    ~ScopedGpuTimer() { ctx_.end_timer(name_); }
    ScopedGpuTimer(const ScopedGpuTimer&) = delete;
    ScopedGpuTimer& operator=(const ScopedGpuTimer&) = delete;

private:
    CommandContext& ctx_;
    std::string_view name_;
};

} // namespace f3d::render::rhi
