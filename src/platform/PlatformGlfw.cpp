#include "platform/PlatformGlfw.hpp"

#include <cstdio>

#include <GLFW/glfw3.h>

#include "platform/NativeWindow.hpp"

namespace tt {

namespace {
void glfwErrorCallback(int code, const char* desc) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", code, desc);
}
} // namespace

bool PlatformGlfw::init(const char* title) {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) return false;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE); // required for core on macOS
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE); // per-pixel alpha
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);              // borderless
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);                // always on top
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);                // start hidden (resident)
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);        // HiDPI

    GLFWmonitor* mon = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = mon ? glfwGetVideoMode(mon) : nullptr;
    const int w = mode ? mode->width : 1280;
    const int h = mode ? mode->height : 720;

    win_ = glfwCreateWindow(w, h, title, nullptr, nullptr);
    if (!win_) { glfwTerminate(); return false; }

    glfwMakeContextCurrent(win_);
    glfwSwapInterval(1);

    // Start on the primary monitor's index (which need not be monitors[0]).
    int count = 0;
    GLFWmonitor** mons = glfwGetMonitors(&count);
    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    for (int i = 0; i < count; ++i)
        if (mons[i] == primary) { monitorIndex_ = i; break; }

    native::applyOverlayHints(win_);
    coverPrimaryMonitor();
    return true;
}

void PlatformGlfw::coverMonitorIndex(int index) {
    if (!win_) return;
    int count = 0;
    GLFWmonitor** mons = glfwGetMonitors(&count);
    if (count <= 0) return;
    monitorIndex_ = ((index % count) + count) % count; // wrap
    GLFWmonitor* mon = mons[monitorIndex_];
    int mx = 0, my = 0;
    glfwGetMonitorPos(mon, &mx, &my);
    if (const GLFWvidmode* mode = glfwGetVideoMode(mon)) {
        glfwSetWindowPos(win_, mx, my);
        glfwSetWindowSize(win_, mode->width, mode->height);
    }
}

void PlatformGlfw::coverPrimaryMonitor() { coverMonitorIndex(monitorIndex_); }

void PlatformGlfw::moveToNextMonitor() {
    int count = 0;
    glfwGetMonitors(&count);
    if (count > 1) coverMonitorIndex(monitorIndex_ + 1);
}

void PlatformGlfw::showOverlay() {
    if (!win_) return;
    coverPrimaryMonitor();      // re-cover in case the monitor changed
    glfwShowWindow(win_);
    native::activateForInput(win_);
}

void PlatformGlfw::hideOverlay() {
    if (!win_) return;
    glfwHideWindow(win_);
}

void PlatformGlfw::wake() { glfwPostEmptyEvent(); }

void PlatformGlfw::registerHotkey(const HotkeySpec& spec, std::function<void()> cb) {
    hotkeys_.add(spec, std::move(cb));
}

bool PlatformGlfw::startHotkeys() {
    hotkeys_.setWake([this]() { wake(); });
    return hotkeys_.start();
}

void PlatformGlfw::shutdown() {
    hotkeys_.stop();
    if (win_) { glfwDestroyWindow(win_); win_ = nullptr; }
    glfwTerminate();
}

} // namespace tt
