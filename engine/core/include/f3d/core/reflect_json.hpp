// SPDX-License-Identifier: Apache-2.0
#pragma once

// Convenience wrappers around the reflection JSON layer (reflect.hpp) for
// concrete reflected types. Includes <nlohmann/json.hpp>, so keep it out of
// headers that only need the declarations.

#include "f3d/core/reflect.hpp"

#include <nlohmann/json.hpp>

namespace f3d::core::reflect {

/// Serialises a reflected value (the type must be registered).
template <class T>
[[nodiscard]] nlohmann::json to_json(const T& value) {
    nlohmann::json out;
    value_to_json(out, entt::forward_as_meta(value));
    return out;
}

/// Deserialises a registered type from JSON.
template <class T>
[[nodiscard]] T from_json(const nlohmann::json& in) {
    return value_from_json(in, entt::resolve<T>()).template cast<T>();
}

} // namespace f3d::core::reflect
