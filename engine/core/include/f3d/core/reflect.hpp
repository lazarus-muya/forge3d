// SPDX-License-Identifier: Apache-2.0
#pragma once

// Reflection registry (design §3.A, phases.md P2.2). A single type registry built
// on entt::meta; it drives JSON (de)serialisation, the inspector UI and, later,
// the Python/RPC/MCP schemas.
//
// Types register themselves with entt::meta directly:
//
//   entt::meta<MyType>().type("MyType")
//       .data<&MyType::value>("value")
//           .custom<reflect::FieldMeta>(reflect::FieldMeta{}.with_range(0.0, 1.0, 0.01f));
//
// Value types the JSON layer knows how to (de)serialise (glm vectors/quaternions,
// Guid, AssetRef, ...) are registered by register_value_types().

#include <entt/core/hashed_string.hpp>
#include <entt/meta/container.hpp> // std::vector fields are reflected as sequence containers
#include <entt/meta/factory.hpp>
#include <entt/meta/meta.hpp>

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace f3d::core::reflect {

using entt::meta_any;
using entt::meta_data;
using entt::meta_type;

/// Optional metadata attached to a reflected field. Stored on the field's meta
/// node via `.custom<FieldMeta>(...)` and read back with field_meta().
struct FieldMeta {
    std::string label; ///< inspector label; empty: the field name, humanised ("min_spacing": "Min spacing")
    std::string tooltip;
    std::string unit;
    bool hidden = false;    ///< not shown in the inspector
    bool read_only = false; ///< shown but not editable
    bool has_range = false; ///< a UI slider/drag range is available
    bool color = false;     ///< a vec3/vec4 linear RGB(A) colour: the inspector shows a colour picker
    bool hdr = false;       ///< a colour that may exceed 1 (emission)
    double min = 0.0;
    double max = 0.0;
    double step = 0.0;

    FieldMeta& with_label(std::string text) {
        label = std::move(text);
        return *this;
    }
    FieldMeta& with_tooltip(std::string text) {
        tooltip = std::move(text);
        return *this;
    }
    FieldMeta& with_unit(std::string text) {
        unit = std::move(text);
        return *this;
    }
    FieldMeta& with_range(double low, double high, double increment = 0.0) {
        has_range = true;
        min = low;
        max = high;
        step = increment;
        return *this;
    }
    FieldMeta& with_color(bool high_dynamic_range = false) {
        color = true;
        hdr = high_dynamic_range;
        return *this;
    }
    FieldMeta& with_hidden() {
        hidden = true;
        return *this;
    }
    FieldMeta& with_read_only() {
        read_only = true;
        return *this;
    }
};

/// The FieldMeta registered on `data`, or null if none was attached.
[[nodiscard]] const FieldMeta* field_meta(const meta_data& data) noexcept;

/// Registers the value types the JSON layer serialises as scalars/arrays rather
/// than as reflected objects (bool, integers, floats, string, glm vec2/3/4/quat,
/// Guid, AssetRef, std::vector<Guid>, std::vector<AssetRef>), under the names
/// schemas report: "bool", "int32", "uint32", "int64", "float", "double",
/// "string", "vec2", "vec3", "vec4", "quat", "guid", "asset_ref", "guid[]",
/// "asset_ref[]". Idempotent.
///
/// Any std::vector<T> field of a registered T is (de)serialised as a JSON array;
/// register the vector type with a name (as here) so schemas can describe it.
void register_value_types();

/// Writes `value` to `out` as JSON. Throws std::runtime_error for types the
/// reflection layer does not know how to serialise.
void value_to_json(nlohmann::json& out, const meta_any& value);

/// Reads a value of `type` from `in`. Throws std::runtime_error on a malformed
/// value or an unsupported type.
[[nodiscard]] meta_any value_from_json(const nlohmann::json& in, meta_type type);

/// Applies the fields present in `in` to an existing reflected class instance
/// (a component). No-op for non-class types.
void apply_json(meta_any& instance, const nlohmann::json& in);

/// The field of `type` with this name, or an invalid meta_data.
[[nodiscard]] meta_data field_by_name(meta_type type, std::string_view name);

} // namespace f3d::core::reflect
