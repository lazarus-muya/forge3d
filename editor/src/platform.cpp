// SPDX-License-Identifier: Apache-2.0
#include "platform.hpp"

#include <SDL3/SDL.h>

#include <cstdint>

namespace f3d::editor {

render::rhi::NativeWindow native_window(SDL_Window* window) {
    render::rhi::NativeWindow native;
    if (!window) {
        return native;
    }
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
#if defined(_WIN32)
    native.hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(__linux__)
    native.x11_display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    native.x11_window = static_cast<std::uint32_t>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
#else
    (void)props;
#endif
    return native;
}

} // namespace f3d::editor
