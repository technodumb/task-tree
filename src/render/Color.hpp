#pragma once
// RGBA colour (components 0..1), with hex parsing. Pure; the renderer converts
// these to NVGcolor at draw time.

#include <cctype>
#include <cstdio>
#include <string>

namespace tt {

struct Color {
    float r = 0.f, g = 0.f, b = 0.f, a = 1.f;
};

// Parse "#RRGGBB", "#RRGGBBAA" (also without the leading '#'). Returns `fallback`
// if the string is not a valid hex colour.
inline Color colorFromHex(const std::string& in, Color fallback = {}) {
    std::string s = in;
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() != 6 && s.size() != 8) return fallback;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return fallback;

    auto hex2 = [&](std::size_t i) {
        return static_cast<float>(std::stoi(s.substr(i, 2), nullptr, 16)) / 255.f;
    };
    Color c;
    c.r = hex2(0);
    c.g = hex2(2);
    c.b = hex2(4);
    c.a = (s.size() == 8) ? hex2(6) : 1.f;
    return c;
}

inline std::string colorToHex(const Color& c) {
    auto q = [](float v) { return static_cast<int>(v * 255.f + 0.5f) & 0xff; };
    char buf[10];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", q(c.r), q(c.g), q(c.b), q(c.a));
    return buf;
}

} // namespace tt
