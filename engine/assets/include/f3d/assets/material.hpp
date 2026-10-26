// SPDX-License-Identifier: Apache-2.0
#pragma once

// Materials (phases.md P4.2): metal/rough PBR parameters plus texture slots.
// Materials are reflected (inspector, JSON) and stored as .f3dmat files in a
// project; imported models carry their own as sub-assets ("<model>#material/<n>").

#include "f3d/assets/texture.hpp"
#include "f3d/core/asset_ref.hpp"
#include "f3d/core/math.hpp"

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace f3d::assets {

enum class AlphaMode : std::uint8_t {
    Opaque,
    Mask, ///< alpha below the cutoff is discarded
};

/// Metallic/roughness material (glTF 2.0 core model). Colours are linear.
struct Material {
    std::string name;
    glm::vec4 base_color{1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    glm::vec3 emissive{0.0f};
    float emissive_strength = 1.0f;
    float normal_scale = 1.0f;
    float occlusion_strength = 1.0f;
    AlphaMode alpha_mode = AlphaMode::Opaque;
    float alpha_cutoff = 0.5f;
    bool double_sided = false;

    core::AssetRef base_color_texture; ///< sRGB colour, alpha
    core::AssetRef normal_texture;     ///< tangent space; X/Y used, Z reconstructed
    core::AssetRef orm_texture;        ///< G roughness, B metallic, R occlusion if orm_occlusion
    bool orm_occlusion = false;
    core::AssetRef occlusion_texture; ///< R occlusion, when not packed into the ORM texture
    core::AssetRef emissive_texture;  ///< sRGB colour

    friend bool operator==(const Material&, const Material&) = default;
};

/// The texture kind each slot is cooked as.
inline constexpr TextureKind kBaseColorKind = TextureKind::Color;
inline constexpr TextureKind kNormalKind = TextureKind::Normal;
inline constexpr TextureKind kOrmKind = TextureKind::Linear;
inline constexpr TextureKind kOcclusionKind = TextureKind::Grayscale;
inline constexpr TextureKind kEmissiveKind = TextureKind::Color;

/// Registers Material (and AlphaMode) with the reflection registry. Idempotent.
void register_material_types();

/// Material file extension and format id.
inline constexpr std::string_view kMaterialExtension = ".f3dmat";
inline constexpr std::string_view kMaterialFormat = "forge3d.material";

/// .f3dmat JSON: {"format": "forge3d.material", "version": 1, "material": {...}}.
[[nodiscard]] std::string write_material(const Material& material);
[[nodiscard]] std::optional<Material> read_material(std::string_view text, std::string* error = nullptr);
[[nodiscard]] nlohmann::json material_to_json(const Material& material);
[[nodiscard]] std::optional<Material> material_from_json(const nlohmann::json& json, std::string* error = nullptr);

/// A sub-asset path ("#image/3") is rebased onto its model: "<model path>#image/3"
/// with the model's GUID. Other references are returned unchanged.
[[nodiscard]] core::AssetRef rebase_sub_asset(const core::AssetRef& ref, const core::AssetRef& model);
/// rebase_sub_asset() on every texture slot.
[[nodiscard]] Material rebase_textures(Material material, const core::AssetRef& model);

/// The part of a reference's path before '#' (the file), and after it (without '#'; empty if none).
[[nodiscard]] std::string_view asset_file(const core::AssetRef& ref) noexcept;
[[nodiscard]] std::string_view sub_asset(const core::AssetRef& ref) noexcept;

} // namespace f3d::assets
