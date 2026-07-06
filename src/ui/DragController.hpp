#pragma once
// Drag & drop reparenting.
//
// Stability: while dragging, the tree is NOT re-laid-out. Every node stays at its
// committed position — the target and everything above/beside it does not move — and
// only the target's existing children shift horizontally to open a gap for the
// incoming node. The full re-layout (and re-centering) happens once, on drop. This
// keeps the drop target still while you aim for it.

#include <unordered_map>

#include "layout/Geometry.hpp"
#include "layout/TidyLayout.hpp"
#include "model/Task.hpp"

namespace tt {

class DragController {
public:
    bool   active() const { return active_; }
    bool   moved() const { return moved_; }   // true once dragged past the click threshold
    TaskId dragged() const { return dragged_; }
    TaskId target() const { return target_; }
    int    insertIndex() const { return insertIndex_; }
    bool   validTarget() const { return validTarget_; }
    Vec2   cursor() const { return cursor_; }
    const Rect& ghost() const { return ghost_; }

    // Preview edge endpoints (valid after update() when validTarget && target != 0).
    Vec2 targetBottom() const { return targetBottom_; }
    Vec2 slotTop() const { return slotTop_; }

    // Start dragging node `id`; `nodeRect` is its current laid-out rectangle.
    void begin(TaskId id, Vec2 cursor, const Rect& nodeRect);

    // Recompute ghost position, hovered target, insertion index, and slot geometry on
    // cursor move. Uses the committed `rects` (which stay fixed during the drag).
    void update(Vec2 cursor, const Forest& f,
                const std::unordered_map<TaskId, Rect>& rects, const LayoutParams& params);

    // Apply the pending move. Returns true if the forest changed.
    bool drop(Forest& f);
    void cancel();

    // The committed layout with only the target's children[insertIndex..] shifted
    // right to open a gap. Everything else keeps its `base` position. No full re-layout.
    std::unordered_map<TaskId, Rect> previewLayout(
        const Forest& f, const std::unordered_map<TaskId, Rect>& base) const;

private:
    bool   active_ = false;
    bool   validTarget_ = false;
    TaskId dragged_ = 0;
    TaskId target_ = 0;       // 0 => drop at the root level
    int    insertIndex_ = 0;
    Vec2   cursor_;
    Vec2   startCursor_;      // cursor at pickup (for the click-vs-drag threshold)
    bool   moved_ = false;
    Vec2   grabOffset_;       // cursor - node top-left at pickup
    Rect   ghost_;

    Vec2   targetBottom_;     // preview edge start (target bottom-centre)
    Vec2   slotTop_;          // preview edge end (opened-gap centre, child layer)
    float  dragWidth_ = 0.f;  // width of the dragged node (from base rects)

    // Reflow of the target's children (re-centred under the fixed target with a gap
    // slot at insertIndex). Empty/false when hovering the root level -> no reflow.
    bool   reflowOk_ = false;
    std::unordered_map<TaskId, float> reflowKidCenter_; // new centre-x per child
};

} // namespace tt
