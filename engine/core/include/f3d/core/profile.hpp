// SPDX-License-Identifier: Apache-2.0
#pragma once

// Profiling macros. With the CMake option F3D_TRACY=ON they expand to Tracy
// instrumentation; otherwise they expand to nothing.
//
//   F3D_ZONE;                 scoped zone named after the enclosing function
//   F3D_ZONE_N("name");       scoped zone with an explicit name (string literal)
//   F3D_FRAME_MARK;           end of a frame
//   F3D_ZONE_TEXT(str, len);  attach text to the current zone

#if defined(F3D_TRACY_ENABLED) && F3D_TRACY_ENABLED
#include <tracy/Tracy.hpp>
#define F3D_ZONE ZoneScoped
#define F3D_ZONE_N(name) ZoneScopedN(name)
#define F3D_ZONE_TEXT(text, size) ZoneText(text, size)
#define F3D_FRAME_MARK FrameMark
#else
#define F3D_ZONE static_cast<void>(0)
#define F3D_ZONE_N(name) static_cast<void>(0)
#define F3D_ZONE_TEXT(text, size) static_cast<void>(0)
#define F3D_FRAME_MARK static_cast<void>(0)
#endif
