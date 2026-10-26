// SPDX-License-Identifier: Apache-2.0
#include "f3d/assets/material.hpp"

#include "f3d/core/reflect_json.hpp"

#include <entt/meta/factory.hpp>

namespace f3d::assets {

void register_material_types() {
    static const bool registered = [] {
        using core::reflect::FieldMeta;
        core::reflect::register_value_types();
        entt::meta_factory<AlphaMode>{}
            .type("AlphaMode")
            .data<AlphaMode::Opaque>("Opaque")
            .data<AlphaMode::Mask>("Mask");
        const auto unit = FieldMeta{}.with_range(0.0, 1.0, 0.01);
        entt::meta_factory<Material>{}
            .type("Material")
            .data<&Material::name>("name")
            .data<&Material::base_color>("base_color")
            .custom<FieldMeta>(FieldMeta{}.with_color().with_tooltip("Multiplied with the base colour texture; alpha: opacity"))
            .data<&Material::metallic>("metallic")
            .custom<FieldMeta>(unit)
            .data<&Material::roughness>("roughness")
            .custom<FieldMeta>(unit)
            .data<&Material::emissive>("emissive")
            .custom<FieldMeta>(FieldMeta{}.with_color(true).with_tooltip("Emitted light colour (times the emissive strength)"))
            .data<&Material::emissive_strength>("emissive_strength")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 1000.0, 0.1))
            .data<&Material::normal_scale>("normal_scale")
            .custom<FieldMeta>(FieldMeta{}.with_range(0.0, 4.0, 0.01))
            .data<&Material::occlusion_strength>("occlusion_strength")
            .custom<FieldMeta>(unit)
            .data<&Material::alpha_mode>("alpha_mode")
            .data<&Material::alpha_cutoff>("alpha_cutoff")
            .custom<FieldMeta>(unit)
            .data<&Material::double_sided>("double_sided")
            .data<&Material::base_color_texture>("base_color_texture")
            .data<&Material::normal_texture>("normal_texture")
            .data<&Material::orm_texture>("orm_texture")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("G: roughness, B: metallic (R: occlusion if packed)"))
            .data<&Material::orm_occlusion>("orm_occlusion")
            .custom<FieldMeta>(FieldMeta{}.with_tooltip("The ORM texture's red channel holds occlusion"))
            .data<&Material::occlusion_texture>("occlusion_texture")
            .data<&Material::emissive_texture>("emissive_texture");
        return true;
    }();
    static_cast<void>(registered);
}

nlohmann::json material_to_json(const Material& material) {
    register_material_types();
    return core::reflect::to_json(material);
}

std::optional<Material> material_from_json(const nlohmann::json& json, std::string* error) {
    register_material_types();
    if (!json.is_object()) {
        if (error) {
            *error = "material is not a JSON object";
        }
        return std::nullopt;
    }
    try {
        // Missing fields keep their defaults.
        Material material;
        entt::meta_any any = entt::forward_as_meta(material);
        core::reflect::apply_json(any, json);
        return material;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return std::nullopt;
    }
}

std::string write_material(const Material& material) {
    nlohmann::json doc;
    doc["format"] = kMaterialFormat;
    doc["version"] = 1;
    doc["material"] = material_to_json(material);
    return doc.dump(2) + "\n";
}

std::optional<Material> read_material(std::string_view text, std::string* error) {
    const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || doc.value("format", "") != kMaterialFormat) {
        if (error) {
            *error = "not a forge3d material file";
        }
        return std::nullopt;
    }
    if (doc.value("version", 0) != 1) {
        if (error) {
            *error = "unsupported material version";
        }
        return std::nullopt;
    }
    return material_from_json(doc.value("material", nlohmann::json::object()), error);
}

std::string_view asset_file(const core::AssetRef& ref) noexcept {
    const std::string_view path = ref.path;
    return path.substr(0, path.find('#'));
}

std::string_view sub_asset(const core::AssetRef& ref) noexcept {
    const std::string_view path = ref.path;
    const auto hash = path.find('#');
    return hash == std::string_view::npos ? std::string_view{} : path.substr(hash + 1);
}

core::AssetRef rebase_sub_asset(const core::AssetRef& ref, const core::AssetRef& model) {
    if (!ref.guid.is_null() || !ref.path.starts_with('#')) {
        return ref;
    }
    return {model.guid, std::string(asset_file(model)) + ref.path};
}

Material rebase_textures(Material material, const core::AssetRef& model) {
    for (core::AssetRef* slot : {&material.base_color_texture, &material.normal_texture, &material.orm_texture,
                                 &material.occlusion_texture, &material.emissive_texture}) {
        *slot = rebase_sub_asset(*slot, model);
    }
    return material;
}

} // namespace f3d::assets
