#include "app/App.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <GLFW/glfw3.h>

#include "app/DevRoute.hpp"
#include "llm/LlmLog.hpp"
#include "model/Store.hpp"

namespace tt {
namespace {

std::int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

constexpr int kAppendIndex = 1 << 30; // clamped to end by Forest::reparent
constexpr double kFlashDuration = 1.6; // seconds the new-task path stays highlighted
constexpr double kSearchDebounce = 0.2; // seconds after typing settles before auto-panning
constexpr double kPanAnimDur = 0.28;    // seconds for the search / new-node camera glide

} // namespace

App::App(IPlatform& platform, Renderer& renderer, IClassifier& classifier,
         Config& cfg, Forest& forest, std::string tasksPath)
    : platform_(platform), renderer_(renderer), classifier_(classifier),
      cfg_(cfg), forest_(forest), tasksPath_(std::move(tasksPath)) {}

// ---- hotkey actions --------------------------------------------------------

void App::toggleOverlay() {
    if (mode_ == Mode::Full) {
        hide();
    } else {
        mode_ = Mode::Full;
        input_.clear();
        editingNode_ = 0;
        input_.setFocused(true);
        platform_.showOverlay();
    }
}

void App::showQuickAdd() {
    mode_ = Mode::QuickAdd;
    input_.clear();
    editingNode_ = 0;
    input_.setFocused(true);
    platform_.showOverlay();
}

void App::hide() {
    mode_ = Mode::Hidden;
    input_.setFocused(false);
    input_.clear();
    editingNode_ = 0;
    selected_ = 0;
    drag_.cancel();
    cancelPanAnim();
    exitSearch();
    platform_.hideOverlay();
}

// ---- input -----------------------------------------------------------------

void App::onChar(unsigned int codepoint) {
    if (mode_ == Mode::Hidden) return;
    if (searching_) { search_.onChar(codepoint); updateSearchMatches(); }
    else            input_.onChar(codepoint);
}

void App::onKey(int key, int action, int mods) {
    if (action == GLFW_RELEASE || mode_ == Mode::Hidden) return;

    if (key == GLFW_KEY_ESCAPE && drag_.active()) { drag_.cancel(); return; }
    if (key == GLFW_KEY_ESCAPE && editingNode_ != 0) { cancelEditing(); return; }
    // Esc clears a selection first (only when it isn't needed for the field): so a
    // selected node can be dismissed without also closing the overlay.
    if (key == GLFW_KEY_ESCAPE && selected_ != 0 && !searching_ && input_.text().empty()) {
        selected_ = 0;
        return;
    }
    // F2, or Enter on a selected node with an empty input bar, edits that node's text.
    if (!searching_ && editingNode_ == 0 && selected_ != 0 && forest_.exists(selected_)) {
        const bool enterEmpty = (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) &&
                                input_.text().empty();
        if (key == GLFW_KEY_F2 || enterEmpty) { startEditing(selected_); return; }
    }

    TextInput& field = searching_ ? search_ : input_;

    if (mods & GLFW_MOD_CONTROL) {
        if (key == GLFW_KEY_V) {
            if (const char* clip = glfwGetClipboardString(platform_.window())) {
                field.insert(clip);
                if (searching_) updateSearchMatches();
            }
            return;
        }
        if (key == GLFW_KEY_M) { moveOverlayToNextMonitor(); return; }        // next monitor
        if (key == GLFW_KEY_F && mode_ == Mode::Full) { toggleSearch(); return; } // find
        if (mode_ == Mode::Full && key == GLFW_KEY_Z) {   // undo / redo
            if (mods & GLFW_MOD_SHIFT) redo(); else undo();
            return;
        }
        if (mode_ == Mode::Full && key == GLFW_KEY_Y) { redo(); return; }
    }

    if (searching_) {
        switch (search_.onKey(key, mods)) {
            case TextInput::Action::Cancel: exitSearch(); break;
            case TextInput::Action::Submit: // Enter: jump to the nearest match now, keep searching
                focusNode_ = nearestSearchHit(lastWinW_, lastWinH_);
                searchPanDue_ = 0.0; // Enter pre-empts the pending debounced pan
                break;
            case TextInput::Action::None: updateSearchMatches(); break;
        }
        return;
    }

    // Delete / Backspace removes the selected subtree — but only when the input bar is
    // empty (otherwise those keys edit the text being typed). Undo-backed, so no confirm.
    if ((key == GLFW_KEY_DELETE || key == GLFW_KEY_BACKSPACE) &&
        selected_ != 0 && editingNode_ == 0 && input_.text().empty()) {
        deleteSelected();
        return;
    }

    switch (input_.onKey(key, mods)) {
        case TextInput::Action::Submit: commitInput(); break;
        case TextInput::Action::Cancel: hide(); break;
        case TextInput::Action::None:   break;
    }
}

void App::toggleSearch() {
    searching_ = !searching_;
    search_.clear();
    searchHits_.clear();
    searchPanDue_ = 0.0;
    search_.setFocused(searching_);
}

void App::exitSearch() {
    searching_ = false;
    search_.clear();
    searchHits_.clear();
    searchPanDue_ = 0.0;
}

void App::updateSearchMatches() {
    searchHits_.clear();
    searchPanDue_ = 0.0;
    std::string q = search_.text();
    for (char& c : q) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (q.empty()) return;
    for (const auto& [id, t] : forest_.nodes) {
        if (forest_.isInDoneSection(id)) continue; // only canvas nodes are on screen
        std::string lt = t.text;
        for (char& c : lt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lt.find(q) != std::string::npos) searchHits_.insert(id);
    }
    // Live-highlight is immediate; panning waits a beat so it doesn't chase every keystroke.
    if (!searchHits_.empty()) searchPanDue_ = glfwGetTime() + kSearchDebounce;
}

// The matching node whose centre is closest to the current viewport centre, so the
// auto-pan makes the least jarring camera move. 0 if there are no matches on screen.
TaskId App::nearestSearchHit(int winW, int winH) const {
    const float vcx = (winW * 0.5f - pan_.x) / zoom_;
    const float vcy = (winH * 0.5f - pan_.y) / zoom_;
    TaskId best = 0;
    float bestD2 = std::numeric_limits<float>::max();
    for (TaskId id : searchHits_) {
        auto it = rects_.find(id);
        if (it == rects_.end()) continue;
        const float dx = it->second.cx() - vcx, dy = it->second.cy() - vcy;
        const float d2 = dx * dx + dy * dy;
        if (d2 < bestD2) { bestD2 = d2; best = id; }
    }
    return best;
}

void App::onMouseButton(int button, int action, int mods) {
    if (mode_ != Mode::Full) return;

    // Clicking anywhere while editing commits the pending text edit first, then the click
    // is handled normally (select another node, pan, etc.).
    if (editingNode_ != 0 && action == GLFW_PRESS) commitEdit(trim(input_.text()));

    // Right-click a canvas node to cycle its status colour: default -> yellow
    // (in progress) -> orange (priority) -> default.
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        if (action == GLFW_PRESS && !pointInPanel(mouse_)) {
            const TaskId id = hitTest(worldMouse());
            if (Task* t = forest_.get(id)) { history_.snapshot(forest_); t->status = (t->status + 1) % 3; save(); }
        }
        return;
    }

    // Pan the canvas with middle-drag. (Alt+drag is intentionally NOT used — the window
    // manager grabs Alt+drag to move the whole window; left-drag on empty canvas pans.)
    if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        if (action == GLFW_PRESS) { cancelPanAnim(); panning_ = true; panGrab_ = mouse_; panOrigin_ = pan_; }
        else if (action == GLFW_RELEASE) panning_ = false;
        return;
    }
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;

    if (action == GLFW_PRESS) {
        // Autohide toggle button.
        if (donePanel_.visible && donePanel_.pinButton.contains(mouse_.x, mouse_.y)) {
            pinned_ = !pinned_;
            return;
        }
        // Double-click (same spot, within 400 ms).
        const double t = glfwGetTime();
        const bool dbl = (t - lastClickTime_ < 0.40) &&
                         std::fabs(mouse_.x - lastClickPos_.x) < 8.f &&
                         std::fabs(mouse_.y - lastClickPos_.y) < 8.f;
        lastClickTime_ = dbl ? 0.0 : t;
        lastClickPos_ = mouse_;
        if (dbl) { handleDoubleClick(); return; }
        // Single press: DONE panel toggles a row's children (screen space); on the
        // canvas, a node starts a drag, empty space starts a pan (world space).
        if (pointInPanel(mouse_)) {
            const TaskId id = hitTestDone(mouse_);
            if (id != 0) {
                if (doneExpanded_.count(id)) doneExpanded_.erase(id);
                else                         doneExpanded_.insert(id);
            }
        } else {
            const TaskId id = hitTest(worldMouse());
            if (id != 0) {
                auto it = rects_.find(id);
                if (it != rects_.end()) { cancelPanAnim(); drag_.begin(id, worldMouse(), it->second); }
            } else {
                cancelPanAnim();
                selected_ = 0;   // pressing empty canvas clears the selection
                panning_ = true; panGrab_ = mouse_; panOrigin_ = pan_; // drag empty bg to pan
            }
        }
    } else if (action == GLFW_RELEASE) {
        if (panning_) { panning_ = false; return; }
        if (drag_.active()) {
            const TaskId node = drag_.dragged();
            Forest before = forest_;   // snapshot only if the drop actually reparents
            if (drag_.drop(forest_)) { history_.record(std::move(before)); forceRelayout(); save(); }
            selected_ = node;          // clicking (or dragging) a node selects it
        }
    }
}

void App::onCursorPos(double x, double y) {
    mouse_ = {static_cast<float>(x), static_cast<float>(y)};
    if (panning_) {
        pan_ = {panOrigin_.x + (mouse_.x - panGrab_.x), panOrigin_.y + (mouse_.y - panGrab_.y)};
        return;
    }
    // Autohide reveal with hysteresis. Only within the overlay's own bounds: if the
    // cursor is reported past the right edge it's on a monitor to the right, not asking
    // for the panel — hide rather than pin (paired with onCursorEnter's leave handling).
    if (mode_ == Mode::Full && lastWinW_ > 0) {
        const float w = static_cast<float>(lastWinW_);
        if (mouse_.x > w) doneHover_ = false;                // on a monitor to the right
        else if (mouse_.x >= w * 0.85f) doneHover_ = true;   // within the right 15%
        else if (mouse_.x < w * 0.83f && !pointInPanel(mouse_))
            doneHover_ = false;                              // 17% out AND off the panel
    }
    if (drag_.active()) drag_.update(worldMouse(), forest_, rects_, params_);
}

void App::onCursorEnter(bool entered) {
    // The overlay only covers the primary monitor. When the cursor leaves it (e.g. moving
    // onto a monitor to the right), we stop getting move events, so the autohide reveal
    // would otherwise stay stuck open. Drop it on leave — PIN still overrides autohide.
    if (!entered) doneHover_ = false;
}

void App::onScroll(double dx, double dy) {
    if (mode_ != Mode::Full) return;
    // Over the DONE panel: scroll its list.
    if (donePanel_.visible && pointInPanel(mouse_)) {
        scrollY_ -= static_cast<float>(dy) * 48.f;
        scrollY_ = std::max(0.f, std::min(scrollY_, doneMaxScroll_));
        return;
    }
    // Canvas wheel behaviour is configurable (config [input] scroll_mode).
    if (cfg_.scrollMode == "off") return;

    GLFWwindow* w = platform_.window();
    const bool ctrl = glfwGetKey(w, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                      glfwGetKey(w, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    const bool shift = glfwGetKey(w, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                       glfwGetKey(w, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;

    // "zoom": wheel zooms. "pan": wheel pans, but Ctrl+wheel still zooms.
    const bool doZoom = (cfg_.scrollMode == "zoom") || (cfg_.scrollMode == "pan" && ctrl);
    if (doZoom) {
        if (dy == 0.0) return;
        cancelPanAnim();
        const float factor = (dy > 0.0) ? 1.1f : 1.f / 1.1f;
        const float nz = std::max(0.3f, std::min(3.0f, zoom_ * factor));
        const float wx = (mouse_.x - pan_.x) / zoom_; // keep the world point under the cursor fixed
        const float wy = (mouse_.y - pan_.y) / zoom_;
        pan_.x = mouse_.x - nz * wx;
        pan_.y = mouse_.y - nz * wy;
        zoom_ = nz;
        return;
    }

    if (cfg_.scrollMode == "pan") {
        cancelPanAnim();
        const float step = 48.f;
        pan_.x += static_cast<float>(dx) * step;
        if (shift) pan_.x += static_cast<float>(dy) * step;
        else       pan_.y += static_cast<float>(dy) * step;
    }
}

void App::moveOverlayToNextMonitor() {
    platform_.moveToNextMonitor();
    if (mode_ == Mode::Hidden) {
        mode_ = Mode::Full;
        input_.clear();
        input_.setFocused(true);
        platform_.showOverlay();
    }
}

// ---- task creation + classification ----------------------------------------

void App::commitInput() {
    if (editingNode_ != 0) { commitEdit(trim(input_.text())); return; }

    const std::string txt = trim(input_.text());
    if (txt.empty()) { if (mode_ == Mode::QuickAdd) hide(); return; }

    history_.snapshot(forest_);   // undo checkpoint before the add
    TaskId id;
    if (isDevTask(txt)) {
        // Dev fast path: TaskTree's own to-dos ("ttd> ...") park directly under the
        // "tasktree dev" node and skip LLM classification entirely.
        id = forest_.addTask(txt, ensureDevRoot(forest_, nowMs()), nowMs());
    } else {
        id = forest_.addTask(txt, kNoParent, nowMs());
        if (classifier_.enabled()) {
            classifier_.classify(txt, buildTreeOutline(id),
                                  [this, id](ClassifyResult r) { pushClassification(id, r); });
        }
    }

    input_.clear();
    forceRelayout();
    flashPath(id);   // briefly show where the new node landed (standalone -> just itself)
    focusNode_ = id; // pan the canvas to bring the new node into view
    save();
    if (mode_ == Mode::QuickAdd) hide();
}

void App::flashPath(TaskId leaf) {
    highlightSet_.clear();
    TaskId cur = leaf;
    for (std::size_t i = 0; cur != kNoParent && i <= forest_.size(); ++i) {
        const Task* t = forest_.get(cur);
        if (!t) break;
        highlightSet_.insert(cur);
        cur = t->parent;
    }
    highlightUntil_ = glfwGetTime() + kFlashDuration;
}

std::string App::buildTreeOutline(TaskId exclude) const {
    // One line per canvas node: "[id] parent=<pid|none>: text". Explicit parent ids
    // (not indentation) so the model reconstructs the tree unambiguously. Pre-order
    // over canvas roots (DONE roots live in a separate list, so they're excluded).
    std::string out;
    std::vector<TaskId> stack;
    for (auto it = forest_.roots.rbegin(); it != forest_.roots.rend(); ++it)
        stack.push_back(*it);
    while (!stack.empty()) {
        const TaskId id = stack.back();
        stack.pop_back();
        if (id == exclude) continue; // skip the just-created node
        const Task* t = forest_.get(id);
        if (!t) continue;
        out += "[" + std::to_string(id) + "] parent=";
        out += (t->parent == kNoParent) ? "none" : std::to_string(t->parent);
        out += ": " + t->text + "\n";
        for (auto cit = t->children.rbegin(); cit != t->children.rend(); ++cit)
            stack.push_back(*cit);
    }
    return out;
}

void App::pushClassification(TaskId newTask, ClassifyResult result) {
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending_.emplace_back(newTask, result);
    }
    platform_.wake(); // wake the (possibly blocked) main loop
}

void App::applyPendingClassifications() {
    std::vector<std::pair<TaskId, ClassifyResult>> local;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        local.swap(pending_);
    }
    if (local.empty()) return;
    Forest before = forest_;   // pre-batch undo checkpoint (recorded only if something moves)
    const bool L = llmlog::enabled();
    auto textOf = [&](TaskId t) {
        const Task* n = forest_.get(t);
        return n ? n->text : std::string("<gone>");
    };
    bool changed = false;
    TaskId flashLeaf = 0;
    for (const auto& [id, r] : local) {
        if (r.relation == Relation::Standalone || r.targetId == 0) continue; // classifier logged it
        if (!forest_.exists(id)) {
            if (L) llmlog::write("APPLY skipped: new task " + std::to_string(id) + " no longer exists");
            continue;
        }
        if (!forest_.exists(r.targetId)) {
            if (L) llmlog::write("APPLY rejected: target id " + std::to_string(r.targetId) +
                                 " does not exist (kept '" + textOf(id) + "' standalone)");
            continue;
        }
        if (forest_.isInDoneSection(r.targetId)) { // never attach to / resurrect DONE tasks
            if (L) llmlog::write("APPLY rejected: target id " + std::to_string(r.targetId) +
                                 " is in the DONE section (kept '" + textOf(id) + "' standalone)");
            continue;
        }
        const bool childOf = (r.relation == Relation::ChildOf);
        bool c = false;
        if (childOf) {
            c = forest_.reparent(id, r.targetId, kAppendIndex);
        } else {
            // parent_of: insert the new task as target's parent — it takes target's
            // current slot (under target's old parent) and target becomes its child.
            // This is how a node lands BETWEEN an existing task and its parent.
            const TaskId oldParent = forest_.get(r.targetId)->parent;
            c = forest_.reparent(id, oldParent, kAppendIndex) &&
                forest_.reparent(r.targetId, id, kAppendIndex);
        }
        if (c) {
            changed = true;
            flashLeaf = id;
            if (L) llmlog::write("APPLY: '" + textOf(id) + "' (id " + std::to_string(id) + ") " +
                                 (childOf ? "-> child of '" : "-> parent of '") + textOf(r.targetId) +
                                 "' (id " + std::to_string(r.targetId) + ")");
        } else if (L) {
            llmlog::write("APPLY rejected: reparent failed (cycle / invalid) new=" +
                          std::to_string(id) + " target=" + std::to_string(r.targetId));
        }
    }
    if (changed) {
        history_.record(std::move(before));
        forceRelayout(); save(); flashPath(flashLeaf); focusNode_ = flashLeaf;
    }
}

// ---- layout + render -------------------------------------------------------

void App::relayoutIfNeeded() {
    if (!needsRelayout_) return;
    renderer_.measureSizes(forest_, cfg_, sizes_);
    params_.defaultSize = {cfg_.maxNodeWidth * 0.6f, renderer_.fontSize() + 20.f};
    rects_ = computeLayout(forest_, sizes_, params_);
    needsRelayout_ = false;
}

DragVisual App::buildDragVisual() {
    DragVisual dv;
    if (!drag_.active() || !drag_.moved()) return dv;
    previewRects_ = drag_.previewLayout(forest_, rects_);
    dv.active = true;
    dv.dragged = drag_.dragged();
    dv.target = drag_.target();
    dv.validTarget = drag_.validTarget();
    dv.ghost = drag_.ghost();
    if (dv.validTarget && dv.target != 0) {
        dv.fromPoint = drag_.targetBottom();
        dv.toPoint = drag_.slotTop();
        dv.showPreviewEdge = true;
    }
    return dv;
}

void App::startPanTo(Vec2 target) {
    const float dx = target.x - pan_.x, dy = target.y - pan_.y;
    if (dx * dx + dy * dy < 1.f) { pan_ = target; panAnimActive_ = false; return; } // already there
    panFrom_ = pan_;
    panTo_ = target;
    panAnimStart_ = glfwGetTime();
    panAnimActive_ = true;
}

void App::drawScene(int winW, int winH, float dpr) {
    lastWinW_ = winW;
    lastWinH_ = winH;
    // Centre the forest across the full window (roots -> true top-centre). The DONE
    // panel is translucent and autohides, so it overlays rather than reserving space.
    const float cw = static_cast<float>(winW);
    if (params_.centerWidth != cw) { params_.centerWidth = cw; needsRelayout_ = true; }
    relayoutIfNeeded();

    // Debounced search auto-pan: once typing settles, bring the nearest match into view.
    if (searching_ && searchPanDue_ != 0.0 && glfwGetTime() >= searchPanDue_) {
        searchPanDue_ = 0.0;
        focusNode_ = nearestSearchHit(winW, winH);
    }

    // Pan to bring a just-added / just-reparented / searched node into view (centre-ish,
    // above the input bar). Done after relayout so its rect is current. Rather than
    // snapping, start a smooth glide — the search auto-pan and the new-node pan both
    // land here, so both animate.
    if (focusNode_ != 0) {
        auto it = rects_.find(focusNode_);
        if (it != rects_.end())
            startPanTo({winW * 0.5f - zoom_ * it->second.cx(),
                        winH * 0.4f - zoom_ * it->second.cy()});
        focusNode_ = 0;
    }

    // Advance the camera glide (ease-out cubic). The loop wakes at ~60fps while it runs
    // (desiredTimeout), then drops back to idle once pan_ reaches the target.
    if (panAnimActive_) {
        const double t = (glfwGetTime() - panAnimStart_) / kPanAnimDur;
        if (t >= 1.0) { pan_ = panTo_; panAnimActive_ = false; }
        else {
            const float e = static_cast<float>(1.0 - std::pow(1.0 - t, 3.0));
            pan_.x = panFrom_.x + (panTo_.x - panFrom_.x) * e;
            pan_.y = panFrom_.y + (panTo_.y - panFrom_.y) * e;
        }
    }

    layoutDonePanel(winW, winH);

    renderer_.beginFrame(winW, winH, dpr);
    if (mode_ == Mode::Full) {
        renderer_.drawScrim(static_cast<float>(winW), static_cast<float>(winH), cfg_);
        DragVisual dv = buildDragVisual();
        const auto& drawRects = dv.active ? previewRects_ : rects_;
        const double now = glfwGetTime();
        float hi = 0.f;
        if (!highlightSet_.empty()) {
            if (now < highlightUntil_) hi = static_cast<float>((highlightUntil_ - now) / kFlashDuration);
            else highlightSet_.clear();
        }
        renderer_.drawTree(forest_, drawRects, cfg_, dv, pan_, zoom_, highlightSet_, hi, searchHits_,
                           selected_);
        if (donePanel_.visible)
            renderer_.drawDonePanel(donePanel_, forest_, doneRows_, cfg_);
        if (searching_)
            renderer_.drawSearchBar(winW, search_.text(), search_.caret(), caretOn(),
                                    (int)searchHits_.size(), cfg_);
        else
            renderer_.drawInput(winW, winH, input_.text(), input_.caret(), caretOn(), cfg_, false,
                                editingNode_ != 0);
    } else if (mode_ == Mode::QuickAdd) {
        renderer_.drawInput(winW, winH, input_.text(), input_.caret(), caretOn(), cfg_, true);
    }
    renderer_.endFrame();
}

void App::layoutDonePanel(int winW, int winH) {
    DonePanelLayout L;
    L.pinned = pinned_;
    L.visible = (mode_ == Mode::Full) && (pinned_ || doneHover_);
    // Floating rounded card: inset from the screen edges so it reads as a raised
    // surface (with a drop shadow), consistent with the input/search boxes.
    const float margin = 10.f;
    const float pw = std::max(winW * 0.17f, 312.f);
    L.panel = {winW - pw - margin, margin, pw, winH - 2 * margin};
    const float titleH = 46.f;
    L.titleBar = {L.panel.x, L.panel.y, pw, titleH};
    const float btnW = 70.f, btnH = 24.f;
    L.pinButton = {L.panel.right() - btnW - 14.f, L.panel.y + (titleH - btnH) * 0.5f, btnW, btnH};
    L.contentClipTop = L.panel.y + titleH;
    L.contentClipBottom = L.panel.bottom() - 8.f;
    for (const auto& [id, t] : forest_.nodes)
        if (t.done) ++L.itemCount;
    donePanel_ = L;

    // Measure card heights, clamp scroll, then position (screen coords, scrolled).
    // Flatten the expanded DONE tree in display order (pre-order) with depth. Top-level
    // roots are shown latest-completed first (display-only sort; the persisted doneRoots
    // order is left untouched). Primary key is doneAt (newest first); items with an
    // unknown doneAt (0, completed before the field existed) fall back to completion
    // order — markDone appends, so reversing doneRoots puts the most recent first, and a
    // stable sort keeps that fallback order within the undated group.
    std::vector<TaskId> order(forest_.doneRoots.rbegin(), forest_.doneRoots.rend());
    std::stable_sort(order.begin(), order.end(), [&](TaskId a, TaskId b) {
        const Task* ta = forest_.get(a);
        const Task* tb = forest_.get(b);
        return (ta ? ta->doneAt : 0) > (tb ? tb->doneAt : 0);
    });
    std::vector<std::pair<TaskId, int>> flat;
    {
        std::vector<std::pair<TaskId, int>> stack;
        for (auto it = order.rbegin(); it != order.rend(); ++it)
            stack.emplace_back(*it, 0);
        while (!stack.empty()) {
            const auto [id, depth] = stack.back();
            stack.pop_back();
            const Task* t = forest_.get(id);
            if (!t) continue;
            flat.emplace_back(id, depth);
            if (doneExpanded_.count(id))
                for (auto cit = t->children.rbegin(); cit != t->children.rend(); ++cit)
                    stack.emplace_back(*cit, depth + 1);
        }
    }

    const float pad = 14.f, gap = 9.f, indent = 18.f;
    const float glyphGutter = 18.f;   // space before text for the chevron/check
    auto rowX = [&](int depth) { return L.panel.x + pad + depth * indent; };
    auto textW = [&](int depth) {
        return std::max(30.f, L.panel.right() - pad - (rowX(depth) + glyphGutter));
    };

    // Measure heights (roomier vertical padding per row -> calmer, more modern spacing).
    std::vector<float> heights;
    heights.reserve(flat.size());
    float totalH = 0.f;
    for (const auto& [id, depth] : flat) {
        const Task* t = forest_.get(id);
        float h = renderer_.measureTextHeight(t ? t->text : std::string{}, textW(depth)) + 22.f;
        h = std::max(h, 40.f);
        heights.push_back(h);
        totalH += h + gap;
    }
    const float visibleH = L.contentClipBottom - L.contentClipTop;
    doneMaxScroll_ = std::max(0.f, totalH - visibleH);
    scrollY_ = std::max(0.f, std::min(scrollY_, doneMaxScroll_));

    // Position rows.
    const bool canHover = donePanel_.visible && pointInPanel(mouse_) &&
                          mouse_.y >= L.contentClipTop && mouse_.y <= L.contentClipBottom;
    doneRows_.clear();
    doneRows_.reserve(flat.size());
    float y = L.contentClipTop + 6.f - scrollY_;
    for (std::size_t i = 0; i < flat.size(); ++i) {
        const TaskId id = flat[i].first;
        const int depth = flat[i].second;
        const Task* t = forest_.get(id);
        DoneRow row;
        row.id = id;
        row.depth = depth;
        row.hasChildren = t && !t->children.empty();
        row.expanded = doneExpanded_.count(id) != 0;
        row.rect = Rect{rowX(depth), y, L.panel.right() - pad - rowX(depth), heights[i]};
        row.hovered = canHover && mouse_.y >= y && mouse_.y < y + heights[i];
        doneRows_.push_back(row);
        y += heights[i] + gap;
    }
}

bool App::pointInPanel(Vec2 p) const {
    return donePanel_.visible && donePanel_.panel.contains(p.x, p.y);
}

TaskId App::hitTestDone(Vec2 p) const {
    if (!pointInPanel(p)) return 0;
    if (p.y < donePanel_.contentClipTop || p.y > donePanel_.contentClipBottom) return 0;
    for (const DoneRow& row : doneRows_)
        if (row.rect.contains(p.x, p.y)) return row.id;
    return 0;
}

void App::handleDoubleClick() {
    if (pointInPanel(mouse_)) {
        const TaskId id = hitTestDone(mouse_);
        if (id != 0) {
            Forest before = forest_;
            if (forest_.restoreFromDone(id)) {
                history_.record(std::move(before));
                doneExpanded_.erase(id);
                forceRelayout();
                save();
            }
        }
    } else {
        const TaskId id = hitTest(worldMouse());
        if (id != 0) {
            if (drag_.active()) drag_.cancel();
            Forest before = forest_;
            if (forest_.markDone(id)) {
                history_.record(std::move(before));
                if (Task* t = forest_.get(id)) t->doneAt = nowMs();  // record done date
                forceRelayout();
                save();
            }
        } else {
            cancelPanAnim();
            pan_ = {0.f, 0.f}; // double-click empty canvas recenters + resets zoom
            zoom_ = 1.f;
        }
    }
}

TaskId App::hitTest(Vec2 p) const {
    for (const auto& [id, r] : rects_)
        if (pointInRoundedRect(r, cfg_.cornerRadius, p.x, p.y)) return id;
    return 0;
}

bool App::caretOn() const {
    return mode_ != Mode::Hidden && std::fmod(glfwGetTime(), 1.0) < 0.5;
}

double App::desiredTimeout() const {
    if (drag_.active()) return 0.0;        // poll for smooth drag
    if (panAnimActive_) return 1.0 / 60.0; // ~60fps while the camera glides to a node
    if (searching_ && searchPanDue_ != 0.0) // wake exactly when the debounced pan is due
        return std::max(0.0, searchPanDue_ - glfwGetTime());
    if (!highlightSet_.empty() && glfwGetTime() < highlightUntil_) return 0.03; // animate flash
    if (mode_ != Mode::Hidden) return 0.5; // caret blink while visible
    return -1.0;                           // block until an event/hotkey
}

void App::save() { store::save(forest_, tasksPath_); }

void App::undo() {
    if (!history_.undo(forest_)) return;
    afterHistoryChange();
}

void App::redo() {
    if (!history_.redo(forest_)) return;
    afterHistoryChange();
}

void App::startEditing(TaskId id) {
    const Task* t = forest_.get(id);
    if (!t) return;
    editingNode_ = id;
    selected_ = id;
    input_.setText(t->text);   // seed with current text, caret at end
    input_.setFocused(true);
}

void App::commitEdit(const std::string& txt) {
    const TaskId id = editingNode_;
    editingNode_ = 0;
    input_.clear();
    Task* t = forest_.get(id);
    if (!t) return;
    if (txt.empty() || txt == t->text) return;  // empty or unchanged -> leave the node as-is
    history_.snapshot(forest_);                  // undo checkpoint before the text change
    t->text = txt;
    forceRelayout();  // text changed -> re-measure box + relayout
    save();
}

void App::cancelEditing() {
    editingNode_ = 0;
    input_.clear();  // node text untouched; selection stays
}

void App::deleteSelected() {
    if (selected_ == 0 || !forest_.exists(selected_)) { selected_ = 0; return; }
    if (drag_.active()) drag_.cancel();
    history_.snapshot(forest_);          // undo checkpoint before removal
    const TaskId victim = selected_;
    selected_ = 0;
    doneExpanded_.erase(victim);
    forest_.removeSubtree(victim);       // removes the node and its whole subtree
    forceRelayout();
    if (searching_) updateSearchMatches();
    save();
}

void App::afterHistoryChange() {
    drag_.cancel();
    focusNode_ = 0;
    if (!forest_.exists(selected_)) selected_ = 0;  // don't keep a ring on a vanished node
    forceRelayout();
    if (searching_) updateSearchMatches();  // hit set may reference now-removed/added nodes
    save();
}

} // namespace tt
