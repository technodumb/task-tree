#pragma once
// Drag & drop reparenting. The drop region for a target node is its box PLUS a band
// directly below it. While hovering, previewLayout() reflows siblings to open a gap
// at the computed insertion index (without mutating the forest). Dropping applies
// the reparent (with cycle prevention) and the tree snaps to its new layout.

#include <unordered_map>

#include "layout/Geometry.hpp"
#include "layout/TidyLayout.hpp"
#include "model/Task.hpp"

namespace tt {

class DragController {
public:
    bool   active() const { return active_; }
    TaskId dragged() const { return dragged_; }
    TaskId target() const { return target_; }
    int    insertIndex() const { return insertIndex_; }
    bool   validTarget() const { return validTarget_; }
    Vec2   cursor() const { return cursor_; }
    const Rect& ghost() const { return ghost_; }

    // Start dragging node `id`; `nodeRect` is its current laid-out rectangle.
    void begin(TaskId id, Vec2 cursor, const Rect& nodeRect);

    // Recompute ghost position, hovered target, and insertion index on cursor move.
    // `dropBandH` is how far below a node its drop region extends.
    void update(Vec2 cursor, const Forest& f,
                const std::unordered_map<TaskId, Rect>& rects, float dropBandH);

    // Apply the pending move. Returns true if the forest changed.
    bool drop(Forest& f);
    void cancel();

    // Layout with the dragged node virtually reparented to the hovered slot, so
    // siblings open a gap. Returns `base` unchanged when there is no valid target.
    std::unordered_map<TaskId, Rect> previewLayout(
        const Forest& f, const std::unordered_map<TaskId, Size>& sizes,
        const LayoutParams& params, const std::unordered_map<TaskId, Rect>& base) const;

private:
    bool   active_ = false;
    bool   validTarget_ = false;
    TaskId dragged_ = 0;
    TaskId target_ = 0;       // 0 => drop at the root level
    int    insertIndex_ = 0;
    Vec2   cursor_;
    Vec2   grabOffset_;       // cursor - node top-left at pickup
    Rect   ghost_;
};

} // namespace tt
