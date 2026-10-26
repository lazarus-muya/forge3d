// SPDX-License-Identifier: Apache-2.0
// Expected result of tests/data/input/nav_*.json: the same logical gestures in
// every preset's chords. Computed with the camera primitives directly, so the
// tests check that each preset maps its chords onto the right primitive.
#pragma once

#include "f3d/editor/camera.hpp"
#include "f3d/editor/navigation.hpp"

#include <cmath>

namespace f3d::editor::test {

inline EditorCamera expected_replay_camera() {
    const NavigationSettings s; // presets use the default sensitivities
    const float viewport_height = 600.0f;
    EditorCamera cam;                                           // default camera
    cam.orbit(-120.0f * s.orbit_speed, -40.0f * s.orbit_speed); // orbit drag +120, +40 px
    const float units = cam.units_per_pixel(viewport_height);
    cam.pan(-60.0f * units, -30.0f * units);        // pan drag +60, -30 px
    cam.zoom(std::pow(s.wheel_zoom_factor, -1.0f)); // two wheel notches
    cam.zoom(std::pow(s.wheel_zoom_factor, -1.0f));
    return cam;
}

} // namespace f3d::editor::test
