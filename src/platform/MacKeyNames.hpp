#pragma once
// Maps the X-keysym-style key names used in config.toml ("space", "Return", "a",
// "F5") to macOS virtual key codes, so one config file works on both platforms.
//
// The numbers are the `kVK_*` constants from <Carbon/HIToolbox/Events.h>. They are
// spelled out rather than included so this header stays pure — no Carbon, no
// platform guard — and can be unit tested anywhere (see tests/hotkey_tests.cpp).
// They describe *physical* keys on the ANSI layout, which is exactly what
// RegisterEventHotKey wants: the chord then lands on the same physical key
// regardless of the user's keyboard layout, matching X11 keycode behaviour.

#include <cctype>
#include <string>

namespace tt {

// macOS virtual key code for `name`, or -1 when there is no mapping.
// Case-insensitive, so "Return" and "return" both work.
inline int macVirtualKeyFromName(const std::string& name) {
    std::string k;
    k.reserve(name.size());
    for (char c : name) k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (k.empty()) return -1;

    // Letters — not contiguous on the Apple keycode map, so table them out.
    if (k.size() == 1 && k[0] >= 'a' && k[0] <= 'z') {
        static const int kLetters[26] = {
            /*a*/  0, /*b*/ 11, /*c*/  8, /*d*/  2, /*e*/ 14, /*f*/  3, /*g*/  5,
            /*h*/  4, /*i*/ 34, /*j*/ 38, /*k*/ 40, /*l*/ 37, /*m*/ 46, /*n*/ 45,
            /*o*/ 31, /*p*/ 35, /*q*/ 12, /*r*/ 15, /*s*/  1, /*t*/ 17, /*u*/ 32,
            /*v*/  9, /*w*/ 13, /*x*/  7, /*y*/ 16, /*z*/  6,
        };
        return kLetters[k[0] - 'a'];
    }

    // Digit row (note 5 and 6 are swapped relative to the obvious order).
    if (k.size() == 1 && k[0] >= '0' && k[0] <= '9') {
        static const int kDigits[10] = {
            /*0*/ 29, /*1*/ 18, /*2*/ 19, /*3*/ 20, /*4*/ 21,
            /*5*/ 23, /*6*/ 22, /*7*/ 26, /*8*/ 28, /*9*/ 25,
        };
        return kDigits[k[0] - '0'];
    }

    struct Named { const char* name; int vk; };
    static const Named kNamed[] = {
        // Whitespace / editing
        {"space",        49}, {"return",       36}, {"enter",        36},
        {"tab",          48}, {"escape",       53}, {"esc",          53},
        {"backspace",    51}, {"delete",      117}, // X11 "Delete" is forward-delete
        {"insert",      114}, {"help",        114},

        // Navigation
        {"left",        123}, {"right",       124}, {"down",        125}, {"up",     126},
        {"home",        115}, {"end",         119},
        {"prior",       116}, {"page_up",     116}, {"pageup",      116},
        {"next",        121}, {"page_down",   121}, {"pagedown",    121},

        // Punctuation, under their X keysym names (plus friendlier aliases)
        {"minus",        27}, {"equal",        24},
        {"bracketleft",  33}, {"bracketright", 30},
        {"backslash",    42}, {"semicolon",    41},
        {"apostrophe",   39}, {"quote",        39},
        {"grave",        50}, {"comma",        43},
        {"period",       47}, {"slash",        44},

        // Function keys — scattered across the map, hence the explicit list
        {"f1",  122}, {"f2",  120}, {"f3",   99}, {"f4",  118}, {"f5",   96},
        {"f6",   97}, {"f7",   98}, {"f8",  100}, {"f9",  101}, {"f10", 109},
        {"f11", 103}, {"f12", 111}, {"f13", 105}, {"f14", 107}, {"f15", 113},
        {"f16", 106}, {"f17",  64}, {"f18",  79}, {"f19",  80}, {"f20",  90},

        // Keypad
        {"kp_0",  82}, {"kp_1",  83}, {"kp_2",  84}, {"kp_3",  85}, {"kp_4", 86},
        {"kp_5",  87}, {"kp_6",  88}, {"kp_7",  89}, {"kp_8",  91}, {"kp_9", 92},
        {"kp_enter",    76}, {"kp_decimal",  65}, {"kp_multiply", 67},
        {"kp_add",      69}, {"kp_subtract", 78}, {"kp_divide",   75},
        {"kp_equal",    81},
    };

    for (const Named& n : kNamed)
        if (k == n.name) return n.vk;
    return -1;
}

} // namespace tt
