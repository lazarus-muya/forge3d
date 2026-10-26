// SPDX-License-Identifier: Apache-2.0
#pragma once

// Plain data types of the forge3d RHI. Nothing in f3d/render/rhi/*.hpp includes
// a Diligent header; the backend is an implementation detail of f3d_render_rhi.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace f3d::render::rhi {

enum class Backend : std::uint8_t { D3D12, Vulkan };

[[nodiscard]] std::string_view to_string(Backend backend) noexcept;
[[nodiscard]] std::optional<Backend> parse_backend(std::string_view text) noexcept;
/// True if the backend was compiled into this build.
[[nodiscard]] bool is_backend_available(Backend backend) noexcept;
/// D3D12 on Windows, Vulkan elsewhere.
[[nodiscard]] Backend default_backend() noexcept;

enum class Format : std::uint8_t {
    Unknown,
    RGBA8Unorm,
    RGBA8UnormSrgb,
    BGRA8Unorm,
    BGRA8UnormSrgb,
    RGBA16Float,
    RGBA32Float,
    R32Uint,
    R32Float,
    R16Unorm, ///< terrain heights
    RG32Float,
    RGB32Float,
    D32Float,
    // Block-compressed (4x4 blocks).
    BC1Unorm,
    BC1UnormSrgb,
    BC3Unorm,
    BC3UnormSrgb,
    BC4Unorm,
    BC5Unorm,
    BC7Unorm,
    BC7UnormSrgb,
};

[[nodiscard]] bool is_depth_format(Format format) noexcept;
[[nodiscard]] bool is_srgb_format(Format format) noexcept;
[[nodiscard]] std::uint32_t format_size(Format format) noexcept; ///< bytes per pixel / element (0 for block formats)
/// Bytes per 4x4 block of a block-compressed format, 0 otherwise.
[[nodiscard]] std::uint32_t format_block_bytes(Format format) noexcept;
/// Bytes of one level `width` x `height` of `format` (block formats round up to whole blocks).
[[nodiscard]] std::uint64_t texture_level_bytes(Format format, std::uint32_t width, std::uint32_t height) noexcept;

enum class ShaderStage : std::uint8_t { Vertex, Pixel, Compute };

[[nodiscard]] std::string_view to_string(ShaderStage stage) noexcept;

enum class BindFlags : std::uint8_t {
    None = 0,
    VertexBuffer = 1 << 0,
    IndexBuffer = 1 << 1,
    UniformBuffer = 1 << 2,
    ShaderResource = 1 << 3,
    UnorderedAccess = 1 << 4,
    RenderTarget = 1 << 5,
    DepthStencil = 1 << 6,
};

[[nodiscard]] constexpr BindFlags operator|(BindFlags a, BindFlags b) noexcept {
    using U = std::underlying_type_t<BindFlags>;
    return static_cast<BindFlags>(static_cast<U>(a) | static_cast<U>(b));
}
[[nodiscard]] constexpr bool has_flag(BindFlags flags, BindFlags flag) noexcept {
    using U = std::underlying_type_t<BindFlags>;
    return (static_cast<U>(flags) & static_cast<U>(flag)) != 0;
}

/// How a resource's contents change.
enum class Usage : std::uint8_t {
    Default,   ///< GPU memory, updated occasionally with CommandContext::update_buffer
    Immutable, ///< initial data only
    Dynamic,   ///< rewritten (usually every frame) with CommandContext::update_buffer
};

struct BufferDesc {
    std::string name;
    std::uint64_t size = 0;
    BindFlags bind = BindFlags::None;
    Usage usage = Usage::Default;
    std::uint32_t stride = 0; ///< element stride for structured buffers
};

/// Initial data of one texture level.
struct TextureLevelData {
    const void* data = nullptr;
    std::size_t row_pitch = 0;
};

enum class TextureDimension : std::uint8_t {
    Tex2D,
    Tex2DArray, ///< `array_size` slices
    Cube,       ///< six faces (+X, -X, +Y, -Y, +Z, -Z); `array_size` is ignored
    Tex3D,      ///< a volume `array_size` texels deep
};

struct TextureDesc {
    std::string name;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    Format format = Format::RGBA8Unorm;
    BindFlags bind = BindFlags::ShaderResource;
    std::uint32_t mip_levels = 1; ///< 0: the full chain
    TextureDimension dimension = TextureDimension::Tex2D;
    std::uint32_t array_size = 1; ///< slices (Tex2DArray) or depth (Tex3D)
    bool generate_mips = false; ///< allows CommandContext::generate_mips
};

/// What a texture is used for at a point in a frame (the render graph emits the
/// transitions between them).
enum class ResourceState : std::uint8_t {
    Undefined, ///< contents not needed
    RenderTarget,
    DepthWrite,
    DepthRead,
    ShaderResource,
    UnorderedAccess,
    CopySource,
    CopyDest,
};

[[nodiscard]] std::string_view to_string(ResourceState state) noexcept;

enum class IndexType : std::uint8_t { UInt16, UInt32 };
enum class PrimitiveTopology : std::uint8_t { TriangleList, TriangleStrip, LineList };
enum class CullMode : std::uint8_t { None, Back, Front };
enum class CompareFunc : std::uint8_t { Never, Less, LessEqual, Equal, Greater, GreaterEqual, Always };
enum class BlendMode : std::uint8_t { Opaque, AlphaBlend, PremultipliedAlpha, Additive };
/// Anisotropic: linear with up to 8x anisotropy (surfaces seen at grazing angles,
/// e.g. terrain far away); falls back to Linear for comparison samplers.
enum class Filter : std::uint8_t { Point, Linear, Anisotropic };
enum class AddressMode : std::uint8_t { Wrap, Clamp };

/// One vertex shader input. HLSL declares it as `F3D_LOCATION(N) ... : ATTRIBN` (shaders/common.hlsli).
struct VertexAttribute {
    std::uint32_t location = 0;
    std::uint32_t buffer_slot = 0;
    Format format = Format::RGB32Float;
    std::uint32_t offset = 0;
};

/// A sampler for texture `texture_name` (HLSL sampler `<texture_name>_sampler`).
struct SamplerDesc {
    std::string texture_name;
    Filter filter = Filter::Linear;
    AddressMode address = AddressMode::Wrap;
    /// Comparison sampler (HLSL SamplerComparisonState, e.g. shadow-map PCF).
    std::optional<CompareFunc> compare;
};

/// Platform window handle used to create a swapchain.
struct NativeWindow {
    void* hwnd = nullptr;         ///< Windows: HWND
    void* x11_display = nullptr;  ///< Linux: Display*
    std::uint32_t x11_window = 0; ///< Linux: X11 Window
};

enum class AdapterType : std::uint8_t { Unknown, Integrated, Discrete, Software };

[[nodiscard]] std::string_view to_string(AdapterType type) noexcept;

struct AdapterInfo {
    std::uint32_t index = 0;
    std::string name;
    AdapterType type = AdapterType::Unknown;
    std::uint64_t dedicated_memory = 0; ///< bytes
};

/// Which GPU to use when no explicit index is given.
enum class AdapterPreference : std::uint8_t {
    HighPerformance, ///< discrete if present (default)
    LowPower,        ///< integrated if present (closest to the reference hardware)
    Software,        ///< a software rasteriser (WARP on D3D12, lavapipe on Vulkan): golden-image tests
};

struct DeviceDesc {
    Backend backend = default_backend();
    bool allow_fallback = true; ///< try the other backend if this one fails
    std::optional<std::uint32_t> adapter_index;
    AdapterPreference adapter_preference = AdapterPreference::HighPerformance;
    bool validation = false; ///< backend debug/validation layers
};

/// Applies an adapter choice as the editor and CLI spell it: an index, "discrete",
/// "integrated" or "software". False (desc unchanged) for anything else.
bool apply_adapter_choice(std::string_view text, DeviceDesc& desc);

struct DeviceCaps {
    std::string api_version;
    bool compute = false;
    bool bindless = false;
    bool ray_tracing = false;
    bool ray_query = false;
    bool mesh_shaders = false;
};

struct MemoryUsage {
    std::uint64_t vram_used = 0;   ///< bytes in use by this process (0 if unknown)
    std::uint64_t vram_budget = 0; ///< bytes the OS lets this process use (0 if unknown)
};

} // namespace f3d::render::rhi
