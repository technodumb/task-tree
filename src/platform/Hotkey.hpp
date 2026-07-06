#pragma once
// Hotkey specification + parser. Pure (no X11/GLFW) so it is unit testable and
// shared by Config (parsing) and the platform hotkey grabber (which turns the
// keysym name + mod mask into an X keycode/modifier grab).

#include <cctype>
#include <string>

namespace tt {

enum HotkeyMod : unsigned {
    Mod_None  = 0,
    Mod_Ctrl  = 1u << 0,
    Mod_Alt   = 1u << 1,
    Mod_Shift = 1u << 2,
    Mod_Super = 1u << 3,
};

struct HotkeySpec {
    unsigned mods = Mod_None;
    std::string key;              // X keysym name, e.g. "space", "Return", "a"
    bool valid() const { return !key.empty(); }
};

// Parse a human string like "Ctrl+Alt+Space" into mods + a keysym name. Modifier
// tokens are case-insensitive; the final non-modifier token is the key (kept as
// typed so the X keysym lookup can try it verbatim and normalised).
inline HotkeySpec parseHotkey(const std::string& s) {
    HotkeySpec out;
    std::size_t start = 0;
    auto lower = [](std::string t) {
        for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t;
    };
    auto trim = [](std::string t) {
        std::size_t a = t.find_first_not_of(" \t");
        std::size_t b = t.find_last_not_of(" \t");
        return (a == std::string::npos) ? std::string{} : t.substr(a, b - a + 1);
    };
    while (start <= s.size()) {
        std::size_t plus = s.find('+', start);
        std::string tok = trim(s.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
        if (!tok.empty()) {
            const std::string lt = lower(tok);
            if (lt == "ctrl" || lt == "control")            out.mods |= Mod_Ctrl;
            else if (lt == "alt" || lt == "mod1" || lt == "option") out.mods |= Mod_Alt;
            else if (lt == "shift")                          out.mods |= Mod_Shift;
            else if (lt == "super" || lt == "meta" || lt == "win" || lt == "cmd") out.mods |= Mod_Super;
            else out.key = tok; // last non-modifier token wins
        }
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return out;
}

} // namespace tt
