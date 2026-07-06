// Dependency-free verification of the pure core (model + tidy layout).
// Built by CMake as the `core_tests` CTest target; also compilable standalone:
//   g++ -std=c++17 -I ../src core_tests.cpp ../src/model/Forest.cpp ../src/layout/TidyLayout.cpp
#include "layout/TidyLayout.hpp"
#include "model/Task.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

using namespace tt;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                            \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) { ++g_fail; std::printf("  FAIL: %s\n", msg); }               \
    } while (0)

static const float EPS = 0.5f; // half a logical unit tolerance

static void checkLayout(const Forest& f, const LayoutParams& p,
                        const std::unordered_map<TaskId, Size>& sizes, const char* name) {
    std::printf("[layout] %s (%zu nodes)\n", name, f.size());
    auto rects = computeLayout(f, sizes, p);
    CHECK(rects.size() == f.size(), "every node laid out");

    std::map<int, std::vector<TaskId>> byLayer;
    for (auto& [id, r] : rects) byLayer[(int)std::lround(r.y)].push_back(id);

    // (a) No two nodes on the same layer overlap by less than hGap.
    for (auto& [y, ids] : byLayer) {
        (void)y;
        std::vector<Rect> row;
        for (TaskId id : ids) row.push_back(rects[id]);
        std::sort(row.begin(), row.end(), [](const Rect& a, const Rect& b) { return a.x < b.x; });
        for (std::size_t i = 1; i < row.size(); ++i) {
            const float gap = row[i].left() - row[i - 1].right();
            CHECK(gap >= p.hGap - EPS, "same-layer nodes respect hGap");
        }
    }

    // (b) parents centred over children; (c) children on a strictly lower layer.
    for (auto& [id, t] : f.nodes) {
        if (t.children.empty()) continue;
        const Rect& pr = rects[id];
        float cmin = 1e30f, cmax = -1e30f;
        for (TaskId c : t.children) {
            const Rect& cr = rects[c];
            cmin = std::min(cmin, cr.cx());
            cmax = std::max(cmax, cr.cx());
            CHECK(cr.y >= pr.bottom() - EPS, "child layer below parent");
        }
        const float mid = (cmin + cmax) * 0.5f;
        CHECK(std::fabs(pr.cx() - mid) <= EPS, "parent centred over children");
    }
}

static void checkDeterminism(const Forest& f, const std::unordered_map<TaskId, Size>& s) {
    LayoutParams p;
    auto a = computeLayout(f, s, p);
    auto b = computeLayout(f, s, p);
    bool same = a.size() == b.size();
    for (auto& [id, r] : a) {
        auto it = b.find(id);
        if (it == b.end() || std::fabs(it->second.x - r.x) > 1e-4f ||
            std::fabs(it->second.y - r.y) > 1e-4f) same = false;
    }
    CHECK(same, "layout is deterministic across runs");
}

int main() {
    LayoutParams P;

    { // single chain
        Forest f;
        TaskId a = f.addTask("root");
        TaskId b = f.addTask("b", a);
        f.addTask("c", b);
        std::unordered_map<TaskId, Size> s;
        checkLayout(f, P, s, "chain");
        auto r = computeLayout(f, s, P);
        CHECK(std::fabs(r[a].cx() - r[b].cx()) <= EPS, "chain stays vertically aligned");
        checkDeterminism(f, s);
    }

    { // wide fan-out
        Forest f;
        TaskId root = f.addTask("root");
        for (int i = 0; i < 6; ++i) f.addTask("child", root);
        std::unordered_map<TaskId, Size> s;
        checkLayout(f, P, s, "wide fan-out");
    }

    { // unbalanced + variable heights
        Forest f;
        TaskId root = f.addTask("root");
        TaskId l = f.addTask("left", root);
        TaskId rr = f.addTask("right", root);
        f.addTask("ll", l);
        f.addTask("lr", l);
        TaskId deep = f.addTask("rr", rr);
        f.addTask("deep", deep);
        std::unordered_map<TaskId, Size> s;
        s[root] = {200, 90};
        s[l] = {120, 44};
        s[rr] = {260, 60};
        checkLayout(f, P, s, "unbalanced + variable sizes");
    }

    { // forest of several trees
        Forest f;
        TaskId t1 = f.addTask("tree1");
        f.addTask("a", t1);
        f.addTask("b", t1);
        TaskId t2 = f.addTask("tree2");
        f.addTask("c", t2);
        f.addTask("tree3");
        std::unordered_map<TaskId, Size> s;
        checkLayout(f, P, s, "forest (3 trees)");
        auto r = computeLayout(f, s, P);
        CHECK(std::fabs(r[t1].y - r[t2].y) <= EPS, "roots share top layer");
        checkDeterminism(f, s);
    }

    { // forest model: reparent + cycle prevention + removeSubtree
        Forest f;
        TaskId a = f.addTask("a");
        TaskId b = f.addTask("b", a);
        TaskId c = f.addTask("c", b);
        TaskId d = f.addTask("d");

        CHECK(f.isDescendantOf(c, a), "c is descendant of a");
        CHECK(!f.isDescendantOf(a, c), "a is not descendant of c");
        CHECK(!f.reparent(a, c, 0), "reject cycle-creating reparent");
        CHECK(f.get(a)->parent == kNoParent, "a still a root after rejected move");
        CHECK(!f.reparent(a, a, 0), "reject self-parenting");

        CHECK(f.reparent(d, a, 0), "valid reparent accepted");
        CHECK(f.get(d)->parent == a, "d's parent updated");
        CHECK(f.get(a)->children.front() == d, "d inserted at index 0");
        CHECK(std::find(f.roots.begin(), f.roots.end(), d) == f.roots.end(), "d removed from roots");

        CHECK(f.reparent(c, kNoParent, 0), "reparent to root accepted");
        CHECK(f.get(c)->parent == kNoParent, "c is now a root");
        CHECK(f.get(b)->children.empty(), "b lost its child");

        TaskId e = f.addTask("e", a);
        f.addTask("e1", e);
        const std::size_t before = f.size();
        CHECK(f.removeSubtree(e) == 2, "removeSubtree removes e + e1");
        CHECK(f.size() == before - 2, "size shrank by 2");
    }

    { // DONE section: markDone / restoreFromDone
        Forest f;
        TaskId a = f.addTask("a");
        TaskId b = f.addTask("b", a);
        CHECK(f.markDone(a), "markDone moves a off the canvas");
        CHECK(f.get(a)->done, "a flagged done");
        CHECK(std::find(f.roots.begin(), f.roots.end(), a) == f.roots.end(), "a left roots");
        CHECK(std::find(f.doneRoots.begin(), f.doneRoots.end(), a) != f.doneRoots.end(), "a in doneRoots");
        CHECK(f.get(b)->parent == a, "subtree stays intact under a done root");
        CHECK(!f.markDone(a), "markDone twice is a no-op");
        CHECK(f.restoreFromDone(a), "restoreFromDone brings a back");
        CHECK(!f.get(a)->done, "a not done after restore");
        CHECK(std::find(f.roots.begin(), f.roots.end(), a) != f.roots.end(), "a back on the canvas");
        CHECK(f.doneRoots.empty(), "doneRoots empty after restore");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
