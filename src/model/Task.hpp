#pragma once
// Task + Forest data model. Pure data + tree operations, no rendering deps, so it
// is stack-agnostic (reused verbatim on the qt6 / imgui branches) and unit testable.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tt {

using TaskId = std::uint64_t;

// A parent id of 0 means "no parent" — the task is a top-level root.
constexpr TaskId kNoParent = 0;

struct Task {
    TaskId id = 0;
    TaskId parent = kNoParent;
    std::string text;               // UTF-8
    std::vector<TaskId> children;   // ordered; sibling order IS the layout order
    bool done = false;
    bool collapsed = false;         // when true, layout hides this node's subtree (chevron toggles)
    int  status = 0;                // 0 = default, 1 = in progress (yellow), 2 = priority (orange)
    std::int64_t createdAt = 0;     // epoch ms when created (0 = unknown, e.g. pre-existing)
    std::int64_t doneAt = 0;        // epoch ms when marked done (0 = not done / null)
};

// A forest of task trees. `roots` holds the top-level tasks in display order.
class Forest {
public:
    std::unordered_map<TaskId, Task> nodes;
    std::vector<TaskId> roots;      // top-level tasks on the canvas
    std::vector<TaskId> doneRoots;  // tasks moved to the DONE section (subtrees intact)
    TaskId nextId = 1;

    Task*       get(TaskId id)       { auto it = nodes.find(id); return it == nodes.end() ? nullptr : &it->second; }
    const Task* get(TaskId id) const { auto it = nodes.find(id); return it == nodes.end() ? nullptr : &it->second; }
    bool exists(TaskId id) const { return nodes.count(id) != 0; }
    std::size_t size() const { return nodes.size(); }

    // Create a task with a fresh id. If `parent` is a real node the task is appended
    // to its children, otherwise it becomes a new root. Returns the new id.
    TaskId addTask(std::string text, TaskId parent = kNoParent, std::int64_t createdAt = 0);

    // Is `node` inside the subtree rooted at `ancestor` (walking parent links up)?
    // Returns true when node == ancestor as well.
    bool isDescendantOf(TaskId node, TaskId ancestor) const;

    // Is `node` in the DONE section (it is, or descends from, a done root)?
    bool isInDoneSection(TaskId node) const;

    // Move `child` under `newParent` (kNoParent → make it a root) at position `index`
    // among the destination's children (clamped). Rejects moves that would create a
    // cycle (dropping a node onto itself or one of its own descendants). Returns
    // false if the move was rejected or ids are invalid.
    bool reparent(TaskId child, TaskId newParent, int index);

    // Remove a task and its whole subtree. Returns number of tasks removed.
    std::size_t removeSubtree(TaskId id);

    // Move a task (with its subtree) off the canvas into the DONE section. Returns
    // false if it is already there or invalid.
    bool markDone(TaskId id);

    // Bring a DONE task (with its subtree) back to the canvas as a root. Returns
    // false if it is not in the DONE section.
    bool restoreFromDone(TaskId id);

    // Rebuild `roots` and repair parent links after a bulk load: any task whose
    // parent is missing/invalid is promoted to a root (defensive against corruption).
    void reindexRootsAfterLoad();

private:
    void detachFromParent(TaskId child); // remove id from its current parent/roots list
    void collectSubtree(TaskId id, std::vector<TaskId>& out) const;
};

} // namespace tt
