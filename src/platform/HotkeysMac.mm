// macOS backend for Hotkeys, via Carbon's RegisterEventHotKey.
//
// Why Carbon and not NSEvent's addGlobalMonitorForEventsMatchingMask: the NSEvent
// monitor needs the user to grant Input Monitoring / Accessibility rights in System
// Settings before it sees a single key. RegisterEventHotKey needs no permission at
// all, and it is still the supported way to claim a system-wide chord (deprecated
// Carbon UI is a separate matter — the HIToolbox hot key API is not).
//
// No listener thread is needed here, unlike X11: hot key events are delivered on the
// main thread by the same NSApplication event pump that glfwWaitEvents is already
// blocking on. We still only *queue* the press, so callbacks run from drain() at a
// well-defined point in the main loop exactly as they do on X11.

#include "platform/Hotkeys.hpp"

#include <cstdio>

#include "platform/MacKeyNames.hpp"

#import <Carbon/Carbon.h>

namespace tt {
namespace {

UInt32 toCarbonMods(unsigned mods) {
    UInt32 m = 0;
    if (mods & Mod_Ctrl)  m |= controlKey;
    if (mods & Mod_Alt)   m |= optionKey;   // Option
    if (mods & Mod_Shift) m |= shiftKey;
    if (mods & Mod_Super) m |= cmdKey;      // Command
    return m;
}

// Tags our hot keys so the handler can tell them apart from anyone else's.
constexpr UInt32 kSignature = 'ttHK';

OSStatus hotKeyPressed(EventHandlerCallRef, EventRef event, void* userData) {
    EventHotKeyID id{};
    if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr,
                          sizeof(id), nullptr, &id) != noErr)
        return eventNotHandledErr;
    if (id.signature != kSignature) return eventNotHandledErr;

    static_cast<Hotkeys*>(userData)->queuePress(id.id);
    return noErr;
}

} // namespace

struct Hotkeys::Impl {
    EventHandlerUPP upp = nullptr;
    EventHandlerRef handler = nullptr;
    std::vector<EventHotKeyRef> refs; // parallel to bindings_; null where the grab failed
};

Hotkeys::Hotkeys() : impl_(std::make_unique<Impl>()) {}
Hotkeys::~Hotkeys() { stop(); }

bool Hotkeys::start() {
    if (bindings_.empty()) return true;

    EventTypeSpec type{kEventClassKeyboard, kEventHotKeyPressed};
    impl_->upp = NewEventHandlerUPP(hotKeyPressed);
    if (InstallEventHandler(GetApplicationEventTarget(), impl_->upp, 1, &type, this,
                            &impl_->handler) != noErr) {
        DisposeEventHandlerUPP(impl_->upp);
        impl_->upp = nullptr;
        return false;
    }

    impl_->refs.assign(bindings_.size(), nullptr);
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        const HotkeySpec& spec = bindings_[i].spec;
        const int vk = macVirtualKeyFromName(spec.key);
        if (vk < 0) {
            std::fprintf(stderr, "[hotkeys] no macOS key code for '%s'\n", spec.key.c_str());
            failed_.push_back(describeHotkey(spec));
            continue;
        }

        EventHotKeyID id{kSignature, static_cast<UInt32>(i)};
        EventHotKeyRef ref = nullptr;
        const OSStatus rc = RegisterEventHotKey(static_cast<UInt32>(vk),
                                                toCarbonMods(spec.mods), id,
                                                GetApplicationEventTarget(), 0, &ref);
        if (rc != noErr || !ref) failed_.push_back(describeHotkey(spec));
        else                     impl_->refs[i] = ref;
    }
    return true;
}

void Hotkeys::stop() {
    if (!impl_) return;
    for (EventHotKeyRef ref : impl_->refs)
        if (ref) UnregisterEventHotKey(ref);
    impl_->refs.clear();

    if (impl_->handler) { RemoveEventHandler(impl_->handler); impl_->handler = nullptr; }
    if (impl_->upp)     { DisposeEventHandlerUPP(impl_->upp); impl_->upp = nullptr; }
}

} // namespace tt
