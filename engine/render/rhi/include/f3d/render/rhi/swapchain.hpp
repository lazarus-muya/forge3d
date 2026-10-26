// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/render/rhi/resources.hpp"

#include <cstdint>
#include <vector>

namespace f3d::render::rhi {

class Device;

/// Presentable surface of one OS window. Create with Device::create_swapchain.
class Swapchain {
public:
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    /// Resizes the back buffers (pixels). Zero sizes are ignored (minimised window).
    void resize(std::uint32_t width, std::uint32_t height);
    void present(bool vsync = true);

    /// The back buffer to render into this frame.
    [[nodiscard]] Texture back_buffer() const;
    [[nodiscard]] std::uint32_t width() const;
    [[nodiscard]] std::uint32_t height() const;
    [[nodiscard]] Format format() const;

    [[nodiscard]] void* native() const noexcept { return swapchain_; }

private:
    friend class Device;
    Swapchain(Device& device, void* swapchain) noexcept : device_(device), swapchain_(swapchain) {}
    void forget_back_buffers();

    Device& device_;
    void* swapchain_ = nullptr;               // backend swapchain, one reference owned
    mutable std::vector<void*> back_buffers_; // textures registered with the device's RTV map
};

} // namespace f3d::render::rhi
