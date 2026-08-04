// macOS half of the native window seam. The Cocoa equivalents of the X11 EWMH
// hints: raise the window above the menu bar, let it follow the user across Spaces
// and over full-screen apps, and drop the Dock icon (the counterpart of
// _NET_WM_STATE_SKIP_TASKBAR) so a hotkey-driven overlay stays out of the way.

#include "platform/NativeWindow.hpp"

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#import <AppKit/AppKit.h>

namespace tt::native {

void applyOverlayHints(GLFWwindow* win) {
    if (!win) return;
    NSWindow* w = static_cast<NSWindow*>(glfwGetCocoaWindow(win));
    if (!w) return;

    // Above the menu bar (GLFW_FLOATING only reaches NSFloatingWindowLevel, which
    // the menu bar still covers), and visible on whichever Space / full-screen app
    // the user is in when the hotkey fires.
    [w setLevel:NSStatusWindowLevel];
    [w setCollectionBehavior:(NSWindowCollectionBehaviorCanJoinAllSpaces |
                              NSWindowCollectionBehaviorFullScreenAuxiliary |
                              NSWindowCollectionBehaviorIgnoresCycle)];

    // The framebuffer is per-pixel alpha; a drop shadow around a screen-sized
    // transparent window would be drawn around the whole screen.
    [w setOpaque:NO];
    [w setHasShadow:NO];
    [w setBackgroundColor:[NSColor clearColor]];

    // Accessory = no Dock tile and no menu bar of our own, but still activatable.
    // This is the macOS analogue of the SKIP_TASKBAR/SKIP_PAGER hints on X11.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
}

void activateForInput(GLFWwindow* win) {
    if (!win) return;
    // An accessory app is not the active app, so makeKeyAndOrderFront alone would
    // show the overlay without giving it the keyboard. Activate the process first.
    [NSApp activateIgnoringOtherApps:YES];
    if (NSWindow* w = static_cast<NSWindow*>(glfwGetCocoaWindow(win)))
        [w makeKeyAndOrderFront:nil];
    glfwFocusWindow(win);
}

} // namespace tt::native
