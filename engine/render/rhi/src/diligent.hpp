// SPDX-License-Identifier: Apache-2.0
// Private to f3d_render_rhi: the only place (with the .cpp files next to it)
// that sees Diligent headers.
#pragma once

#include "f3d/render/rhi/device.hpp"

#include "DeviceContext.h"
#include "DurationQueryHelper.hpp"
#include "EngineFactory.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "SwapChain.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace f3d::render::rhi {

namespace dg = Diligent;

struct Device::Impl {
    Backend backend = Backend::D3D12;
    AdapterInfo adapter;
    DeviceCaps caps;
    dg::IEngineFactory* factory = nullptr;
    dg::RefCntAutoPtr<dg::IRenderDevice> device;
    dg::RefCntAutoPtr<dg::IDeviceContext> context;

    /// Swapchain back buffers render through the swapchain's own RTV (e.g. an sRGB
    /// view of a UNORM D3D12 buffer), not the texture's default view.
    std::unordered_map<dg::ITexture*, dg::RefCntAutoPtr<dg::ITextureView>> swapchain_rtvs;

    std::map<std::string, std::unique_ptr<dg::DurationQueryHelper>, std::less<>> timers;
    std::map<std::string, double, std::less<>> timings_ms;

    [[nodiscard]] dg::ITextureView* render_target_view(dg::ITexture* texture) const;
};

// --- Handles -----------------------------------------------------------------

/// Wraps a Diligent object in an rhi handle, adding a reference.
template <class Handle, class T>
Handle make_handle(T* object) {
    if (!object) {
        return Handle{};
    }
    object->AddRef();
    return Handle{static_cast<dg::IObject*>(object)};
}

/// The Diligent object behind an rhi handle (nullptr if empty).
template <class T>
T* native_as(const detail::ObjectRef& ref) noexcept {
    return static_cast<T*>(static_cast<dg::IObject*>(ref.native()));
}

// --- Conversions ---------------------------------------------------------------

[[nodiscard]] dg::TEXTURE_FORMAT to_dg(Format format) noexcept;
[[nodiscard]] Format from_dg(dg::TEXTURE_FORMAT format) noexcept;
[[nodiscard]] dg::BIND_FLAGS to_dg(BindFlags flags) noexcept;
[[nodiscard]] dg::SHADER_TYPE to_dg(ShaderStage stage) noexcept;
[[nodiscard]] ShaderStage from_dg(dg::SHADER_TYPE type) noexcept;
[[nodiscard]] dg::COMPARISON_FUNCTION to_dg(CompareFunc func) noexcept;
[[nodiscard]] dg::RESOURCE_STATE to_dg(ResourceState state) noexcept;

} // namespace f3d::render::rhi
