// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/render/rhi/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace f3d::render::rhi {

namespace detail {

/// Reference-counted handle to a backend object. Copying shares the object;
/// the object is released when the last handle goes away. A default-constructed
/// handle is empty (false).
class ObjectRef {
public:
    ObjectRef() noexcept = default;
    /// Adopts one reference to `object` (does not add one).
    explicit ObjectRef(void* object) noexcept : object_(object) {}
    ObjectRef(const ObjectRef& other) noexcept;
    ObjectRef(ObjectRef&& other) noexcept : object_(other.object_) { other.object_ = nullptr; }
    ObjectRef& operator=(const ObjectRef& other) noexcept;
    ObjectRef& operator=(ObjectRef&& other) noexcept;
    ~ObjectRef();

    void reset() noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return object_ != nullptr; }
    /// The backend object (internal use by f3d_render_rhi).
    [[nodiscard]] void* native() const noexcept { return object_; }

    friend bool operator==(const ObjectRef& a, const ObjectRef& b) noexcept { return a.object_ == b.object_; }

private:
    void* object_ = nullptr;
};

} // namespace detail

class Buffer : public detail::ObjectRef {
public:
    using ObjectRef::ObjectRef;
    [[nodiscard]] std::uint64_t size() const;
};

enum class ViewType : std::uint8_t { ShaderResource, RenderTarget, DepthStencil, UnorderedAccess };

/// A range of a texture's mips and slices (cube faces are slices 0-5).
struct TextureViewDesc {
    ViewType type = ViewType::ShaderResource;
    std::uint32_t first_mip = 0;
    std::uint32_t mip_count = 0; ///< 0: every mip from first_mip (shader resource), else 1
    std::uint32_t first_slice = 0;
    std::uint32_t slice_count = 0; ///< 0: every slice from first_slice
    bool as_array = false;         ///< shader resource of a cube viewed as a 2D array
};

class Texture;

/// A view of part of a texture (one mip, one slice, ...). Keeps its texture alive.
class TextureView : public detail::ObjectRef {
public:
    using ObjectRef::ObjectRef;
    [[nodiscard]] Texture texture() const;
    /// Size of the view's first mip.
    [[nodiscard]] std::uint32_t width() const;
    [[nodiscard]] std::uint32_t height() const;
};

class Texture : public detail::ObjectRef {
public:
    using ObjectRef::ObjectRef;
    [[nodiscard]] std::uint32_t width() const;
    [[nodiscard]] std::uint32_t height() const;
    [[nodiscard]] Format format() const;
    [[nodiscard]] std::uint32_t mip_levels() const;
    /// Slices (6 for a cube).
    [[nodiscard]] std::uint32_t array_size() const;
    /// Creates a view of part of the texture (empty on failure).
    [[nodiscard]] TextureView view(const TextureViewDesc& desc) const;
};

class Shader : public detail::ObjectRef {
public:
    using ObjectRef::ObjectRef;
    [[nodiscard]] ShaderStage stage() const;
};

/// Resources bound to one pipeline (textures, buffers). Create with
/// Pipeline::create_bindings, fill with set(), bind with CommandContext::commit.
/// Setting a variable to the resource it already holds is a no-op; to bind a
/// *different* resource after the Bindings were used, create new Bindings
/// (frames in flight may still read the old ones).
class Bindings : public detail::ObjectRef {
public:
    using ObjectRef::ObjectRef;
    /// Binds a texture's shader-resource view. Returns false if no stage declares `name`.
    bool set(std::string_view name, const Texture& texture);
    /// Binds a texture's unordered-access view (RW texture).
    bool set_uav(std::string_view name, const Texture& texture);
    /// Binds a texture view (shader-resource or unordered-access, as the view's type).
    bool set(std::string_view name, const TextureView& view);
    /// Binds a uniform / structured buffer.
    bool set(std::string_view name, const Buffer& buffer);
};

class Pipeline : public detail::ObjectRef {
public:
    using ObjectRef::ObjectRef;
    [[nodiscard]] Bindings create_bindings() const;
};

struct GraphicsPipelineDesc {
    std::string name;
    Shader vs;
    Shader ps;
    std::vector<VertexAttribute> vertex_layout;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    CullMode cull = CullMode::Back;
    bool depth_test = true;
    bool depth_write = true;
    CompareFunc depth_func = CompareFunc::LessEqual;
    std::int32_t depth_bias = 0; ///< constant depth bias (depth-format units)
    float slope_scaled_depth_bias = 0.0f;
    bool depth_clip = true; ///< false: geometry beyond the near/far planes is clamped (shadow pancaking)
    BlendMode blend = BlendMode::Opaque;
    bool wireframe = false;
    std::vector<Format> color_formats;
    Format depth_format = Format::Unknown;
    std::vector<SamplerDesc> samplers;
};

struct ComputePipelineDesc {
    std::string name;
    Shader cs;
    std::vector<SamplerDesc> samplers;
};

} // namespace f3d::render::rhi
