// Dependency-free verification of the pure core (model + tidy layout).
// Built by CMake as the `core_tests` CTest target; also compilable standalone:
//   g++ -std=c++17 -I ../src core_tests.cpp ../src/model/Forest.cpp ../src/layout/TidyLayout.cpp
#include "app/DevRoute.hpp"
#include "app/Palette.hpp"
#include "layout/TidyLayout.hpp"
#include "model/History.hpp"
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

    { // Ctrl+click reparent: the selection lands as the target's LAST child, keeping the
      // target's existing children in order. (App appends at children.size().)
        Forest f;
        TaskId p = f.addTask("p");
        TaskId k1 = f.addTask("k1", p);
        TaskId k2 = f.addTask("k2", p);
        TaskId moved = f.addTask("moved");
        TaskId kid = f.addTask("kid", moved);

        const int last = static_cast<int>(f.get(p)->children.size());
        CHECK(f.reparent(moved, p, last), "append-as-last-child accepted");
        CHECK((f.get(p)->children == std::vector<TaskId>{k1, k2, moved}), "appended after k1,k2");
        CHECK(f.get(kid)->parent == moved, "the moved node keeps its own subtree");
        // Clamping: an out-of-range index still appends rather than failing.
        CHECK(f.reparent(k1, p, 99), "over-large index clamps");
        CHECK(f.get(p)->children.back() == k1, "k1 clamped to the end");
        // The reverse move (parent under its own descendant) stays rejected.
        CHECK(!f.reparent(p, kid, 0), "reject moving p under its own descendant");
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
        CHECK(f.isInDoneSection(a), "done root is in the DONE section");
        CHECK(f.isInDoneSection(b), "descendant of a done root is in the DONE section");
        TaskId canvasRoot = f.addTask("still on canvas");
        CHECK(!f.isInDoneSection(canvasRoot), "a canvas task is not in the DONE section");
        CHECK(!f.markDone(a), "markDone twice is a no-op");
        CHECK(f.restoreFromDone(a), "restoreFromDone brings a back");
        CHECK(!f.get(a)->done, "a not done after restore");
        CHECK(std::find(f.roots.begin(), f.roots.end(), a) != f.roots.end(), "a back on the canvas");
        CHECK(f.doneRoots.empty(), "doneRoots empty after restore");
    }

    { // dev fast-path: ttd> detection + dev-root find/create
        CHECK(isDevTask("ttd> do a thing"), "ttd> prefix detected");
        CHECK(isDevTask("TTD>caps, no space"), "case-insensitive, space after > optional");
        CHECK(isDevTask("   ttd> leading whitespace ignored"), "leading whitespace ignored");
        CHECK(!isDevTask("todo> not a dev task"), "non-marker prefix is not a dev task");
        CHECK(!isDevTask("do ttd> in the middle"), "marker only counts at the start");
        CHECK(!isDevTask(""), "empty text is not a dev task");
        CHECK(!isDevTask("ttd"), "bare 'ttd' without '>' is not a dev task");

        Forest f;
        f.addTask("unrelated root");
        const TaskId dev1 = ensureDevRoot(f, 0);
        CHECK(f.get(dev1) && f.get(dev1)->text == kDevNodeTitle, "dev root created with the right title");
        CHECK(f.get(dev1)->parent == kNoParent, "dev root is a canvas root");
        const TaskId dev2 = ensureDevRoot(f, 0);
        CHECK(dev1 == dev2, "second call reuses the existing dev root (no duplicate)");
        TaskId task = f.addTask("ttd> a dev task", dev1);
        CHECK(f.get(task)->parent == dev1, "dev task lands under the dev root");

        Forest g; // case-insensitive reuse of a pre-existing dev node
        const TaskId existing = g.addTask("Tasktree Dev");
        CHECK(ensureDevRoot(g, 0) == existing, "existing dev root reused case-insensitively");

        Forest h; // a DONE dev root must not be reused (only canvas roots are scanned)
        const TaskId doneDev = h.addTask(kDevNodeTitle);
        h.markDone(doneDev);
        const TaskId freshDev = ensureDevRoot(h, 0);
        CHECK(freshDev != doneDev, "does not resurrect a DONE dev node; makes a fresh one");
    }

    { // collapse hides a subtree from layout (the nodes remain in the model)
        Forest f;
        TaskId root = f.addTask("root");
        TaskId a = f.addTask("a", root);
        TaskId b = f.addTask("b", root);
        f.addTask("a1", a);
        f.addTask("a2", a);
        std::unordered_map<TaskId, Size> s;

        auto full = computeLayout(f, s, P);
        CHECK(full.size() == f.size(), "all 5 nodes laid out when nothing is collapsed");

        f.get(a)->collapsed = true;
        auto col = computeLayout(f, s, P);
        CHECK(col.size() == 3, "collapsed a's two children are omitted from layout");
        CHECK(col.count(root) && col.count(a) && col.count(b), "root, a, b still laid out");
        CHECK(col.count(f.get(a)->children[0]) == 0, "hidden child a1 not in layout");
        CHECK(f.size() == 5, "model still holds every node (collapse is view-only)");
        CHECK(col[a].y >= col[root].bottom() - EPS, "collapsed a still sits below its parent");
    }

    { // undo/redo history over Forest snapshots
        Forest f;
        TaskId a = f.addTask("a");
        History h;
        CHECK(!h.canUndo() && !h.canRedo(), "history starts empty");
        CHECK(!h.undo(f), "undo on empty history is a no-op");

        h.snapshot(f);                 // checkpoint {a}
        TaskId b = f.addTask("b", a);  // {a, a>b}
        CHECK(h.canUndo(), "can undo after a snapshot");
        CHECK(h.undo(f), "undo succeeds");
        CHECK(!f.exists(b) && f.size() == 1, "undo restored the single-node state");
        CHECK(h.canRedo() && h.redo(f), "redo succeeds");
        CHECK(f.exists(b) && f.size() == 2, "redo re-added b");
        CHECK(!h.canRedo(), "redo stack emptied by the redo");

        // A fresh edit forks history: the redo future is dropped.
        h.snapshot(f);
        f.addTask("c");
        CHECK(h.undo(f), "undo the c add");
        CHECK(h.canRedo(), "c is redoable");
        h.snapshot(f);
        f.addTask("d");
        CHECK(!h.canRedo(), "a new edit cleared the redo future");

        // nextId is part of the snapshot, so undo must not cause id reuse.
        Forest g;
        History hg;
        hg.snapshot(g);
        TaskId g1 = g.addTask("one");
        CHECK(hg.undo(g), "undo the add");
        TaskId g2 = g.addTask("two");
        CHECK(g2 != g1, "ids not reused after undo (nextId snapshotted)");

        // Bounded depth: only the last `maxDepth` checkpoints are retained.
        Forest k;
        History hk(2);
        for (int i = 0; i < 5; ++i) { hk.snapshot(k); k.addTask("x"); }
        int undos = 0;
        while (hk.undo(k)) ++undos;
        CHECK(undos == 2, "history depth bounded to 2");
    }

    { // command palette: prefix -> mode, and (mode, argument) -> action
        using namespace tt::palette;
        std::printf("[palette] grammar\n");

        // A prefix names a mode; the symbol is consumed, so the bar then holds only the
        // argument (App enters the mode; interpret() never sees the symbol again).
        CHECK(modeForPrefix('?') == Mode::Find, "? -> find mode");
        CHECK(modeForPrefix(':') == Mode::Select, ": -> select mode");
        CHECK(modeForPrefix('>') == Mode::Parent, "> -> parent mode");
        CHECK(modeForPrefix('/') == Mode::Menu, "/ -> mode menu");
        CHECK(modeForPrefix('x') == Mode::Add, "an ordinary character is not a prefix");
        CHECK(modeForPrefix(' ') == Mode::Add, "space is not a prefix");

        CHECK(infoFor(Mode::Find)->prefix == '?', "find's symbol");
        CHECK(std::string(infoFor(Mode::Select)->name) == "select", "select's chip label");
        CHECK(infoFor(Mode::Add) == nullptr, "add has no chip");
        CHECK(modes().size() == 3, "three modes are offered in the menu");

        CHECK(interpret(Mode::Add, "buy milk").kind == Kind::AddTask, "add mode -> add");
        CHECK(interpret(Mode::Add, "buy milk").body == "buy milk", "add keeps the text");
        CHECK(interpret(Mode::Add, "  spaced  ").body == "spaced", "add trims");
        CHECK(!interpret(Mode::Add, ":x").isCommand(), "a colon mid-text is just text");
        CHECK(interpret(Mode::Menu, "anything").kind == Kind::AddTask, "menu acts on nothing");

        CHECK(interpret(Mode::Find, "beta").kind == Kind::Find, "find mode -> find");
        CHECK(interpret(Mode::Find, " beta ").query == "beta", "find query trimmed");
        CHECK(interpret(Mode::Find, "").query.empty(), "empty find has no query");

        CHECK(interpret(Mode::Select, "12").kind == Kind::SelectId, "digits -> select by id");
        CHECK(interpret(Mode::Select, "12").id == 12, "select id parsed");
        CHECK(interpret(Mode::Select, " 7 ").id == 7, "select id trims");
        CHECK(interpret(Mode::Select, "foo").kind == Kind::SelectText, "text -> select by text");
        CHECK(interpret(Mode::Select, "foo").query == "foo", "select query parsed");
        // ':?12' as typed: ':' entered the mode, so the argument is "?12".
        CHECK(interpret(Mode::Select, "?12").kind == Kind::SelectText, "? forces a text query");
        CHECK(interpret(Mode::Select, "?12").query == "12", "forced query keeps the digits");
        CHECK(interpret(Mode::Select, "").kind == Kind::SelectText, "empty select picks by text");
        CHECK(interpret(Mode::Select, "").id == 0, "empty select targets nothing");

        CHECK(interpret(Mode::Parent, "7").kind == Kind::ParentId, "digits -> parent by id");
        CHECK(interpret(Mode::Parent, "7").id == 7, "parent id parsed");
        CHECK(interpret(Mode::Parent, "deep work").kind == Kind::ParentText, "text -> by text");
        CHECK(interpret(Mode::Parent, "?deep").query == "deep", "forced parent query");
        CHECK(interpret(Mode::Parent, "99999999999999999999999999").id == 0,
              "absurd id clamps to none");

        CHECK(interpret(Mode::Find, "x").picksByText() &&
              interpret(Mode::Select, "?x").picksByText() &&
              interpret(Mode::Parent, "?x").picksByText(), "text modes drive the candidate list");
        CHECK(!interpret(Mode::Select, "9").picksByText() &&
              !interpret(Mode::Parent, "9").picksByText(), "id modes don't");
        CHECK(interpret(Mode::Select, "9").selects() && interpret(Mode::Select, "x").selects(),
              "select actions flagged");
        CHECK(interpret(Mode::Parent, "9").reparents() && interpret(Mode::Parent, "x").reparents(),
              "parent actions flagged");
        CHECK(!interpret(Mode::Add, "plain").isCommand() && interpret(Mode::Find, "q").isCommand(),
              "isCommand");

        // The '/' menu is built straight from modes(): every mode has the three things the
        // picker renders — a symbol for the list, a name for the bar, a blurb for the status.
        for (const ModeInfo& i : modes()) {
            CHECK(i.prefix != '\0' && i.name && i.blurb && i.hint, "mode row is complete");
            CHECK(modeForPrefix(i.prefix) == i.mode, "prefix round-trips to its mode");
            CHECK(infoFor(i.mode) == &i, "infoFor returns that row");
        }
        CHECK(infoFor(Mode::Menu) != nullptr, "the menu has a chip label of its own");
    }

    { // command palette: match ranking (best first, deterministic)
        using namespace tt::palette;
        std::printf("[palette] ranking\n");
        Forest f;
        TaskId alpha = f.addTask("alpha beta");    // match at 6
        TaskId beta  = f.addTask("beta");          // match at 0, shortest
        TaskId gamma = f.addTask("gamma beta");    // match at 6, same length as alpha
        TaskId betas = f.addTask("beta gamma");    // match at 0, longer than "beta"
        TaskId done  = f.addTask("beta done");
        f.markDone(done);                          // DONE isn't on the canvas

        auto r = rankMatches(f, "beta");
        CHECK(r.size() == 4, "DONE nodes excluded from matches");
        CHECK(r[0] == beta, "shortest prefix match ranks first");
        CHECK(r[1] == betas, "other prefix match next");
        CHECK(std::find(r.begin(), r.end(), done) == r.end(), "the done node is absent");
        // pos 6 for both; equal length -> smaller id wins, so ordering is stable.
        CHECK(r[2] == std::min(alpha, gamma) && r[3] == std::max(alpha, gamma),
              "equal-rank matches ordered by id");

        CHECK(rankMatches(f, "BETA").size() == 4, "matching is case-insensitive");
        CHECK(rankMatches(f, "beta", 2).size() == 2, "limit respected");
        CHECK(rankMatches(f, "beta", 2)[0] == beta, "limit keeps the best");
        CHECK(rankMatches(f, "").empty(), "empty query matches nothing");
        CHECK(rankMatches(f, "   ").empty(), "whitespace query matches nothing");
        CHECK(rankMatches(f, "zzz").empty(), "no false matches");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
