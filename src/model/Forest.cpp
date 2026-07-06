#include "model/Task.hpp"

#include <algorithm>

namespace tt {

namespace {
// Remove the first occurrence of `id` from `vec`; returns true if found.
bool eraseId(std::vector<TaskId>& vec, TaskId id) {
    auto it = std::find(vec.begin(), vec.end(), id);
    if (it == vec.end()) return false;
    vec.erase(it);
    return true;
}
} // namespace

TaskId Forest::addTask(std::string text, TaskId parent, std::int64_t createdAt) {
    const TaskId id = nextId++;
    Task t;
    t.id = id;
    t.text = std::move(text);
    t.createdAt = createdAt;

    Task* p = (parent != kNoParent) ? get(parent) : nullptr;
    t.parent = p ? parent : kNoParent;
    nodes.emplace(id, std::move(t));

    if (p) p->children.push_back(id);
    else   roots.push_back(id);
    return id;
}

bool Forest::isDescendantOf(TaskId node, TaskId ancestor) const {
    TaskId cur = node;
    // Walk up parent links. Guard against malformed cycles with a bounded loop.
    for (std::size_t steps = 0; cur != kNoParent && steps <= nodes.size(); ++steps) {
        if (cur == ancestor) return true;
        const Task* t = get(cur);
        if (!t) break;
        cur = t->parent;
    }
    return false;
}

void Forest::detachFromParent(TaskId child) {
    Task* c = get(child);
    if (!c) return;
    if (c->parent != kNoParent) {
        if (Task* p = get(c->parent)) eraseId(p->children, child);
    } else {
        eraseId(roots, child);
        eraseId(doneRoots, child);
    }
    c->parent = kNoParent;
}

bool Forest::reparent(TaskId child, TaskId newParent, int index) {
    if (child == kNoParent || !exists(child)) return false;
    if (newParent != kNoParent && !exists(newParent)) return false;

    // Cycle guard: cannot drop a node onto itself or one of its own descendants.
    if (child == newParent) return false;
    if (newParent != kNoParent && isDescendantOf(newParent, child)) return false;

    detachFromParent(child);

    std::vector<TaskId>& dest = (newParent != kNoParent) ? get(newParent)->children : roots;
    const int clamped = std::max(0, std::min(index, static_cast<int>(dest.size())));
    dest.insert(dest.begin() + clamped, child);
    get(child)->parent = newParent;
    return true;
}

void Forest::collectSubtree(TaskId id, std::vector<TaskId>& out) const {
    const Task* t = get(id);
    if (!t) return;
    out.push_back(id);
    for (TaskId c : t->children) collectSubtree(c, out);
}

std::size_t Forest::removeSubtree(TaskId id) {
    if (!exists(id)) return 0;
    detachFromParent(id);
    std::vector<TaskId> victims;
    collectSubtree(id, victims);
    for (TaskId v : victims) nodes.erase(v);
    return victims.size();
}

bool Forest::markDone(TaskId id) {
    Task* t = get(id);
    if (!t) return false;
    if (std::find(doneRoots.begin(), doneRoots.end(), id) != doneRoots.end()) return false;
    detachFromParent(id); // remove from parent/roots; parent -> kNoParent
    t->done = true;
    doneRoots.push_back(id);
    return true;
}

bool Forest::restoreFromDone(TaskId id) {
    Task* t = get(id);
    if (!t) return false;
    auto it = std::find(doneRoots.begin(), doneRoots.end(), id);
    if (it != doneRoots.end()) doneRoots.erase(it);
    else detachFromParent(id); // a descendant expanded inside a done subtree
    t->done = false;
    t->parent = kNoParent;
    roots.push_back(id);
    return true;
}

void Forest::reindexRootsAfterLoad() {
    roots.clear();
    doneRoots.clear();
    // First pass: any node whose parent is missing becomes a top-level node.
    for (auto& [id, t] : nodes) {
        if (t.parent != kNoParent && !exists(t.parent)) t.parent = kNoParent;
    }
    // A parent-less node is a canvas root, or a DONE root if flagged done. Ordered by
    // id for determinism when the loader recorded no explicit order.
    for (auto& [id, t] : nodes) {
        if (t.parent != kNoParent) continue;
        if (t.done) doneRoots.push_back(id);
        else        roots.push_back(id);
    }
    std::sort(roots.begin(), roots.end());
    std::sort(doneRoots.begin(), doneRoots.end());
}

} // namespace tt
