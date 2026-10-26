// SPDX-License-Identifier: Apache-2.0
#pragma once

// Sky and image-based lighting on the GPU (phases.md P4.3).
//
//   transmittance + multiple-scattering LUTs  when the atmosphere changes
//   sky-view LUT                              every frame with a physical sky
//   sky pass                                  the background (sky-view LUT + sun and moon discs)
//   environment                               when the sky changes: sky -> cube (with mips),
//                                             GGX-prefiltered specular cube, irradiance SH (slot 0)
//
// The prefilter and SH passes also serve reflection probes (their own cubes and SH
// slots). Without a Sky entity the environment is the hemisphere colours.

#include "f3d/render/raster/atmosphere.hpp"
#include "f3d/render/rhi/command_context.hpp"
#include "f3d/render/rhi/device.hpp"
#include "f3d/render/rhi/shader_library.hpp"

#include <cstdint>

namespace f3d::render::raster {

/// Mips of every prefiltered specular cube (sky and probes); roughness = mip / (kSpecularMips - 1).
inline constexpr std::uint32_t kSpecularMips = 6;
/// Face size of the sky's environment cubes.
inline constexpr std::uint32_t kSkyEnvSize = 64;
/// Environment SH slots: 0 is the sky, probes follow.
inline constexpr std::uint32_t kMaxEnvSlots = 65;

/// GPU layout of shaders/ibl.hlsli `FilterConstants`.
struct FilterConstants {
    glm::vec4 params; ///< roughness, output size, source size, source mips
    glm::uvec4 slot;  ///< x: SH slot
};

class SkyRenderer {
public:
    SkyRenderer(rhi::Device& device, rhi::ShaderLibrary& shaders);

    /// Re-creates the pipelines (after a shader reload).
    void create_pipelines();

    /// Uploads `constants`. With a physical sky (ground_albedo.w = 1), rebuilds the
    /// transmittance and multiple-scattering LUTs if the atmosphere changed and
    /// renders the sky-view LUT. Returns true if the environment must be re-baked
    /// (anything the sky's radiance depends on changed).
    bool update(rhi::CommandContext& ctx, const SkyConstants& constants);
    /// Re-bakes the sky's environment: cube, prefiltered specular cube, SH slot 0.
    void bake_environment(rhi::CommandContext& ctx);

    /// Draws the sky behind the scene (render target and depth bound by the caller).
    /// `view_constants`: the frame's ViewConstants (camera); `exposure`: the exposure state.
    void draw_sky(rhi::CommandContext& ctx, const rhi::Buffer& view_constants, const rhi::Buffer& exposure);
    [[nodiscard]] bool physical() const noexcept { return physical_; }

    /// GGX-prefilters `source` (a cube with a full mip chain) into the kSpecularMips
    /// mips of `target`.
    void prefilter(rhi::CommandContext& ctx, const rhi::Texture& source, const rhi::Texture& target);
    /// Projects `source` onto irradiance SH, stored in `slot`.
    void project_sh(rhi::CommandContext& ctx, const rhi::Texture& source, std::uint32_t slot);

    /// A cube for an environment: RGBA16F, `size` faces, a full mip chain that
    /// generate_mips fills (the capture/source side).
    [[nodiscard]] rhi::Texture create_source_cube(const std::string& name, std::uint32_t size,
                                                  bool render_target) const;
    /// A prefiltered specular cube: RGBA16F, kSpecularMips mips, writable by compute.
    [[nodiscard]] rhi::Texture create_specular_cube(const std::string& name, std::uint32_t size) const;

    [[nodiscard]] const rhi::Texture& environment() const noexcept { return sky_specular_; }
    [[nodiscard]] const rhi::Buffer& sh_buffer() const noexcept { return sh_; }
    [[nodiscard]] const rhi::Buffer& constants() const noexcept { return constants_; }
    /// Look-up tables (tests read them back).
    [[nodiscard]] const rhi::Texture& transmittance_lut() const noexcept { return transmittance_; }
    [[nodiscard]] const rhi::Texture& multiscatter_lut() const noexcept { return multiscatter_; }
    [[nodiscard]] const rhi::Texture& sky_view_lut() const noexcept { return sky_view_; }
    [[nodiscard]] const rhi::Texture& sky_source() const noexcept { return sky_source_; }

private:
    void build_luts(rhi::CommandContext& ctx);
    void render_sky_view(rhi::CommandContext& ctx);
    void set_filter(rhi::CommandContext& ctx, const FilterConstants& filter);

    rhi::Device& device_;
    rhi::ShaderLibrary& shaders_;
    std::uint64_t shader_generation_ = ~0ull;

    rhi::Buffer constants_;        ///< SkyConstants
    rhi::Buffer filter_constants_; ///< FilterConstants (dynamic, per dispatch)
    rhi::Buffer sh_;               ///< float4[kMaxEnvSlots * 9]
    rhi::Texture transmittance_;
    rhi::Texture multiscatter_;
    rhi::Texture sky_view_;
    rhi::Texture sky_source_;   ///< the sky as a cube (full mips)
    rhi::Texture sky_specular_; ///< prefiltered

    rhi::Pipeline transmittance_pipeline_;
    rhi::Pipeline multiscatter_pipeline_;
    rhi::Pipeline sky_view_pipeline_;
    rhi::Pipeline env_pipeline_;
    rhi::Pipeline prefilter_pipeline_;
    rhi::Pipeline sh_pipeline_;
    rhi::Pipeline sky_pipeline_;
    rhi::Bindings sky_bindings_;
    const void* sky_bindings_view_ = nullptr; ///< the view-constants buffer the sky bindings use
    const void* sky_bindings_exposure_ = nullptr;

    SkyConstants last_{};
    bool have_last_ = false;
    bool luts_valid_ = false;
    bool physical_ = false;
};

} // namespace f3d::render::raster
