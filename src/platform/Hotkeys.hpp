#pragma once
// Global hotkey grabber. Uses its OWN X11 connection on a dedicated thread so that
// passive root-window grabs are delivered to us (not swallowed by GLFW's event
// pump) and keep firing while our overlay is hidden/unfocused. Matched presses are
// queued and the main loop is woken; callbacks run on the main thread via drain().

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "platform/Hotkey.hpp"

namespace tt {

class Hotkeys {
public:
    ~Hotkeys();

    // Set the callback used to wake the main event loop when a hotkey fires.
    void setWake(std::function<void()> wake) { wake_ = std::move(wake); }

    // Register a hotkey + its (main-thread) callback. Must be called before start().
    void add(const HotkeySpec& spec, std::function<void()> cb);

    // Open the X connection, grab all registered chords, and spawn the listener
    // thread. Returns false only if the X connection could not be opened; per-chord
    // grab failures are reported via failedChords().
    bool start();

    // Stop the listener thread, ungrab, and close the connection.
    void stop();

    // Main thread: invoke callbacks for any presses queued since the last call.
    void drain();

    // Human-readable chords whose grab failed (already taken). Valid after start().
    const std::vector<std::string>& failedChords() const { return failed_; }

private:
    struct Binding {
        HotkeySpec spec;
        std::function<void()> cb;
        unsigned int keycode = 0;   // resolved X keycode
        unsigned int baseMods = 0;  // X modifier mask
    };

    void threadMain();

    std::vector<Binding> bindings_;
    std::vector<std::string> failed_;
    std::function<void()> wake_;

    std::thread thread_;
    std::mutex queueMutex_;
    std::vector<std::size_t> pending_; // indices into bindings_

    void* display_ = nullptr;          // Display* (opaque here to avoid X in the header)
    unsigned long root_ = 0;           // Window
    int wakePipe_[2] = {-1, -1};       // self-pipe to break the blocking select()
    std::atomic<bool> running_{false};
};

} // namespace tt
