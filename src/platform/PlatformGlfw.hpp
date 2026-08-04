#pragma once
// GLFW implementation of the platform seam: a borderless, transparent,
// always-on-top window that covers the current monitor, plus the global hotkey
// grabber. GLFW already abstracts X11 vs Cocoa, so this file is shared; the two
// things it cannot express portably are delegated to `native::` (NativeWindow.hpp)
// and `Hotkeys` (Hotkeys.hpp), each of which has a per-OS implementation file.

#include <functional>
#include <string>
#include <vector>

#include "platform/Hotkeys.hpp"
#include "platform/IPlatform.hpp"

struct GLFWwindow;

namespace tt {

class PlatformGlfw final : public IPlatform {
public:
    // Initialise GLFW and create the (hidden) overlay window + GL context. Returns
    // false on failure. `title` is only used for window manager identification.
    bool init(const char* title = "TaskTree");
    void shutdown();

    // Register a hotkey callback (invoked on the main thread via pumpPlatformEvents).
    void registerHotkey(const HotkeySpec& spec, std::function<void()> cb);
    // Grab all registered hotkeys and start listening.
    bool startHotkeys();
    const std::vector<std::string>& failedChords() const { return hotkeys_.failedChords(); }

    // IPlatform
    GLFWwindow* window() override { return win_; }
    void coverPrimaryMonitor() override;
    void moveToNextMonitor() override;
    void showOverlay() override;
    void hideOverlay() override;
    void pumpPlatformEvents() override { hotkeys_.drain(); }
    void wake() override;

private:
    void coverMonitorIndex(int index); // clamps/wraps into the monitor list

    GLFWwindow* win_ = nullptr;
    Hotkeys     hotkeys_;
    int         monitorIndex_ = 0;
};

} // namespace tt
