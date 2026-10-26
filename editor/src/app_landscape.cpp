// SPDX-License-Identifier: Apache-2.0
// The Landscape panel's hooks into the app: image file dialogs for layer textures.
#include "app.hpp"

#include <SDL3/SDL_dialog.h>

#include <mutex>

namespace f3d::editor {

void App::show_image_dialog(std::function<void(std::vector<std::string>)> done) {
    static const SDL_DialogFileFilter kFilters[] = {
        {"Images", "png;jpg;jpeg;tga;bmp;psd;exr;hdr"},
        {"All files", "*"},
    };
    const auto callback = [](void* user, const char* const* files, int) {
        auto* request = static_cast<ImageDialogRequest*>(user);
        std::vector<std::string> paths;
        for (const char* const* f = files; f && *f; ++f) {
            paths.emplace_back(*f);
        }
        std::scoped_lock lock(request->app->pending_dialog_mutex_);
        request->app->pending_images_ = std::move(paths);
    };
    image_dialog_ = {this, std::move(done)};
    SDL_ShowOpenFileDialog(callback, &image_dialog_, window_, kFilters, 2, nullptr, true);
}

void App::update_landscape_dialogs() {
    std::optional<std::vector<std::string>> images;
    {
        std::scoped_lock lock(pending_dialog_mutex_);
        images.swap(pending_images_);
    }
    if (images && !images->empty() && image_dialog_.done) {
        image_dialog_.done(std::move(*images));
    }
}

} // namespace f3d::editor
