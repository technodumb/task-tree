#include "ui/DragController.hpp"

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

} // namespace

void DragController::begin(TaskId id, Vec2 cursor, const Rect& nodeRect) {
    active_ = true;
    validTarget_ = false;
    dragged_ = id;
    target_ = 0;
    insertIndex_ = 0;
    cursor_ = cursor;
    grabOffset_ = {cursor.x - nodeRect.x, cursor.y - nodeRect.y};
    ghost_ = nodeRect;
    dragWidth_ = nodeRect.w;
    gap_ = 0.f;
}

void DragController::update(Vec2 cursor, const Forest& f,
                            const std::unordered_map<TaskId, Rect>& rects, const LayoutParams& params) {
    if (!active_) return;
    cursor_ = cursor;
    ghost_.x = cursor.x - grabOffset_.x;
    ghost_.y = cursor.y - grabOffset_.y;
    if (auto it = rects.find(dragged_); it != rects.end()) dragWidth_ = it->second.w;
    gap_ = dragWidth_ + params.hGap;

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

    computeSlot(f, rects, params);
}

void DragController::computeSlot(const Forest& f, const std::unordered_map<TaskId, Rect>& rects,
                                 const LayoutParams& params) {
    targetBottom_ = {};
    slotTop_ = {};
    if (target_ != 0) {
        auto tr = rects.find(target_);
        if (tr != rects.end()) targetBottom_ = {tr->second.cx(), tr->second.bottom()};
    }

    const std::vector<TaskId> kids = siblingsExcludingDragged(f, target_, dragged_);
    float layerY, slotLeft;
    if (kids.empty()) {
        // Gap sits directly below the (childless) target.
        const auto tr = rects.find(target_);
        const float baseY = (tr != rects.end()) ? tr->second.bottom() : params.topMargin;
        const float baseX = (tr != rects.end()) ? tr->second.cx() : params.centerWidth * 0.5f;
        layerY = (target_ == 0) ? params.topMargin : baseY + params.vGap;
        slotLeft = baseX - dragWidth_ * 0.5f;
    } else {
        const int k = insertIndex_;
        auto first = rects.find(kids.front());
        layerY = (first != rects.end()) ? first->second.y : params.topMargin;
        if (k <= 0) {
            slotLeft = (first != rects.end()) ? first->second.left() : 0.f;
        } else if (k >= static_cast<int>(kids.size())) {
            auto last = rects.find(kids.back());
            slotLeft = (last != rects.end()) ? last->second.right() + params.hGap : 0.f;
        } else {
            auto prev = rects.find(kids[k - 1]);
            slotLeft = (prev != rects.end()) ? prev->second.right() + params.hGap : 0.f;
        }
    }
    slotTop_ = {slotLeft + dragWidth_ * 0.5f, layerY};
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
    gap_ = 0.f;
}

std::unordered_map<TaskId, Rect> DragController::previewLayout(
    const Forest& f, const std::unordered_map<TaskId, Rect>& base) const {
    std::unordered_map<TaskId, Rect> result = base;
    if (!active_ || !validTarget_) return result;

    // Open a gap: shift the target's children at index >= insertIndex (and their whole
    // subtrees) right by gap_. Nothing else moves.
    const std::vector<TaskId> kids = siblingsExcludingDragged(f, target_, dragged_);
    for (std::size_t i = static_cast<std::size_t>(std::max(0, insertIndex_)); i < kids.size(); ++i)
        translateSubtree(result, f, kids[i], gap_);
    return result;
}

} // namespace tt
