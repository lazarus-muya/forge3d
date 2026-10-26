// SPDX-License-Identifier: Apache-2.0
#pragma once

// IUpscaler: turns a frame rendered below output resolution into an output-sized
// image, as passes in the engine's render graph. Any IRenderEngine can use one.
// FSR is the first implementation (P8.3); vendor upscalers come as plugins.

#include "f3d/core/math.hpp"
#include "f3d/render/graph/render_graph.hpp"

#include <cstdint>
#include <string_view>

namespace f3d::render {

struct UpscaleInputs {
    graph::TextureHandle color;  ///< linear HDR scene colour at render resolution
    graph::TextureHandle depth;  ///< scene depth at render resolution (may be invalid)
    graph::TextureHandle motion; ///< screen-space motion vectors (may be invalid)
    glm::vec2 jitter{0.0f};      ///< sub-pixel projection jitter of this frame, in pixels
    bool reset_history = false;  ///< camera cut: drop temporal history
};

class IUpscaler {
public:
    virtual ~IUpscaler() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    /// Adds the upscaling passes; returns the output-sized colour (same format as the input).
    [[nodiscard]] virtual graph::TextureHandle add_passes(graph::RenderGraph& graph, const UpscaleInputs& inputs,
                                                          std::uint32_t output_width, std::uint32_t output_height) = 0;
};

} // namespace f3d::render
