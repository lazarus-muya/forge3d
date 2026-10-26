// SPDX-License-Identifier: Apache-2.0
#include "f3d/render/raster/capture.hpp"

#include "f3d/core/image.hpp"
#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/ops/operation.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/layers.hpp"
#include "f3d/scene/world.hpp"

#include <entt/meta/factory.hpp>

#include <algorithm>
#include <cmath>

namespace f3d::render::raster {
namespace {

// Matches editor::EditorCamera (its defaults and orbit convention).
constexpr float kFovY = 0.872665f; // 50 degrees
constexpr float kNear = 0.05f;
constexpr float kFar = 5000.0f;

View orbit_view(const glm::vec3& target, float distance, float yaw, float pitch, bool orthographic, float aspect) {
    const glm::quat orientation = glm::angleAxis(yaw, core::kWorldUp) * glm::angleAxis(pitch, core::kWorldRight);
    const glm::vec3 position = target - (orientation * core::kWorldForward) * distance;
    View view;
    view.camera_position = position;
    view.view = glm::inverse(glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(orientation));
    view.orthographic = orthographic;
    view.near_plane = kNear;
    view.far_plane = kFar;
    if (orthographic) {
        const float h = distance * std::tan(kFovY * 0.5f);
        const float w = h * aspect;
        view.projection = glm::orthoRH_ZO(-w, w, -h, h, -kFar * 0.5f, kFar);
    } else {
        view.projection = glm::perspectiveRH_ZO(kFovY, aspect, kNear, kFar);
    }
    return view;
}

void fail(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
}

} // namespace

std::optional<View> capture_view(scene::World& world, assets::MeshLibrary* meshes, const CaptureRequest& request,
                                 const View* fallback, std::string* error) {
    const float aspect =
        static_cast<float>(std::max(request.width, 1u)) / static_cast<float>(std::max(request.height, 1u));
    std::optional<View> view;
    if (!request.camera.is_null()) {
        const entt::entity e = world.find(request.camera);
        const auto* camera = e == entt::null ? nullptr : world.registry().try_get<scene::Camera>(e);
        if (!camera) {
            fail(error, "no Camera entity " + request.camera.to_string());
            return std::nullopt;
        }
        world.update_transforms();
        const glm::mat4& m = world.transform(e).world;
        // The camera's position and orientation (its scale doesn't affect the view).
        const glm::vec3 position(m[3]);
        const glm::mat3 axes(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])),
                             glm::normalize(glm::vec3(m[2])));
        glm::mat4 rigid(axes);
        rigid[3] = glm::vec4(position, 1.0f);
        View v;
        v.camera_position = position;
        v.view = glm::inverse(rigid);
        v.near_plane = camera->near_plane;
        v.far_plane = camera->far_plane;
        v.orthographic = camera->projection == scene::CameraProjection::Orthographic;
        if (v.orthographic) {
            const float h = camera->ortho_height * 0.5f;
            v.projection = glm::orthoRH_ZO(-h * aspect, h * aspect, -h, h, camera->near_plane, camera->far_plane);
        } else {
            v.projection = glm::perspectiveRH_ZO(camera->fov_y, aspect, camera->near_plane, camera->far_plane);
        }
        view = v;
    } else if (!request.bookmark.empty()) {
        const scene::CameraBookmark* bookmark = world.bookmarks().find(request.bookmark);
        if (!bookmark) {
            fail(error, "no camera bookmark '" + request.bookmark + "'");
            return std::nullopt;
        }
        view = orbit_view(bookmark->target, bookmark->distance, bookmark->yaw, bookmark->pitch, bookmark->orthographic,
                          aspect);
    } else if (fallback) {
        view = *fallback;
        // Same camera, the capture's aspect ratio.
        if (fallback->orthographic) {
            const float h = 1.0f / fallback->projection[1][1];
            view->projection =
                glm::orthoRH_ZO(-h * aspect, h * aspect, -h, h, -fallback->far_plane * 0.5f, fallback->far_plane);
        } else {
            const float fov = 2.0f * std::atan(1.0f / fallback->projection[1][1]);
            view->projection = glm::perspectiveRH_ZO(fov, aspect, fallback->near_plane, fallback->far_plane);
        }
    } else {
        // Frame every visible mesh (the editor's Frame Scene: looking down and across).
        core::Bounds bounds;
        if (meshes) {
            world.update_transforms();
            for (const entt::entity e : world.depth_first()) {
                const auto* renderer = world.registry().try_get<scene::MeshRenderer>(e);
                if (!renderer || !renderer->visible || !scene::is_visible(world, e)) {
                    continue;
                }
                if (const auto mesh = meshes->get(renderer->mesh); mesh && !mesh->bounds.empty()) {
                    bounds.add(mesh->bounds.transformed(world.transform(e).world));
                }
            }
        }
        const glm::vec3 centre = bounds.empty() ? glm::vec3(0.0f) : bounds.center();
        const float radius = bounds.empty() ? 5.0f : std::max(glm::length(bounds.extent()) * 0.5f, 0.1f);
        const float distance = radius / std::sin(kFovY * 0.5f) * 1.1f;
        view = orbit_view(centre, distance, glm::radians(35.0f), glm::radians(-25.0f), false, aspect);
    }
    view->width = request.width;
    view->height = request.height;
    return view;
}

Capturer::Capturer(rhi::Device& device, rhi::ShaderLibrary& shaders) : device_(device), engine_(device, shaders) {}

bool Capturer::capture(scene::World& world, assets::MeshLibrary& meshes, const CaptureRequest& request,
                       const View* fallback, std::string* error) {
    F3D_ZONE;
    if (request.path.empty()) {
        fail(error, "no output path");
        return false;
    }
    if (request.width == 0 || request.height == 0 || request.width > 16384 || request.height > 16384) {
        fail(error, "the size must be 1..16384 pixels");
        return false;
    }
    const std::optional<View> view = capture_view(world, &meshes, request, fallback, error);
    if (!view) {
        return false;
    }
    const rhi::Texture output =
        device_.create_texture({"Capture", request.width, request.height, RasterEngine::display_format(),
                                rhi::BindFlags::RenderTarget | rhi::BindFlags::ShaderResource});
    if (!output) {
        fail(error, "could not create the capture target");
        return false;
    }
    engine_.settings().mode = request.mode;
    engine_.settings().screen_percentage = request.screen_percentage;
    engine_.settings().fog_quality = request.fog_quality;
    engine_.grid().visible = false;
    engine_.set_debug_lines(false);
    engine_.reset_exposure();
    rhi::CommandContext& ctx = device_.context();
    const RenderTarget target{output, {0.16f, 0.17f, 0.19f, 1.0f}};
    // Twice: the first frame meters the exposure (and captures pending probes).
    for (int frame = 0; frame < 2; ++frame) {
        engine_.render(ctx, *view, {&world, &meshes}, target);
    }
    while (engine_.pending_probes() > 0) {
        engine_.render(ctx, *view, {&world, &meshes}, target);
    }
    // Volumetric fog accumulates jittered samples over frames: let it converge, with
    // the exposure held where it is.
    if (engine_.last_fog_grid().z > 0) {
        engine_.set_frame_delta(0.0f);
        for (int frame = 0; frame < 30; ++frame) {
            engine_.render(ctx, *view, {&world, &meshes}, target);
            ctx.wait_idle(); // each pass's GPU timer holds only a few frames of queries
        }
        engine_.set_frame_delta(-1.0f);
    }
    const core::Image image = ctx.read_texture(output);
    if (image.rgba.empty() || !core::write_png(request.path, image)) {
        fail(error, "could not write " + request.path);
        return false;
    }
    return true;
}

namespace {

struct ViewportCaptureOpParams {
    std::string path;
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    std::string mode = "lit";
    core::Guid camera;
    std::string bookmark;
    float screen_percentage = 100.0f;
    std::string fog_quality = "high";
};

void register_param_types() {
    static const bool registered = [] {
        entt::meta_factory<ViewportCaptureOpParams>{}
            .type("viewport.capture.params")
            .data<&ViewportCaptureOpParams::path>("path")
            .data<&ViewportCaptureOpParams::width>("width")
            .data<&ViewportCaptureOpParams::height>("height")
            .data<&ViewportCaptureOpParams::mode>("mode")
            .data<&ViewportCaptureOpParams::camera>("camera")
            .data<&ViewportCaptureOpParams::bookmark>("bookmark")
            .data<&ViewportCaptureOpParams::screen_percentage>("screen_percentage")
            .data<&ViewportCaptureOpParams::fog_quality>("fog_quality");
        return true;
    }();
    static_cast<void>(registered);
}

class ViewportCaptureOp final : public ops::OperationBase<ViewportCaptureOpParams> {
public:
    explicit ViewportCaptureOp(CaptureFunction capture) : capture_(std::move(capture)) {}

    [[nodiscard]] std::string_view id() const override { return "viewport.capture"; }
    [[nodiscard]] std::string display_name() const override { return "Capture viewport to " + params.path; }
    [[nodiscard]] bool records_history() const override { return false; }

    bool execute(ops::Context& context) override {
        const auto mode = parse_view_mode(params.mode);
        if (!mode) {
            F3D_LOG_ERROR("viewport.capture: unknown mode '{}' (lit, unlit, wireframe, normals, cascades)",
                          params.mode);
            return false;
        }
        if (!capture_) {
            F3D_LOG_ERROR("viewport.capture: no renderer available");
            return false;
        }
        const auto fog_quality = parse_fog_quality(params.fog_quality);
        if (!fog_quality) {
            F3D_LOG_ERROR("viewport.capture: unknown fog_quality '{}' (off, low, medium, high, epic)",
                          params.fog_quality);
            return false;
        }
        CaptureRequest request{params.path,   params.width,    params.height,
                               *mode,         params.camera,   params.bookmark,
                               params.screen_percentage, *fog_quality};
        std::string error;
        if (!capture_(context.world, request, &error)) {
            F3D_LOG_ERROR("viewport.capture: {}", error);
            return false;
        }
        F3D_LOG_INFO("Captured {}x{} ({}) to {}", params.width, params.height, params.mode, params.path);
        return true;
    }
    void undo(ops::Context&) override {}

private:
    CaptureFunction capture_;
};

} // namespace

void register_capture_op(ops::OpRegistry& registry, CaptureFunction capture) {
    register_param_types();
    registry.add({"viewport.capture", "Capture Viewport", entt::resolve<ViewportCaptureOpParams>()},
                 [capture = std::move(capture)] { return std::make_unique<ViewportCaptureOp>(capture); });
}

} // namespace f3d::render::raster
