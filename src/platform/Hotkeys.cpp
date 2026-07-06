#include "platform/Hotkeys.hpp"

#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>

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

std::string describe(const HotkeySpec& s) {
    std::string out;
    if (s.mods & Mod_Ctrl)  out += "Ctrl+";
    if (s.mods & Mod_Alt)   out += "Alt+";
    if (s.mods & Mod_Shift) out += "Shift+";
    if (s.mods & Mod_Super) out += "Super+";
    out += s.key;
    return out;
}

std::atomic<bool> g_grabError{false};
int grabErrorHandler(Display*, XErrorEvent* e) {
    if (e->error_code == BadAccess) g_grabError = true;
    return 0;
}

} // namespace

Hotkeys::~Hotkeys() { stop(); }

void Hotkeys::add(const HotkeySpec& spec, std::function<void()> cb) {
    if (!spec.valid()) return;
    bindings_.push_back(Binding{spec, std::move(cb), 0, 0});
}

bool Hotkeys::start() {
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) return false;
    display_ = dpy;
    root_ = DefaultRootWindow(dpy);

    if (pipe(wakePipe_) != 0) { XCloseDisplay(dpy); display_ = nullptr; return false; }

    const auto combos = nuisanceCombos();
    XErrorHandler prev = XSetErrorHandler(grabErrorHandler);
    for (Binding& b : bindings_) {
        KeySym ks = resolveKeysym(b.spec.key);
        KeyCode kc = (ks != NoSymbol) ? XKeysymToKeycode(dpy, ks) : 0;
        if (kc == 0) { failed_.push_back(describe(b.spec)); continue; }
        b.keycode = kc;
        b.baseMods = toXMods(b.spec.mods);

        g_grabError = false;
        for (unsigned nuis : combos)
            XGrabKey(dpy, kc, b.baseMods | nuis, root_, False, GrabModeAsync, GrabModeAsync);
        XSync(dpy, False);
        if (g_grabError) failed_.push_back(describe(b.spec));
    }
    XSetErrorHandler(prev);

    running_ = true;
    thread_ = std::thread(&Hotkeys::threadMain, this);
    return true;
}

void Hotkeys::threadMain() {
    Display* dpy = static_cast<Display*>(display_);
    const int xfd = ConnectionNumber(dpy);
    while (running_) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        FD_SET(wakePipe_[0], &fds);
        const int maxfd = std::max(xfd, wakePipe_[0]) + 1;

        if (select(maxfd, &fds, nullptr, nullptr, nullptr) < 0) break;

        if (FD_ISSET(wakePipe_[0], &fds)) {
            char buf[16];
            while (read(wakePipe_[0], buf, sizeof(buf)) > 0) {}
            if (!running_) break;
        }

        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type != KeyPress) continue;
            const unsigned state = ev.xkey.state & ~kNuisance;
            for (std::size_t i = 0; i < bindings_.size(); ++i) {
                if (bindings_[i].keycode == ev.xkey.keycode &&
                    state == bindings_[i].baseMods) {
                    {
                        std::lock_guard<std::mutex> lock(queueMutex_);
                        pending_.push_back(i);
                    }
                    if (wake_) wake_();
                    break;
                }
            }
        }
    }
}

void Hotkeys::drain() {
    std::vector<std::size_t> local;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        local.swap(pending_);
    }
    for (std::size_t i : local)
        if (i < bindings_.size() && bindings_[i].cb) bindings_[i].cb();
}

void Hotkeys::stop() {
    if (!display_) return;
    running_ = false;
    if (wakePipe_[1] >= 0) {
        const char b = 1;
        ssize_t n = write(wakePipe_[1], &b, 1);
        (void)n;
    }
    if (thread_.joinable()) thread_.join();

    Display* dpy = static_cast<Display*>(display_);
    for (const Binding& b : bindings_)
        if (b.keycode) XUngrabKey(dpy, b.keycode, AnyModifier, root_);
    XCloseDisplay(dpy);
    display_ = nullptr;

    for (int& fd : wakePipe_)
        if (fd >= 0) { close(fd); fd = -1; }
}

} // namespace tt
