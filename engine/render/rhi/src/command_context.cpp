// SPDX-License-Identifier: Apache-2.0
#include "diligent.hpp"

#include "f3d/core/assert.hpp"
#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"

#include "Buffer.h"
#include "MapHelper.hpp"
#include "PipelineState.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"
#include "TextureView.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

namespace f3d::render::rhi {
namespace {
constexpr auto kTransition = dg::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
dg::IDeviceContext* ctx_of(void* p) noexcept {
    return static_cast<dg::IDeviceContext*>(p);
}
} // namespace

void CommandContext::set_render_targets(std::span<const Texture> colors, const Texture& depth) {
    std::vector<dg::ITextureView*> rtvs;
    rtvs.reserve(colors.size());
    for (const Texture& c : colors) {
        rtvs.push_back(device_.impl().render_target_view(native_as<dg::ITexture>(c)));
    }
    dg::ITextureView* dsv = nullptr;
    if (auto* d = native_as<dg::ITexture>(depth)) {
        dsv = d->GetDefaultView(dg::TEXTURE_VIEW_DEPTH_STENCIL);
    }
    ctx_of(context_)->SetRenderTargets(static_cast<dg::Uint32>(rtvs.size()), rtvs.data(), dsv, kTransition);
}

void CommandContext::set_render_target(const Texture& color, const Texture& depth) {
    set_render_targets(std::span<const Texture>(&color, color ? 1u : 0u), depth);
}

void CommandContext::set_render_targets(std::span<const TextureView> colors, const TextureView& depth) {
    std::vector<dg::ITextureView*> rtvs;
    rtvs.reserve(colors.size());
    for (const TextureView& c : colors) {
        rtvs.push_back(native_as<dg::ITextureView>(c));
    }
    ctx_of(context_)->SetRenderTargets(static_cast<dg::Uint32>(rtvs.size()), rtvs.data(),
                                       native_as<dg::ITextureView>(depth), kTransition);
}

void CommandContext::set_render_target(const TextureView& color, const TextureView& depth) {
    set_render_targets(std::span<const TextureView>(&color, color ? 1u : 0u), depth);
}

void CommandContext::set_viewport(float x, float y, float width, float height) {
    dg::Viewport vp;
    vp.TopLeftX = x;
    vp.TopLeftY = y;
    vp.Width = width;
    vp.Height = height;
    ctx_of(context_)->SetViewports(1, &vp, 0, 0);
}

void CommandContext::clear_color(const Texture& target, const std::array<float, 4>& rgba) {
    if (dg::ITextureView* rtv = device_.impl().render_target_view(native_as<dg::ITexture>(target))) {
        ctx_of(context_)->ClearRenderTarget(rtv, rgba.data(), kTransition);
    }
}

void CommandContext::clear_color_uint(const Texture& target, std::uint32_t value) {
    if (dg::ITextureView* rtv = device_.impl().render_target_view(native_as<dg::ITexture>(target))) {
        const dg::Uint32 rgba[4] = {value, value, value, value};
        ctx_of(context_)->ClearRenderTarget(rtv, rgba, kTransition);
    }
}

void CommandContext::clear_depth(const Texture& target, float depth) {
    if (auto* t = native_as<dg::ITexture>(target)) {
        ctx_of(context_)->ClearDepthStencil(t->GetDefaultView(dg::TEXTURE_VIEW_DEPTH_STENCIL), dg::CLEAR_DEPTH_FLAG,
                                            depth, 0, kTransition);
    }
}

void CommandContext::clear_color(const TextureView& target, const std::array<float, 4>& rgba) {
    if (auto* rtv = native_as<dg::ITextureView>(target)) {
        ctx_of(context_)->ClearRenderTarget(rtv, rgba.data(), kTransition);
    }
}

void CommandContext::clear_depth(const TextureView& target, float depth) {
    if (auto* dsv = native_as<dg::ITextureView>(target)) {
        ctx_of(context_)->ClearDepthStencil(dsv, dg::CLEAR_DEPTH_FLAG, depth, 0, kTransition);
    }
}

void CommandContext::update_texture(const Texture& texture, std::uint32_t x, std::uint32_t y, std::uint32_t width,
                                    std::uint32_t height, const void* data, std::size_t row_pitch, std::uint32_t slice) {
    auto* t = native_as<dg::ITexture>(texture);
    if (!t || !data || width == 0 || height == 0) {
        return;
    }
    dg::Box box(x, x + width, y, y + height);
    dg::TextureSubResData sub;
    sub.pData = data;
    sub.Stride = row_pitch;
    ctx_of(context_)->UpdateTexture(t, 0, slice, box, sub, kTransition, kTransition);
}

void CommandContext::generate_mips(const Texture& texture) {
    auto* t = native_as<dg::ITexture>(texture);
    if (!t || (t->GetDesc().MiscFlags & dg::MISC_TEXTURE_FLAG_GENERATE_MIPS) == 0) {
        F3D_LOG_ERROR("rhi: generate_mips needs a texture created with generate_mips");
        return;
    }
    // The backends re-commit the bound render targets afterwards: unbind them, they
    // may be this texture (or one already moved to another state).
    ctx_of(context_)->SetRenderTargets(0, nullptr, nullptr, kTransition);
    ctx_of(context_)->GenerateMips(t->GetDefaultView(dg::TEXTURE_VIEW_SHADER_RESOURCE));
}

void CommandContext::set_pipeline(const Pipeline& pipeline) {
    ctx_of(context_)->SetPipelineState(native_as<dg::IPipelineState>(pipeline));
}

void CommandContext::commit(const Bindings& bindings) {
    ctx_of(context_)->CommitShaderResources(native_as<dg::IShaderResourceBinding>(bindings), kTransition);
}

void CommandContext::set_vertex_buffer(const Buffer& buffer, std::uint64_t offset) {
    dg::IBuffer* buffers[] = {native_as<dg::IBuffer>(buffer)};
    const dg::Uint64 offsets[] = {offset};
    ctx_of(context_)->SetVertexBuffers(0, 1, buffers, offsets, kTransition, dg::SET_VERTEX_BUFFERS_FLAG_RESET);
}

void CommandContext::set_vertex_buffers(std::initializer_list<const Buffer*> buffers) {
    std::array<dg::IBuffer*, 8> native{};
    std::array<dg::Uint64, 8> offsets{};
    dg::Uint32 count = 0;
    for (const Buffer* buffer : buffers) {
        if (count < native.size()) {
            native[count++] = buffer ? native_as<dg::IBuffer>(*buffer) : nullptr;
        }
    }
    ctx_of(context_)->SetVertexBuffers(0, count, native.data(), offsets.data(), kTransition,
                                       dg::SET_VERTEX_BUFFERS_FLAG_RESET);
}

void CommandContext::set_index_buffer(const Buffer& buffer, std::uint64_t offset) {
    ctx_of(context_)->SetIndexBuffer(native_as<dg::IBuffer>(buffer), offset, kTransition);
}

void CommandContext::draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex) {
    dg::DrawAttribs attribs;
    attribs.NumVertices = vertex_count;
    attribs.NumInstances = instance_count;
    attribs.StartVertexLocation = first_vertex;
    attribs.Flags = dg::DRAW_FLAG_VERIFY_ALL;
    ctx_of(context_)->Draw(attribs);
}

void CommandContext::draw_indexed(std::uint32_t index_count, IndexType type, std::uint32_t instance_count,
                                  std::uint32_t first_index, std::int32_t base_vertex) {
    dg::DrawIndexedAttribs attribs;
    attribs.NumIndices = index_count;
    attribs.IndexType = type == IndexType::UInt16 ? dg::VT_UINT16 : dg::VT_UINT32;
    attribs.NumInstances = instance_count;
    attribs.FirstIndexLocation = first_index;
    attribs.BaseVertex = static_cast<dg::Uint32>(base_vertex);
    attribs.Flags = dg::DRAW_FLAG_VERIFY_ALL;
    ctx_of(context_)->DrawIndexed(attribs);
}

void CommandContext::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    dg::DispatchComputeAttribs attribs;
    attribs.ThreadGroupCountX = x;
    attribs.ThreadGroupCountY = y;
    attribs.ThreadGroupCountZ = z;
    ctx_of(context_)->DispatchCompute(attribs);
}

void CommandContext::update_buffer(const Buffer& buffer, const void* data, std::size_t size, std::uint64_t offset) {
    auto* b = native_as<dg::IBuffer>(buffer);
    if (!b || !data || size == 0) {
        return;
    }
    if (b->GetDesc().Usage == dg::USAGE_DYNAMIC) {
        F3D_ASSERT(offset == 0, "dynamic buffers are rewritten whole");
        void* mapped = nullptr;
        ctx_of(context_)->MapBuffer(b, dg::MAP_WRITE, dg::MAP_FLAG_DISCARD, mapped);
        if (mapped) {
            std::memcpy(mapped, data, size);
            ctx_of(context_)->UnmapBuffer(b, dg::MAP_WRITE);
        }
    } else {
        ctx_of(context_)->UpdateBuffer(b, offset, size, data, kTransition);
    }
}

core::Image CommandContext::read_texture(const Texture& texture) {
    F3D_ZONE;
    core::Image image;
    auto* source = native_as<dg::ITexture>(texture);
    if (!source) {
        return image;
    }
    dg::TextureDesc desc = source->GetDesc();
    const Format format = from_dg(desc.Format);
    const bool bgra = format == Format::BGRA8Unorm || format == Format::BGRA8UnormSrgb;
    if (format_size(format) != 4 || !(bgra || format == Format::RGBA8Unorm || format == Format::RGBA8UnormSrgb)) {
        F3D_LOG_ERROR("rhi: read_texture supports 8-bit RGBA/BGRA textures only");
        return image;
    }
    desc.Name = "Readback staging";
    desc.Usage = dg::USAGE_STAGING;
    desc.BindFlags = dg::BIND_NONE;
    desc.CPUAccessFlags = dg::CPU_ACCESS_READ;
    desc.MiscFlags = dg::MISC_TEXTURE_FLAG_NONE;
    desc.MipLevels = 1;
    dg::RefCntAutoPtr<dg::ITexture> staging;
    device_.impl().device->CreateTexture(desc, nullptr, &staging);
    if (!staging) {
        return image;
    }
    dg::CopyTextureAttribs copy(source, kTransition, staging, kTransition);
    dg::IDeviceContext* ctx = ctx_of(context_);
    ctx->CopyTexture(copy);
    ctx->WaitForIdle();

    dg::MappedTextureSubresource mapped;
    ctx->MapTextureSubresource(staging, 0, 0, dg::MAP_READ, dg::MAP_FLAG_DO_NOT_WAIT, nullptr, mapped);
    if (!mapped.pData) {
        return image;
    }
    image.width = desc.Width;
    image.height = desc.Height;
    image.rgba.resize(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
    for (dg::Uint32 y = 0; y < desc.Height; ++y) {
        const auto* src = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(mapped.Stride) * y;
        std::uint8_t* dst = image.rgba.data() + static_cast<std::size_t>(desc.Width) * 4 * y;
        for (dg::Uint32 x = 0; x < desc.Width; ++x) {
            dst[x * 4 + 0] = src[x * 4 + (bgra ? 2 : 0)];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + (bgra ? 0 : 2)];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }
    ctx->UnmapTextureSubresource(staging, 0, 0);
    return image;
}

void CommandContext::transition(std::span<const TextureTransition> transitions) {
    std::vector<dg::StateTransitionDesc> barriers;
    barriers.reserve(transitions.size());
    for (const TextureTransition& t : transitions) {
        auto* texture = native_as<dg::ITexture>(t.texture);
        if (!texture || t.state == ResourceState::Undefined) {
            continue;
        }
        const dg::RESOURCE_STATE target = to_dg(t.state);
        const dg::RESOURCE_STATE current = texture->GetState();
        if (current == dg::RESOURCE_STATE_UNKNOWN || // not tracked: left to the caller
            (current == target && target != dg::RESOURCE_STATE_UNORDERED_ACCESS)) {
            continue;
        }
        // Unknown old state: Diligent uses the state it tracks.
        barriers.emplace_back(texture, dg::RESOURCE_STATE_UNKNOWN, target, dg::STATE_TRANSITION_FLAG_UPDATE_STATE);
    }
    if (!barriers.empty()) {
        ctx_of(context_)->TransitionResourceStates(static_cast<dg::Uint32>(barriers.size()), barriers.data());
    }
}

ResourceState CommandContext::state(const Texture& texture) const {
    const auto* t = native_as<dg::ITexture>(texture);
    if (!t) {
        return ResourceState::Undefined;
    }
    switch (t->GetState()) {
    case dg::RESOURCE_STATE_RENDER_TARGET:
        return ResourceState::RenderTarget;
    case dg::RESOURCE_STATE_DEPTH_WRITE:
        return ResourceState::DepthWrite;
    case dg::RESOURCE_STATE_DEPTH_READ:
        return ResourceState::DepthRead;
    case dg::RESOURCE_STATE_SHADER_RESOURCE:
        return ResourceState::ShaderResource;
    case dg::RESOURCE_STATE_UNORDERED_ACCESS:
        return ResourceState::UnorderedAccess;
    case dg::RESOURCE_STATE_COPY_SOURCE:
        return ResourceState::CopySource;
    case dg::RESOURCE_STATE_COPY_DEST:
        return ResourceState::CopyDest;
    default:
        return ResourceState::Undefined;
    }
}

void CommandContext::flush() {
    ctx_of(context_)->Flush();
}

std::vector<std::uint32_t> CommandContext::read_texture_uint32(const Texture& texture) {
    F3D_ZONE;
    std::vector<std::uint32_t> out;
    auto* source = native_as<dg::ITexture>(texture);
    if (!source) {
        return out;
    }
    dg::TextureDesc desc = source->GetDesc();
    const Format format = from_dg(desc.Format);
    if (format != Format::R32Uint && format != Format::R32Float) {
        F3D_LOG_ERROR("rhi: read_texture_uint32 supports R32_UINT / R32_FLOAT only");
        return out;
    }
    desc.Name = "Readback staging";
    desc.Usage = dg::USAGE_STAGING;
    desc.BindFlags = dg::BIND_NONE;
    desc.CPUAccessFlags = dg::CPU_ACCESS_READ;
    desc.MiscFlags = dg::MISC_TEXTURE_FLAG_NONE;
    desc.MipLevels = 1;
    dg::RefCntAutoPtr<dg::ITexture> staging;
    device_.impl().device->CreateTexture(desc, nullptr, &staging);
    if (!staging) {
        return out;
    }
    dg::CopyTextureAttribs copy(source, kTransition, staging, kTransition);
    dg::IDeviceContext* ctx = ctx_of(context_);
    ctx->CopyTexture(copy);
    ctx->WaitForIdle();

    dg::MappedTextureSubresource mapped;
    ctx->MapTextureSubresource(staging, 0, 0, dg::MAP_READ, dg::MAP_FLAG_DO_NOT_WAIT, nullptr, mapped);
    if (!mapped.pData) {
        return out;
    }
    out.resize(static_cast<std::size_t>(desc.Width) * desc.Height);
    for (dg::Uint32 y = 0; y < desc.Height; ++y) {
        const auto* src = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(mapped.Stride) * y;
        std::memcpy(out.data() + static_cast<std::size_t>(desc.Width) * y, src,
                    static_cast<std::size_t>(desc.Width) * sizeof(std::uint32_t));
    }
    ctx->UnmapTextureSubresource(staging, 0, 0);
    return out;
}

std::vector<float> CommandContext::read_texture_float(const Texture& texture, std::uint32_t mip, std::uint32_t slice) {
    F3D_ZONE;
    std::vector<float> out;
    auto* source = native_as<dg::ITexture>(texture);
    if (!source) {
        return out;
    }
    const dg::TextureDesc& source_desc = source->GetDesc();
    const Format format = from_dg(source_desc.Format);
    if ((format != Format::RGBA16Float && format != Format::RGBA32Float) || mip >= source_desc.MipLevels ||
        slice >= source_desc.ArraySize) {
        F3D_LOG_ERROR("rhi: read_texture_float supports RGBA16F / RGBA32F (and an existing mip and slice) only");
        return out;
    }
    dg::TextureDesc desc;
    desc.Name = "Readback staging";
    desc.Type = dg::RESOURCE_DIM_TEX_2D;
    desc.Width = std::max(source_desc.Width >> mip, 1u);
    desc.Height = std::max(source_desc.Height >> mip, 1u);
    desc.Format = source_desc.Format;
    desc.Usage = dg::USAGE_STAGING;
    desc.CPUAccessFlags = dg::CPU_ACCESS_READ;
    dg::RefCntAutoPtr<dg::ITexture> staging;
    device_.impl().device->CreateTexture(desc, nullptr, &staging);
    if (!staging) {
        return out;
    }
    dg::CopyTextureAttribs copy(source, kTransition, staging, kTransition);
    copy.SrcMipLevel = mip;
    copy.SrcSlice = slice;
    dg::IDeviceContext* ctx = ctx_of(context_);
    ctx->CopyTexture(copy);
    ctx->WaitForIdle();

    dg::MappedTextureSubresource mapped;
    ctx->MapTextureSubresource(staging, 0, 0, dg::MAP_READ, dg::MAP_FLAG_DO_NOT_WAIT, nullptr, mapped);
    if (!mapped.pData) {
        return out;
    }
    const std::size_t texels = static_cast<std::size_t>(desc.Width) * desc.Height;
    out.resize(texels * 4);
    for (dg::Uint32 y = 0; y < desc.Height; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(mapped.Stride) * y;
        float* dst = out.data() + static_cast<std::size_t>(desc.Width) * 4 * y;
        if (format == Format::RGBA32Float) {
            std::memcpy(dst, row, static_cast<std::size_t>(desc.Width) * 16);
        } else {
            const auto* half = reinterpret_cast<const std::uint16_t*>(row);
            for (dg::Uint32 i = 0; i < desc.Width * 4; ++i) {
                dst[i] = glm::unpackHalf1x16(half[i]);
            }
        }
    }
    ctx->UnmapTextureSubresource(staging, 0, 0);
    return out;
}

std::vector<std::uint8_t> CommandContext::read_buffer(const Buffer& buffer) {
    F3D_ZONE;
    std::vector<std::uint8_t> out;
    auto* source = native_as<dg::IBuffer>(buffer);
    if (!source) {
        return out;
    }
    dg::BufferDesc desc;
    desc.Name = "Buffer readback";
    desc.Size = source->GetDesc().Size;
    desc.Usage = dg::USAGE_STAGING;
    desc.CPUAccessFlags = dg::CPU_ACCESS_READ;
    dg::RefCntAutoPtr<dg::IBuffer> staging;
    device_.impl().device->CreateBuffer(desc, nullptr, &staging);
    if (!staging) {
        return out;
    }
    dg::IDeviceContext* ctx = ctx_of(context_);
    ctx->CopyBuffer(source, 0, kTransition, staging, 0, desc.Size, kTransition);
    ctx->WaitForIdle();
    void* mapped = nullptr;
    ctx->MapBuffer(staging, dg::MAP_READ, dg::MAP_FLAG_DO_NOT_WAIT, mapped);
    if (mapped) {
        out.resize(desc.Size);
        std::memcpy(out.data(), mapped, desc.Size);
        ctx->UnmapBuffer(staging, dg::MAP_READ);
    }
    return out;
}

void CommandContext::wait_idle() {
    ctx_of(context_)->Flush();
    ctx_of(context_)->WaitForIdle();
}

} // namespace f3d::render::rhi
