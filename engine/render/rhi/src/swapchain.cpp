// SPDX-License-Identifier: Apache-2.0
#include "diligent.hpp"

#include "f3d/core/profile.hpp"

namespace f3d::render::rhi {
namespace {
dg::ISwapChain* as_swapchain(void* p) noexcept {
    return static_cast<dg::ISwapChain*>(p);
}
} // namespace

Swapchain::~Swapchain() {
    if (swapchain_) {
        Device::Impl& impl = device_.impl();
        impl.context->Flush();
        impl.context->WaitForIdle();
        forget_back_buffers();
        as_swapchain(swapchain_)->Release();
        swapchain_ = nullptr;
    }
}

void Swapchain::forget_back_buffers() {
    auto& rtvs = device_.impl().swapchain_rtvs;
    for (void* texture : back_buffers_) {
        rtvs.erase(static_cast<dg::ITexture*>(texture));
    }
    back_buffers_.clear();
}

void Swapchain::resize(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }
    dg::ISwapChain* sc = as_swapchain(swapchain_);
    if (sc->GetDesc().Width == width && sc->GetDesc().Height == height) {
        return;
    }
    forget_back_buffers();
    sc->Resize(width, height);
}

void Swapchain::present(bool vsync) {
    F3D_ZONE;
    as_swapchain(swapchain_)->Present(vsync ? 1 : 0);
}

Texture Swapchain::back_buffer() const {
    dg::ITextureView* rtv = as_swapchain(swapchain_)->GetCurrentBackBufferRTV();
    if (!rtv) {
        return {};
    }
    dg::ITexture* texture = rtv->GetTexture();
    auto [it, inserted] = device_.impl().swapchain_rtvs.try_emplace(texture, rtv);
    if (inserted) {
        back_buffers_.push_back(texture);
    } else {
        it->second = rtv;
    }
    return make_handle<Texture>(texture);
}

std::uint32_t Swapchain::width() const {
    return as_swapchain(swapchain_)->GetDesc().Width;
}
std::uint32_t Swapchain::height() const {
    return as_swapchain(swapchain_)->GetDesc().Height;
}
Format Swapchain::format() const {
    return from_dg(as_swapchain(swapchain_)->GetDesc().ColorBufferFormat);
}

} // namespace f3d::render::rhi
