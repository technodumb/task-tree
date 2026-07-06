#include "ui/DragController.hpp"

#include <algorithm>
#include <limits>
#include <vector>

namespace tt {
namespace {

// The target's (or root-level) children in layout order, excluding the dragged node.
std::vector<TaskId> siblingsExcludingDragged(const Forest& f, TaskId parent, TaskId dragged) {
    const std::vector<TaskId>* src = (parent == kNoParent)
        ? &f.roots
        : (f.get(parent) ? &f.get(parent)->children : nullptr);
    std::vector<TaskId> out;
    if (src)
        for (TaskId id : *src)
            if (id != dragged) out.push_back(id);
    return out;
}

void translateSubtree(std::unordered_map<TaskId, Rect>& r, const Forest& f, TaskId id, float dx) {
    if (auto it = r.find(id); it != r.end()) it->second.x += dx;
    if (const Task* t = f.get(id))
        for (TaskId c : t->children) translateSubtree(r, f, c, dx);
}

// Arrange the target's children (excluding the dragged node) plus a gap slot of
// width `dragWidth` at `insertIndex`, centred horizontally under the target's fixed
// position. Returns the new centre-x for each child, the slot centre, and the child
// layer y. `ok` is false at the root level (target == 0) — the root layer never
// reflows, so nudging a node in empty space doesn't move the tree.
struct Reflow {
    bool ok = false;
    std::unordered_map<TaskId, float> kidCenter;
    float slotCenterX = 0.f;
    float layerY = 0.f;
};

Reflow computeReflow(TaskId target, TaskId dragged, int insertIndex, float dragWidth,
                     const Forest& f, const std::unordered_map<TaskId, Rect>& base,
                     const LayoutParams& params) {
    Reflow r;
    if (target == kNoParent) return r; // root level: no reflow
    auto trIt = base.find(target);
    if (trIt == base.end()) return r;
    const float centerX = trIt->second.cx();

    const std::vector<TaskId> kids = siblingsExcludingDragged(f, target, dragged);
    const int n = static_cast<int>(kids.size());

    if (n == 0) {
        r.layerY = trIt->second.bottom() + params.vGap;
    } else {
        auto it0 = base.find(kids[0]);
        r.layerY = (it0 != base.end()) ? it0->second.y : trIt->second.bottom() + params.vGap;
    }

    auto widthOf = [&](TaskId id) {
        auto it = base.find(id);
        return it != base.end() ? it->second.w : params.defaultSize.w;
    };

    // Total width of (n children + 1 slot) with n gaps between the n+1 items.
    float totalW = dragWidth + params.hGap * n;
    for (TaskId k : kids) totalW += widthOf(k);

    const int k = std::max(0, std::min(insertIndex, n));
    float x = centerX - totalW * 0.5f; // centre the row under the fixed target
    auto place = [&](float w, TaskId id) {
        const float c = x + w * 0.5f;
        if (id == kNoParent) r.slotCenterX = c;
        else                 r.kidCenter[id] = c;
        x += w + params.hGap;
    };
    for (int i = 0; i <= n; ++i) {
        if (i == k) place(dragWidth, kNoParent); // the gap slot
        if (i < n)  place(widthOf(kids[i]), kids[i]);
    }
    r.ok = true;
    return r;
}

} // namespace

void DragController::begin(TaskId id, Vec2 cursor, const Rect& nodeRect) {
    active_ = true;
    validTarget_ = false;
    dragged_ = id;
    target_ = 0;
    insertIndex_ = 0;
    cursor_ = cursor;
    startCursor_ = cursor;
    moved_ = false;
    grabOffset_ = {cursor.x - nodeRect.x, cursor.y - nodeRect.y};
    ghost_ = nodeRect;
    dragWidth_ = nodeRect.w;
    reflowOk_ = false;
    reflowKidCenter_.clear();
}

void DragController::update(Vec2 cursor, const Forest& f,
                            const std::unordered_map<TaskId, Rect>& rects, const LayoutParams& params) {
    if (!active_) return;
    cursor_ = cursor;
    ghost_.x = cursor.x - grabOffset_.x;
    ghost_.y = cursor.y - grabOffset_.y;
    if (auto it = rects.find(dragged_); it != rects.end()) dragWidth_ = it->second.w;

    // Below the click threshold this is a click, not a drag: don't reparent/reflow.
    if (!moved_) {
        const float dx = cursor.x - startCursor_.x, dy = cursor.y - startCursor_.y;
        if (dx * dx + dy * dy < 25.f) {
            validTarget_ = false;
            reflowOk_ = false;
            slotTop_ = {};
            targetBottom_ = {};
            return;
        }
        moved_ = true;
    }

    // Pick the hovered target: a node whose body (priority 2) or below-band (priority
    // 1) contains the cursor; nearest centre wins ties. Skip the dragged node and its
    // own subtree (cycle prevention).
    TaskId best = 0;
    int bestPrio = -1;
    float bestDist = std::numeric_limits<float>::max();
    for (const auto& [id, rect] : rects) {
        if (id == dragged_ || f.isDescendantOf(id, dragged_)) continue;
        int prio;
        if (rect.contains(cursor.x, cursor.y)) prio = 2;
        else if (Rect{rect.x, rect.y, rect.w, rect.h + params.vGap}.contains(cursor.x, cursor.y)) prio = 1;
        else continue;
        const float dx = cursor.x - rect.cx(), dy = cursor.y - rect.cy();
        const float dist = dx * dx + dy * dy;
        if (prio > bestPrio || (prio == bestPrio && dist < bestDist)) {
            best = id;
            bestPrio = prio;
            bestDist = dist;
        }
    }

    target_ = best; // 0 => root level
    validTarget_ = true;

    const std::vector<TaskId> kids = siblingsExcludingDragged(f, target_, dragged_);
    int idx = 0;
    for (TaskId s : kids) {
        auto it = rects.find(s);
        if (it != rects.end() && it->second.cx() < cursor.x) ++idx;
    }
    insertIndex_ = idx;

    // Compute the reflow (fixed target, children re-centred with a gap slot).
    const Reflow rf = computeReflow(target_, dragged_, insertIndex_, dragWidth_, f, rects, params);
    reflowOk_ = rf.ok;
    reflowKidCenter_ = rf.kidCenter;
    targetBottom_ = {};
    slotTop_ = {};
    if (target_ != 0) {
        if (auto it = rects.find(target_); it != rects.end())
            targetBottom_ = {it->second.cx(), it->second.bottom()};
    }
    if (rf.ok) slotTop_ = {rf.slotCenterX, rf.layerY};
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
    moved_ = false;
    dragged_ = 0;
    target_ = 0;
    insertIndex_ = 0;
    reflowOk_ = false;
    reflowKidCenter_.clear();
}

std::unordered_map<TaskId, Rect> DragController::previewLayout(
    const Forest& f, const std::unordered_map<TaskId, Rect>& base) const {
    std::unordered_map<TaskId, Rect> result = base;
    if (!active_ || !validTarget_ || !reflowOk_) return result; // root level -> tree frozen

    // Slide each of the target's children (and its subtree) to its re-centred position.
    for (const auto& [id, newCenter] : reflowKidCenter_) {
        auto it = base.find(id);
        if (it == base.end()) continue;
        translateSubtree(result, f, id, newCenter - it->second.cx());
    }
    return result;
}

} // namespace tt
