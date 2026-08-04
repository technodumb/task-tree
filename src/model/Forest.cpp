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

bool Forest::isInDoneSection(TaskId node) const {
    TaskId cur = node;
    // Walk up: a done flag anywhere on the ancestor chain (including this node) takes it
    // off the canvas. Bounded like the other walks, in case stored data holds a cycle.
    for (std::size_t steps = 0; cur != kNoParent && steps <= nodes.size(); ++steps) {
        const Task* t = get(cur);
        if (!t) break;
        if (t->isDone()) return true;
        cur = t->parent;
    }
    return false;
}

std::vector<TaskId> Forest::doneSectionRoots() const {
    std::vector<TaskId> out;
    for (const auto& [id, t] : nodes)
        if (t.isDone() && !isInDoneSection(t.parent)) out.push_back(id);
    // Newest completion first. Tasks finished before doneAt existed have doneAt == 0 and
    // fall back to id order (later id = created later = almost certainly finished later).
    std::sort(out.begin(), out.end(), [this](TaskId a, TaskId b) {
        const Task* ta = get(a);
        const Task* tb = get(b);
        const std::int64_t da = ta ? ta->doneAt : 0, dbb = tb ? tb->doneAt : 0;
        if (da != dbb) return da > dbb;
        return a > b;
    });
    return out;
}

void Forest::detachFromParent(TaskId child) {
    Task* c = get(child);
    if (!c) return;
    if (c->parent != kNoParent) {
        if (Task* p = get(c->parent)) eraseId(p->children, child);
    } else {
        eraseId(roots, child);
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

bool Forest::markDone(TaskId id, std::int64_t doneAtMs) {
    Task* t = get(id);
    if (!t || t->isDone()) return false;
    // Just a timestamp: the task keeps its parent and its slot among its siblings, so the
    // DONE section is a view of the same tree rather than a second one. Layout skips it.
    t->doneAt = (doneAtMs != 0) ? doneAtMs : kDoneAtUnknown;
    return true;
}

bool Forest::restoreFromDone(TaskId id) {
    Task* t = get(id);
    if (!t || !t->isDone()) return false;
    t->doneAt = 0;             // clearing the timestamp IS "not done"

    // It never moved, so clearing the flag already put it back under its original parent in
    // its original position. The exception: if an ancestor is still done the task would stay
    // invisible, so promote it to a root where the user can actually see it.
    if (t->parent != kNoParent && isInDoneSection(t->parent)) {
        detachFromParent(id);
        roots.push_back(id);
    }
    return true;
}

bool equivalent(const Forest& a, const Forest& b) {
    if (a.nextId != b.nextId) return false;
    if (a.roots != b.roots) return false;
    if (a.nodes.size() != b.nodes.size()) return false;
    for (const auto& [id, ta] : a.nodes) {
        const Task* tb = b.get(id);
        if (!tb) return false;
        if (ta.id != tb->id || ta.parent != tb->parent || ta.text != tb->text ||
            ta.collapsed != tb->collapsed || ta.status != tb->status ||
            ta.createdAt != tb->createdAt || ta.doneAt != tb->doneAt ||
            ta.children != tb->children)
            return false;
    }
    return true;
}

void Forest::repairAfterLoad() {
    // A parent that no longer exists means top level.
    for (auto& [id, t] : nodes)
        if (t.parent != kNoParent && !exists(t.parent)) t.parent = kNoParent;

    // Keep only children that exist and name this node as their parent, without duplicates.
    // Deterministic ids for the passes below.
    std::vector<TaskId> all;
    all.reserve(nodes.size());
    for (const auto& [id, t] : nodes) all.push_back(id);
    std::sort(all.begin(), all.end());

    for (TaskId id : all) {
        Task& t = nodes[id];
        std::vector<TaskId> kept;
        kept.reserve(t.children.size());
        for (TaskId c : t.children) {
            const Task* ct = get(c);
            if (!ct || ct->parent != id) continue;
            if (std::find(kept.begin(), kept.end(), c) != kept.end()) continue;
            kept.push_back(c);
        }
        t.children = std::move(kept);
    }

    // A task whose parent never listed it: append, so it is reachable by the walks.
    for (TaskId id : all) {
        const TaskId p = nodes[id].parent;
        if (p == kNoParent) continue;
        std::vector<TaskId>& kids = nodes[p].children;
        if (std::find(kids.begin(), kids.end(), id) == kids.end()) kids.push_back(id);
    }

    // Same for the top level: a parent-less task the loader's `roots` order omitted.
    for (TaskId id : all) {
        if (nodes[id].parent != kNoParent) continue;
        if (std::find(roots.begin(), roots.end(), id) == roots.end()) roots.push_back(id);
    }
    // And drop anything in `roots` that is not actually a parent-less task.
    roots.erase(std::remove_if(roots.begin(), roots.end(),
                               [this](TaskId id) {
                                   const Task* t = get(id);
                                   return !t || t->parent != kNoParent;
                               }),
                roots.end());
}

} // namespace tt
