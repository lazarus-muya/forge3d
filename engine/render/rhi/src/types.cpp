// SPDX-License-Identifier: Apache-2.0
#include "diligent.hpp"

#include <charconv>
#include <system_error>

namespace f3d::render::rhi {

std::string_view to_string(Backend backend) noexcept {
    return backend == Backend::D3D12 ? "d3d12" : "vulkan";
}

std::optional<Backend> parse_backend(std::string_view text) noexcept {
    if (text == "d3d12" || text == "D3D12" || text == "dx12") {
        return Backend::D3D12;
    }
    if (text == "vulkan" || text == "Vulkan" || text == "vk") {
        return Backend::Vulkan;
    }
    return std::nullopt;
}

bool is_backend_available(Backend backend) noexcept {
    switch (backend) {
    case Backend::D3D12:
#if F3D_DILIGENT_D3D12
        return true;
#else
        return false;
#endif
    case Backend::Vulkan:
#if F3D_DILIGENT_VULKAN
        return true;
#else
        return false;
#endif
    }
    return false;
}

Backend default_backend() noexcept {
#if defined(_WIN32)
    return Backend::D3D12;
#else
    return Backend::Vulkan;
#endif
}

bool is_depth_format(Format format) noexcept {
    return format == Format::D32Float;
}

bool is_srgb_format(Format format) noexcept {
    return format == Format::RGBA8UnormSrgb || format == Format::BGRA8UnormSrgb || format == Format::BC1UnormSrgb ||
           format == Format::BC3UnormSrgb || format == Format::BC7UnormSrgb;
}

std::uint32_t format_block_bytes(Format format) noexcept {
    switch (format) {
    case Format::BC1Unorm:
    case Format::BC1UnormSrgb:
    case Format::BC4Unorm:
        return 8;
    case Format::BC3Unorm:
    case Format::BC3UnormSrgb:
    case Format::BC5Unorm:
    case Format::BC7Unorm:
    case Format::BC7UnormSrgb:
        return 16;
    default:
        return 0;
    }
}

std::uint64_t texture_level_bytes(Format format, std::uint32_t width, std::uint32_t height) noexcept {
    if (const std::uint32_t block = format_block_bytes(format)) {
        return static_cast<std::uint64_t>((width + 3) / 4) * ((height + 3) / 4) * block;
    }
    return static_cast<std::uint64_t>(width) * height * format_size(format);
}

std::uint32_t format_size(Format format) noexcept {
    switch (format) {
    case Format::RGBA8Unorm:
    case Format::RGBA8UnormSrgb:
    case Format::BGRA8Unorm:
    case Format::BGRA8UnormSrgb:
    case Format::R32Uint:
    case Format::R32Float:
    case Format::D32Float:
        return 4;
    case Format::R16Unorm:
        return 2;
    case Format::RGBA16Float:
    case Format::RG32Float:
        return 8;
    case Format::RGB32Float:
        return 12;
    case Format::RGBA32Float:
        return 16;
    default:
        break;
    }
    return 0;
}

std::string_view to_string(ShaderStage stage) noexcept {
    switch (stage) {
    case ShaderStage::Vertex:
        return "vs";
    case ShaderStage::Pixel:
        return "ps";
    case ShaderStage::Compute:
        return "cs";
    }
    return "?";
}

std::string_view to_string(AdapterType type) noexcept {
    switch (type) {
    case AdapterType::Integrated:
        return "integrated";
    case AdapterType::Discrete:
        return "discrete";
    case AdapterType::Software:
        return "software";
    case AdapterType::Unknown:
        break;
    }
    return "unknown";
}

std::string_view to_string(ResourceState state) noexcept {
    switch (state) {
    case ResourceState::Undefined:
        return "undefined";
    case ResourceState::RenderTarget:
        return "render_target";
    case ResourceState::DepthWrite:
        return "depth_write";
    case ResourceState::DepthRead:
        return "depth_read";
    case ResourceState::ShaderResource:
        return "shader_resource";
    case ResourceState::UnorderedAccess:
        return "unordered_access";
    case ResourceState::CopySource:
        return "copy_source";
    case ResourceState::CopyDest:
        return "copy_dest";
    }
    return "?";
}

// --- Conversions -----------------------------------------------------------------

dg::TEXTURE_FORMAT to_dg(Format format) noexcept {
    switch (format) {
    case Format::RGBA8Unorm:
        return dg::TEX_FORMAT_RGBA8_UNORM;
    case Format::RGBA8UnormSrgb:
        return dg::TEX_FORMAT_RGBA8_UNORM_SRGB;
    case Format::BGRA8Unorm:
        return dg::TEX_FORMAT_BGRA8_UNORM;
    case Format::BGRA8UnormSrgb:
        return dg::TEX_FORMAT_BGRA8_UNORM_SRGB;
    case Format::RGBA16Float:
        return dg::TEX_FORMAT_RGBA16_FLOAT;
    case Format::RGBA32Float:
        return dg::TEX_FORMAT_RGBA32_FLOAT;
    case Format::R32Uint:
        return dg::TEX_FORMAT_R32_UINT;
    case Format::R32Float:
        return dg::TEX_FORMAT_R32_FLOAT;
    case Format::R16Unorm:
        return dg::TEX_FORMAT_R16_UNORM;
    case Format::RG32Float:
        return dg::TEX_FORMAT_RG32_FLOAT;
    case Format::RGB32Float:
        return dg::TEX_FORMAT_RGB32_FLOAT;
    case Format::D32Float:
        return dg::TEX_FORMAT_D32_FLOAT;
    case Format::BC1Unorm:
        return dg::TEX_FORMAT_BC1_UNORM;
    case Format::BC1UnormSrgb:
        return dg::TEX_FORMAT_BC1_UNORM_SRGB;
    case Format::BC3Unorm:
        return dg::TEX_FORMAT_BC3_UNORM;
    case Format::BC3UnormSrgb:
        return dg::TEX_FORMAT_BC3_UNORM_SRGB;
    case Format::BC4Unorm:
        return dg::TEX_FORMAT_BC4_UNORM;
    case Format::BC5Unorm:
        return dg::TEX_FORMAT_BC5_UNORM;
    case Format::BC7Unorm:
        return dg::TEX_FORMAT_BC7_UNORM;
    case Format::BC7UnormSrgb:
        return dg::TEX_FORMAT_BC7_UNORM_SRGB;
    case Format::Unknown:
        break;
    }
    return dg::TEX_FORMAT_UNKNOWN;
}

Format from_dg(dg::TEXTURE_FORMAT format) noexcept {
    switch (format) {
    case dg::TEX_FORMAT_RGBA8_UNORM:
        return Format::RGBA8Unorm;
    case dg::TEX_FORMAT_RGBA8_UNORM_SRGB:
        return Format::RGBA8UnormSrgb;
    case dg::TEX_FORMAT_BGRA8_UNORM:
        return Format::BGRA8Unorm;
    case dg::TEX_FORMAT_BGRA8_UNORM_SRGB:
        return Format::BGRA8UnormSrgb;
    case dg::TEX_FORMAT_RGBA16_FLOAT:
        return Format::RGBA16Float;
    case dg::TEX_FORMAT_RGBA32_FLOAT:
        return Format::RGBA32Float;
    case dg::TEX_FORMAT_R32_UINT:
        return Format::R32Uint;
    case dg::TEX_FORMAT_R32_FLOAT:
        return Format::R32Float;
    case dg::TEX_FORMAT_R16_UNORM:
        return Format::R16Unorm;
    case dg::TEX_FORMAT_RG32_FLOAT:
        return Format::RG32Float;
    case dg::TEX_FORMAT_RGB32_FLOAT:
        return Format::RGB32Float;
    case dg::TEX_FORMAT_D32_FLOAT:
        return Format::D32Float;
    case dg::TEX_FORMAT_BC1_UNORM:
        return Format::BC1Unorm;
    case dg::TEX_FORMAT_BC1_UNORM_SRGB:
        return Format::BC1UnormSrgb;
    case dg::TEX_FORMAT_BC3_UNORM:
        return Format::BC3Unorm;
    case dg::TEX_FORMAT_BC3_UNORM_SRGB:
        return Format::BC3UnormSrgb;
    case dg::TEX_FORMAT_BC4_UNORM:
        return Format::BC4Unorm;
    case dg::TEX_FORMAT_BC5_UNORM:
        return Format::BC5Unorm;
    case dg::TEX_FORMAT_BC7_UNORM:
        return Format::BC7Unorm;
    case dg::TEX_FORMAT_BC7_UNORM_SRGB:
        return Format::BC7UnormSrgb;
    default:
        return Format::Unknown;
    }
}

dg::BIND_FLAGS to_dg(BindFlags flags) noexcept {
    dg::BIND_FLAGS out = dg::BIND_NONE;
    if (has_flag(flags, BindFlags::VertexBuffer)) {
        out |= dg::BIND_VERTEX_BUFFER;
    }
    if (has_flag(flags, BindFlags::IndexBuffer)) {
        out |= dg::BIND_INDEX_BUFFER;
    }
    if (has_flag(flags, BindFlags::UniformBuffer)) {
        out |= dg::BIND_UNIFORM_BUFFER;
    }
    if (has_flag(flags, BindFlags::ShaderResource)) {
        out |= dg::BIND_SHADER_RESOURCE;
    }
    if (has_flag(flags, BindFlags::UnorderedAccess)) {
        out |= dg::BIND_UNORDERED_ACCESS;
    }
    if (has_flag(flags, BindFlags::RenderTarget)) {
        out |= dg::BIND_RENDER_TARGET;
    }
    if (has_flag(flags, BindFlags::DepthStencil)) {
        out |= dg::BIND_DEPTH_STENCIL;
    }
    return out;
}

dg::SHADER_TYPE to_dg(ShaderStage stage) noexcept {
    switch (stage) {
    case ShaderStage::Vertex:
        return dg::SHADER_TYPE_VERTEX;
    case ShaderStage::Pixel:
        return dg::SHADER_TYPE_PIXEL;
    case ShaderStage::Compute:
        return dg::SHADER_TYPE_COMPUTE;
    }
    return dg::SHADER_TYPE_UNKNOWN;
}

ShaderStage from_dg(dg::SHADER_TYPE type) noexcept {
    switch (type) {
    case dg::SHADER_TYPE_PIXEL:
        return ShaderStage::Pixel;
    case dg::SHADER_TYPE_COMPUTE:
        return ShaderStage::Compute;
    default:
        return ShaderStage::Vertex;
    }
}

dg::COMPARISON_FUNCTION to_dg(CompareFunc func) noexcept {
    switch (func) {
    case CompareFunc::Never:
        return dg::COMPARISON_FUNC_NEVER;
    case CompareFunc::Less:
        return dg::COMPARISON_FUNC_LESS;
    case CompareFunc::LessEqual:
        return dg::COMPARISON_FUNC_LESS_EQUAL;
    case CompareFunc::Equal:
        return dg::COMPARISON_FUNC_EQUAL;
    case CompareFunc::Greater:
        return dg::COMPARISON_FUNC_GREATER;
    case CompareFunc::GreaterEqual:
        return dg::COMPARISON_FUNC_GREATER_EQUAL;
    case CompareFunc::Always:
        break;
    }
    return dg::COMPARISON_FUNC_ALWAYS;
}

dg::RESOURCE_STATE to_dg(ResourceState state) noexcept {
    switch (state) {
    case ResourceState::Undefined:
        return dg::RESOURCE_STATE_UNDEFINED;
    case ResourceState::RenderTarget:
        return dg::RESOURCE_STATE_RENDER_TARGET;
    case ResourceState::DepthWrite:
        return dg::RESOURCE_STATE_DEPTH_WRITE;
    case ResourceState::DepthRead:
        return dg::RESOURCE_STATE_DEPTH_READ;
    case ResourceState::ShaderResource:
        return dg::RESOURCE_STATE_SHADER_RESOURCE;
    case ResourceState::UnorderedAccess:
        return dg::RESOURCE_STATE_UNORDERED_ACCESS;
    case ResourceState::CopySource:
        return dg::RESOURCE_STATE_COPY_SOURCE;
    case ResourceState::CopyDest:
        return dg::RESOURCE_STATE_COPY_DEST;
    }
    return dg::RESOURCE_STATE_UNKNOWN;
}

bool apply_adapter_choice(std::string_view text, DeviceDesc& desc) {
    if (text == "discrete") {
        desc.adapter_preference = AdapterPreference::HighPerformance;
    } else if (text == "integrated") {
        desc.adapter_preference = AdapterPreference::LowPower;
    } else if (text == "software") {
        desc.adapter_preference = AdapterPreference::Software;
    } else {
        std::uint32_t index = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), index);
        if (ec != std::errc{} || end != text.data() + text.size() || text.empty()) {
            return false;
        }
        desc.adapter_index = index;
        return true;
    }
    desc.adapter_index.reset();
    return true;
}

} // namespace f3d::render::rhi
