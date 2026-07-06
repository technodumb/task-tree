#include "platform/PlatformX11.hpp"

#include <cstdio>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#include <X11/Xatom.h>

namespace tt {

namespace {
void glfwErrorCallback(int code, const char* desc) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", code, desc);
}
} // namespace

bool PlatformX11::init(const char* title) {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) return false;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
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

    applyEwmhHints();
    coverPrimaryMonitor();
    return true;
}

void PlatformX11::applyEwmhHints() {
    Display* d = glfwGetX11Display();
    Window   w = glfwGetX11Window(win_);
    if (!d || !w) return;

    Atom wtype = XInternAtom(d, "_NET_WM_WINDOW_TYPE", False);
    Atom util  = XInternAtom(d, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    XChangeProperty(d, w, wtype, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&util), 1);

    Atom state = XInternAtom(d, "_NET_WM_STATE", False);
    Atom states[] = {
        XInternAtom(d, "_NET_WM_STATE_ABOVE", False),
        XInternAtom(d, "_NET_WM_STATE_SKIP_TASKBAR", False),
        XInternAtom(d, "_NET_WM_STATE_SKIP_PAGER", False),
    };
    XChangeProperty(d, w, state, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(states), 3);
    XFlush(d);
}

void PlatformX11::coverPrimaryMonitor() {
    if (!win_) return;
    GLFWmonitor* mon = glfwGetPrimaryMonitor();
    if (!mon) return;
    int mx = 0, my = 0;
    glfwGetMonitorPos(mon, &mx, &my);
    if (const GLFWvidmode* mode = glfwGetVideoMode(mon)) {
        glfwSetWindowPos(win_, mx, my);
        glfwSetWindowSize(win_, mode->width, mode->height);
    }
}

void PlatformX11::showOverlay() {
    if (!win_) return;
    coverPrimaryMonitor();      // re-cover in case the monitor changed
    glfwShowWindow(win_);
    glfwFocusWindow(win_);      // borderless windows don't always auto-focus on X11
    visible_ = true;
}

void PlatformX11::hideOverlay() {
    if (!win_) return;
    glfwHideWindow(win_);
    visible_ = false;
}

void PlatformX11::wake() { glfwPostEmptyEvent(); }

void PlatformX11::registerHotkey(const HotkeySpec& spec, std::function<void()> cb) {
    hotkeys_.add(spec, std::move(cb));
}

bool PlatformX11::startHotkeys() {
    hotkeys_.setWake([this]() { wake(); });
    return hotkeys_.start();
}

void PlatformX11::shutdown() {
    hotkeys_.stop();
    if (win_) { glfwDestroyWindow(win_); win_ = nullptr; }
    glfwTerminate();
}

} // namespace tt
