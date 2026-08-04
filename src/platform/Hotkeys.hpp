#pragma once
// Global hotkey grabber: fires whether or not our overlay is visible or focused.
//
// The queue plumbing is shared and lives here; the actual grabbing is per-OS and
// the constructor/destructor/start/stop are implemented in exactly one backend TU:
//
//   HotkeysX11.cpp — own X11 connection on a dedicated thread, so passive
//                    root-window XGrabKey grabs reach us instead of being swallowed
//                    by GLFW's event pump.
//   HotkeysMac.mm  — Carbon RegisterEventHotKey, delivered on the main thread by
//                    the run loop GLFW is already pumping (no extra thread, and no
//                    Accessibility permission, unlike an NSEvent global monitor).
//
// Either way, a matched press is queued (never dispatched from the backend) and the
// main loop is woken; callbacks run on the main thread via drain(), so App and all
// GL/window calls stay single-threaded.

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "platform/Hotkey.hpp"

namespace tt {

class Hotkeys {
public:
    Hotkeys();
    ~Hotkeys();

    Hotkeys(const Hotkeys&) = delete;
    Hotkeys& operator=(const Hotkeys&) = delete;

    // Set the callback used to wake the main event loop when a hotkey fires.
    void setWake(std::function<void()> wake) { wake_ = std::move(wake); }

    // Register a hotkey + its (main-thread) callback. Must be called before start().
    void add(const HotkeySpec& spec, std::function<void()> cb) {
        if (!spec.valid()) return;
        bindings_.push_back(Binding{spec, std::move(cb)});
    }

    // Grab all registered chords and start listening. Returns false only if the
    // backend could not be set up at all; per-chord failures go to failedChords().
    bool start();

    // Stop listening and release every grab.
    void stop();

    // Main thread: invoke callbacks for any presses queued since the last call.
    void drain() {
        std::vector<std::size_t> local;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            local.swap(pending_);
        }
        for (std::size_t i : local)
            if (i < bindings_.size() && bindings_[i].cb) bindings_[i].cb();
    }

    // Human-readable chords whose grab failed (already taken). Valid after start().
    const std::vector<std::string>& failedChords() const { return failed_; }

    // Backend entry point: record that binding `index` fired and wake the main loop.
    // Safe to call from the backend's own thread.
    void queuePress(std::size_t index) {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            pending_.push_back(index);
        }
        if (wake_) wake_();
    }

    std::size_t count() const { return bindings_.size(); }
    const HotkeySpec& specAt(std::size_t i) const { return bindings_[i].spec; }

private:
    struct Binding {
        HotkeySpec spec;
        std::function<void()> cb;
    };

    struct Impl;                        // per-OS backend state

    std::vector<Binding> bindings_;
    std::vector<std::string> failed_;
    std::function<void()> wake_;

    std::mutex queueMutex_;
    std::vector<std::size_t> pending_;  // indices into bindings_

    std::unique_ptr<Impl> impl_;
};

// Render a chord back to the form used in config.toml ("Ctrl+Alt+Space"), for the
// warnings the backends emit about grabs they could not take.
inline std::string describeHotkey(const HotkeySpec& s) {
    std::string out;
    if (s.mods & Mod_Ctrl)  out += "Ctrl+";
    if (s.mods & Mod_Alt)   out += "Alt+";
    if (s.mods & Mod_Shift) out += "Shift+";
    if (s.mods & Mod_Super) out += "Super+";
    out += s.key;
    return out;
}

} // namespace tt
