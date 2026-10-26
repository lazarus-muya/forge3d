// SPDX-License-Identifier: Apache-2.0
#include "diligent.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"

#include "GraphicsAccessories.hpp"
#include "PipelineState.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_4.h>
#endif
#if F3D_DILIGENT_D3D12
#include "EngineFactoryD3D12.h"
#include "RenderDeviceD3D12.h"
#endif
#if F3D_DILIGENT_VULKAN
// volk declares the Vulkan types (VK_NO_PROTOTYPES) that RenderDeviceVk.h uses.
#include <volk.h>
// After volk.
#include "EngineFactoryVk.h"
#include "RenderDeviceVk.h"
#endif

#include <algorithm>
#include <cstring>

namespace f3d::render::rhi {
namespace {

void DILIGENT_CALL_TYPE on_diligent_message(dg::DEBUG_MESSAGE_SEVERITY severity, const dg::Char* message,
                                            const dg::Char* function, const dg::Char* /*file*/, int /*line*/) {
    const char* fn = function ? function : "";
    switch (severity) {
    case dg::DEBUG_MESSAGE_SEVERITY_INFO:
        F3D_LOG_DEBUG("[diligent] {}", message);
        break;
    case dg::DEBUG_MESSAGE_SEVERITY_WARNING:
        // Expected for our build configuration; not worth a warning on every start.
        if (std::strstr(message, "without PIX support") != nullptr) {
            F3D_LOG_DEBUG("[diligent] {}", message);
        } else {
            F3D_LOG_WARN("[diligent] {}", message);
        }
        break;
    case dg::DEBUG_MESSAGE_SEVERITY_ERROR:
    case dg::DEBUG_MESSAGE_SEVERITY_FATAL_ERROR:
        F3D_LOG_ERROR("[diligent] {} {}", message, fn);
        break;
    }
}

dg::IEngineFactory* engine_factory(Backend backend) {
    dg::IEngineFactory* factory = nullptr;
    switch (backend) {
    case Backend::D3D12:
#if F3D_DILIGENT_D3D12
    {
        auto* f = dg::GetEngineFactoryD3D12();
        if (f && f->LoadD3D12()) {
            factory = f;
        }
    }
#endif
    break;
    case Backend::Vulkan:
#if F3D_DILIGENT_VULKAN
        factory = dg::GetEngineFactoryVk();
#endif
        break;
    }
    if (factory) {
        factory->SetMessageCallback(on_diligent_message);
        factory->SetBreakOnError(false);
    }
    return factory;
}

dg::Version min_api_version(Backend backend) {
    return backend == Backend::D3D12 ? dg::Version{11, 0} : dg::Version{1, 1};
}

AdapterType to_adapter_type(dg::ADAPTER_TYPE type) {
    switch (type) {
    case dg::ADAPTER_TYPE_INTEGRATED:
        return AdapterType::Integrated;
    case dg::ADAPTER_TYPE_DISCRETE:
        return AdapterType::Discrete;
    case dg::ADAPTER_TYPE_SOFTWARE:
        return AdapterType::Software;
    default:
        return AdapterType::Unknown;
    }
}

std::vector<dg::GraphicsAdapterInfo> enumerate(dg::IEngineFactory* factory, Backend backend) {
    dg::Uint32 count = 0;
    factory->EnumerateAdapters(min_api_version(backend), count, nullptr);
    std::vector<dg::GraphicsAdapterInfo> adapters(count);
    if (count > 0) {
        factory->EnumerateAdapters(min_api_version(backend), count, adapters.data());
    }
    adapters.resize(count);
    return adapters;
}

AdapterInfo to_adapter_info(const dg::GraphicsAdapterInfo& info, std::uint32_t index) {
    AdapterInfo a;
    a.index = index;
    a.name = info.Description;
    a.type = to_adapter_type(info.Type);
    a.dedicated_memory = info.Memory.LocalMemory;
    return a;
}

/// Picks an adapter index: explicit index, else by preference, never a software adapter if avoidable.
std::optional<std::uint32_t> choose_adapter(const std::vector<dg::GraphicsAdapterInfo>& adapters,
                                            const DeviceDesc& desc, std::string& error) {
    if (adapters.empty()) {
        error = "no compatible adapter";
        return std::nullopt;
    }
    if (desc.adapter_index) {
        if (*desc.adapter_index >= adapters.size()) {
            error = "adapter index " + std::to_string(*desc.adapter_index) + " out of range (" +
                    std::to_string(adapters.size()) + " adapters)";
            return std::nullopt;
        }
        return *desc.adapter_index;
    }
    if (desc.adapter_preference == AdapterPreference::Software) {
        for (std::uint32_t i = 0; i < adapters.size(); ++i) {
            if (adapters[i].Type == dg::ADAPTER_TYPE_SOFTWARE) {
                return i;
            }
        }
        error = "no software adapter (WARP on D3D12, lavapipe on Vulkan)";
        return std::nullopt;
    }
    const dg::ADAPTER_TYPE wanted = desc.adapter_preference == AdapterPreference::HighPerformance
                                        ? dg::ADAPTER_TYPE_DISCRETE
                                        : dg::ADAPTER_TYPE_INTEGRATED;
    for (std::uint32_t i = 0; i < adapters.size(); ++i) {
        if (adapters[i].Type == wanted) {
            return i;
        }
    }
    for (std::uint32_t i = 0; i < adapters.size(); ++i) {
        if (adapters[i].Type != dg::ADAPTER_TYPE_SOFTWARE) {
            return i;
        }
    }
    return 0u;
}

DeviceCaps read_caps(dg::IRenderDevice* device) {
    const auto& info = device->GetDeviceInfo();
    const auto& adapter = device->GetAdapterInfo();
    DeviceCaps caps;
    caps.api_version = std::to_string(info.APIVersion.Major) + "." + std::to_string(info.APIVersion.Minor);
    caps.compute = info.Features.ComputeShaders == dg::DEVICE_FEATURE_STATE_ENABLED;
    caps.bindless = info.Features.BindlessResources == dg::DEVICE_FEATURE_STATE_ENABLED;
    caps.ray_tracing = info.Features.RayTracing == dg::DEVICE_FEATURE_STATE_ENABLED;
    caps.ray_query = (adapter.RayTracing.CapFlags & dg::RAY_TRACING_CAP_FLAG_INLINE_RAY_TRACING) != 0;
    caps.mesh_shaders = info.Features.MeshShaders == dg::DEVICE_FEATURE_STATE_ENABLED;
    return caps;
}

std::unique_ptr<Device::Impl> create_impl(Backend backend, const DeviceDesc& desc, std::string& error) {
    if (!is_backend_available(backend)) {
        error = std::string(to_string(backend)) + " is not compiled into this build";
        return nullptr;
    }
    dg::IEngineFactory* factory = engine_factory(backend);
    if (!factory) {
        error = std::string(to_string(backend)) + " runtime could not be loaded";
        return nullptr;
    }
    const auto adapters = enumerate(factory, backend);
    const auto index = choose_adapter(adapters, desc, error);
    if (!index) {
        return nullptr;
    }

    auto impl = std::make_unique<Device::Impl>();
    impl->backend = backend;
    impl->factory = factory;
    // Enable every optional feature the adapter offers; capabilities are reported in caps().
    const dg::DeviceFeatures features(dg::DEVICE_FEATURE_STATE_OPTIONAL);

    switch (backend) {
    case Backend::D3D12: {
#if F3D_DILIGENT_D3D12
        dg::EngineD3D12CreateInfo ci;
        ci.AdapterId = *index;
        ci.EnableValidation = desc.validation;
        ci.Features = features;
        static_cast<dg::IEngineFactoryD3D12*>(factory)->CreateDeviceAndContextsD3D12(ci, &impl->device, &impl->context);
#endif
        break;
    }
    case Backend::Vulkan: {
#if F3D_DILIGENT_VULKAN
        dg::EngineVkCreateInfo ci;
        ci.AdapterId = *index;
        ci.EnableValidation = desc.validation;
        ci.Features = features;
        static_cast<dg::IEngineFactoryVk*>(factory)->CreateDeviceAndContextsVk(ci, &impl->device, &impl->context);
#endif
        break;
    }
    }
    if (!impl->device || !impl->context) {
        error = std::string(to_string(backend)) + " device creation failed on '" +
                std::string(adapters[*index].Description) + "'";
        return nullptr;
    }
    impl->adapter = to_adapter_info(adapters[*index], *index);
    impl->caps = read_caps(impl->device);
    return impl;
}

#if defined(_WIN32)
MemoryUsage query_dxgi_memory(LUID luid) {
    MemoryUsage usage;
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)))) {
        return usage;
    }
    IDXGIAdapter3* adapter = nullptr;
    if (SUCCEEDED(factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter)))) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            usage.vram_used = info.CurrentUsage;
            usage.vram_budget = info.Budget;
        }
        adapter->Release();
    }
    factory->Release();
    return usage;
}
#endif

dg::PRIMITIVE_TOPOLOGY to_dg(PrimitiveTopology topology) {
    switch (topology) {
    case PrimitiveTopology::TriangleStrip:
        return dg::PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::LineList:
        return dg::PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PrimitiveTopology::TriangleList:
        break;
    }
    return dg::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

dg::CULL_MODE to_dg(CullMode mode) {
    switch (mode) {
    case CullMode::None:
        return dg::CULL_MODE_NONE;
    case CullMode::Front:
        return dg::CULL_MODE_FRONT;
    case CullMode::Back:
        break;
    }
    return dg::CULL_MODE_BACK;
}

void set_blend(dg::RenderTargetBlendDesc& rt, BlendMode mode) {
    rt.BlendEnable = mode != BlendMode::Opaque;
    rt.BlendOp = dg::BLEND_OPERATION_ADD;
    rt.BlendOpAlpha = dg::BLEND_OPERATION_ADD;
    rt.SrcBlendAlpha = dg::BLEND_FACTOR_ONE;
    rt.DestBlendAlpha = dg::BLEND_FACTOR_INV_SRC_ALPHA;
    switch (mode) {
    case BlendMode::AlphaBlend:
        rt.SrcBlend = dg::BLEND_FACTOR_SRC_ALPHA;
        rt.DestBlend = dg::BLEND_FACTOR_INV_SRC_ALPHA;
        break;
    case BlendMode::PremultipliedAlpha:
        rt.SrcBlend = dg::BLEND_FACTOR_ONE;
        rt.DestBlend = dg::BLEND_FACTOR_INV_SRC_ALPHA;
        break;
    case BlendMode::Additive:
        rt.SrcBlend = dg::BLEND_FACTOR_ONE;
        rt.DestBlend = dg::BLEND_FACTOR_ONE;
        rt.DestBlendAlpha = dg::BLEND_FACTOR_ONE;
        break;
    case BlendMode::Opaque:
        break;
    }
}

/// Vertex attribute format -> (components, value type, normalized).
bool to_layout(Format format, dg::Uint32& components, dg::VALUE_TYPE& type, bool& normalized) {
    normalized = false;
    switch (format) {
    case Format::R32Float:
        components = 1;
        type = dg::VT_FLOAT32;
        return true;
    case Format::RG32Float:
        components = 2;
        type = dg::VT_FLOAT32;
        return true;
    case Format::RGB32Float:
        components = 3;
        type = dg::VT_FLOAT32;
        return true;
    case Format::RGBA32Float:
        components = 4;
        type = dg::VT_FLOAT32;
        return true;
    case Format::RGBA8Unorm:
        components = 4;
        type = dg::VT_UINT8;
        normalized = true;
        return true;
    case Format::R32Uint:
        components = 1;
        type = dg::VT_UINT32;
        return true;
    default:
        return false;
    }
}

std::vector<dg::ImmutableSamplerDesc> to_dg_samplers(const std::vector<SamplerDesc>& samplers, dg::SHADER_TYPE stages) {
    std::vector<dg::ImmutableSamplerDesc> out;
    out.reserve(samplers.size());
    for (const SamplerDesc& s : samplers) {
        dg::SamplerDesc d;
        const dg::FILTER_TYPE f = s.filter == Filter::Point        ? dg::FILTER_TYPE_POINT
                                  : s.filter == Filter::Anisotropic ? dg::FILTER_TYPE_ANISOTROPIC
                                                                    : dg::FILTER_TYPE_LINEAR;
        d.MinFilter = d.MagFilter = d.MipFilter = f;
        if (s.filter == Filter::Anisotropic) {
            d.MaxAnisotropy = 8;
        }
        const dg::TEXTURE_ADDRESS_MODE a =
            s.address == AddressMode::Wrap ? dg::TEXTURE_ADDRESS_WRAP : dg::TEXTURE_ADDRESS_CLAMP;
        d.AddressU = d.AddressV = d.AddressW = a;
        if (s.compare) {
            const dg::FILTER_TYPE cf =
                s.filter == Filter::Point ? dg::FILTER_TYPE_COMPARISON_POINT : dg::FILTER_TYPE_COMPARISON_LINEAR;
            d.MinFilter = d.MagFilter = d.MipFilter = cf;
            d.MaxAnisotropy = 0;
            d.ComparisonFunc = to_dg(*s.compare);
        }
        out.emplace_back(stages, s.texture_name.c_str(), d);
    }
    return out;
}

} // namespace

dg::ITextureView* Device::Impl::render_target_view(dg::ITexture* texture) const {
    if (!texture) {
        return nullptr;
    }
    if (const auto it = swapchain_rtvs.find(texture); it != swapchain_rtvs.end()) {
        return it->second;
    }
    return texture->GetDefaultView(dg::TEXTURE_VIEW_RENDER_TARGET);
}

// --- Device ------------------------------------------------------------------------------

Device::Device(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
    context_.reset(new CommandContext(*this, impl_->context.RawPtr()));
}

Device::~Device() {
    if (impl_ && impl_->context) {
        impl_->context->Flush();
        impl_->context->WaitForIdle();
    }
}

Device::CreateResult Device::create(const DeviceDesc& desc) {
    F3D_ZONE;
    CreateResult result;
    std::vector<Backend> order{desc.backend};
    if (desc.allow_fallback) {
        order.push_back(desc.backend == Backend::D3D12 ? Backend::Vulkan : Backend::D3D12);
    }
    for (const Backend backend : order) {
        std::string error;
        auto impl = create_impl(backend, desc, error);
        if (impl) {
            if (backend != desc.backend) {
                F3D_LOG_WARN("rhi: {} unavailable ({}); fell back to {}", to_string(desc.backend), result.error,
                             to_string(backend));
            }
            F3D_LOG_INFO("rhi: {} on '{}' ({}, adapter {}), API {}; compute {}, bindless {}, ray tracing {}",
                         to_string(backend), impl->adapter.name, to_string(impl->adapter.type), impl->adapter.index,
                         impl->caps.api_version, impl->caps.compute, impl->caps.bindless, impl->caps.ray_tracing);
            result.device.reset(new Device(std::move(impl)));
            result.error.clear();
            return result;
        }
        result.error += (result.error.empty() ? "" : "; ") + error;
    }
    return result;
}

std::vector<AdapterInfo> Device::enumerate_adapters(Backend backend) {
    std::vector<AdapterInfo> out;
    if (!is_backend_available(backend)) {
        return out;
    }
    dg::IEngineFactory* factory = engine_factory(backend);
    if (!factory) {
        return out;
    }
    const auto adapters = enumerate(factory, backend);
    for (std::uint32_t i = 0; i < adapters.size(); ++i) {
        out.push_back(to_adapter_info(adapters[i], i));
    }
    return out;
}

Backend Device::backend() const noexcept {
    return impl_->backend;
}
const AdapterInfo& Device::adapter() const noexcept {
    return impl_->adapter;
}
const DeviceCaps& Device::caps() const noexcept {
    return impl_->caps;
}
CommandContext& Device::context() noexcept {
    return *context_;
}
void* Device::native() const noexcept {
    return impl_->device.RawPtr();
}

MemoryUsage Device::memory_usage() const {
#if defined(_WIN32)
    switch (impl_->backend) {
    case Backend::D3D12:
#if F3D_DILIGENT_D3D12
    {
        dg::RefCntAutoPtr<dg::IRenderDeviceD3D12> d3d(impl_->device, dg::IID_RenderDeviceD3D12);
        if (d3d) {
            return query_dxgi_memory(d3d->GetD3D12Device()->GetAdapterLuid());
        }
    }
#endif
    break;
    case Backend::Vulkan:
#if F3D_DILIGENT_VULKAN
    {
        // Vulkan has no portable usage query without VK_EXT_memory_budget; on
        // Windows map the physical device to its DXGI adapter through the LUID.
        dg::RefCntAutoPtr<dg::IRenderDeviceVk> vk(impl_->device, dg::IID_RenderDeviceVk);
        if (vk && vkGetPhysicalDeviceProperties2) {
            VkPhysicalDeviceIDProperties id{};
            id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
            VkPhysicalDeviceProperties2 props{};
            props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            props.pNext = &id;
            vkGetPhysicalDeviceProperties2(vk->GetVkPhysicalDevice(), &props);
            if (id.deviceLUIDValid) {
                LUID luid{};
                std::memcpy(&luid, id.deviceLUID, sizeof(luid));
                return query_dxgi_memory(luid);
            }
        }
    }
#endif
    break;
    }
#endif
    return {};
}

Buffer Device::create_buffer(const BufferDesc& desc, const void* initial_data) {
    dg::BufferDesc bd;
    bd.Name = desc.name.c_str();
    bd.Size = desc.size;
    bd.BindFlags = to_dg(desc.bind);
    bd.ElementByteStride = desc.stride;
    switch (desc.usage) {
    case Usage::Default:
        bd.Usage = dg::USAGE_DEFAULT;
        break;
    case Usage::Immutable:
        bd.Usage = dg::USAGE_IMMUTABLE;
        break;
    case Usage::Dynamic:
        bd.Usage = dg::USAGE_DYNAMIC;
        bd.CPUAccessFlags = dg::CPU_ACCESS_WRITE;
        break;
    }
    if (desc.stride != 0 &&
        (has_flag(desc.bind, BindFlags::ShaderResource) || has_flag(desc.bind, BindFlags::UnorderedAccess))) {
        bd.Mode = dg::BUFFER_MODE_STRUCTURED;
    }
    dg::BufferData data;
    data.pData = initial_data;
    data.DataSize = initial_data ? desc.size : 0;
    dg::IBuffer* buffer = nullptr;
    impl_->device->CreateBuffer(bd, initial_data ? &data : nullptr, &buffer);
    if (!buffer) {
        F3D_LOG_ERROR("rhi: failed to create buffer '{}'", desc.name);
    }
    return Buffer{buffer ? static_cast<dg::IObject*>(buffer) : nullptr};
}

Texture Device::create_texture(const TextureDesc& desc) {
    dg::TextureDesc td;
    td.Name = desc.name.c_str();
    switch (desc.dimension) {
    case TextureDimension::Tex2D:
        td.Type = dg::RESOURCE_DIM_TEX_2D;
        break;
    case TextureDimension::Tex2DArray:
        td.Type = dg::RESOURCE_DIM_TEX_2D_ARRAY;
        td.ArraySize = std::max(desc.array_size, 1u);
        break;
    case TextureDimension::Cube:
        td.Type = dg::RESOURCE_DIM_TEX_CUBE;
        td.ArraySize = 6;
        break;
    case TextureDimension::Tex3D:
        td.Type = dg::RESOURCE_DIM_TEX_3D;
        td.Depth = std::max(desc.array_size, 1u);
        break;
    }
    td.Width = desc.width;
    td.Height = desc.height;
    td.Format = to_dg(desc.format);
    td.BindFlags = to_dg(desc.bind);
    td.MipLevels = desc.mip_levels;
    if (desc.generate_mips) {
        td.MiscFlags |= dg::MISC_TEXTURE_FLAG_GENERATE_MIPS;
    }
    if (has_flag(desc.bind, BindFlags::DepthStencil)) {
        td.ClearValue.Format = td.Format;
        td.ClearValue.DepthStencil.Depth = 1.0f;
    } else if (has_flag(desc.bind, BindFlags::RenderTarget)) {
        td.ClearValue.Format = td.Format;
    }
    dg::ITexture* texture = nullptr;
    impl_->device->CreateTexture(td, nullptr, &texture);
    if (!texture) {
        F3D_LOG_ERROR("rhi: failed to create texture '{}' ({}x{})", desc.name, desc.width, desc.height);
    }
    return Texture{texture ? static_cast<dg::IObject*>(texture) : nullptr};
}

Texture Device::create_texture(const TextureDesc& desc, const void* data, std::size_t row_pitch) {
    dg::TextureDesc td;
    td.Name = desc.name.c_str();
    td.Type = dg::RESOURCE_DIM_TEX_2D;
    td.Width = desc.width;
    td.Height = desc.height;
    td.Format = to_dg(desc.format);
    td.BindFlags = to_dg(desc.bind);
    td.MipLevels = 1;
    td.Usage = dg::USAGE_IMMUTABLE;
    dg::TextureSubResData sub;
    sub.pData = data;
    sub.Stride = row_pitch;
    dg::TextureData init(&sub, 1);
    dg::ITexture* texture = nullptr;
    impl_->device->CreateTexture(td, &init, &texture);
    if (!texture) {
        F3D_LOG_ERROR("rhi: failed to create texture '{}' ({}x{}) with data", desc.name, desc.width, desc.height);
    }
    return Texture{texture ? static_cast<dg::IObject*>(texture) : nullptr};
}

Texture Device::create_texture(const TextureDesc& desc, std::span<const TextureLevelData> levels) {
    dg::TextureDesc td;
    td.Name = desc.name.c_str();
    td.Type = dg::RESOURCE_DIM_TEX_2D;
    td.Width = desc.width;
    td.Height = desc.height;
    td.Format = to_dg(desc.format);
    td.BindFlags = to_dg(desc.bind);
    td.MipLevels = static_cast<dg::Uint32>(levels.size());
    td.Usage = dg::USAGE_IMMUTABLE;
    std::vector<dg::TextureSubResData> subresources(levels.size());
    for (std::size_t i = 0; i < levels.size(); ++i) {
        subresources[i].pData = levels[i].data;
        subresources[i].Stride = levels[i].row_pitch;
    }
    dg::TextureData init(subresources.data(), static_cast<dg::Uint32>(subresources.size()));
    dg::ITexture* texture = nullptr;
    impl_->device->CreateTexture(td, &init, &texture);
    if (!texture) {
        F3D_LOG_ERROR("rhi: failed to create texture '{}' ({}x{}, {} levels)", desc.name, desc.width, desc.height,
                      levels.size());
    }
    return Texture{texture ? static_cast<dg::IObject*>(texture) : nullptr};
}

Shader Device::create_shader(ShaderStage stage, std::span<const std::byte> bytecode, std::string_view name) {
    const std::string shader_name(name);
    dg::ShaderCreateInfo sci;
    sci.Desc.Name = shader_name.c_str();
    sci.Desc.ShaderType = to_dg(stage);
    sci.Desc.UseCombinedTextureSamplers = true;
    sci.SourceLanguage = dg::SHADER_SOURCE_LANGUAGE_HLSL;
    sci.EntryPoint = "main";
    sci.ByteCode = bytecode.data();
    sci.ByteCodeSize = bytecode.size();
    dg::IShader* shader = nullptr;
    impl_->device->CreateShader(sci, &shader);
    if (!shader) {
        F3D_LOG_ERROR("rhi: failed to create shader '{}' from {} bytes of bytecode", name, bytecode.size());
    }
    return Shader{shader ? static_cast<dg::IObject*>(shader) : nullptr};
}

Shader Device::create_shader_from_source(ShaderStage stage, std::string_view hlsl, std::string_view name,
                                         std::string_view entry_point) {
    const std::string shader_name(name);
    const std::string source(hlsl);
    const std::string entry(entry_point);
    dg::ShaderCreateInfo sci;
    sci.Desc.Name = shader_name.c_str();
    sci.Desc.ShaderType = to_dg(stage);
    sci.Desc.UseCombinedTextureSamplers = true;
    sci.SourceLanguage = dg::SHADER_SOURCE_LANGUAGE_HLSL;
    sci.EntryPoint = entry.c_str();
    sci.Source = source.c_str();
    sci.SourceLength = source.size();
    dg::IShader* shader = nullptr;
    dg::IDataBlob* output = nullptr;
    impl_->device->CreateShader(sci, &shader, &output);
    if (!shader) {
        F3D_LOG_ERROR("rhi: failed to compile shader '{}': {}", name,
                      output ? static_cast<const char*>(output->GetConstDataPtr()) : "(no output)");
    }
    if (output) {
        output->Release();
    }
    return Shader{shader ? static_cast<dg::IObject*>(shader) : nullptr};
}

Pipeline Device::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
    F3D_ZONE;
    dg::GraphicsPipelineStateCreateInfo pci;
    pci.PSODesc.Name = desc.name.c_str();
    pci.PSODesc.PipelineType = dg::PIPELINE_TYPE_GRAPHICS;
    pci.PSODesc.ResourceLayout.DefaultVariableType = dg::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;

    auto& gp = pci.GraphicsPipeline;
    gp.NumRenderTargets = static_cast<dg::Uint8>(desc.color_formats.size());
    for (std::size_t i = 0; i < desc.color_formats.size() && i < 8; ++i) {
        gp.RTVFormats[i] = to_dg(desc.color_formats[i]);
        set_blend(gp.BlendDesc.RenderTargets[i], desc.blend);
    }
    gp.DSVFormat = to_dg(desc.depth_format);
    gp.PrimitiveTopology = to_dg(desc.topology);
    gp.RasterizerDesc.CullMode = to_dg(desc.cull);
    gp.RasterizerDesc.FillMode = desc.wireframe ? dg::FILL_MODE_WIREFRAME : dg::FILL_MODE_SOLID;
    gp.RasterizerDesc.FrontCounterClockwise = true; // glTF / right-handed convention
    gp.RasterizerDesc.DepthBias = desc.depth_bias;
    gp.RasterizerDesc.SlopeScaledDepthBias = desc.slope_scaled_depth_bias;
    gp.RasterizerDesc.DepthClipEnable = desc.depth_clip;
    gp.DepthStencilDesc.DepthEnable = desc.depth_test && desc.depth_format != Format::Unknown;
    gp.DepthStencilDesc.DepthWriteEnable = desc.depth_write && gp.DepthStencilDesc.DepthEnable;
    gp.DepthStencilDesc.DepthFunc = to_dg(desc.depth_func);

    std::vector<dg::LayoutElement> layout;
    for (const VertexAttribute& a : desc.vertex_layout) {
        dg::Uint32 components = 0;
        dg::VALUE_TYPE type = dg::VT_UNDEFINED;
        bool normalized = false;
        if (!to_layout(a.format, components, type, normalized)) {
            F3D_LOG_ERROR("rhi: pipeline '{}': unsupported vertex format for location {}", desc.name, a.location);
            return {};
        }
        layout.emplace_back(a.location, a.buffer_slot, components, type, normalized, a.offset);
    }
    gp.InputLayout.LayoutElements = layout.data();
    gp.InputLayout.NumElements = static_cast<dg::Uint32>(layout.size());

    pci.pVS = native_as<dg::IShader>(desc.vs);
    pci.pPS = native_as<dg::IShader>(desc.ps);
    const auto samplers = to_dg_samplers(desc.samplers, dg::SHADER_TYPE_VERTEX | dg::SHADER_TYPE_PIXEL);
    pci.PSODesc.ResourceLayout.ImmutableSamplers = samplers.data();
    pci.PSODesc.ResourceLayout.NumImmutableSamplers = static_cast<dg::Uint32>(samplers.size());

    dg::IPipelineState* pso = nullptr;
    impl_->device->CreateGraphicsPipelineState(pci, &pso);
    if (!pso) {
        F3D_LOG_ERROR("rhi: failed to create graphics pipeline '{}'", desc.name);
    }
    return Pipeline{pso ? static_cast<dg::IObject*>(pso) : nullptr};
}

Pipeline Device::create_compute_pipeline(const ComputePipelineDesc& desc) {
    F3D_ZONE;
    dg::ComputePipelineStateCreateInfo pci;
    pci.PSODesc.Name = desc.name.c_str();
    pci.PSODesc.PipelineType = dg::PIPELINE_TYPE_COMPUTE;
    pci.PSODesc.ResourceLayout.DefaultVariableType = dg::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    pci.pCS = native_as<dg::IShader>(desc.cs);
    const auto samplers = to_dg_samplers(desc.samplers, dg::SHADER_TYPE_COMPUTE);
    pci.PSODesc.ResourceLayout.ImmutableSamplers = samplers.data();
    pci.PSODesc.ResourceLayout.NumImmutableSamplers = static_cast<dg::Uint32>(samplers.size());
    dg::IPipelineState* pso = nullptr;
    impl_->device->CreateComputePipelineState(pci, &pso);
    if (!pso) {
        F3D_LOG_ERROR("rhi: failed to create compute pipeline '{}'", desc.name);
    }
    return Pipeline{pso ? static_cast<dg::IObject*>(pso) : nullptr};
}

std::unique_ptr<Swapchain> Device::create_swapchain(const NativeWindow& window, std::uint32_t width,
                                                    std::uint32_t height, Format format) {
    F3D_ZONE;
    dg::SwapChainDesc desc;
    desc.Width = width;
    desc.Height = height;
    desc.ColorBufferFormat = to_dg(format);
    desc.DepthBufferFormat = dg::TEX_FORMAT_UNKNOWN;
    desc.Usage = dg::SWAP_CHAIN_USAGE_RENDER_TARGET | dg::SWAP_CHAIN_USAGE_COPY_SOURCE;

    dg::NativeWindow native;
#if defined(_WIN32)
    native.hWnd = window.hwnd;
#elif defined(__linux__)
    native.pDisplay = window.x11_display;
    native.WindowId = window.x11_window;
#endif

    dg::ISwapChain* swapchain = nullptr;
    switch (impl_->backend) {
    case Backend::D3D12:
#if F3D_DILIGENT_D3D12
        static_cast<dg::IEngineFactoryD3D12*>(impl_->factory)
            ->CreateSwapChainD3D12(impl_->device, impl_->context, desc, dg::FullScreenModeDesc{}, native, &swapchain);
#endif
        break;
    case Backend::Vulkan:
#if F3D_DILIGENT_VULKAN
        static_cast<dg::IEngineFactoryVk*>(impl_->factory)
            ->CreateSwapChainVk(impl_->device, impl_->context, desc, native, &swapchain);
#endif
        break;
    }
    if (!swapchain) {
        F3D_LOG_ERROR("rhi: swapchain creation failed");
        return nullptr;
    }
    return std::unique_ptr<Swapchain>(new Swapchain(*this, swapchain));
}

std::vector<std::pair<std::string, double>> Device::gpu_timings() const {
    return {impl_->timings_ms.begin(), impl_->timings_ms.end()};
}

// --- GPU timers (CommandContext) -------------------------------------------------------------

void CommandContext::begin_timer(std::string_view name) {
    Device::Impl& impl = device_.impl();
    if (impl.device->GetDeviceInfo().Features.DurationQueries != dg::DEVICE_FEATURE_STATE_ENABLED &&
        impl.device->GetDeviceInfo().Features.TimestampQueries != dg::DEVICE_FEATURE_STATE_ENABLED) {
        return;
    }
    auto it = impl.timers.find(name);
    if (it == impl.timers.end()) {
        it = impl.timers.emplace(std::string(name), std::make_unique<dg::DurationQueryHelper>(impl.device, 4)).first;
    }
    it->second->Begin(impl.context);
}

void CommandContext::end_timer(std::string_view name) {
    Device::Impl& impl = device_.impl();
    const auto it = impl.timers.find(name);
    if (it == impl.timers.end()) {
        return;
    }
    double seconds = 0.0;
    if (it->second->End(impl.context, seconds)) {
        impl.timings_ms[it->first] = seconds * 1000.0;
    }
}

} // namespace f3d::render::rhi
