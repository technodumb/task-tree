// Dependency-free verification of the hotkey seam's pure parts: the config chord
// parser (Hotkey.hpp) and the macOS key-name -> virtual-keycode table
// (MacKeyNames.hpp). Both are header-only, so this runs on every platform.
//   g++ -std=c++17 -I ../src hotkey_tests.cpp
#include "platform/Hotkey.hpp"
#include "platform/MacKeyNames.hpp"

#include <cstdio>
#include <set>
#include <string>

using namespace tt;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                            \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) { ++g_fail; std::printf("  FAIL: %s\n", msg); }               \
    } while (0)

int main() {
    std::printf("[hotkey] chord parsing\n");
    {
        // The two shipped defaults (see Config.hpp).
        HotkeySpec toggle = parseHotkey("Ctrl+Alt+Space");
        CHECK(toggle.mods == (Mod_Ctrl | Mod_Alt), "Ctrl+Alt parsed");
        CHECK(toggle.key == "Space", "key token kept as typed");
        CHECK(toggle.valid(), "default toggle chord is valid");

        HotkeySpec quick = parseHotkey("Ctrl+Alt+Return");
        CHECK(quick.mods == (Mod_Ctrl | Mod_Alt) && quick.key == "Return",
              "default quick-add chord");

        // Modifier tokens are case-insensitive and have aliases.
        CHECK(parseHotkey("CTRL+a").mods == Mod_Ctrl, "uppercase CTRL");
        CHECK(parseHotkey("control+a").mods == Mod_Ctrl, "control alias");
        CHECK(parseHotkey("option+a").mods == Mod_Alt, "option alias (macOS naming)");
        CHECK(parseHotkey("mod1+a").mods == Mod_Alt, "mod1 alias (X11 naming)");
        CHECK(parseHotkey("cmd+a").mods == Mod_Super, "cmd alias (macOS naming)");
        CHECK(parseHotkey("Super+a").mods == Mod_Super, "super");
        CHECK(parseHotkey("meta+a").mods == Mod_Super, "meta alias");
        CHECK(parseHotkey("win+a").mods == Mod_Super, "win alias");
        CHECK(parseHotkey("Shift+Ctrl+Alt+Super+F5").mods ==
                  (Mod_Shift | Mod_Ctrl | Mod_Alt | Mod_Super), "all four modifiers");

        // Whitespace, ordering, and degenerate input.
        CHECK(parseHotkey(" Ctrl + Alt + Space ").key == "Space", "surrounding spaces trimmed");
        CHECK(parseHotkey(" Ctrl + Alt + Space ").mods == (Mod_Ctrl | Mod_Alt),
              "spaces around modifiers");
        CHECK(parseHotkey("Space").mods == Mod_None, "bare key has no modifiers");
        CHECK(!parseHotkey("").valid(), "empty string is invalid");
        CHECK(!parseHotkey("Ctrl+Alt").valid(), "modifiers with no key is invalid");
        CHECK(parseHotkey("Ctrl+a+b").key == "b", "last non-modifier token wins");
    }

    std::printf("[hotkey] macOS key codes\n");
    {
        // Spot-check against the kVK_* constants in <Carbon/HIToolbox/Events.h>.
        CHECK(macVirtualKeyFromName("Space") == 49, "Space -> kVK_Space");
        CHECK(macVirtualKeyFromName("Return") == 36, "Return -> kVK_Return");
        CHECK(macVirtualKeyFromName("a") == 0, "a -> kVK_ANSI_A");
        CHECK(macVirtualKeyFromName("z") == 6, "z -> kVK_ANSI_Z");
        CHECK(macVirtualKeyFromName("m") == 46, "m -> kVK_ANSI_M (Ctrl+M next monitor)");
        CHECK(macVirtualKeyFromName("0") == 29, "0 -> kVK_ANSI_0");
        CHECK(macVirtualKeyFromName("5") == 23, "5 -> kVK_ANSI_5 (out of order on Apple maps)");
        CHECK(macVirtualKeyFromName("6") == 22, "6 -> kVK_ANSI_6");
        CHECK(macVirtualKeyFromName("F1") == 122, "F1 -> kVK_F1");
        CHECK(macVirtualKeyFromName("F5") == 96, "F5 -> kVK_F5");
        CHECK(macVirtualKeyFromName("F12") == 111, "F12 -> kVK_F12");
        CHECK(macVirtualKeyFromName("Escape") == 53, "Escape -> kVK_Escape");
        CHECK(macVirtualKeyFromName("Left") == 123, "Left -> kVK_LeftArrow");
        CHECK(macVirtualKeyFromName("Up") == 126, "Up -> kVK_UpArrow");

        // X11 names BackSpace and Delete as distinct keys; keep that distinction.
        CHECK(macVirtualKeyFromName("BackSpace") == 51, "BackSpace -> kVK_Delete (backspace)");
        CHECK(macVirtualKeyFromName("Delete") == 117, "Delete -> kVK_ForwardDelete");
        CHECK(macVirtualKeyFromName("Prior") == 116, "Prior -> kVK_PageUp");
        CHECK(macVirtualKeyFromName("Next") == 121, "Next -> kVK_PageDown");

        // Case-insensitive, so a hand-edited config.toml is forgiving.
        CHECK(macVirtualKeyFromName("space") == macVirtualKeyFromName("SPACE"),
              "lookup is case-insensitive");
        CHECK(macVirtualKeyFromName("f5") == macVirtualKeyFromName("F5"),
              "function keys case-insensitive");

        // Unmapped input must be reported, not silently turned into key code 0
        // (which is the A key) — that would grab the wrong chord.
        CHECK(macVirtualKeyFromName("") == -1, "empty name unmapped");
        CHECK(macVirtualKeyFromName("NoSuchKey") == -1, "unknown name unmapped");
        CHECK(macVirtualKeyFromName("F21") == -1, "F21 unmapped (no kVK constant)");

        // Every mapping must be a distinct, in-range virtual key code.
        const char* names[] = {
            "a","b","c","d","e","f","g","h","i","j","k","l","m",
            "n","o","p","q","r","s","t","u","v","w","x","y","z",
            "0","1","2","3","4","5","6","7","8","9",
            "space","return","tab","escape","backspace","delete",
            "left","right","up","down","home","end","prior","next",
            "minus","equal","bracketleft","bracketright","backslash",
            "semicolon","apostrophe","grave","comma","period","slash",
            "f1","f2","f3","f4","f5","f6","f7","f8","f9","f10",
            "f11","f12","f13","f14","f15","f16","f17","f18","f19","f20",
        };
        std::set<int> seen;
        bool allInRange = true, allUnique = true;
        for (const char* n : names) {
            const int vk = macVirtualKeyFromName(n);
            if (vk < 0 || vk > 127) { allInRange = false; std::printf("    range: %s\n", n); }
            if (!seen.insert(vk).second) { allUnique = false; std::printf("    dup: %s\n", n); }
        }
        CHECK(allInRange, "every key code is a valid 7-bit virtual key code");
        CHECK(allUnique, "no two key names share a virtual key code");

        // Aliases are meant to collide with their canonical spelling.
        CHECK(macVirtualKeyFromName("enter") == macVirtualKeyFromName("return"), "enter alias");
        CHECK(macVirtualKeyFromName("esc") == macVirtualKeyFromName("escape"), "esc alias");
        CHECK(macVirtualKeyFromName("quote") == macVirtualKeyFromName("apostrophe"),
              "quote alias");
        CHECK(macVirtualKeyFromName("page_up") == macVirtualKeyFromName("prior"),
              "page_up alias");
    }

    std::printf("[hotkey] parse -> key code, end to end\n");
    {
        // What the macOS backend actually does with a config string.
        HotkeySpec s = parseHotkey("Ctrl+Alt+Space");
        CHECK(macVirtualKeyFromName(s.key) == 49, "parsed default toggle resolves on macOS");
        HotkeySpec q = parseHotkey("Ctrl+Alt+Return");
        CHECK(macVirtualKeyFromName(q.key) == 36, "parsed default quick-add resolves on macOS");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
