#pragma once
// Platform seam: window management + global hotkeys. The X11 implementation lives
// in PlatformX11; a future Wayland implementation (portal GlobalShortcuts + a
// layer-shell/portal overlay) can be added behind this same interface.

#include <functional>

#include "platform/Hotkey.hpp"

struct GLFWwindow;

namespace tt {

struct IPlatform {
    virtual ~IPlatform() = default;

    virtual GLFWwindow* window() = 0;

    // Size/position the (borderless) window to cover the primary monitor.
    virtual void coverPrimaryMonitor() = 0;

    virtual void showOverlay() = 0;             // map + raise + focus
    virtual void hideOverlay() = 0;             // unmap
    virtual bool overlayVisible() const = 0;

    // Called on the main thread each loop iteration: drains queued hotkey presses
    // and invokes their callbacks (so all GL/window calls stay on the main thread).
    virtual void pumpPlatformEvents() = 0;

    // Thread-safe wake of the blocking event loop (glfwPostEmptyEvent under the hood).
    virtual void wake() = 0;
};

} // namespace tt
