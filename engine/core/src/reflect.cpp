// SPDX-License-Identifier: Apache-2.0
#include "f3d/core/reflect.hpp"

#include "f3d/core/asset_ref.hpp"
#include "f3d/core/guid.hpp"
#include "f3d/core/math.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace f3d::core::reflect {
namespace {

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error("reflect: " + std::string(message));
}

template <class T>
[[nodiscard]] bool is_type(const meta_type& type) {
    return type == entt::resolve<T>();
}

/// Converts a scalar/enum meta value to T (using the registered conversion).
template <class T>
[[nodiscard]] T scalar(const meta_any& value) {
    meta_any converted = std::as_const(value).allow_cast<T>();
    if (!converted) {
        fail("cannot convert value to a scalar");
    }
    return converted.cast<T>();
}

[[nodiscard]] float f32(const nlohmann::json& in) {
    return in.get<float>();
}

} // namespace

const FieldMeta* field_meta(const meta_data& data) noexcept {
    if (!data) {
        return nullptr;
    }
    return static_cast<const FieldMeta*>(data.custom());
}

void register_value_types() {
    static const bool registered = [] {
        entt::meta_factory<bool>{}.type("bool");
        entt::meta_factory<std::int32_t>{}.type("int32");
        entt::meta_factory<std::uint32_t>{}.type("uint32");
        entt::meta_factory<std::int64_t>{}.type("int64");
        entt::meta_factory<float>{}.type("float");
        entt::meta_factory<double>{}.type("double");
        entt::meta_factory<std::string>{}.type("string");
        entt::meta_factory<glm::vec2>{}.type("vec2");
        entt::meta_factory<glm::vec3>{}.type("vec3");
        entt::meta_factory<glm::vec4>{}.type("vec4");
        entt::meta_factory<glm::quat>{}.type("quat");
        entt::meta_factory<Guid>{}.type("guid");
        entt::meta_factory<AssetRef>{}.type("asset_ref");
        entt::meta_factory<std::vector<Guid>>{}.type("guid[]");
        entt::meta_factory<std::vector<AssetRef>>{}.type("asset_ref[]");
        return true;
    }();
    (void)registered;
}

void value_to_json(nlohmann::json& out, const meta_any& value) {
    const meta_type type = value.type();
    if (!type) {
        fail("value has no type");
    }
    register_value_types();

    if (is_type<bool>(type)) {
        out = scalar<bool>(value);
    } else if (type.is_enum()) {
        out = scalar<std::int64_t>(value);
    } else if (type.is_integral()) {
        out = scalar<std::int64_t>(value);
    } else if (type.is_arithmetic()) {
        out = scalar<double>(value);
    } else if (is_type<std::string>(type)) {
        out = value.cast<const std::string&>();
    } else if (is_type<glm::vec2>(type)) {
        const glm::vec2& v = value.cast<const glm::vec2&>();
        out = nlohmann::json::array({v.x, v.y});
    } else if (is_type<glm::vec3>(type)) {
        const glm::vec3& v = value.cast<const glm::vec3&>();
        out = nlohmann::json::array({v.x, v.y, v.z});
    } else if (is_type<glm::vec4>(type)) {
        const glm::vec4& v = value.cast<const glm::vec4&>();
        out = nlohmann::json::array({v.x, v.y, v.z, v.w});
    } else if (is_type<glm::quat>(type)) {
        const glm::quat& q = value.cast<const glm::quat&>();
        out = nlohmann::json::array({q.w, q.x, q.y, q.z});
    } else if (is_type<Guid>(type)) {
        out = value.cast<const Guid&>().to_string();
    } else if (is_type<AssetRef>(type)) {
        const AssetRef& ref = value.cast<const AssetRef&>();
        out = nlohmann::json::object();
        out["guid"] = ref.guid.to_string();
        out["path"] = ref.path;
    } else if (type.is_sequence_container()) {
        out = nlohmann::json::array();
        for (const meta_any element : value.as_sequence_container()) {
            value_to_json(out.emplace_back(), element);
        }
    } else if (type.is_class()) {
        out = nlohmann::json::object();
        for (const auto& [id, data] : type.data()) {
            (void)id;
            if (data.is_static() || data.name() == nullptr) {
                continue;
            }
            value_to_json(out[data.name()], data.get(value));
        }
    } else {
        fail("unsupported type '" + std::string(type.name() ? type.name() : "?") + "'");
    }
}

meta_any value_from_json(const nlohmann::json& in, meta_type type) {
    if (!type) {
        fail("invalid type");
    }
    register_value_types();

    if (is_type<bool>(type)) {
        return meta_any{in.get<bool>()};
    }
    if (type.is_enum() || type.is_integral()) {
        const auto value = in.get<std::int64_t>();
        meta_any source{value};
        meta_any converted = std::as_const(source).allow_cast(type);
        if (!converted) {
            fail("cannot convert integer to '" + std::string(type.name() ? type.name() : "?") + "'");
        }
        return converted;
    }
    if (type.is_arithmetic()) {
        const auto value = in.get<double>();
        meta_any source{value};
        meta_any converted = std::as_const(source).allow_cast(type);
        if (!converted) {
            fail("cannot convert number to '" + std::string(type.name() ? type.name() : "?") + "'");
        }
        return converted;
    }
    if (is_type<std::string>(type)) {
        return meta_any{in.get<std::string>()};
    }
    if (is_type<glm::vec2>(type)) {
        return meta_any{glm::vec2{f32(in.at(0)), f32(in.at(1))}};
    }
    if (is_type<glm::vec3>(type)) {
        return meta_any{glm::vec3{f32(in.at(0)), f32(in.at(1)), f32(in.at(2))}};
    }
    if (is_type<glm::vec4>(type)) {
        return meta_any{glm::vec4{f32(in.at(0)), f32(in.at(1)), f32(in.at(2)), f32(in.at(3))}};
    }
    if (is_type<glm::quat>(type)) {
        return meta_any{glm::quat{f32(in.at(0)), f32(in.at(1)), f32(in.at(2)), f32(in.at(3))}};
    }
    if (is_type<Guid>(type)) {
        if (in.is_null()) {
            return meta_any{Guid{}};
        }
        const auto parsed = Guid::parse(in.get<std::string>());
        if (!parsed) {
            fail("malformed Guid");
        }
        return meta_any{*parsed};
    }
    if (is_type<AssetRef>(type)) {
        AssetRef ref;
        if (in.contains("guid") && in.at("guid").is_string()) {
            ref.guid = Guid::parse(in.at("guid").get<std::string>()).value_or(Guid{});
        }
        if (in.contains("path") && in.at("path").is_string()) {
            ref.path = in.at("path").get<std::string>();
        }
        return meta_any{ref};
    }
    if (type.is_sequence_container()) {
        meta_any instance = type.construct();
        if (!instance || !in.is_array()) {
            fail("expected an array for '" + std::string(type.name() ? type.name() : "?") + "'");
        }
        entt::meta_sequence_container view = instance.as_sequence_container();
        const meta_type element_type = view.value_type();
        for (const auto& element : in) {
            if (!view.insert(view.end(), value_from_json(element, element_type))) {
                fail("cannot insert into '" + std::string(type.name() ? type.name() : "?") + "'");
            }
        }
        return instance;
    }
    if (type.is_class()) {
        meta_any instance = type.construct();
        if (!instance) {
            fail("cannot construct '" + std::string(type.name() ? type.name() : "?") + "'");
        }
        for (const auto& [id, data] : type.data()) {
            (void)id;
            const char* name = data.name();
            if (data.is_static() || name == nullptr || !in.contains(name)) {
                continue;
            }
            data.set(instance, value_from_json(in.at(name), data.type()));
        }
        return instance;
    }
    fail("unsupported type '" + std::string(type.name() ? type.name() : "?") + "'");
}

void apply_json(meta_any& instance, const nlohmann::json& in) {
    const meta_type type = instance.type();
    if (!type || !type.is_class()) {
        return;
    }
    register_value_types();
    for (const auto& [id, data] : type.data()) {
        (void)id;
        const char* name = data.name();
        if (data.is_static() || name == nullptr || !in.contains(name)) {
            continue;
        }
        data.set(instance, value_from_json(in.at(name), data.type()));
    }
}

meta_data field_by_name(meta_type type, std::string_view name) {
    if (!type) {
        return {};
    }
    for (const auto& [id, data] : type.data()) {
        if (data.name() != nullptr && name == data.name()) {
            return data;
        }
    }
    return {};
}

} // namespace f3d::core::reflect
