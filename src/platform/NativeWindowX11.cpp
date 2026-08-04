// X11 half of the native window seam: EWMH hints that ask the window manager to
// treat the overlay as an always-on-top utility window and keep it out of the
// taskbar/pager.

#include "platform/NativeWindow.hpp"

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#include <X11/Xatom.h>

namespace tt::native {

void applyOverlayHints(GLFWwindow* win) {
    Display* d = glfwGetX11Display();
    Window   w = win ? glfwGetX11Window(win) : 0;
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

// Borderless windows don't always auto-focus on X11, so ask explicitly.
void activateForInput(GLFWwindow* win) {
    if (win) glfwFocusWindow(win);
}

} // namespace tt::native
