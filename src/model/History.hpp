#pragma once
// Undo/redo as whole-Forest snapshots. A Forest is just maps + vectors of small POD
// structs, so copying one (a few hundred tasks) costs microseconds — cheaper to reason
// about than a command stack, and it keeps this in the pure, dependency-free, unit-tested
// core. Depth is bounded so long sessions don't grow without limit.
//
// Usage: call record()/snapshot() with the CURRENT forest immediately BEFORE applying a
// mutation, then undo()/redo() swap the live forest with the neighbouring snapshot.

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "model/Task.hpp"

namespace tt {

class History {
public:
    explicit History(std::size_t maxDepth = 100) : maxDepth_(maxDepth) {}

    // Record a checkpoint (the state to return to on the next undo). Forks history:
    // any redo states are dropped, since a fresh edit invalidates the redone future.
    void record(Forest snapshot) {
        highWater_ = std::max(highWater_, snapshot.nextId);
        past_.push_back(std::move(snapshot));
        if (past_.size() > maxDepth_) past_.erase(past_.begin());
        future_.clear();
    }
    void snapshot(const Forest& cur) { record(cur); } // copy convenience

    bool canUndo() const { return !past_.empty(); }
    bool canRedo() const { return !future_.empty(); }

    // Swap `cur` back to the previous checkpoint; the replaced `cur` becomes redoable.
    bool undo(Forest& cur) {
        if (past_.empty()) return false;
        swapInto(cur, past_, future_);
        return true;
    }

    // Reapply the most recently undone state; the replaced `cur` becomes undoable again.
    bool redo(Forest& cur) {
        if (future_.empty()) return false;
        swapInto(cur, future_, past_);
        return true;
    }

    void clear() { past_.clear(); future_.clear(); }

private:
    // Move `cur` onto `to`, pop the newest `from` into `cur`. `nextId` is kept monotonic
    // across the swap (advanced to the high-water mark) so an id is never reused for a
    // different task after an undo — otherwise stale ids held elsewhere (selection, the
    // DONE-expanded set, in-flight classifications) could silently rebind.
    void swapInto(Forest& cur, std::vector<Forest>& from, std::vector<Forest>& to) {
        highWater_ = std::max(highWater_, cur.nextId);
        to.push_back(std::move(cur));
        cur = std::move(from.back());
        from.pop_back();
        cur.nextId = std::max(cur.nextId, highWater_);
    }

    std::vector<Forest> past_;   // checkpoints, oldest first; back() is the next undo
    std::vector<Forest> future_; // undone states; back() is the next redo
    std::size_t maxDepth_;
    TaskId highWater_ = 0;       // max nextId ever seen; keeps ids monotonic across undo
};

} // namespace tt
