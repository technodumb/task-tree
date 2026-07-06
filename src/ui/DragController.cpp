#include "ui/DragController.hpp"

#include <limits>

namespace tt {

void DragController::begin(TaskId id, Vec2 cursor, const Rect& nodeRect) {
    active_ = true;
    validTarget_ = false;
    dragged_ = id;
    target_ = 0;
    insertIndex_ = 0;
    cursor_ = cursor;
    grabOffset_ = {cursor.x - nodeRect.x, cursor.y - nodeRect.y};
    ghost_ = nodeRect;
}

void DragController::update(Vec2 cursor, const Forest& f,
                            const std::unordered_map<TaskId, Rect>& rects, float dropBandH) {
    if (!active_) return;
    cursor_ = cursor;
    ghost_.x = cursor.x - grabOffset_.x;
    ghost_.y = cursor.y - grabOffset_.y;

    // Pick the best hovered target: a node whose body (priority 2) or below-band
    // (priority 1) contains the cursor, nearest centre wins ties. Skip the dragged
    // node and anything inside its own subtree (cycle prevention).
    TaskId best = 0;
    int bestPrio = -1;
    float bestDist = std::numeric_limits<float>::max();
    for (const auto& [id, rect] : rects) {
        if (id == dragged_ || f.isDescendantOf(id, dragged_)) continue;
        int prio;
        if (rect.contains(cursor.x, cursor.y)) prio = 2;
        else if (Rect{rect.x, rect.y, rect.w, rect.h + dropBandH}.contains(cursor.x, cursor.y)) prio = 1;
        else continue;
        const float dx = cursor.x - rect.cx(), dy = cursor.y - rect.cy();
        const float dist = dx * dx + dy * dy;
        if (prio > bestPrio || (prio == bestPrio && dist < bestDist)) {
            best = id;
            bestPrio = prio;
            bestDist = dist;
        }
    }

    auto indexByCursorX = [&](const std::vector<TaskId>& siblings) {
        int idx = 0;
        for (TaskId s : siblings) {
            if (s == dragged_) continue;
            auto it = rects.find(s);
            if (it != rects.end() && it->second.cx() < cursor.x) ++idx;
        }
        return idx;
    };

    if (best != 0) {
        target_ = best;
        validTarget_ = true;
        const Task* t = f.get(best);
        insertIndex_ = t ? indexByCursorX(t->children) : 0;
    } else {
        // No node hovered -> drop at the root level.
        target_ = 0;
        validTarget_ = true;
        insertIndex_ = indexByCursorX(f.roots);
    }
}

bool DragController::drop(Forest& f) {
    if (!active_) return false;
    bool changed = false;
    if (validTarget_) changed = f.reparent(dragged_, target_, insertIndex_);
    cancel();
    return changed;
}

void DragController::cancel() {
    active_ = false;
    validTarget_ = false;
    dragged_ = 0;
    target_ = 0;
    insertIndex_ = 0;
}

std::unordered_map<TaskId, Rect> DragController::previewLayout(
    const Forest& f, const std::unordered_map<TaskId, Size>& sizes,
    const LayoutParams& params, const std::unordered_map<TaskId, Rect>& base) const {
    if (!active_ || !validTarget_) return base;
    Forest tmp = f;
    if (!tmp.reparent(dragged_, target_, insertIndex_)) return base;
    return computeLayout(tmp, sizes, params);
}

} // namespace tt
