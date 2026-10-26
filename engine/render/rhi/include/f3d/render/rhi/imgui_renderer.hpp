// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/render/rhi/resources.hpp"
#include "f3d/render/rhi/types.hpp"

#include <cstdint>
#include <functional>
#include <memory>

struct ImGuiViewport;

namespace f3d::render::rhi {

class CommandContext;
class Device;

/// Dear ImGui renderer on the RHI. Creating it creates the ImGui context;
/// destroying it destroys the context (shut the platform backend down first).
///
/// Multi-viewport: every secondary ImGui viewport gets its own Swapchain. The
/// platform layer supplies the native window of a viewport through
/// `NativeWindowResolver` (the RHI knows nothing about SDL).
class ImGuiRenderer {
public:
    using NativeWindowResolver = std::function<NativeWindow(ImGuiViewport*)>;

    ImGuiRenderer(Device& device, Format back_buffer_format);
    ~ImGuiRenderer();
    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

    /// Installs the renderer side of ImGui multi-viewports (sets ImGuiBackendFlags_RendererHasViewports).
    void enable_viewports(NativeWindowResolver resolver);

    /// Starts an ImGui frame (calls ImGui::NewFrame). Call the platform backend's NewFrame first.
    void new_frame(std::uint32_t width, std::uint32_t height);
    /// Finishes the frame (ImGui::Render) and draws it into `target`.
    void render(CommandContext& ctx, const Texture& target);
    /// Updates and draws secondary viewport windows (no-op without viewports).
    void render_platform_windows();

    /// Value to pass as ImTextureID for ImGui::Image (the texture's shader-resource view).
    [[nodiscard]] static std::uint64_t texture_id(const Texture& texture);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace f3d::render::rhi
