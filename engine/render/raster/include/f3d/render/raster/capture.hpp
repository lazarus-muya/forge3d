// SPDX-License-Identifier: Apache-2.0
#pragma once

// Viewport capture (phases.md P4.4): renders the scene offscreen at any size and
// writes a PNG, headless. The `viewport.capture` operation (transient: no undo
// step) calls a capture function the host provides, so the editor captures with
// its renderer and the CLI with a device it creates on demand.
//
//   viewport.capture {path, width, height, mode, camera, bookmark, screen_percentage, fog_quality}
//     camera:   a Camera entity's GUID; or
//     bookmark: a camera bookmark's name; or neither: the host's view (the editor's
//               camera) or, without one, a view framing the whole scene.

#include "f3d/core/guid.hpp"
#include "f3d/render/raster/raster_engine.hpp"
#include "f3d/render/view.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace f3d::ops {
class OpRegistry;
}

namespace f3d::render::raster {

struct CaptureRequest {
    std::string path;
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    ViewMode mode = ViewMode::Lit;
    core::Guid camera;    ///< null: none
    std::string bookmark; ///< empty: none
    float screen_percentage = 100.0f;
    FogQuality fog_quality = FogQuality::High;
};

/// The view of a capture. `fallback`: the host's view (may be null); without it and
/// without a camera or bookmark, a view framing every mesh in `meshes`' bounds.
/// nullopt (and `error`) if the camera or bookmark doesn't exist.
[[nodiscard]] std::optional<View> capture_view(scene::World& world, assets::MeshLibrary* meshes,
                                               const CaptureRequest& request, const View* fallback,
                                               std::string* error = nullptr);

/// An offscreen renderer for captures.
class Capturer {
public:
    Capturer(rhi::Device& device, rhi::ShaderLibrary& shaders);

    /// Renders `world` for `request` and writes the PNG. The engine renders twice so
    /// automatic exposure starts metered.
    bool capture(scene::World& world, assets::MeshLibrary& meshes, const CaptureRequest& request, const View* fallback,
                 std::string* error = nullptr);

    [[nodiscard]] RasterEngine& engine() noexcept { return engine_; }

private:
    rhi::Device& device_;
    RasterEngine engine_;
};

/// What the host does for viewport.capture: render `world` for the request.
using CaptureFunction = std::function<bool(scene::World& world, const CaptureRequest& request, std::string* error)>;

/// Registers `viewport.capture` (transient: not recorded in the history).
void register_capture_op(ops::OpRegistry& registry, CaptureFunction capture);

} // namespace f3d::render::raster
