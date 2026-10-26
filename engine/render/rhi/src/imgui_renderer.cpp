// SPDX-License-Identifier: Apache-2.0
#include "f3d/render/rhi/imgui_renderer.hpp"

#include "diligent.hpp"

#include "f3d/core/assert.hpp"
#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"

#include "ImGuiDiligentRenderer.hpp"
#include "ImGuiImplDiligent.hpp"

#include <imgui.h>

namespace f3d::render::rhi {
namespace {

/// Exposes the protected renderer so secondary viewports can reuse it.
class ImGuiBackend final : public dg::ImGuiImplDiligent {
public:
    using ImGuiImplDiligent::ImGuiImplDiligent;
    dg::ImGuiDiligentRenderer& renderer() { return *m_pRenderer; }
};

struct ViewportData {
    std::unique_ptr<Swapchain> swapchain;
};

} // namespace

struct ImGuiRenderer::Impl {
    Device& device;
    ImGuiBackend backend;
    NativeWindowResolver resolver;
    std::uint32_t main_width = 0;
    std::uint32_t main_height = 0;

    Impl(Device& d, Format format)
        : device(d), backend(dg::ImGuiDiligentCreateInfo{d.impl().device, to_dg(format), dg::TEX_FORMAT_UNKNOWN}) {}
};

namespace {

// ImGui's platform callbacks are plain function pointers; there is one ImGui
// renderer per process.
ImGuiRenderer::Impl* g_active = nullptr;

void create_window(ImGuiViewport* vp) {
    F3D_ZONE;
    auto* data = IM_NEW(ViewportData)();
    vp->RendererUserData = data;
    if (!g_active || !g_active->resolver) {
        return;
    }
    const NativeWindow native = g_active->resolver(vp);
    data->swapchain = g_active->device.create_swapchain(native, static_cast<std::uint32_t>(vp->Size.x * vp->DpiScale),
                                                        static_cast<std::uint32_t>(vp->Size.y * vp->DpiScale));
    if (!data->swapchain) {
        F3D_LOG_ERROR("imgui: could not create a swapchain for a detached window");
    }
}

void destroy_window(ImGuiViewport* vp) {
    if (auto* data = static_cast<ViewportData*>(vp->RendererUserData)) {
        IM_DELETE(data); // Swapchain's destructor waits for the GPU
    }
    vp->RendererUserData = nullptr;
}

void set_window_size(ImGuiViewport* vp, ImVec2 size) {
    if (auto* data = static_cast<ViewportData*>(vp->RendererUserData); data && data->swapchain) {
        data->swapchain->resize(static_cast<std::uint32_t>(size.x * vp->DpiScale),
                                static_cast<std::uint32_t>(size.y * vp->DpiScale));
    }
}

void render_window(ImGuiViewport* vp, void*) {
    F3D_ZONE;
    auto* data = static_cast<ViewportData*>(vp->RendererUserData);
    if (!g_active || !data || !data->swapchain) {
        return;
    }
    Swapchain& sc = *data->swapchain;
    CommandContext& ctx = g_active->device.context();
    const Texture target = sc.back_buffer();
    ctx.set_render_target(target);
    ctx.clear_color(target, {0.0f, 0.0f, 0.0f, 1.0f});
    // The Diligent renderer takes the surface size from NewFrame(); switch it for this window.
    dg::ImGuiDiligentRenderer& r = g_active->backend.renderer();
    r.NewFrame(sc.width(), sc.height(), dg::SURFACE_TRANSFORM_IDENTITY);
    const dg::ImGuiDiligentRenderTargetDesc rt{to_dg(sc.format()), dg::TEX_FORMAT_UNKNOWN};
    r.RenderDrawData(static_cast<dg::IDeviceContext*>(ctx.native()), vp->DrawData, &rt);
    r.NewFrame(g_active->main_width, g_active->main_height, dg::SURFACE_TRANSFORM_IDENTITY);
}

void swap_buffers(ImGuiViewport* vp, void*) {
    if (auto* data = static_cast<ViewportData*>(vp->RendererUserData); data && data->swapchain) {
        data->swapchain->present(false); // only the main window waits for vsync
    }
}

} // namespace

ImGuiRenderer::ImGuiRenderer(Device& device, Format back_buffer_format)
    : impl_(std::make_unique<Impl>(device, back_buffer_format)) {
    F3D_ASSERT(g_active == nullptr, "only one ImGuiRenderer may exist");
    g_active = impl_.get();
}

ImGuiRenderer::~ImGuiRenderer() {
    // The context (and with it any detached windows) is destroyed by ~ImGuiBackend,
    // which still needs g_active for destroy_window.
    impl_.reset();
    g_active = nullptr;
}

void ImGuiRenderer::enable_viewports(NativeWindowResolver resolver) {
    impl_->resolver = std::move(resolver);
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Renderer_CreateWindow = create_window;
    pio.Renderer_DestroyWindow = destroy_window;
    pio.Renderer_SetWindowSize = set_window_size;
    pio.Renderer_RenderWindow = render_window;
    pio.Renderer_SwapBuffers = swap_buffers;
}

void ImGuiRenderer::new_frame(std::uint32_t width, std::uint32_t height) {
    F3D_ZONE;
    impl_->main_width = width;
    impl_->main_height = height;
    impl_->backend.NewFrame(width, height, dg::SURFACE_TRANSFORM_IDENTITY);
}

void ImGuiRenderer::render(CommandContext& ctx, const Texture& target) {
    F3D_ZONE;
    ctx.set_render_target(target);
    const auto* texture = native_as<dg::ITexture>(target);
    // Render through the target's actual RTV format (sRGB back buffers convert ImGui's colours).
    dg::TEXTURE_FORMAT format = texture ? texture->GetDesc().Format : dg::TEX_FORMAT_UNKNOWN;
    if (dg::ITextureView* rtv = impl_->device.impl().render_target_view(const_cast<dg::ITexture*>(texture))) {
        format = rtv->GetDesc().Format;
    }
    const dg::ImGuiDiligentRenderTargetDesc rt{format, dg::TEX_FORMAT_UNKNOWN};
    impl_->backend.Render(static_cast<dg::IDeviceContext*>(ctx.native()), &rt);
}

void ImGuiRenderer::render_platform_windows() {
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        F3D_ZONE;
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

std::uint64_t ImGuiRenderer::texture_id(const Texture& texture) {
    auto* t = native_as<dg::ITexture>(texture);
    return t ? reinterpret_cast<std::uint64_t>(t->GetDefaultView(dg::TEXTURE_VIEW_SHADER_RESOURCE)) : 0;
}

} // namespace f3d::render::rhi
