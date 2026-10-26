// SPDX-License-Identifier: Apache-2.0
#pragma once

// IRenderEngine: what the editor, thumbnails and captures render through. The
// raster engine (f3d_render_raster) is the default; a path tracer can be added
// later as another implementation (design §3.G).

#include "f3d/render/rhi/command_context.hpp"
#include "f3d/render/view.hpp"

#include <array>
#include <string_view>

namespace f3d::scene {
class World;
}
namespace f3d::assets {
class MeshLibrary;
}

namespace f3d::render {

/// The scene an engine draws. Either pointer may be null (nothing is drawn).
struct RenderScene {
    scene::World* world = nullptr;
    assets::MeshLibrary* meshes = nullptr; ///< meshes referenced by path (project assets stream separately)
};

/// Where an engine's frame ends up.
struct RenderTarget {
    /// Display-ready colour (RGBA8, sRGB-encoded values in a UNORM texture), left in the
    /// shader-resource state. Its size is the view's size.
    rhi::Texture output;
    std::array<float, 4> clear_color{0.0f, 0.0f, 0.0f, 1.0f}; ///< linear, behind the scene
};

class IRenderEngine {
public:
    virtual ~IRenderEngine() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    /// Renders `scene` as seen by `view` into `target`.
    virtual void render(rhi::CommandContext& ctx, const View& view, const RenderScene& scene,
                        const RenderTarget& target) = 0;
};

} // namespace f3d::render
