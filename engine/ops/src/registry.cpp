// SPDX-License-Identifier: Apache-2.0
#include "f3d/ops/registry.hpp"

#include "f3d/core/reflect.hpp"

#include <stdexcept>

namespace f3d::ops {

void OpRegistry::add(OpInfo info, Factory factory) {
    const std::string id = info.id;
    if (entries_.contains(id)) {
        throw std::runtime_error("duplicate operation id: " + id);
    }
    order_.push_back(id);
    entries_.emplace(id, Entry{std::move(info), std::move(factory)});
}

const OpInfo* OpRegistry::find(std::string_view id) const {
    const auto it = entries_.find(std::string(id));
    return it == entries_.end() ? nullptr : &it->second.info;
}

std::unique_ptr<Operation> OpRegistry::create(std::string_view id, const nlohmann::json& params) const {
    const auto it = entries_.find(std::string(id));
    if (it == entries_.end()) {
        return nullptr;
    }
    std::unique_ptr<Operation> op = it->second.factory();
    if (op) {
        op->set_params_json(params);
    }
    return op;
}

std::vector<OpInfo> OpRegistry::list() const {
    std::vector<OpInfo> out;
    out.reserve(order_.size());
    for (const std::string& id : order_) {
        out.push_back(entries_.at(id).info);
    }
    return out;
}

nlohmann::json param_schema(const OpInfo& info) {
    nlohmann::json schema = param_schema(info.params_type);
    for (const auto& row : info.extra_params) {
        schema.push_back(row);
    }
    return schema;
}

nlohmann::json param_schema(entt::meta_type params_type) {
    nlohmann::json schema = nlohmann::json::array();
    if (!params_type) {
        return schema;
    }
    for (const auto& [id, data] : params_type.data()) {
        (void)id;
        if (data.is_static() || data.name() == nullptr) {
            continue;
        }
        nlohmann::json field;
        field["name"] = data.name();
        const entt::meta_type type = data.type();
        field["type"] = type.name() ? type.name() : "?";
        if (const core::reflect::FieldMeta* meta = core::reflect::field_meta(data)) {
            if (meta->has_range) {
                field["min"] = meta->min;
                field["max"] = meta->max;
                field["step"] = meta->step;
            }
            if (!meta->unit.empty()) {
                field["unit"] = meta->unit;
            }
            if (!meta->tooltip.empty()) {
                field["tooltip"] = meta->tooltip;
            }
            if (meta->read_only) {
                field["read_only"] = true;
            }
            if (meta->color) {
                field["color"] = true;
            }
        }
        schema.push_back(std::move(field));
    }
    return schema;
}

} // namespace f3d::ops
