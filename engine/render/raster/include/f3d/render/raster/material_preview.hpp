// SPDX-License-Identifier: Apache-2.0
#pragma once

// Material preview (phases.md P4.2): a material on a sphere, lit by the default
// sun and ambient, rendered offscreen for the material editor.

#include "f3d/assets/material.hpp"
#include "f3d/assets/mesh_library.hpp"
#include "f3d/render/raster/raster_engine.hpp"
#include "f3d/scene/world.hpp"

#include <cstdint>

namespace f3d::render::raster {

class MaterialPreview {
public:
    MaterialPreview(rhi::Device& device, rhi::ShaderLibrary& shaders);

    /// Streams GUID textures through the editor's residency manager (may be null).
    void set_residency(ResidencyManager* residency) noexcept { engine_.set_residency(residency); }

    /// Renders `material` on the sphere into a `size` x `size` texture (display
    /// colour, shader-resource state) and returns it. Model images referenced by
    /// path are loaded from their files.
    const rhi::Texture& render(const assets::Material& material, std::uint32_t size);

private:
    rhi::Device& device_;
    RasterEngine engine_;
    scene::World world_;
    assets::MeshLibrary meshes_;
    rhi::Texture output_;
};

} // namespace f3d::render::raster
