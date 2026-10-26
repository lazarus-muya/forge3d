// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/editor/preferences.hpp"
#include "f3d/render/rhi/device.hpp"
#include "f3d/render/rhi/imgui_renderer.hpp"

#include <cstdint>
#include <memory>
#include <string>

struct SDL_Window;
union SDL_Event;

namespace f3d::editor {

struct ImGuiLayerDesc {
    SDL_Window* window = nullptr;
    render::rhi::Format back_buffer_format = render::rhi::Format::RGBA8UnormSrgb;
    bool viewports = true; ///< multi-viewport (detachable windows)
    std::string ini_path;  ///< empty: don't load/save the layout
    float scale = 1.0f;    ///< UI scale (display content scale)
};

/// Dear ImGui context + SDL3 platform backend + RHI renderer.
class ImGuiLayer {
public:
    ImGuiLayer(render::rhi::Device& device, const ImGuiLayerDesc& desc);
    ~ImGuiLayer();
    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    void process_event(const SDL_Event& event);
    /// Re-applies style and font scaling (DPI change or UI-scale preference).
    void set_scale(float scale);
    void set_theme(Theme theme, const ThemeColors& colors);
    [[nodiscard]] float scale() const noexcept { return scale_; }

    void begin_frame(std::uint32_t width, std::uint32_t height);
    void render(render::rhi::CommandContext& ctx, const render::rhi::Texture& target);
    void render_platform_windows();

    /// True on the first frame when no saved layout was loaded (build the default one).
    [[nodiscard]] bool loaded_saved_layout() const noexcept { return loaded_saved_layout_; }

private:
    std::unique_ptr<render::rhi::ImGuiRenderer> renderer_;
    std::string ini_path_;
    float scale_ = 1.0f;
    Theme theme_ = Theme::Dark;
    ThemeColors colors_;
    bool loaded_saved_layout_ = false;
};

} // namespace f3d::editor
