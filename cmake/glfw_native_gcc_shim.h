/* Force-included (-include) for imgui_impl_glfw.cpp when building the GUI
 * with GCC on macOS.
 *
 * That backend defines GLFW_EXPOSE_NATIVE_COCOA and includes glfw3native.h,
 * which would pull in Apple framework headers whose blocks syntax (^) GCC
 * cannot parse. GLFW_NATIVE_INCLUDE_NONE suppresses those includes; the two
 * types the Cocoa declarations still need are supplied here. The backend
 * only ever casts the returned id to void*, so the loose typedef is safe in
 * this translation unit. */
#pragma once

#define GLFW_NATIVE_INCLUDE_NONE

#include <stdint.h>

typedef uint32_t CGDirectDisplayID;
typedef void *id;
