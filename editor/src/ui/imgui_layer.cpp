// SPDX-License-Identifier: Apache-2.0
#include "imgui_layer.hpp"

#include "../platform.hpp"
#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace f3d::editor {
namespace {

ImVec4 to_imvec(const Rgba& c) {
    return {c.r, c.g, c.b, c.a};
}

/// Recolours the built-in style's blue accent family (buttons, headers, sliders, tabs,
/// frames...) with `accent`: each such colour keeps its brightness and saturation
/// relative to the stock accent, so the derived shades stay consistent.
void apply_accent(ImGuiStyle& style, const Rgba& accent) {
    constexpr float kStockHue = 0.594f; // ImGui's (0.26, 0.59, 0.98)
    constexpr float kStockSat = 0.735f;
    constexpr float kStockVal = 0.98f;
    float ah = 0.0f;
    float as = 0.0f;
    float av = 0.0f;
    ImGui::ColorConvertRGBtoHSV(accent.r, accent.g, accent.b, ah, as, av);
    for (ImVec4& color : style.Colors) {
        float h = 0.0f;
        float s = 0.0f;
        float v = 0.0f;
        ImGui::ColorConvertRGBtoHSV(color.x, color.y, color.z, h, s, v);
        if (s < 0.2f || std::abs(h - kStockHue) > 0.06f) {
            continue; // greys, and the few non-accent colours (plots, drop targets)
        }
        s = std::clamp(s * as / kStockSat, 0.0f, 1.0f);
        v = std::clamp(v * av / kStockVal, 0.0f, 1.0f);
        ImGui::ColorConvertHSVtoRGB(ah, s, v, color.x, color.y, color.z);
    }
}

void apply_style(float scale, Theme theme, const ThemeColors& colors) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    if (theme == Theme::Light) {
        ImGui::StyleColorsLight(&style);
    } else {
        ImGui::StyleColorsDark(&style);
    }
    apply_accent(style, colors.accent);
    style.Colors[ImGuiCol_WindowBg] = to_imvec(colors.window_bg);
    style.Colors[ImGuiCol_Text] = to_imvec(colors.text);
    const Rgba& bg = colors.window_bg;
    const Rgba& fg = colors.text;
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4((fg.r + bg.r) * 0.5f, (fg.g + bg.g) * 0.5f, (fg.b + bg.b) * 0.5f, fg.a);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(bg.r, bg.g, bg.b, std::max(bg.a, 0.94f));
    style.WindowRounding = 2.0f;
    style.FrameRounding = 2.0f;
    style.TabRounding = 2.0f;
    style.ScrollbarRounding = 2.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    // Detached platform windows look like normal OS windows.
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }
}

} // namespace

ImGuiLayer::ImGuiLayer(render::rhi::Device& device, const ImGuiLayerDesc& desc)
    : renderer_(std::make_unique<render::rhi::ImGuiRenderer>(device, desc.back_buffer_format)),
      ini_path_(desc.ini_path) {
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true; // dragging in the viewport navigates, it doesn't move the window
    if (desc.viewports) {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    }
    io.IniFilename = ini_path_.empty() ? nullptr : ini_path_.c_str();
    std::error_code ec;
    loaded_saved_layout_ = !ini_path_.empty() && std::filesystem::exists(ini_path_, ec);
    io.Fonts->AddFontDefaultVector();

    ImGui_ImplSDL3_InitForOther(desc.window);
    if (desc.viewports) {
        renderer_->enable_viewports([](ImGuiViewport* vp) {
            // imgui_impl_sdl3 stores the SDL window ID in PlatformHandle.
            const auto id = static_cast<SDL_WindowID>(reinterpret_cast<std::uintptr_t>(vp->PlatformHandle));
            return native_window(SDL_GetWindowFromID(id));
        });
    }
    set_scale(desc.scale);
    F3D_LOG_INFO("UI: Dear ImGui {}, docking, viewports {}, scale {:.2f}{}", IMGUI_VERSION, desc.viewports, desc.scale,
                 ini_path_.empty() ? "" : ", layout file " + ini_path_);
}

ImGuiLayer::~ImGuiLayer() {
    ImGui_ImplSDL3_Shutdown();
    renderer_.reset(); // destroys the ImGui context
}

void ImGuiLayer::process_event(const SDL_Event& event) {
    ImGui_ImplSDL3_ProcessEvent(&event);
}

void ImGuiLayer::set_scale(float scale) {
    scale_ = scale > 0.0f ? scale : 1.0f;
    apply_style(scale_, theme_, colors_);
}

void ImGuiLayer::set_theme(Theme theme, const ThemeColors& colors) {
    theme_ = theme;
    colors_ = colors;
    apply_style(scale_, theme_, colors_);
}

void ImGuiLayer::begin_frame(std::uint32_t width, std::uint32_t height) {
    F3D_ZONE;
    ImGui_ImplSDL3_NewFrame();
    renderer_->new_frame(width, height);
}

void ImGuiLayer::render(render::rhi::CommandContext& ctx, const render::rhi::Texture& target) {
    F3D_ZONE;
    render::rhi::ScopedGpuTimer timer(ctx, "ui");
    renderer_->render(ctx, target);
}

void ImGuiLayer::render_platform_windows() {
    renderer_->render_platform_windows();
}

} // namespace f3d::editor
