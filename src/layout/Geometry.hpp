#pragma once
// Pure geometry helpers. No GL / GLFW / X11 dependency so this can be unit tested
// on any host. Coordinates are in logical (device-independent) units.

#include <algorithm>
#include <cmath>

namespace tt {

struct Vec2 {
    float x = 0.f;
    float y = 0.f;
};

struct Size {
    float w = 0.f;
    float h = 0.f;
};

// Axis-aligned rectangle addressed by its top-left corner.
struct Rect {
    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;

    float left()   const { return x; }
    float top()    const { return y; }
    float right()  const { return x + w; }
    float bottom() const { return y + h; }
    float cx()     const { return x + w * 0.5f; }
    float cy()     const { return y + h * 0.5f; }

    bool contains(float px, float py) const {
        return px >= x && px <= x + w && py >= y && py <= y + h;
    }
};

// Bounding box of a node's collapse/expand handle: a small square centred on the middle
// of the node's bottom edge. Shared by the renderer (draw) and App (hit-test) so the
// clickable region and the drawn glyph always coincide.
inline Rect collapseHandle(const Rect& node) {
    constexpr float s = 18.f;
    return Rect{node.cx() - s * 0.5f, node.bottom() - s * 0.5f, s, s};
}

// Hit test against a rounded rectangle: inside the straight edges always counts;
// inside a corner square counts only if within `r` of that corner's arc centre.
inline bool pointInRoundedRect(const Rect& box, float r, float px, float py) {
    if (!box.contains(px, py)) return false;
    r = std::min(r, std::min(box.w, box.h) * 0.5f);
    if (r <= 0.f) return true;

    const float lx = box.x + r, rx = box.right() - r;   // x range of the corner arc centres
    const float ty = box.y + r, by = box.bottom() - r;  // y range of the corner arc centres

    // In the central cross (between the arc centres on either axis) → always inside.
    if ((px >= lx && px <= rx) || (py >= ty && py <= by)) return true;

    // Otherwise we're in a corner region; pick the nearest arc centre and test radius.
    const float ccx = (px < lx) ? lx : rx;
    const float ccy = (py < ty) ? ty : by;
    const float ddx = px - ccx, ddy = py - ccy;
    return ddx * ddx + ddy * ddy <= r * r;
}

} // namespace tt
