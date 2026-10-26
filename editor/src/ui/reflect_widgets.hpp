// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <entt/meta/meta.hpp>
#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <string>

namespace f3d::editor {

/// Called when the user edits a field. `path` is "<Component>.<field>" (e.g.
/// "Transform.translation"); the app routes it through entity.set_property.
/// `continuing` is true for the second and later edits of one widget interaction
/// (a drag, typing into a field), which merge into one undo step.
using PropertyEditCallback = std::function<void(const std::string& path, const nlohmann::json& value, bool continuing)>;

/// Draws an inspector section for a reflected component (its type name as a
/// header, then one widget per reflected field). Edits are reported through
/// `on_edit`; the component is not written to directly. With `on_remove`, the
/// header gets a "Remove Component" context menu. Returns true if edited.
bool draw_reflected_component(entt::meta_any& component, const PropertyEditCallback& on_edit,
                              const std::function<void()>& on_remove = {});

/// Linear <-> sRGB per channel (above 1 a plain power curve, for HDR colours).
[[nodiscard]] float linear_to_srgb(float v);
[[nodiscard]] float srgb_to_linear(float v);
/// A colour picker for a linear RGB (`channels` 3) or RGBA (4) colour: the picker
/// shows display (sRGB) values, as the swatch looks. True when edited.
bool edit_linear_color(const char* label, float* linear, int channels, bool hdr = false);

} // namespace f3d::editor
