#pragma once
// The small per-OS window seam. Everything else about the overlay window is plain
// GLFW and lives in PlatformGlfw; only these two operations need native calls,
// because GLFW has no portable equivalent:
//
//   * telling the window server "this is an always-on-top utility overlay, keep it
//     off the taskbar/Dock" (EWMH properties on X11, NSWindow level + activation
//     policy on macOS),
//   * forcing keyboard focus when the overlay is shown, which a borderless window
//     does not reliably get on its own.
//
// Implementations: NativeWindowX11.cpp, NativeWindowMac.mm. A future Wayland
// backend adds a third file and nothing else changes.

struct GLFWwindow;

namespace tt::native {

// Called once, right after the window is created (while it is still hidden).
void applyOverlayHints(GLFWwindow* win);

// Called every time the overlay is shown, after glfwShowWindow.
void activateForInput(GLFWwindow* win);

} // namespace tt::native
