// SPDX-License-Identifier: Apache-2.0
#pragma once

// Height fog, local fog volumes and froxel volumetric fog (shaders/fog.hlsli). The
// scene's HeightFog and FogVolume components are gathered each frame; the volumetric
// part runs two compute passes over a view-aligned 3D grid (inject + light with
// temporal accumulation, then integrate), and a full-screen pass composites the fog
// over the pre-exposed scene colour. Without volumetric fog only the full-screen
// pass runs (analytic height fog).

#include "f3d/core/math.hpp"
#include "f3d/render/raster/render_settings.hpp"
#include "f3d/render/raster/view.hpp"
#include "f3d/render/rhi/device.hpp"
#include "f3d/render/rhi/shader_library.hpp"
#include "f3d/scene/components.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace f3d::scene {
class World;
}

namespace f3d::render::raster {

inline constexpr std::uint32_t kMaxFogVolumes = 16;

/// GPU layout of shaders/fog.hlsli `FogVolumeGpu`.
struct FogVolumeGpu {
    glm::mat4 world_to_box{1.0f}; ///< world -> [-0.5, 0.5]^3
    glm::vec4 medium{0.0f};       ///< rgb: albedo, a: extinction at the bottom (1/m)
    glm::vec4 shape{0.0f};        ///< x: height falloff (1/m), y: box height (m), z: edge fade
    glm::vec4 emissive{0.0f};     ///< rgb: radiance emitted per metre
};

/// GPU layout of shaders/fog.hlsli `FogConstants`.
struct FogConstants {
    glm::vec4 height{0.0f};      ///< extinction at the base (1/m), falloff (1/m), base height, 1 = height fog
    glm::vec4 albedo{0.0f};      ///< rgb: albedo, w: anisotropy
    glm::vec4 light{0.0f};       ///< sky scale, sun scale, start distance, max opacity
    glm::vec4 range{0.0f};       ///< volumetric depth (0: none), sky distance, volumetric density, history weight
    glm::vec4 grid{0.0f};        ///< froxel grid size, depth jitter
    glm::vec4 jitter{0.0f};      ///< xy: sample offset in the cell (cells)
    glm::vec4 dir_scatter{1.0f}; ///< per directional light: volumetric scattering scale
    glm::vec4 prev_view{0.0f};   ///< row 2 of last frame's view matrix
    glm::mat4 prev_view_projection{1.0f};
    glm::uvec4 counts{0u};       ///< fog volumes, 1 = sky background
    FogVolumeGpu volumes[kMaxFogVolumes];
};

/// A scene's fog for one frame.
struct SceneFog {
    std::optional<scene::HeightFog> height; ///< the first visible HeightFog
    float base_height = 0.0f;               ///< its entity's world height
    std::vector<FogVolumeGpu> volumes;      ///< visible FogVolumes (at most kMaxFogVolumes)

    [[nodiscard]] bool empty() const noexcept { return !height && volumes.empty(); }
    /// Whether the froxel volume is needed: volumetric height fog, or any fog volume.
    [[nodiscard]] bool volumetric() const noexcept { return (height && height->volumetric) || !volumes.empty(); }
};

[[nodiscard]] SceneFog gather_fog(scene::World& world);

/// Optical depth of height fog along origin + dir * s, s in [s0, s1] (`dir` unit): the
/// CPU reference of shaders/fog.hlsli HeightFogOpticalDepth.
[[nodiscard]] float height_fog_optical_depth(const scene::HeightFog& fog, float base_height, const glm::vec3& origin,
                                             const glm::vec3& dir, float s0, float s1) noexcept;

/// Froxel grid size of a view at a quality ({0, 0, 0} for Off).
[[nodiscard]] glm::uvec3 fog_grid_size(std::uint32_t width, std::uint32_t height, FogQuality quality) noexcept;

class FogRenderer {
public:
    FogRenderer(rhi::Device& device, rhi::ShaderLibrary& shaders);

    void create_pipelines();
    /// Gathers the fog of `world` and uploads this frame's constants. False: no fog this
    /// frame (nothing else to call). `sky_background`: the background is a physical sky.
    bool prepare(rhi::CommandContext& ctx, const View& view, scene::World* world,
                 const std::vector<DirectionalLight>& directional, FogQuality quality, bool sky_background);
    /// The froxel passes run this frame (call compute()).
    [[nodiscard]] bool volumetric() const noexcept { return volumetric_; }

    struct Inputs {
        const rhi::Buffer* view_constants = nullptr;
        const rhi::Buffer* lights = nullptr;
        const rhi::Buffer* cluster_counts = nullptr;
        const rhi::Buffer* cluster_indices = nullptr;
        const rhi::Buffer* env_sh = nullptr;
        const rhi::Texture* shadow_map = nullptr;
        const rhi::Buffer* exposure = nullptr;
    };
    /// The froxel passes: inject and light, then integrate.
    void compute(rhi::CommandContext& ctx, const Inputs& inputs);
    /// Composites the fog over the bound render target (the scene colour), reading `depth`.
    void apply(rhi::CommandContext& ctx, const Inputs& inputs, const rhi::Texture& depth);
    /// Drops the temporal history (camera cut).
    void reset_history() noexcept { history_valid_ = false; }

    [[nodiscard]] glm::uvec3 grid_size() const noexcept { return grid_; }

private:
    void ensure_volumes(const glm::uvec3& size);
    /// Bindings cached on the resources they point at.
    struct Cached {
        rhi::Bindings bindings;
        std::array<const void*, 10> key{};
    };
    rhi::Bindings& bindings(Cached& cached, const rhi::Pipeline& pipeline, const std::array<const void*, 10>& key,
                            bool& fresh);

    rhi::Device& device_;
    rhi::ShaderLibrary& shaders_;
    rhi::Pipeline inject_pipeline_;
    rhi::Pipeline integrate_pipeline_;
    rhi::Pipeline apply_pipeline_;
    std::array<Cached, 2> inject_bindings_; ///< by which volume is the output
    Cached integrate_bindings_;
    Cached apply_bindings_;
    rhi::Buffer constants_;
    std::array<rhi::Texture, 2> scatter_; ///< lit medium, ping-pong (this frame / history)
    rhi::Texture integrated_;
    rhi::Texture empty_volume_; ///< 1x1x1 (no fog volume): bound when the volumetric part is off
    glm::uvec3 grid_{0u};
    std::uint32_t current_ = 0;
    std::uint64_t frame_ = 0;
    bool volumetric_ = false;
    bool history_valid_ = false;
    glm::mat4 prev_view_projection_{1.0f};
    glm::vec4 prev_view_row_{0.0f};
    std::optional<scene::HeightFog> prev_fog_;
};

} // namespace f3d::render::raster
