// SPDX-License-Identifier: Apache-2.0
#include "panels.hpp"

#include "asset_browser.hpp"
#include "f3d/assets/asset_database.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/editor/camera_actors.hpp"
#include "f3d/editor/command_registry.hpp"
#include "f3d/editor/frame_constants.hpp"
#include "f3d/editor/import_queue.hpp"
#include "f3d/editor/navigation.hpp"
#include "f3d/editor/scene_editor.hpp"
#include "f3d/editor/viewport_tool.hpp"
#include "f3d/ops/prefab.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/render/rhi/imgui_renderer.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"
#include "reflect_widgets.hpp"

#include <fmt/format.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <ImGuizmo.h> // after imgui.h

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace f3d::editor {

namespace rhi = render::rhi;

// --- Viewport --------------------------------------------------------------------------------

ViewportPanel::ViewportPanel(rhi::Device& device, render::raster::RasterEngine& renderer, render::RenderScene scene,
                             EditorCamera& camera, ViewportTools tools)
    : Panel(panel_ids::kViewport, "Viewport"), device_(device), renderer_(renderer), scene_(scene), camera_(camera),
      tools_(std::move(tools)) {}

render::raster::View ViewportPanel::view() const {
    render::raster::View view;
    view.view = camera_.view_matrix();
    view.width = std::max(width_, 1u);
    view.height = std::max(height_, 1u);
    view.projection = camera_.projection_matrix(static_cast<float>(view.width) / static_cast<float>(view.height));
    view.camera_position = camera_.position();
    view.orthographic = camera_.projection == Projection::Orthographic;
    view.near_plane = camera_.near_plane;
    view.far_plane = camera_.far_plane;
    return view;
}

bool ViewportPanel::cancellable() const {
    if (box_pending_ || (tools_.transform && tools_.transform->active())) {
        return true;
    }
    const ViewportTool* tool = tools_.landscape ? tools_.landscape->active() : nullptr;
    return tool && tool->busy();
}

bool ViewportPanel::ray_at(const glm::vec2& screen, glm::vec3& origin, glm::vec3& direction) const {
    const glm::vec2 local = screen - image_min_;
    if (local.x < 0.0f || local.y < 0.0f || local.x >= logical_size_.x || local.y >= logical_size_.y) {
        return false;
    }
    const glm::vec2 scale = glm::vec2(static_cast<float>(width_), static_cast<float>(height_)) /
                            glm::max(logical_size_, glm::vec2(1.0f));
    view().pixel_ray(local.x * scale.x, local.y * scale.y, origin, direction);
    return true;
}

void ViewportPanel::resize(std::uint32_t width, std::uint32_t height) {
    if (width == width_ && height == height_ && display_) {
        return;
    }
    width_ = width;
    height_ = height;
    display_ =
        device_.create_texture({"Viewport display", width, height, render::raster::RasterEngine::display_format(),
                                rhi::BindFlags::RenderTarget | rhi::BindFlags::ShaderResource});
}

void ViewportPanel::draw_contents() {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 1.0f || avail.y < 1.0f) {
        hovered_ = false;
        return;
    }
    const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    if (forced_size_.x > 0 && forced_size_.y > 0) {
        resize(forced_size_.x, forced_size_.y);
    } else {
        resize(static_cast<std::uint32_t>(std::max(1.0f, std::floor(avail.x * scale.x))),
               static_cast<std::uint32_t>(std::max(1.0f, std::floor(avail.y * scale.y))));
    }
    logical_size_ = {avail.x, avail.y};

    const ImVec2 image_min = ImGui::GetCursorScreenPos();
    image_min_ = {image_min.x, image_min.y};
    ImGui::Image(static_cast<ImTextureID>(rhi::ImGuiRenderer::texture_id(display_)), avail);
    // Not hovered while over the tool overlay (a child window drawn on top).
    hovered_ = ImGui::IsItemHovered();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload);
            payload && tools_.on_asset_drop) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const ImVec2 fb_scale = ImGui::GetIO().DisplayFramebufferScale;
            glm::vec3 origin, direction;
            view().pixel_ray((mouse.x - image_min.x) * fb_scale.x, (mouse.y - image_min.y) * fb_scale.y, origin,
                             direction);
            if (const auto guid = core::Guid::parse(static_cast<const char*>(payload->Data))) {
                tools_.on_asset_drop(*guid, origin, direction);
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (active_tool()) {
        if (tools_.transform) {
            tools_.transform->cancel();
        }
        gizmo_busy_ = false;
        handle_tool_input({image_min.x, image_min.y});
    } else if (measuring()) {
        if (tools_.transform) {
            tools_.transform->cancel();
        }
        gizmo_busy_ = false;
        handle_measure_input({image_min.x, image_min.y}, {avail.x, avail.y});
    } else {
        draw_gizmo({image_min.x, image_min.y}, {avail.x, avail.y});
        handle_selection_input({image_min.x, image_min.y}, {avail.x, avail.y});
    }

    if (!overlay_text_.empty()) {
        const float pad = ImGui::GetStyle().ItemSpacing.x;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddText(ImVec2(image_min.x + pad + 1.0f, image_min.y + pad + 1.0f), IM_COL32(0, 0, 0, 200),
                      overlay_text_.c_str());
        draw->AddText(ImVec2(image_min.x + pad, image_min.y + pad), IM_COL32(225, 225, 225, 230),
                      overlay_text_.c_str());
    }
    // What the active tool is doing (a modal edit's value and keys), bottom left.
    if (const ViewportTool* tool = active_tool(); tool && !tool->scripted) {
        if (const std::string status = tool->status(); !status.empty()) {
            const float pad = ImGui::GetStyle().ItemSpacing.x;
            const ImVec2 at(image_min.x + pad, image_min.y + avail.y - ImGui::GetTextLineHeight() - pad * 1.5f);
            const ImVec2 size = ImGui::CalcTextSize(status.c_str());
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(ImVec2(at.x - pad * 0.5f, at.y - pad * 0.5f),
                                ImVec2(at.x + size.x + pad * 0.5f, at.y + size.y + pad * 0.5f),
                                IM_COL32(0, 0, 0, tool->busy() ? 190 : 120), 4.0f);
            draw->AddText(at, tool->busy() ? IM_COL32(255, 220, 120, 255) : IM_COL32(225, 225, 225, 220),
                          status.c_str());
        }
    }
    draw_axis_widget({image_min.x, image_min.y}, {avail.x, avail.y});
    if (measuring()) {
        draw_measure_labels({image_min.x, image_min.y}, {avail.x, avail.y});
    }
    draw_toolbar({image_min.x, image_min.y}, {avail.x, avail.y});
    draw_stats({image_min.x, image_min.y}, {avail.x, avail.y});
    draw_camera_preview({image_min.x, image_min.y}, {avail.x, avail.y});
    draw_camera_menu({image_min.x, image_min.y});
}

void ViewportPanel::draw_camera_menu(const glm::vec2& image_min) {
    CameraActors* cameras = tools_.cameras;
    if (!cameras) {
        return;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    const float pad = style.ItemSpacing.x;
    // Below the overlay text (top left).
    ImGui::SetCursorScreenPos(ImVec2(image_min.x + pad, image_min.y + pad * 2.0f + ImGui::GetTextLineHeight()));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.09f, 0.72f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
    if (ImGui::BeginChild("##camera_tools", ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY |
                              ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::Button("Camera")) {
            ImGui::OpenPopup("##camera_menu");
        }
        const core::Guid piloted = cameras->piloted();
        if (!piloted.is_null()) {
            // Unreal's "[ Pilot Active ]" banner.
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "[ Piloting %s ]", cameras->name(piloted).c_str());
            ImGui::SameLine();
            if (ImGui::Button("Eject")) {
                cameras->eject(camera_);
            }
        }
        if (ImGui::BeginPopup("##camera_menu")) {
            const core::Guid selected = cameras->selected_camera();
            const core::Guid pinned = cameras->pinned();
            if (ImGui::MenuItem("Add Camera at View")) {
                cameras->create_at_view(camera_);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Pilot Selected Camera", nullptr, false, !selected.is_null() && selected != piloted)) {
                cameras->pilot(selected, camera_);
            }
            if (ImGui::MenuItem("Eject", nullptr, false, !piloted.is_null())) {
                cameras->eject(camera_);
            }
            if (ImGui::MenuItem("Snap Selected Camera to View", nullptr, false,
                                !selected.is_null() && selected != piloted)) {
                cameras->snap_to_view(selected, camera_);
            }
            if (ImGui::MenuItem("Pin Selected Camera Preview", nullptr, !selected.is_null() && pinned == selected,
                                !selected.is_null())) {
                cameras->toggle_pin(selected);
            }
            if (ImGui::MenuItem("Unpin Preview", nullptr, false, !pinned.is_null())) {
                cameras->toggle_pin(pinned);
            }
            ImGui::Separator();
            ImGui::BeginDisabled(!piloted.is_null()); // while piloting, the camera's lens rules
            float fov = glm::degrees(camera_.fov_y);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
            if (ImGui::SliderFloat("Field of View", &fov, 5.0f, 170.0f, "%.0f\u00b0")) {
                camera_.fov_y = glm::radians(fov);
            }
            ImGui::EndDisabled();
            if (NavigationController* navigation = tools_.navigation) {
                float speed = navigation->speed_scale();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
                if (ImGui::SliderFloat("Camera Speed", &speed, NavigationController::kMinSpeedScale,
                                       NavigationController::kMaxSpeedScale, "x%.2f",
                                       ImGuiSliderFlags_Logarithmic)) {
                    navigation->set_speed_scale(speed);
                }
                ImGui::TextDisabled("The mouse wheel changes it while flying (right drag).");
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void ViewportPanel::draw_camera_preview(const glm::vec2& image_min, const glm::vec2& image_size) {
    preview_view_.reset();
    CameraActors* cameras = tools_.cameras;
    if (!cameras || !tools_.preview_renderer) {
        return;
    }
    const core::Guid camera = cameras->preview_camera();
    if (camera.is_null()) {
        return;
    }
    // Bottom right, a quarter of the viewport wide, in the camera's aspect (Unreal's preview).
    const float aspect = cameras->aspect(camera);
    float width = std::clamp(image_size.x * 0.25f, 160.0f, 520.0f);
    float height = width / aspect;
    const float max_height = image_size.y * 0.45f;
    if (height > max_height) {
        height = max_height;
        width = height * aspect;
    }
    if (width < 32.0f || height < 32.0f) {
        return;
    }
    const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    const auto pixel_w = static_cast<std::uint32_t>(std::max(1.0f, std::floor(width * scale.x)));
    const auto pixel_h = static_cast<std::uint32_t>(std::max(1.0f, std::floor(height * scale.y)));
    if (!preview_ || preview_.width() != pixel_w || preview_.height() != pixel_h) {
        preview_ = device_.create_texture({"Camera preview", pixel_w, pixel_h,
                                           render::raster::RasterEngine::display_format(),
                                           rhi::BindFlags::RenderTarget | rhi::BindFlags::ShaderResource});
    }
    preview_view_ = cameras->view(camera, pixel_w, pixel_h);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float pad = style.ItemSpacing.x;
    const float header = ImGui::GetFrameHeight() + style.ItemSpacing.y;
    const ImVec2 min(image_min.x + image_size.x - width - pad - style.WindowPadding.x * 2.0f,
                     image_min.y + image_size.y - height - header - pad - style.WindowPadding.y * 2.0f);
    ImGui::SetCursorScreenPos(min);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.09f, 0.85f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
    if (ImGui::BeginChild("##camera_preview", ImVec2(width + style.WindowPadding.x * 2.0f, 0.0f),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        const bool pinned = cameras->pinned() == camera;
        const char* pin_label = pinned ? "Unpin" : "Pin";
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(cameras->name(camera).c_str());
        const float buttons = ImGui::CalcTextSize(pin_label).x + ImGui::CalcTextSize("Pilot").x +
                              style.FramePadding.x * 4.0f + style.ItemSpacing.x;
        ImGui::SameLine(std::max(ImGui::GetWindowContentRegionMax().x - buttons, ImGui::GetCursorPosX()));
        if (pinned) {
            ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
        }
        if (ImGui::Button(pin_label)) {
            cameras->toggle_pin(camera);
        }
        if (pinned) {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(pinned ? "Stop showing this preview when the camera is not selected"
                                     : "Keep showing this preview after the camera is deselected");
        }
        ImGui::SameLine();
        if (ImGui::Button("Pilot")) {
            cameras->pilot(camera, camera_);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Look through this camera; moving the view moves the camera");
        }
        ImGui::Image(static_cast<ImTextureID>(rhi::ImGuiRenderer::texture_id(preview_)), ImVec2(width, height));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void ViewportPanel::draw_stats(const glm::vec2& image_min, const glm::vec2& image_size) {
    frame_time_.add(ImGui::GetIO().DeltaTime * 1000.0f);
    if (!show_stats_ || !*show_stats_) {
        return;
    }
    ViewportStats stats;
    stats.frame_ms = frame_time_.value();
    stats.gpu_ms = device_.gpu_timings();
    stats.draw_calls = renderer_.last_mesh_draws();
    stats.local_lights = renderer_.last_local_lights();
    const rhi::MemoryUsage memory = device_.memory_usage();
    stats.vram_used = memory.vram_used;
    stats.vram_budget = memory.vram_budget;
    stats.ev100 = std::numeric_limits<float>::quiet_NaN(); // reading the GPU exposure back would stall
    const std::vector<std::string> lines = format_viewport_stats(stats);
    const float pad = ImGui::GetStyle().ItemSpacing.x;
    const float line_height = ImGui::GetTextLineHeight();
    float width = 0.0f;
    for (const std::string& line : lines) {
        width = std::max(width, ImGui::CalcTextSize(line.c_str()).x);
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Top right, below the toolbar row.
    const ImVec2 origin(image_min.x + image_size.x - width - pad * 3.0f, image_min.y + line_height * 2.5f + pad);
    draw->AddRectFilled(ImVec2(origin.x - pad, origin.y - pad),
                        ImVec2(origin.x + width + pad, origin.y + line_height * static_cast<float>(lines.size()) + pad),
                        IM_COL32(0, 0, 0, 140), 4.0f);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        draw->AddText(ImVec2(origin.x, origin.y + line_height * static_cast<float>(i)), IM_COL32(225, 225, 225, 235),
                      lines[i].c_str());
    }
}

void ViewportPanel::handle_measure_input(const glm::vec2& image_min, const glm::vec2&) {
    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 mouse = glm::vec2(io.MousePos.x, io.MousePos.y) - image_min;
    const glm::vec2 scale(io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    const auto clicked = [&](ImGuiMouseButton button) {
        if (!hovered_ || !ImGui::IsMouseReleased(button)) {
            return false;
        }
        const ImVec2 drag = ImGui::GetMouseDragDelta(button, 0.0f);
        return drag.x * drag.x + drag.y * drag.y < 16.0f; // a click, not a navigation drag
    };
    if (clicked(ImGuiMouseButton_Left) && !io.KeyAlt && tools_.surface_point) {
        glm::vec3 origin, direction;
        view().pixel_ray(mouse.x * scale.x, mouse.y * scale.y, origin, direction);
        if (const auto point = tools_.surface_point(origin, direction)) {
            tools_.measure->add(*point);
        }
    }
    if (clicked(ImGuiMouseButton_Right) || (hovered_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        tools_.measure->clear();
    }
}

ViewportTool* ViewportPanel::active_tool() const noexcept {
    return tools_.landscape ? tools_.landscape->sync() : nullptr;
}

void ViewportPanel::handle_tool_input(const glm::vec2& image_min) {
    ViewportTool& tool = *active_tool();
    if (tool.scripted) {
        return; // a replay drives it
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (hovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
        tool_drag_ = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        tool_drag_ = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (tool.busy()) {
            tool.cancel();
        } else if (tools_.landscape) {
            tools_.landscape->activate(nullptr); // Escape with nothing in progress leaves the tool
            return;
        }
        tool_drag_ = false;
    }
    // Blender: a right-click cancels the stroke or modal edit in progress.
    if (hovered_ && tool.busy() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        tool.cancel();
        tool_drag_ = false;
    }
    if (hovered_ && tool.busy() && io.MouseWheel != 0.0f) {
        tool.wheel(io.MouseWheel); // e.g. the number of loop cuts (the App skips zooming then)
    }
    if (!hovered_ && !tool_drag_) {
        tool.leave();
        return;
    }
    const glm::vec2 mouse = glm::vec2(io.MousePos.x, io.MousePos.y) - image_min;
    glm::vec3 origin, direction;
    view().pixel_ray(mouse.x * io.DisplayFramebufferScale.x, mouse.y * io.DisplayFramebufferScale.y, origin,
                     direction);
    tool.update(origin, direction, tool_drag_, tools_.pen_pressure ? *tools_.pen_pressure : 1.0f);
}

void ViewportPanel::draw_measure_labels(const glm::vec2& image_min, const glm::vec2& image_size) const {
    const std::vector<glm::vec3>& points = tools_.measure->points();
    const glm::mat4 view_projection = view().view_projection();
    const auto to_screen = [&](const glm::vec3& p) -> std::optional<ImVec2> {
        const glm::vec4 clip = view_projection * glm::vec4(p, 1.0f);
        if (clip.w <= 1e-5f) {
            return std::nullopt;
        }
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        return ImVec2(image_min.x + (ndc.x * 0.5f + 0.5f) * image_size.x,
                      image_min.y + (0.5f - ndc.y * 0.5f) * image_size.y);
    };
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const auto label = [&](const glm::vec3& at, const char* text) {
        if (const auto s = to_screen(at)) {
            const ImVec2 size = ImGui::CalcTextSize(text);
            const ImVec2 min(s->x - size.x * 0.5f - 4.0f, s->y - size.y - 8.0f);
            draw->AddRectFilled(min, ImVec2(min.x + size.x + 8.0f, min.y + size.y + 4.0f), IM_COL32(0, 0, 0, 170),
                                3.0f);
            draw->AddText(ImVec2(min.x + 4.0f, min.y + 2.0f), IM_COL32(255, 220, 60, 255), text);
        }
    };
    char text[64];
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        std::snprintf(text, sizeof(text), "%.3f m", *tools_.measure->segment(i));
        label((points[i] + points[i + 1]) * 0.5f, text);
    }
    if (const auto angle = tools_.measure->angle_degrees()) {
        std::snprintf(text, sizeof(text), "%.1f\u00b0", *angle);
        label(points[1], text);
    }
    const char* hint = points.empty() ? "Measure: click points on the scene (right-click clears)" : nullptr;
    if (hint) {
        const ImVec2 size = ImGui::CalcTextSize(hint);
        draw->AddText(ImVec2(image_min.x + (image_size.x - size.x) * 0.5f, image_min.y + image_size.y - size.y - 12.0f),
                      IM_COL32(255, 220, 60, 230), hint);
    }
}

void ViewportPanel::draw_gizmo(const glm::vec2& image_min, const glm::vec2& image_size) {
    gizmo_busy_ = false;
    TransformTool* tool = tools_.transform;
    if (!tool) {
        return;
    }
    const std::optional<glm::mat4> current = tool->gizmo_matrix();
    if (!current) {
        tool->cancel(); // the selection vanished mid-drag (undo, delete)
        return;
    }
    const render::raster::View v = view();
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(image_min.x, image_min.y, image_size.x, image_size.y);
    ImGuizmo::SetOrthographic(v.orthographic);
    ImGuizmo::AllowAxisFlip(false);

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    float snap[3] = {tool->snap.translate_step, tool->snap.translate_step, tool->snap.translate_step};
    if (tool->operation == GizmoOperation::Rotate) {
        operation = ImGuizmo::ROTATE;
        snap[0] = tool->snap.rotate_step;
    } else if (tool->operation == GizmoOperation::Scale) {
        operation = ImGuizmo::SCALE;
        snap[0] = snap[1] = snap[2] = tool->snap.scale_step;
    }
    // While dragging, feed ImGuizmo its own output so rotations accumulate; the
    // gizmo's orientation is already in the matrix, so LOCAL mode draws it as is.
    glm::mat4 matrix = tool->active() ? drag_gizmo_ : *current;
    ImGuizmo::Manipulate(glm::value_ptr(v.view), glm::value_ptr(v.projection), operation, ImGuizmo::LOCAL,
                         glm::value_ptr(matrix), nullptr, tool->snap.grid ? snap : nullptr);
    const bool using_gizmo = ImGuizmo::IsUsing();
    gizmo_busy_ = using_gizmo || ImGuizmo::IsOver();
    if (tool->active() &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        tool->cancel(); // back to where the drag started, nothing recorded (Esc or right-click)
        drag_cancelled_ = true;
    }
    if (drag_cancelled_) {
        drag_cancelled_ = using_gizmo; // until the button is released
        return;
    }
    if (using_gizmo) {
        if (!tool->active() && !tool->begin()) {
            return;
        }
        if (tool->operation == GizmoOperation::Translate && tool->snap.target != SnapTarget::None &&
            tools_.snap_point) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
            glm::vec3 origin, direction;
            v.pixel_ray((mouse.x - image_min.x) * scale.x, (mouse.y - image_min.y) * scale.y, origin, direction);
            float max_angle = v.pixel_angle(tool->snap.vertex_radius_px * scale.y);
            if (v.orthographic) {
                max_angle = std::atan(tool->snap.vertex_radius_px * camera_.units_per_pixel(image_size.y) /
                                      std::max(camera_.distance, 1e-3f));
            }
            if (const auto point = tools_.snap_point(origin, direction, max_angle)) {
                matrix[3] = glm::vec4(*point, 1.0f);
            }
        }
        drag_gizmo_ = matrix;
        tool->update(matrix);
    } else if (tool->active()) {
        tool->end();
    }
}

void ViewportPanel::handle_selection_input(const glm::vec2& image_min, const glm::vec2& image_size) {
    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 mouse = glm::vec2(io.MousePos.x, io.MousePos.y) - image_min;
    const glm::vec2 scale(io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    const glm::vec2 max_pixel(static_cast<float>(width_ - 1), static_cast<float>(height_ - 1));
    const auto to_pixels = [&](glm::vec2 p) {
        p = glm::clamp(p, glm::vec2(0.0f), image_size);
        return glm::uvec2(glm::min(p * scale, max_pixel));
    };

    // Shift + right-click places the 3D cursor.
    if (hovered_ && io.KeyShift && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && tools_.surface_point &&
        tools_.transform) {
        const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 0.0f);
        if (drag.x * drag.x + drag.y * drag.y < 16.0f) {
            glm::vec3 origin, direction;
            view().pixel_ray(mouse.x * scale.x, mouse.y * scale.y, origin, direction);
            if (const auto point = tools_.surface_point(origin, direction)) {
                tools_.transform->cursor = *point;
            }
        }
    }

    // Left button: click to pick, drag for a box. Alt + left drags navigate (Maya/Unreal).
    if (hovered_ && !gizmo_busy_ && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        box_pending_ = true;
        box_start_ = mouse;
    }
    if (!box_pending_) {
        return;
    }
    if (tools_.transform && tools_.transform->active()) {
        box_pending_ = false; // the click started a gizmo drag after all
        return;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        box_pending_ = false; // cancelled: the release selects nothing
        return;
    }
    const bool dragged = glm::distance(mouse, box_start_) > 4.0f;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (dragged) {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 a(image_min.x + std::min(box_start_.x, mouse.x),
                           image_min.y + std::min(box_start_.y, mouse.y));
            const ImVec2 b(image_min.x + std::max(box_start_.x, mouse.x),
                           image_min.y + std::max(box_start_.y, mouse.y));
            draw->AddRectFilled(a, b, IM_COL32(90, 150, 255, 40));
            draw->AddRect(a, b, IM_COL32(90, 150, 255, 200));
        }
        return;
    }
    // Released.
    box_pending_ = false;
    if (!tools_.on_pick) {
        return;
    }
    ViewportPick pick;
    pick.box = dragged;
    if (io.KeyCtrl) {
        pick.mode = SelectMode::Remove;
    } else if (io.KeyShift) {
        pick.mode = dragged ? SelectMode::Add : SelectMode::Toggle;
    }
    if (dragged) {
        const glm::uvec2 p0 = to_pixels(glm::min(box_start_, mouse));
        const glm::uvec2 p1 = to_pixels(glm::max(box_start_, mouse));
        pick.x0 = p0.x;
        pick.y0 = p0.y;
        pick.x1 = p1.x + 1;
        pick.y1 = p1.y + 1;
    } else {
        const glm::uvec2 p = to_pixels(mouse);
        pick.x0 = p.x;
        pick.y0 = p.y;
        pick.x1 = p.x + 1;
        pick.y1 = p.y + 1;
    }
    tools_.on_pick(pick);
}

namespace {

/// Draws the Move / Rotate / Scale tool icon into the square [min, min + size).
void draw_gizmo_icon(ImDrawList* draw, const ImVec2& min, float size, GizmoOperation op, ImU32 color) {
    const float t = std::max(1.0f, size * 0.08f); // stroke
    const ImVec2 c(min.x + size * 0.5f, min.y + size * 0.5f);
    const float r = size * 0.36f;
    const auto arrow_head = [&](const ImVec2& tip, const ImVec2& dir) {
        const float h = size * 0.16f;
        const ImVec2 n(-dir.y, dir.x);
        draw->AddTriangleFilled(tip, ImVec2(tip.x - dir.x * h + n.x * h * 0.7f, tip.y - dir.y * h + n.y * h * 0.7f),
                                ImVec2(tip.x - dir.x * h - n.x * h * 0.7f, tip.y - dir.y * h - n.y * h * 0.7f), color);
    };
    switch (op) {
    case GizmoOperation::Translate: // four-way arrows
        draw->AddLine(ImVec2(c.x - r * 0.8f, c.y), ImVec2(c.x + r * 0.8f, c.y), color, t);
        draw->AddLine(ImVec2(c.x, c.y - r * 0.8f), ImVec2(c.x, c.y + r * 0.8f), color, t);
        arrow_head(ImVec2(c.x + r, c.y), ImVec2(1, 0));
        arrow_head(ImVec2(c.x - r, c.y), ImVec2(-1, 0));
        arrow_head(ImVec2(c.x, c.y - r), ImVec2(0, -1));
        arrow_head(ImVec2(c.x, c.y + r), ImVec2(0, 1));
        break;
    case GizmoOperation::Rotate: { // open circle with an arrow at its end
        const float a0 = -1.2f;
        const float a1 = a0 + 5.0f;
        draw->PathArcTo(c, r * 0.85f, a0, a1, 24);
        draw->PathStroke(color, ImDrawFlags_None, t);
        const ImVec2 end(c.x + std::cos(a1) * r * 0.85f, c.y + std::sin(a1) * r * 0.85f);
        const ImVec2 tangent(-std::sin(a1), std::cos(a1));
        arrow_head(ImVec2(end.x + tangent.x * size * 0.08f, end.y + tangent.y * size * 0.08f), tangent);
        break;
    }
    case GizmoOperation::Scale: { // small box, big box outline, diagonal arrow
        const float lo = c.x - r;
        const float hi = c.x + r;
        const float top = c.y - r;
        const float bottom = c.y + r;
        draw->AddRect(ImVec2(lo, top), ImVec2(hi, bottom), color, 0.0f, ImDrawFlags_None, t * 0.75f);
        draw->AddRectFilled(ImVec2(lo, bottom - r * 0.8f), ImVec2(lo + r * 0.8f, bottom), color);
        const ImVec2 tip(hi - r * 0.25f, top + r * 0.25f);
        draw->AddLine(ImVec2(lo + r * 0.8f, bottom - r * 0.8f), ImVec2(tip.x - size * 0.06f, tip.y + size * 0.06f),
                      color, t);
        arrow_head(tip, ImVec2(0.7071f, -0.7071f));
        break;
    }
    }
}

/// A square toggle button showing a tool icon, with its name as the tooltip.
bool gizmo_icon_button(const char* id, const char* tooltip, GizmoOperation op, bool active, float size) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImGuiCol bg = active                                        ? ImGuiCol_ButtonActive
                        : hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left) ? ImGuiCol_ButtonActive
                        : hovered                                     ? ImGuiCol_ButtonHovered
                                                                      : ImGuiCol_Button;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, ImGui::GetColorU32(bg), style.FrameRounding);
    draw_gizmo_icon(draw, min, size, op, ImGui::GetColorU32(ImGuiCol_Text));
    if (hovered) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return pressed;
}

} // namespace

void ViewportPanel::draw_toolbar(const glm::vec2& image_min, const glm::vec2& image_size) {
    TransformTool* tool = tools_.transform;
    if (!tool) {
        return;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    const float pad = style.ItemSpacing.x;
    const float icon = ImGui::GetFrameHeight() * 1.25f;
    // Wide enough for the icon row and the longest pivot option.
    const float width = std::max(3.0f * icon + 2.0f * style.ItemSpacing.x,
                                 ImGui::CalcTextSize("Individual Origins").x + ImGui::GetFrameHeight() +
                                     2.0f * style.FramePadding.x) +
                        2.0f * style.WindowPadding.x;
    ImGui::SetCursorScreenPos(ImVec2(image_min.x + image_size.x - width - pad, image_min.y + pad));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.09f, 0.72f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
    if (ImGui::BeginChild("##viewport_tools", ImVec2(width, 0.0f),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        const auto op_button = [&](const char* id, const char* tooltip, GizmoOperation op) {
            if (gizmo_icon_button(id, tooltip, op, tool->operation == op, icon)) {
                tool->operation = op;
            }
        };
        op_button("##move", "Move", GizmoOperation::Translate);
        ImGui::SameLine();
        op_button("##rotate", "Rotate", GizmoOperation::Rotate);
        ImGui::SameLine();
        op_button("##scale", "Scale", GizmoOperation::Scale);
        if (tools_.measuring && tools_.measure) {
            ImGui::Checkbox("Measure", tools_.measuring);
        }

        const auto combo = [](const char* id, const char* tooltip, auto& value,
                              std::initializer_list<std::decay_t<decltype(value)>> options) {
            ImGui::SetNextItemWidth(-1.0f);
            const bool open = ImGui::BeginCombo(id, std::string(to_string(value)).c_str());
            ImGui::SetItemTooltip("%s", tooltip);
            if (open) {
                for (const auto option : options) {
                    if (ImGui::Selectable(std::string(to_string(option)).c_str(), value == option)) {
                        value = option;
                    }
                }
                ImGui::EndCombo();
            }
        };
        combo("##space", "Gizmo axes: World (fixed X/Y/Z) or Local (the object's own rotation)", tool->space,
              {GizmoSpace::World, GizmoSpace::Local});
        combo("##pivot", "Point that rotation and scaling happen around", tool->pivot,
              {PivotMode::Median, PivotMode::IndividualOrigins, PivotMode::ActiveElement, PivotMode::Cursor});
        ImGui::Checkbox("Grid snap", &tool->snap.grid);
        ImGui::SetItemTooltip("Move, rotate and scale in fixed steps");
        combo("##snap_target", "Snap dragged objects onto: nothing, the surface under the mouse, or the nearest vertex",
              tool->snap.target, {SnapTarget::None, SnapTarget::Surface, SnapTarget::Vertex});
        if (tool->snap.grid) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##step_t", &tool->snap.translate_step, 0.01f, 0.001f, 100.0f, "Move %.3f m");
            ImGui::SetItemTooltip("Grid snap step for moving");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##step_r", &tool->snap.rotate_step, 0.5f, 0.1f, 180.0f, "Rotate %.1f°");
            ImGui::SetItemTooltip("Grid snap step for rotating");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##step_s", &tool->snap.scale_step, 0.01f, 0.001f, 10.0f, "Scale %.3f");
            ImGui::SetItemTooltip("Grid snap step for scaling");
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void ViewportPanel::draw_axis_widget(const glm::vec2& image_min, const glm::vec2& image_size) const {
    // World axes rotated into view space, drawn in the bottom-left corner; back-most axis first.
    const float scale = ImGui::GetFontSize() / 13.0f;
    const float length = 34.0f * scale;
    const ImVec2 centre(image_min.x + 22.0f * scale + length, image_min.y + image_size.y - 22.0f * scale - length);
    const glm::mat3 rotation(camera_.view_matrix());
    struct Axis {
        glm::vec3 dir;
        ImU32 color;
        const char* label;
    };
    std::array<Axis, 3> axes{{{rotation * glm::vec3(1, 0, 0), IM_COL32(230, 70, 70, 255), "X"},
                              {rotation * glm::vec3(0, 1, 0), IM_COL32(110, 200, 80, 255), "Y"},
                              {rotation * glm::vec3(0, 0, 1), IM_COL32(80, 130, 240, 255), "Z"}}};
    std::sort(axes.begin(), axes.end(), [](const Axis& a, const Axis& b) { return a.dir.z < b.dir.z; });

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddCircleFilled(centre, length + 12.0f * scale, IM_COL32(0, 0, 0, 60));
    for (const Axis& axis : axes) {
        const ImVec2 tip(centre.x + axis.dir.x * length, centre.y - axis.dir.y * length);
        draw->AddLine(centre, tip, axis.color, 2.5f * scale);
        draw->AddCircleFilled(tip, 7.5f * scale, axis.color);
        const ImVec2 text_size = ImGui::CalcTextSize(axis.label);
        draw->AddText(ImVec2(tip.x - text_size.x * 0.5f, tip.y - text_size.y * 0.5f), IM_COL32(20, 20, 20, 255),
                      axis.label);
    }
}

void ViewportPanel::render(rhi::CommandContext& ctx) {
    if (!display_ || !is_open()) {
        return;
    }
    F3D_ZONE;
    rhi::ScopedGpuTimer timer(ctx, "viewport");
    renderer_.render(ctx, view(), scene_, {display_, kViewportClearColor});
    // After the viewport, which takes this frame's debug lines: the preview shows none.
    if (preview_view_ && preview_ && tools_.preview_renderer) {
        if (render::raster::RasterEngine* preview = tools_.preview_renderer()) {
            rhi::ScopedGpuTimer preview_timer(ctx, "camera preview");
            preview->render(ctx, *preview_view_, scene_, {preview_, kViewportClearColor});
        }
    }
}

// --- Console ----------------------------------------------------------------------------------

namespace {

constexpr const char* kLevelNames[] = {"Trace", "Debug", "Info", "Warning", "Error", "Critical"};

ImVec4 level_color(core::LogLevel level) {
    switch (level) {
    case core::LogLevel::Trace:
    case core::LogLevel::Debug:
        return {0.55f, 0.55f, 0.60f, 1.0f};
    case core::LogLevel::Warn:
        return {0.95f, 0.75f, 0.30f, 1.0f};
    case core::LogLevel::Error:
    case core::LogLevel::Critical:
        return {0.95f, 0.40f, 0.35f, 1.0f};
    default:
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

} // namespace

ConsolePanel::ConsolePanel(std::shared_ptr<ConsoleModel> model)
    : Panel(panel_ids::kConsole, "Console"), model_(std::move(model)), min_level_(2) {}

void ConsolePanel::draw_contents() {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    ImGui::Combo("##level", &min_level_, kLevelNames, IM_ARRAYSIZE(kLevelNames));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    ImGui::InputTextWithHint("##search", "Search", search_.data(), search_.size());
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        model_->clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &auto_scroll_);
    ImGui::Separator();

    const auto entries = model_->filtered(static_cast<core::LogLevel>(min_level_), search_.data());
    if (ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(entries.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const ConsoleEntry& e = entries[static_cast<std::size_t>(i)];
                ImGui::TextDisabled("%s", e.time.c_str());
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, level_color(e.level));
                ImGui::TextUnformatted(e.message.c_str());
                ImGui::PopStyleColor();
            }
        }
        const std::uint64_t version = model_->version();
        if (auto_scroll_ && version != seen_version_) {
            ImGui::SetScrollHereY(1.0f);
        }
        seen_version_ = version;
    }
    ImGui::EndChild();
}

// --- Placeholders ---------------------------------------------------------------------------

InspectorPanel::InspectorPanel(scene::World* world, SceneEditor* editor, InspectorEditCallback on_edit)
    : Panel(panel_ids::kInspector, "Inspector"), world_(world), editor_(editor), on_edit_(std::move(on_edit)) {}

void InspectorPanel::draw_contents() {
    if (!world_) {
        ImGui::TextDisabled("No scene");
        return;
    }
    const scene::Selection& selection = world_->selection();
    const core::Guid active = selection.active();
    const entt::entity e = active.is_null() ? entt::null : world_->find(active);
    if (e == entt::null) {
        ImGui::TextDisabled("Nothing selected");
        return;
    }
    if (selection.size() > 1) {
        ImGui::TextDisabled("%zu selected; showing the active entity", selection.size());
    }
    ImGui::TextDisabled("%s", active.to_string().c_str());
    ImGui::Separator();
    const core::Guid guid = world_->guid(e);
    std::string remove; // applied after drawing: removing mid-iteration would invalidate the view
    scene::for_each_component(world_->registry(), e, [&](entt::meta_any component) {
        const std::string type_name = component.type().name() ? component.type().name() : "";
        std::function<void()> on_remove;
        if (editor_ && scene::is_addable_component(type_name)) {
            on_remove = [&remove, type_name] { remove = type_name; };
        }
        draw_reflected_component(
            component,
            [&](const std::string& path, const nlohmann::json& value, bool continuing) {
                if (on_edit_) {
                    on_edit_(guid, path, value, continuing);
                }
            },
            on_remove);
    });
    if (!remove.empty()) {
        editor_->run("entity.remove_component", {{"entity", guid.to_string()}, {"component", remove}});
        return;
    }
    if (!editor_) {
        return;
    }
    // Prefab instance: overrides and the actions on them.
    if (const entt::entity root = ops::prefab::instance_root(*world_, e); root != entt::null) {
        const auto& instance = world_->registry().get<scene::PrefabInstance>(root);
        ImGui::SeparatorText("Prefab");
        ImGui::TextDisabled("%s", instance.prefab.path.c_str());
        if (root != e) {
            ImGui::TextDisabled("part of the instance %s", world_->name(root).c_str());
        }
        ImGui::Text("%zu override%s", instance.overrides.size(), instance.overrides.size() == 1 ? "" : "s");
        for (const scene::PrefabOverride& o : instance.overrides) {
            ImGui::BulletText("%s", o.path.c_str());
        }
        const std::string root_guid = world_->guid(root).to_string();
        ImGui::BeginDisabled(instance.overrides.empty());
        if (ImGui::Button("Apply to Prefab")) {
            editor_->run("prefab.apply_overrides", {{"entity", root_guid}});
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            editor_->run("prefab.revert_overrides", {{"entity", root_guid}});
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Update Instances")) {
            editor_->run("prefab.update_instances", {{"prefab", instance.prefab.path}});
        }
    }
    ImGui::Spacing();
    if (ImGui::Button("Add Component", ImVec2(-1.0f, 0.0f))) {
        ImGui::OpenPopup("##add_component");
    }
    if (ImGui::BeginPopup("##add_component")) {
        bool any = false;
        for (const std::string_view name : scene::addable_components()) {
            if (scene::component_by_name(world_->registry(), e, name)) {
                continue;
            }
            any = true;
            if (ImGui::MenuItem(std::string(name).c_str())) {
                editor_->run("entity.add_component", {{"entity", guid.to_string()}, {"component", std::string(name)}});
            }
        }
        if (!any) {
            ImGui::TextDisabled("Every component is present");
        }
        ImGui::EndPopup();
    }
}

// --- Operation log ---------------------------------------------------------------------------

OperationLogPanel::OperationLogPanel(ops::OpStack* stack)
    : Panel(panel_ids::kOperationLog, "Operation Log"), stack_(stack) {}

void OperationLogPanel::draw_contents() {
    if (!stack_) {
        ImGui::TextDisabled("No history");
        return;
    }
    const std::vector<std::string> undo = stack_->undo_names();
    const std::vector<std::string> redo = stack_->redo_names();
    if (undo.empty() && redo.empty()) {
        ImGui::TextDisabled("No operations yet");
        return;
    }
    for (std::size_t i = 0; i < undo.size(); ++i) {
        const bool current = i + 1 == undo.size();
        ImGui::TextColored(current ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%zu  %s", i + 1,
                           undo[i].c_str());
    }
    for (const std::string& name : redo) {
        ImGui::TextDisabled("%s (redo)", name.c_str());
    }
}

ImportQueuePanel::ImportQueuePanel(ImportQueue* queue, const assets::AssetDatabase* database)
    : Panel(panel_ids::kImportQueue, "Import Queue"), queue_(queue), database_(database) {}

void ImportQueuePanel::draw_contents() {
    if (!queue_) {
        ImGui::TextDisabled("No import queue");
        return;
    }
    std::size_t stale = 0;
    if (database_) {
        for (const assets::AssetRecord& record : database_->records()) {
            stale += record.stale() ? 1 : 0;
        }
    }
    ImGui::BeginDisabled(stale == 0 || !database_);
    if (ImGui::Button(("Import Stale (" + std::to_string(stale) + ")").c_str())) {
        queue_->enqueue_stale(*database_);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(queue_->active() == 0);
    if (ImGui::Button("Cancel All")) {
        queue_->cancel_all();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear Finished")) {
        queue_->clear_finished();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu running or queued", queue_->active());
    ImGui::Separator();
    if (queue_->items().empty()) {
        ImGui::TextDisabled("Nothing imported yet. Imports run in the background; errors also go to the console.");
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV;
    if (!ImGui::BeginTable("##imports", 3, flags)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 10.0f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.0f);
    ImGui::TableHeadersRow();
    const std::vector<ImportQueue::Item> items = queue_->items(); // copy: Retry changes the list
    for (std::size_t i = items.size(); i-- > 0;) {                // newest first
        const ImportQueue::Item& item = items[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(item.path.c_str());
        if (!item.error.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", item.error.c_str());
        }
        ImGui::TableSetColumnIndex(1);
        if (item.state == ImportQueue::State::Running) {
            ImGui::ProgressBar(item.progress, ImVec2(-1.0f, 0.0f));
        } else if (item.state == ImportQueue::State::Failed) {
            ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.35f, 1.0f), "%s", to_string(item.state));
        } else {
            ImGui::TextDisabled("%s", to_string(item.state));
        }
        ImGui::TableSetColumnIndex(2);
        if ((item.state == ImportQueue::State::Failed || item.state == ImportQueue::State::Cancelled) && database_) {
            if (const assets::AssetRecord* record = database_->find(item.guid); record && ImGui::SmallButton("Retry")) {
                queue_->enqueue(*record, true);
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

ResidencyPanel::ResidencyPanel(const render::raster::ResidencyManager* residency)
    : Panel(panel_ids::kResidency, "Residency"), residency_(residency) {
    set_open(false); // a diagnostics panel: Window menu
}

void ResidencyPanel::draw_contents() {
    if (!residency_) {
        ImGui::TextDisabled("No residency manager");
        return;
    }
    constexpr double kMiB = 1024.0 * 1024.0;
    const render::raster::ResidencyStats& stats = residency_->stats();
    const float fraction =
        stats.budget ? static_cast<float>(static_cast<double>(stats.used) / static_cast<double>(stats.budget)) : 0.0f;
    const std::string overlay = fmt::format("{:.1f} / {:.0f} MB", static_cast<double>(stats.used) / kMiB,
                                            static_cast<double>(stats.budget) / kMiB);
    ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay.c_str());
    ImGui::TextDisabled("%zu meshes, %zu textures resident; %zu requested last frame; peak %.1f MB", stats.meshes,
                        stats.textures, stats.requested, static_cast<double>(stats.peak) / kMiB);
    ImGui::TextDisabled("%llu uploads, %llu evictions", static_cast<unsigned long long>(stats.uploads),
                        static_cast<unsigned long long>(stats.evictions));
    ImGui::Separator();
    std::vector<render::raster::ResidencyRow> rows = residency_->rows();
    if (rows.empty()) {
        ImGui::TextDisabled("Nothing streamed. Project meshes and textures appear here once drawn.");
        return;
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.bytes > b.bytes; });
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV;
    if (!ImGui::BeginTable("##residency", 5, flags)) {
        return;
    }
    const float em = ImGui::GetFontSize();
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, em * 4.0f);
    ImGui::TableSetupColumn("Resident / wanted", ImGuiTableColumnFlags_WidthFixed, em * 8.0f);
    ImGui::TableSetupColumn("Screen px", ImGuiTableColumnFlags_WidthFixed, em * 5.0f);
    ImGui::TableSetupColumn("MB", ImGuiTableColumnFlags_WidthFixed, em * 4.0f);
    ImGui::TableHeadersRow();
    const auto level = [](int value) { return value < 0 ? std::string("-") : std::to_string(value); };
    for (const render::raster::ResidencyRow& row : rows) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(row.name.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("%s", row.texture ? "mip" : "LOD");
        ImGui::TableSetColumnIndex(2);
        const std::string levels =
            level(row.resident) + " / " + level(row.wanted) + " of " + std::to_string(row.levels);
        if (row.wanted >= 0 && row.resident != row.wanted) {
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1.0f), "%s", levels.c_str()); // still streaming
        } else {
            ImGui::TextUnformatted(levels.c_str());
        }
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%.0f", row.screen_px);
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%.2f", static_cast<double>(row.bytes) / kMiB);
    }
    ImGui::EndTable();
}

} // namespace f3d::editor
