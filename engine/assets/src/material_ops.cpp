// SPDX-License-Identifier: Apache-2.0
#include "f3d/assets/material_ops.hpp"

#include "f3d/assets/material.hpp"
#include "f3d/core/reflect_json.hpp"
#include "f3d/core/vfs.hpp"

#include <entt/meta/factory.hpp>

#include <optional>

namespace f3d::assets {
namespace {

// Parameter structs have globally unique names: EnTT identifies types by name
// (see prefab_ops.cpp), and MSVC names every anonymous namespace alike.
struct MaterialCreateOpParams {
    std::string path; ///< VFS path of the new .f3dmat
};

struct MaterialDeleteOpParams {
    std::string path; ///< VFS path of the .f3dmat
};

struct MaterialSetPropertyOpParams {
    std::string path;     ///< VFS path of the .f3dmat
    std::string property; ///< a Material field, e.g. "roughness"
};

void register_param_types() {
    static const bool registered = [] {
        entt::meta_factory<MaterialCreateOpParams>{}
            .type("material.create.params")
            .data<&MaterialCreateOpParams::path>("path");
        entt::meta_factory<MaterialDeleteOpParams>{}
            .type("material.delete.params")
            .data<&MaterialDeleteOpParams::path>("path");
        entt::meta_factory<MaterialSetPropertyOpParams>{}
            .type("material.set_property.params")
            .data<&MaterialSetPropertyOpParams::path>("path")
            .data<&MaterialSetPropertyOpParams::property>("property");
        return true;
    }();
    static_cast<void>(registered);
}

void asset_changed(ops::Context& context, const std::string& path) {
    context.changes.emit({ops::ChangeKind::AssetChanged, core::Guid{}, path});
}

/// The material stored at `path`, or nullopt.
std::optional<Material> load(const core::vfs::FileSystem& files, const std::string& path) {
    const auto text = files.read_text(path);
    return text ? read_material(*text) : std::nullopt;
}

class MaterialCreateOp final : public ops::OperationBase<MaterialCreateOpParams> {
public:
    nlohmann::json material = nlohmann::json::object(); ///< the material's fields (missing: defaults)

    [[nodiscard]] std::string_view id() const override { return "material.create"; }
    [[nodiscard]] std::string display_name() const override { return "Create material " + params.path; }

    [[nodiscard]] nlohmann::json params_json() const override {
        nlohmann::json json = OperationBase::params_json();
        json["material"] = material;
        return json;
    }
    void set_params_json(const nlohmann::json& json) override {
        OperationBase::set_params_json(json);
        material = json.value("material", nlohmann::json::object());
    }

    bool execute(ops::Context& context) override {
        if (!context.files || !params.path.ends_with(kMaterialExtension) || context.files->exists(params.path)) {
            return false;
        }
        const auto parsed = material_from_json(material);
        if (!parsed || !context.files->write_text(params.path, write_material(*parsed))) {
            return false;
        }
        asset_changed(context, params.path);
        return true;
    }

    void undo(ops::Context& context) override {
        if (context.files) {
            context.files->remove(params.path);
            asset_changed(context, params.path);
        }
    }

    [[nodiscard]] std::size_t memory_size() const override { return sizeof(*this) + material.dump().size(); }
};

/// Removes a project material file and its .meta (undo writes both back, so the
/// material keeps its GUID). Slots that use it fall back to the default material.
class MaterialDeleteOp final : public ops::OperationBase<MaterialDeleteOpParams> {
public:
    [[nodiscard]] std::string_view id() const override { return "material.delete"; }
    [[nodiscard]] std::string display_name() const override { return "Delete material " + params.path; }

    bool execute(ops::Context& context) override {
        if (!context.files || !params.path.ends_with(kMaterialExtension)) {
            return false;
        }
        text_ = context.files->read_text(params.path);
        if (!text_) {
            return false;
        }
        meta_ = context.files->read_text(params.path + ".meta");
        if (!context.files->remove(params.path)) {
            return false;
        }
        if (meta_) {
            context.files->remove(params.path + ".meta");
        }
        asset_changed(context, params.path);
        return true;
    }

    void undo(ops::Context& context) override {
        if (context.files && text_) {
            context.files->write_text(params.path, *text_);
            if (meta_) {
                context.files->write_text(params.path + ".meta", *meta_);
            }
            asset_changed(context, params.path);
        }
    }

    [[nodiscard]] std::size_t memory_size() const override {
        return sizeof(*this) + (text_ ? text_->size() : 0) + (meta_ ? meta_->size() : 0);
    }

private:
    std::optional<std::string> text_;
    std::optional<std::string> meta_;
};

class MaterialSetPropertyOp final : public ops::OperationBase<MaterialSetPropertyOpParams> {
public:
    nlohmann::json value;

    [[nodiscard]] std::string_view id() const override { return "material.set_property"; }
    [[nodiscard]] std::string display_name() const override { return "Set material " + params.property; }

    [[nodiscard]] nlohmann::json params_json() const override {
        nlohmann::json json = OperationBase::params_json();
        json["value"] = value;
        return json;
    }
    void set_params_json(const nlohmann::json& json) override {
        OperationBase::set_params_json(json);
        value = json.contains("value") ? json.at("value") : nlohmann::json();
    }

    bool execute(ops::Context& context) override {
        if (!context.files) {
            return false;
        }
        const auto text = context.files->read_text(params.path);
        auto material = text ? read_material(*text) : std::nullopt;
        register_material_types();
        const entt::meta_data field = core::reflect::field_by_name(entt::resolve<Material>(), params.property);
        if (!material || !field) {
            return false;
        }
        if (!old_text_) {
            old_text_ = *text; // a merged drag keeps the text from before its first edit
        }
        entt::meta_any any = entt::forward_as_meta(*material);
        try {
            if (!field.set(any, core::reflect::value_from_json(value, field.type()))) {
                return false;
            }
        } catch (const std::exception&) {
            return false; // malformed value for this field
        }
        if (!context.files->write_text(params.path, write_material(*material))) {
            return false;
        }
        asset_changed(context, params.path);
        return true;
    }

    void undo(ops::Context& context) override {
        if (context.files && old_text_) {
            context.files->write_text(params.path, *old_text_);
            asset_changed(context, params.path);
        }
    }

    bool merge(const ops::Operation& next) override {
        const auto* other = dynamic_cast<const MaterialSetPropertyOp*>(&next);
        if (!other || other->params.path != params.path || other->params.property != params.property) {
            return false;
        }
        value = other->value;
        return true;
    }

    [[nodiscard]] std::size_t memory_size() const override {
        return sizeof(*this) + (old_text_ ? old_text_->size() : 0) + value.dump().size();
    }

private:
    std::optional<std::string> old_text_;
};

} // namespace

void register_material_ops(ops::OpRegistry& registry) {
    register_param_types();
    register_material_types();
    registry.add(
        {"material.create", "Create Material", entt::resolve<MaterialCreateOpParams>(),
         nlohmann::json::array({{{"name", "material"},
                                 {"type", "json"},
                                 {"tooltip", "Material fields (Material reflection); missing ones default"}}})},
        [] { return std::make_unique<MaterialCreateOp>(); });
    registry.add({"material.delete", "Delete Material", entt::resolve<MaterialDeleteOpParams>()},
                 [] { return std::make_unique<MaterialDeleteOp>(); });
    registry.add({"material.set_property", "Set Material Property", entt::resolve<MaterialSetPropertyOpParams>(),
                  nlohmann::json::array(
                      {{{"name", "value"}, {"type", "json"}, {"tooltip", "The new value, in the field's JSON form"}}})},
                 [] { return std::make_unique<MaterialSetPropertyOp>(); });
}

} // namespace f3d::assets
