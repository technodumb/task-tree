#pragma once
// Tidy-tree layout engine. PURE: given the forest and the measured size of each
// node, it produces a top-left Rect per node. No GL / text measuring here — the
// renderer measures node sizes and feeds them in, keeping this fully unit testable.
//
// Conventions: tree grows top -> down. Siblings spread on x, depth maps to y.
// Every node at a given depth shares the same top y (a node's own height only
// affects the y of the layer *below* it). Parents are centred over their children,
// and subtrees never overlap (minimum horizontal gap == params.hGap between boxes).

#include <unordered_map>

#include "layout/Geometry.hpp"
#include "model/Task.hpp"

namespace tt {

struct LayoutParams {
    float hGap = 28.f;       // minimum horizontal gap between node boxes
    float vGap = 52.f;       // vertical gap between layers
    float topMargin = 48.f;  // y of the first (root) layer
    float leftMargin = 48.f; // x the leftmost node is normalised to
    Size  defaultSize{160.f, 44.f}; // used when a node has no measured size
};

// Returns a map from task id to its laid-out Rect (top-left based, logical units).
// Multiple roots are laid out as siblings of an implicit (undrawn) super-root, so
// separate trees are spaced horizontally and real roots sit on the top layer.
std::unordered_map<TaskId, Rect> computeLayout(const Forest& forest,
                                               const std::unordered_map<TaskId, Size>& sizes,
                                               const LayoutParams& params);

} // namespace tt
