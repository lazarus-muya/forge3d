// SPDX-License-Identifier: Apache-2.0
#include "diligent.hpp"

#include "f3d/core/log.hpp"

#include "PipelineResourceSignature.h"
#include "PipelineState.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"
#include "TextureView.h"

#include <algorithm>
#include <array>
#include <string>

namespace f3d::render::rhi {

// --- ObjectRef ---------------------------------------------------------------------

namespace detail {

namespace {
dg::IObject* as_object(void* p) noexcept {
    return static_cast<dg::IObject*>(p);
}
} // namespace

ObjectRef::ObjectRef(const ObjectRef& other) noexcept : object_(other.object_) {
    if (object_) {
        as_object(object_)->AddRef();
    }
}

ObjectRef& ObjectRef::operator=(const ObjectRef& other) noexcept {
    if (this != &other) {
        if (other.object_) {
            as_object(other.object_)->AddRef();
        }
        reset();
        object_ = other.object_;
    }
    return *this;
}

ObjectRef& ObjectRef::operator=(ObjectRef&& other) noexcept {
    if (this != &other) {
        reset();
        object_ = other.object_;
        other.object_ = nullptr;
    }
    return *this;
}

ObjectRef::~ObjectRef() {
    reset();
}

void ObjectRef::reset() noexcept {
    if (object_) {
        as_object(object_)->Release();
        object_ = nullptr;
    }
}

} // namespace detail

// --- Resources -----------------------------------------------------------------------

std::uint64_t Buffer::size() const {
    const auto* b = native_as<dg::IBuffer>(*this);
    return b ? b->GetDesc().Size : 0;
}

std::uint32_t Texture::width() const {
    const auto* t = native_as<dg::ITexture>(*this);
    return t ? t->GetDesc().Width : 0;
}

std::uint32_t Texture::height() const {
    const auto* t = native_as<dg::ITexture>(*this);
    return t ? t->GetDesc().Height : 0;
}

Format Texture::format() const {
    const auto* t = native_as<dg::ITexture>(*this);
    return t ? from_dg(t->GetDesc().Format) : Format::Unknown;
}

std::uint32_t Texture::mip_levels() const {
    const auto* t = native_as<dg::ITexture>(*this);
    return t ? t->GetDesc().MipLevels : 0;
}

std::uint32_t Texture::array_size() const {
    const auto* t = native_as<dg::ITexture>(*this);
    return t ? t->GetDesc().ArraySize : 0;
}

TextureView Texture::view(const TextureViewDesc& desc) const {
    auto* t = native_as<dg::ITexture>(*this);
    if (!t) {
        return {};
    }
    const dg::TextureDesc& td = t->GetDesc();
    dg::TextureViewDesc vd;
    vd.MostDetailedMip = desc.first_mip;
    vd.FirstArraySlice = desc.first_slice;
    const dg::Uint32 slices = desc.slice_count ? desc.slice_count : td.ArraySize - desc.first_slice;
    vd.NumArraySlices = slices;
    const bool layered = td.IsArray() || td.IsCube();
    switch (desc.type) {
    case ViewType::ShaderResource:
        vd.ViewType = dg::TEXTURE_VIEW_SHADER_RESOURCE;
        vd.NumMipLevels = desc.mip_count ? desc.mip_count : td.MipLevels - desc.first_mip;
        vd.TextureDim = td.Type;
        if (td.IsCube() && (desc.as_array || desc.first_slice != 0 || slices != td.ArraySize)) {
            vd.TextureDim = dg::RESOURCE_DIM_TEX_2D_ARRAY;
        }
        break;
    case ViewType::RenderTarget:
        vd.ViewType = dg::TEXTURE_VIEW_RENDER_TARGET;
        vd.NumMipLevels = 1;
        vd.TextureDim = layered ? dg::RESOURCE_DIM_TEX_2D_ARRAY : dg::RESOURCE_DIM_TEX_2D;
        break;
    case ViewType::DepthStencil:
        vd.ViewType = dg::TEXTURE_VIEW_DEPTH_STENCIL;
        vd.NumMipLevels = 1;
        vd.TextureDim = layered ? dg::RESOURCE_DIM_TEX_2D_ARRAY : dg::RESOURCE_DIM_TEX_2D;
        break;
    case ViewType::UnorderedAccess:
        vd.ViewType = dg::TEXTURE_VIEW_UNORDERED_ACCESS;
        vd.NumMipLevels = 1;
        vd.TextureDim = layered ? dg::RESOURCE_DIM_TEX_2D_ARRAY : dg::RESOURCE_DIM_TEX_2D;
        break;
    }
    if (!layered) {
        vd.FirstArraySlice = 0;
        vd.NumArraySlices = 1;
    }
    dg::ITextureView* view = nullptr;
    t->CreateView(vd, &view);
    if (!view) {
        F3D_LOG_ERROR("rhi: failed to create a view of texture '{}' (mip {}, slice {})", td.Name ? td.Name : "",
                      desc.first_mip, desc.first_slice);
    }
    return TextureView{view ? static_cast<dg::IObject*>(view) : nullptr}; // adopts the creation reference
}

Texture TextureView::texture() const {
    auto* v = native_as<dg::ITextureView>(*this);
    return v ? make_handle<Texture>(v->GetTexture()) : Texture{};
}

std::uint32_t TextureView::width() const {
    auto* v = native_as<dg::ITextureView>(*this);
    if (!v) {
        return 0;
    }
    return std::max<std::uint32_t>(v->GetTexture()->GetDesc().Width >> v->GetDesc().MostDetailedMip, 1u);
}

std::uint32_t TextureView::height() const {
    auto* v = native_as<dg::ITextureView>(*this);
    if (!v) {
        return 0;
    }
    return std::max<std::uint32_t>(v->GetTexture()->GetDesc().Height >> v->GetDesc().MostDetailedMip, 1u);
}

ShaderStage Shader::stage() const {
    const auto* s = native_as<dg::IShader>(*this);
    return s ? from_dg(s->GetDesc().ShaderType) : ShaderStage::Vertex;
}

namespace {

constexpr std::array<dg::SHADER_TYPE, 3> kStages = {dg::SHADER_TYPE_VERTEX, dg::SHADER_TYPE_PIXEL,
                                                    dg::SHADER_TYPE_COMPUTE};

/// Sets the object `pick(variable)` returns on every stage that declares `name`; false if none does.
template <class Pick>
bool set_variable(dg::IShaderResourceBinding* srb, std::string_view name, Pick&& pick) {
    if (!srb) {
        return false;
    }
    // Only ask the stages the pipeline has (Diligent warns about the others).
    const dg::PipelineResourceSignatureDesc& signature = srb->GetPipelineResourceSignature()->GetDesc();
    dg::SHADER_TYPE pipeline_stages = dg::SHADER_TYPE_UNKNOWN;
    for (dg::Uint32 i = 0; i < signature.NumResources; ++i) {
        pipeline_stages |= signature.Resources[i].ShaderStages;
    }
    const std::string key(name);
    bool found = false;
    for (const auto stage : kStages) {
        if ((pipeline_stages & stage) == 0) {
            continue;
        }
        if (dg::IShaderResourceVariable* var = srb->GetVariableByName(stage, key.c_str())) {
            dg::IDeviceObject* object = pick(*var);
            if (!object) {
                continue;
            }
            // Re-setting the bound resource is a no-op. Binding a different resource to a
            // used Bindings is not allowed (the GPU may still read it): create new Bindings.
            if (var->Get() != object) {
                var->Set(object);
            }
            found = true;
        }
    }
    if (!found) {
        F3D_LOG_WARN("rhi: pipeline '{}' has no shader variable '{}'", signature.Name, name);
    }
    return found;
}

} // namespace

bool Bindings::set(std::string_view name, const Texture& texture) {
    auto* t = native_as<dg::ITexture>(texture);
    return t && set_variable(native_as<dg::IShaderResourceBinding>(*this), name, [&](dg::IShaderResourceVariable&) {
               return static_cast<dg::IDeviceObject*>(t->GetDefaultView(dg::TEXTURE_VIEW_SHADER_RESOURCE));
           });
}

bool Bindings::set_uav(std::string_view name, const Texture& texture) {
    auto* t = native_as<dg::ITexture>(texture);
    return t && set_variable(native_as<dg::IShaderResourceBinding>(*this), name, [&](dg::IShaderResourceVariable&) {
               return static_cast<dg::IDeviceObject*>(t->GetDefaultView(dg::TEXTURE_VIEW_UNORDERED_ACCESS));
           });
}

bool Bindings::set(std::string_view name, const TextureView& view) {
    auto* v = native_as<dg::ITextureView>(view);
    return v && set_variable(native_as<dg::IShaderResourceBinding>(*this), name,
                             [&](dg::IShaderResourceVariable&) { return static_cast<dg::IDeviceObject*>(v); });
}

bool Bindings::set(std::string_view name, const Buffer& buffer) {
    auto* b = native_as<dg::IBuffer>(buffer);
    // Constant buffers bind the buffer; structured buffers its SRV or UAV, as the shader declares.
    return b && set_variable(native_as<dg::IShaderResourceBinding>(*this), name,
                             [&](dg::IShaderResourceVariable& var) -> dg::IDeviceObject* {
                                 dg::ShaderResourceDesc desc;
                                 var.GetResourceDesc(desc);
                                 switch (desc.Type) {
                                 case dg::SHADER_RESOURCE_TYPE_BUFFER_SRV:
                                     return b->GetDefaultView(dg::BUFFER_VIEW_SHADER_RESOURCE);
                                 case dg::SHADER_RESOURCE_TYPE_BUFFER_UAV:
                                     return b->GetDefaultView(dg::BUFFER_VIEW_UNORDERED_ACCESS);
                                 default:
                                     return b;
                                 }
                             });
}

Bindings Pipeline::create_bindings() const {
    auto* pso = native_as<dg::IPipelineState>(*this);
    if (!pso) {
        return {};
    }
    dg::IShaderResourceBinding* srb = nullptr;
    pso->CreateShaderResourceBinding(&srb, true);
    return Bindings{srb ? static_cast<dg::IObject*>(srb) : nullptr}; // adopts the creation reference
}

} // namespace f3d::render::rhi
