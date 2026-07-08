#pragma once
// X11 implementation of the platform seam: a borderless, transparent, always-on-top
// GLFW window that covers the primary monitor, plus the global hotkey grabber.

#include <functional>
#include <string>
#include <vector>

#include "platform/Hotkeys.hpp"
#include "platform/IPlatform.hpp"

struct GLFWwindow;

namespace tt {

class PlatformX11 final : public IPlatform {
public:
    // Initialise GLFW and create the (hidden) overlay window + GL context. Returns
    // false on failure. `title` is only used for window manager identification.
    bool init(const char* title = "TaskTree");
    void shutdown();

    // Register a hotkey callback (invoked on the main thread via pumpPlatformEvents).
    void registerHotkey(const HotkeySpec& spec, std::function<void()> cb);
    // Grab all registered hotkeys and start the listener thread.
    bool startHotkeys();
    const std::vector<std::string>& failedChords() const { return hotkeys_.failedChords(); }

    // IPlatform
    GLFWwindow* window() override { return win_; }
    void coverPrimaryMonitor() override;
    void moveToNextMonitor() override;
    void showOverlay() override;
    void hideOverlay() override;
    bool overlayVisible() const override { return visible_; }
    void pumpPlatformEvents() override { hotkeys_.drain(); }
    void wake() override;

private:
    void applyEwmhHints();
    void coverMonitorIndex(int index); // clamps/wraps into the monitor list

    GLFWwindow* win_ = nullptr;
    Hotkeys     hotkeys_;
    bool        visible_ = false;
    int         monitorIndex_ = 0;
};

} // namespace tt
