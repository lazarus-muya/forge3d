// SPDX-License-Identifier: Apache-2.0
#include "reflect_widgets.hpp"

#include "f3d/core/asset_ref.hpp"
#include "f3d/core/guid.hpp"
#include "f3d/core/math.hpp"
#include "f3d/core/reflect.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace f3d::editor {
namespace {

using core::reflect::FieldMeta;
using entt::meta_any;
using entt::meta_data;
using entt::meta_type;

template <class T>
bool is_type(const meta_type& type) {
    return type == entt::resolve<T>();
}

/// The JSON form of a reflected value (matches the set_property value format).
template <class T>
nlohmann::json value_json(const T& value) {
    nlohmann::json out;
    core::reflect::value_to_json(out, entt::forward_as_meta(value));
    return out;
}

/// "min_spacing" -> "Min spacing".
std::string humanise(const char* name) {
    std::string out = name ? name : "field";
    for (char& c : out) {
        if (c == '_') {
            c = ' ';
        }
    }
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') {
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    }
    return out;
}

std::string field_label(const meta_data& data, const FieldMeta* meta) {
    std::string label = meta && !meta->label.empty() ? meta->label : humanise(data.name());
    if (meta && !meta->unit.empty()) {
        label += " (" + meta->unit + ")";
    }
    return label;
}

/// The field's label and description (the label alone when there is none: it may be clipped).
void draw_tooltip(const std::string& label, const FieldMeta* meta) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
        return;
    }
    if (ImGui::BeginTooltip()) {
        ImGui::TextUnformatted(label.c_str());
        if (meta && !meta->tooltip.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
            ImGui::TextDisabled("%s", meta->tooltip.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndTooltip();
    }
}

/// Enum combo: returns true and fills `out` with the chosen value on change.
bool draw_enum(meta_any& field, nlohmann::json& out) {
    const meta_type type = field.type();
    std::vector<std::string> names;
    std::vector<meta_any> values;
    for (const auto& [id, entry] : type.data()) {
        (void)id;
        if (entry.is_static()) {
            names.push_back(entry.name() ? entry.name() : "?");
            values.push_back(entry.get(field));
        }
    }
    if (names.empty()) {
        return false;
    }
    const meta_any current = std::as_const(field).allow_cast<std::int64_t>();
    const auto current_value = current ? current.cast<std::int64_t>() : 0;
    int selected = 0;
    for (int i = 0; i < static_cast<int>(values.size()); ++i) {
        const meta_any as_int = std::as_const(values[static_cast<std::size_t>(i)]).allow_cast<std::int64_t>();
        if (as_int && as_int.cast<std::int64_t>() == current_value) {
            selected = i;
            break;
        }
    }
    std::vector<const char*> pointers;
    pointers.reserve(names.size());
    for (const std::string& name : names) {
        pointers.push_back(name.c_str());
    }
    if (ImGui::Combo("##value", &selected, pointers.data(), static_cast<int>(pointers.size()))) {
        core::reflect::value_to_json(out, values[static_cast<std::size_t>(selected)]);
        return true;
    }
    return false;
}

/// The widget whose interaction produced the last edit; reset whenever a widget
/// is activated, so the first edit of every new interaction starts an undo step.
ImGuiID g_edit_item = 0;

/// Rotation shown as XYZ Euler angles in degrees, edited as a quaternion.
bool draw_rotation(const char* label, const glm::quat& q, float speed, nlohmann::json& out) {
    glm::vec3 degrees = glm::degrees(glm::eulerAngles(q));
    for (int i = 0; i < 3; ++i) {
        if (std::abs(degrees[i]) < 5e-4f) {
            degrees[i] = 0.0f; // avoid showing -0.00
        }
    }
    if (!ImGui::DragFloat3(label, &degrees.x, speed > 0.0f ? speed : 0.5f, 0.0f, 0.0f, "%.2f°")) {
        return false;
    }
    out = value_json(glm::normalize(glm::quat(glm::radians(degrees))));
    return true;
}

} // namespace

/// Linear <-> sRGB per channel (the sRGB curve, extended as a power curve above 1 for HDR).
float linear_to_srgb(float v) {
    if (v <= 0.0031308f) {
        return std::max(v, 0.0f) * 12.92f;
    }
    return v <= 1.0f ? 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f : std::pow(v, 1.0f / 2.2f);
}

float srgb_to_linear(float v) {
    if (v <= 0.04045f) {
        return std::max(v, 0.0f) / 12.92f;
    }
    return v <= 1.0f ? std::pow((v + 0.055f) / 1.055f, 2.4f) : std::pow(v, 2.2f);
}

/// A linear colour (vec3 or vec4: `channels`) edited with a colour picker in display
/// (sRGB) values, as the swatch shows it; alpha stays linear.
bool edit_linear_color(const char* label, float* linear, int channels, bool hdr) {
    float shown[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (int i = 0; i < channels; ++i) {
        shown[i] = i < 3 ? linear_to_srgb(linear[i]) : linear[i];
    }
    ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_PickerHueWheel;
    if (hdr) {
        flags |= ImGuiColorEditFlags_HDR;
    }
    const bool edited = channels == 4 ? ImGui::ColorEdit4(label, shown, flags | ImGuiColorEditFlags_AlphaBar |
                                                                            ImGuiColorEditFlags_AlphaPreviewHalf)
                                      : ImGui::ColorEdit3(label, shown, flags);
    if (edited) {
        for (int i = 0; i < channels; ++i) {
            linear[i] = i < 3 ? srgb_to_linear(shown[i]) : std::clamp(shown[i], 0.0f, 1.0f);
        }
    }
    return edited;
}

bool draw_reflected_component(meta_any& component, const PropertyEditCallback& on_edit,
                              const std::function<void()>& on_remove) {
    const meta_type type = component.type();
    const std::string component_name = type.name() ? type.name() : "Component";
    const bool open = ImGui::CollapsingHeader(component_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
    if (on_remove && ImGui::BeginPopupContextItem(("##remove_" + component_name).c_str())) {
        if (ImGui::MenuItem("Remove Component")) {
            ImGui::EndPopup();
            on_remove();
            return false; // the component is gone: don't touch it
        }
        ImGui::EndPopup();
    }
    if (!open) {
        return false;
    }
    bool changed = false;
    ImGui::PushID(component_name.c_str());
    // Label column + widget column: a full-width widget would push its own label out of view.
    if (!ImGui::BeginTable("##fields", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::PopID();
        return false;
    }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.4f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    for (const auto& [id, data] : type.data()) {
        (void)id;
        if (data.is_static() || data.name() == nullptr) {
            continue;
        }
        const FieldMeta* meta = core::reflect::field_meta(data);
        if (meta && meta->hidden) {
            continue;
        }
        meta_any field = data.get(component);
        if (!field) {
            continue;
        }
        const meta_type field_type = field.type();
        const std::string label = field_label(data, meta);
        const std::string path = component_name + "." + data.name();
        ImGui::PushID(data.name());
        const bool read_only = meta && meta->read_only;
        if (read_only) {
            ImGui::BeginDisabled();
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label.c_str());
        draw_tooltip(label, meta);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        constexpr const char* kId = "##value";

        const float speed = meta && meta->has_range && meta->step > 0.0 ? static_cast<float>(meta->step) : 0.1f;
        const float min_v = meta && meta->has_range ? static_cast<float>(meta->min) : 0.0f;
        const float max_v = meta && meta->has_range ? static_cast<float>(meta->max) : 0.0f;
        bool edited = false;
        nlohmann::json value;

        if (is_type<bool>(field_type)) {
            bool v = field.cast<bool>();
            edited = ImGui::Checkbox(kId, &v);
            value = v;
        } else if (field_type.is_enum()) {
            edited = draw_enum(field, value);
        } else if (field_type.is_integral()) {
            const meta_any as_int = std::as_const(field).allow_cast<std::int64_t>();
            int v = static_cast<int>(as_int ? as_int.cast<std::int64_t>() : 0);
            edited = ImGui::DragInt(kId, &v, speed, static_cast<int>(min_v), static_cast<int>(max_v));
            value = v;
        } else if (is_type<float>(field_type)) {
            float v = field.cast<float>();
            edited = ImGui::DragFloat(kId, &v, speed, min_v, max_v);
            value = v;
        } else if (is_type<double>(field_type)) {
            double v = field.cast<double>();
            edited = ImGui::DragScalar(kId, ImGuiDataType_Double, &v, speed);
            value = v;
        } else if (is_type<std::string>(field_type)) {
            const std::string& text = field.cast<const std::string&>();
            char buffer[512];
            std::snprintf(buffer, sizeof(buffer), "%s", text.c_str());
            edited = ImGui::InputText(kId, buffer, sizeof(buffer));
            value = std::string(buffer);
        } else if (is_type<glm::vec2>(field_type)) {
            glm::vec2 v = field.cast<glm::vec2>();
            edited = ImGui::DragFloat2(kId, &v.x, speed);
            value = value_json(v);
        } else if (meta && meta->color && is_type<glm::vec3>(field_type)) {
            glm::vec3 v = field.cast<glm::vec3>();
            edited = edit_linear_color(kId, &v.x, 3, meta->hdr);
            value = value_json(v);
        } else if (meta && meta->color && is_type<glm::vec4>(field_type)) {
            glm::vec4 v = field.cast<glm::vec4>();
            edited = edit_linear_color(kId, &v.x, 4, meta->hdr);
            value = value_json(v);
        } else if (is_type<glm::vec3>(field_type)) {
            glm::vec3 v = field.cast<glm::vec3>();
            edited = ImGui::DragFloat3(kId, &v.x, speed);
            value = value_json(v);
        } else if (is_type<glm::vec4>(field_type)) {
            glm::vec4 v = field.cast<glm::vec4>();
            edited = ImGui::DragFloat4(kId, &v.x, speed);
            value = value_json(v);
        } else if (is_type<glm::quat>(field_type)) {
            edited =
                draw_rotation(kId, field.cast<glm::quat>(), meta && meta->has_range ? speed : 0.0f, value);
        } else if (is_type<core::Guid>(field_type)) {
            ImGui::TextUnformatted(field.cast<const core::Guid&>().to_string().c_str());
        } else if (is_type<core::AssetRef>(field_type)) {
            const core::AssetRef& ref = field.cast<const core::AssetRef&>();
            ImGui::TextUnformatted(ref.path.empty() ? "(none)" : ref.path.c_str());
        } else {
            ImGui::TextDisabled("<unsupported>");
        }

        draw_tooltip(label, meta);
        if (read_only) {
            ImGui::EndDisabled();
        }
        const ImGuiID item = ImGui::GetItemID();
        if (ImGui::IsItemActivated() && g_edit_item == item) {
            g_edit_item = 0; // a new interaction with the same widget
        }
        if (edited && !read_only && on_edit) {
            const bool continuing = g_edit_item == item && ImGui::IsItemActive();
            g_edit_item = ImGui::IsItemActive() ? item : 0;
            on_edit(path, value, continuing);
            changed = true;
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    ImGui::PopID();
    return changed;
}

} // namespace f3d::editor
