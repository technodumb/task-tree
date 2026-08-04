#pragma once
// Platform seam: window management + global hotkeys. `PlatformGlfw` implements it on
// both X11 and macOS (the per-OS parts are behind NativeWindow.hpp and Hotkeys.hpp);
// a future Wayland implementation (portal GlobalShortcuts + a layer-shell/portal
// overlay) can be added behind this same interface.

#include <functional>

#include "platform/Hotkey.hpp"

struct GLFWwindow;

namespace tt {

struct IPlatform {
    virtual ~IPlatform() = default;

    virtual GLFWwindow* window() = 0;

    // Size/position the (borderless) window to cover the current monitor.
    virtual void coverPrimaryMonitor() = 0;

    // Move the overlay to the next monitor (wraps around) for multi-monitor setups.
    virtual void moveToNextMonitor() = 0;

    virtual void showOverlay() = 0;             // map + raise + focus
    virtual void hideOverlay() = 0;             // unmap

    // Called on the main thread each loop iteration: drains queued hotkey presses
    // and invokes their callbacks (so all GL/window calls stay on the main thread).
    virtual void pumpPlatformEvents() = 0;

    // Thread-safe wake of the blocking event loop (glfwPostEmptyEvent under the hood).
    virtual void wake() = 0;
};

} // namespace tt
