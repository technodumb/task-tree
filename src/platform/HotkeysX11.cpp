// X11 backend for Hotkeys: passive XGrabKey grabs on the root window, read on a
// dedicated thread with our own X connection so they are delivered to us rather
// than swallowed by GLFW's event pump.

#include "platform/Hotkeys.hpp"

#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <thread>

#include <X11/Xlib.h>
#include <X11/keysym.h>

namespace tt {
namespace {

// Nuisance modifiers we ignore when matching (CapsLock, NumLock, and a common
// Level3/ScrollLock mask). We grab every combination so the chord fires regardless
// of their state.
constexpr unsigned kNuisance = LockMask | Mod2Mask | Mod5Mask;

std::vector<unsigned> nuisanceCombos() {
    const unsigned bits[] = {LockMask, Mod2Mask, Mod5Mask};
    std::vector<unsigned> out;
    for (int m = 0; m < 8; ++m) {
        unsigned mask = 0;
        for (int b = 0; b < 3; ++b)
            if (m & (1 << b)) mask |= bits[b];
        out.push_back(mask);
    }
    return out;
}

unsigned toXMods(unsigned mods) {
    unsigned x = 0;
    if (mods & Mod_Ctrl)  x |= ControlMask;
    if (mods & Mod_Alt)   x |= Mod1Mask;
    if (mods & Mod_Shift) x |= ShiftMask;
    if (mods & Mod_Super) x |= Mod4Mask;
    return x;
}

KeySym resolveKeysym(const std::string& key) {
    KeySym s = XStringToKeysym(key.c_str());
    if (s != NoSymbol) return s;
    // X keysym names for punctuation/space are lowercase ("space"); try lowercased.
    std::string lo = key;
    std::transform(lo.begin(), lo.end(), lo.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    s = XStringToKeysym(lo.c_str());
    if (s != NoSymbol) return s;
    // Try capitalised first letter ("Return", "F1" already handled above).
    std::string cap = lo;
    if (!cap.empty()) cap[0] = static_cast<char>(std::toupper((unsigned char)cap[0]));
    return XStringToKeysym(cap.c_str());
}

std::atomic<bool> g_grabError{false};
int grabErrorHandler(Display*, XErrorEvent* e) {
    if (e->error_code == BadAccess) g_grabError = true;
    return 0;
}

} // namespace

struct Hotkeys::Impl {
    Hotkeys* owner = nullptr;
    Display* display = nullptr;
    Window   root = 0;
    int      wakePipe[2] = {-1, -1}; // self-pipe to break the blocking select()
    std::atomic<bool> running{false};
    std::thread thread;

    // Resolved grabs, parallel to owner->bindings_.
    std::vector<KeyCode>  keycodes;
    std::vector<unsigned> baseMods;

    void threadMain();
};

Hotkeys::Hotkeys() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }
Hotkeys::~Hotkeys() { stop(); }

bool Hotkeys::start() {
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) return false;
    impl_->display = dpy;
    impl_->root = DefaultRootWindow(dpy);

    if (pipe(impl_->wakePipe) != 0) {
        XCloseDisplay(dpy);
        impl_->display = nullptr;
        return false;
    }

    impl_->keycodes.assign(bindings_.size(), 0);
    impl_->baseMods.assign(bindings_.size(), 0);

    const auto combos = nuisanceCombos();
    XErrorHandler prev = XSetErrorHandler(grabErrorHandler);
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        const HotkeySpec& spec = bindings_[i].spec;
        KeySym ks = resolveKeysym(spec.key);
        KeyCode kc = (ks != NoSymbol) ? XKeysymToKeycode(dpy, ks) : 0;
        if (kc == 0) { failed_.push_back(describeHotkey(spec)); continue; }
        impl_->keycodes[i] = kc;
        impl_->baseMods[i] = toXMods(spec.mods);

        g_grabError = false;
        for (unsigned nuis : combos)
            XGrabKey(dpy, kc, impl_->baseMods[i] | nuis, impl_->root, False,
                     GrabModeAsync, GrabModeAsync);
        XSync(dpy, False);
        if (g_grabError) failed_.push_back(describeHotkey(spec));
    }
    XSetErrorHandler(prev);

    impl_->running = true;
    impl_->thread = std::thread(&Hotkeys::Impl::threadMain, impl_.get());
    return true;
}

void Hotkeys::Impl::threadMain() {
    const int xfd = ConnectionNumber(display);
    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        FD_SET(wakePipe[0], &fds);
        const int maxfd = std::max(xfd, wakePipe[0]) + 1;

        if (select(maxfd, &fds, nullptr, nullptr, nullptr) < 0) break;

        if (FD_ISSET(wakePipe[0], &fds)) {
            char buf[16];
            while (read(wakePipe[0], buf, sizeof(buf)) > 0) {}
            if (!running) break;
        }

        while (XPending(display)) {
            XEvent ev;
            XNextEvent(display, &ev);
            if (ev.type != KeyPress) continue;
            const unsigned state = ev.xkey.state & ~kNuisance;
            for (std::size_t i = 0; i < keycodes.size(); ++i) {
                if (keycodes[i] == ev.xkey.keycode && state == baseMods[i]) {
                    owner->queuePress(i);
                    break;
                }
            }
        }
    }
}

void Hotkeys::stop() {
    if (!impl_ || !impl_->display) return;
    impl_->running = false;
    if (impl_->wakePipe[1] >= 0) {
        const char b = 1;
        ssize_t n = write(impl_->wakePipe[1], &b, 1);
        (void)n;
    }
    if (impl_->thread.joinable()) impl_->thread.join();

    for (KeyCode kc : impl_->keycodes)
        if (kc) XUngrabKey(impl_->display, kc, AnyModifier, impl_->root);
    XCloseDisplay(impl_->display);
    impl_->display = nullptr;

    for (int& fd : impl_->wakePipe)
        if (fd >= 0) { close(fd); fd = -1; }
}

} // namespace tt
