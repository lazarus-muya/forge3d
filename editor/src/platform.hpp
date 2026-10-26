// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/render/rhi/types.hpp"

struct SDL_Window;

namespace f3d::editor {

/// Native handle of an SDL window, for swapchain creation.
[[nodiscard]] render::rhi::NativeWindow native_window(SDL_Window* window);

} // namespace f3d::editor
