#include "app/App.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <GLFW/glfw3.h>

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
        input_.setFocused(true);
        platform_.showOverlay();
    }
}

void App::showQuickAdd() {
    mode_ = Mode::QuickAdd;
    input_.clear();
    input_.setFocused(true);
    platform_.showOverlay();
}

void App::hide() {
    mode_ = Mode::Hidden;
    input_.setFocused(false);
    input_.clear();
    drag_.cancel();
    platform_.hideOverlay();
}

// ---- input -----------------------------------------------------------------

void App::onChar(unsigned int codepoint) {
    if (mode_ == Mode::Hidden) return;
    input_.onChar(codepoint);
}

void App::onKey(int key, int action, int mods) {
    if (action == GLFW_RELEASE || mode_ == Mode::Hidden) return;

    if (key == GLFW_KEY_ESCAPE && drag_.active()) { drag_.cancel(); return; }

    if ((mods & GLFW_MOD_CONTROL) && key == GLFW_KEY_V) {
        if (const char* clip = glfwGetClipboardString(platform_.window())) input_.insert(clip);
        return;
    }

    switch (input_.onKey(key, mods)) {
        case TextInput::Action::Submit: commitInput(); break;
        case TextInput::Action::Cancel: hide(); break;
        case TextInput::Action::None:   break;
    }
}

void App::onMouseButton(int button, int action, int mods) {
    if (mode_ != Mode::Full) return;

    // Right-click a canvas node to cycle its status colour: default -> yellow
    // (in progress) -> orange (priority) -> default.
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        if (action == GLFW_PRESS && !pointInPanel(mouse_)) {
            const TaskId id = hitTest(worldMouse());
            if (Task* t = forest_.get(id)) { t->status = (t->status + 1) % 3; save(); }
        }
        return;
    }

    // Pan the canvas: middle-drag, or Alt + left-drag.
    if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        if (action == GLFW_PRESS) { panning_ = true; panGrab_ = mouse_; panOrigin_ = pan_; }
        else if (action == GLFW_RELEASE) panning_ = false;
        return;
    }
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;

    if (action == GLFW_PRESS) {
        if (mods & GLFW_MOD_ALT) {
            panning_ = true; panGrab_ = mouse_; panOrigin_ = pan_;
            return;
        }
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
        // Single press: DONE panel toggles a row's children (screen space); the canvas
        // starts a drag (world space).
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
                if (it != rects_.end()) drag_.begin(id, worldMouse(), it->second);
            }
        }
    } else if (action == GLFW_RELEASE) {
        if (panning_) { panning_ = false; return; }
        if (drag_.active() && drag_.drop(forest_)) { forceRelayout(); save(); }
    }
}

void App::onCursorPos(double x, double y) {
    mouse_ = {static_cast<float>(x), static_cast<float>(y)};
    if (panning_) {
        pan_ = {panOrigin_.x + (mouse_.x - panGrab_.x), panOrigin_.y + (mouse_.y - panGrab_.y)};
        return;
    }
    // Autohide reveal with hysteresis.
    if (mode_ == Mode::Full && lastWinW_ > 0) {
        const float w = static_cast<float>(lastWinW_);
        if (mouse_.x >= w * 0.85f) doneHover_ = true;        // within the right 15%
        else if (mouse_.x < w * 0.83f) doneHover_ = false;   // 17% out from the right
    }
    if (drag_.active()) drag_.update(worldMouse(), forest_, rects_, params_);
}

void App::onScroll(double dx, double dy) {
    if (mode_ != Mode::Full) return;
    // Over the DONE panel: scroll its list.
    if (donePanel_.visible && pointInPanel(mouse_)) {
        scrollY_ -= static_cast<float>(dy) * 48.f;
        scrollY_ = std::max(0.f, std::min(scrollY_, doneMaxScroll_));
        return;
    }
    // Over the canvas: pan. Vertical wheel pans vertically; Shift+wheel (or a
    // horizontal wheel) pans horizontally.
    const bool shift = glfwGetKey(platform_.window(), GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                       glfwGetKey(platform_.window(), GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
    const float step = 48.f;
    pan_.x += static_cast<float>(dx) * step;
    if (shift) pan_.x += static_cast<float>(dy) * step;
    else       pan_.y += static_cast<float>(dy) * step;
}

// ---- task creation + classification ----------------------------------------

void App::commitInput() {
    const std::string txt = trim(input_.text());
    if (txt.empty()) { if (mode_ == Mode::QuickAdd) hide(); return; }

    const TaskId id = forest_.addTask(txt, kNoParent, nowMs());

    if (classifier_.enabled()) {
        std::vector<std::pair<TaskId, std::string>> existing;
        existing.reserve(forest_.nodes.size());
        for (const auto& [eid, t] : forest_.nodes)
            if (eid != id) existing.emplace_back(eid, t.text);
        classifier_.classify(txt, std::move(existing),
                              [this, id](ClassifyResult r) { pushClassification(id, r); });
    }

    input_.clear();
    forceRelayout();
    save();
    if (mode_ == Mode::QuickAdd) hide();
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
    bool changed = false;
    for (const auto& [id, r] : local) {
        if (!forest_.exists(id) || r.relation == Relation::Standalone ||
            r.targetId == 0 || !forest_.exists(r.targetId))
            continue;
        if (r.relation == Relation::ChildOf)
            changed |= forest_.reparent(id, r.targetId, kAppendIndex);
        else // ParentOf: the existing task becomes a child of the new one
            changed |= forest_.reparent(r.targetId, id, kAppendIndex);
    }
    if (changed) { forceRelayout(); save(); }
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

void App::drawScene(int winW, int winH, float dpr) {
    lastWinW_ = winW;
    lastWinH_ = winH;
    // Centre the forest across the full window (roots -> true top-centre). The DONE
    // panel is translucent and autohides, so it overlays rather than reserving space.
    const float cw = static_cast<float>(winW);
    if (params_.centerWidth != cw) { params_.centerWidth = cw; needsRelayout_ = true; }
    relayoutIfNeeded();
    layoutDonePanel(winW, winH);

    renderer_.beginFrame(winW, winH, dpr);
    if (mode_ == Mode::Full) {
        renderer_.drawScrim(static_cast<float>(winW), static_cast<float>(winH), cfg_);
        DragVisual dv = buildDragVisual();
        const auto& drawRects = dv.active ? previewRects_ : rects_;
        renderer_.drawTree(forest_, drawRects, cfg_, dv, pan_);
        if (donePanel_.visible)
            renderer_.drawDonePanel(donePanel_, forest_, doneRows_, cfg_);
        renderer_.drawInput(winW, winH, input_.text(), input_.caret(), caretOn(), cfg_, false);
    } else if (mode_ == Mode::QuickAdd) {
        renderer_.drawInput(winW, winH, input_.text(), input_.caret(), caretOn(), cfg_, true);
    }
    renderer_.endFrame();
}

void App::layoutDonePanel(int winW, int winH) {
    DonePanelLayout L;
    L.pinned = pinned_;
    L.visible = (mode_ == Mode::Full) && (pinned_ || doneHover_);
    const float pw = winW * 0.15f;
    L.panel = {winW - pw, 0.f, pw, static_cast<float>(winH)};
    const float titleH = 50.f;
    L.titleBar = {L.panel.x, 0.f, pw, titleH};
    const float btnW = 74.f, btnH = 26.f;
    L.pinButton = {L.panel.right() - btnW - 12.f, (titleH - btnH) * 0.5f, btnW, btnH};
    L.contentClipTop = titleH + 6.f;
    L.contentClipBottom = static_cast<float>(winH) - 8.f;
    donePanel_ = L;

    // Measure card heights, clamp scroll, then position (screen coords, scrolled).
    // Flatten the expanded DONE tree in display order (pre-order) with depth.
    std::vector<std::pair<TaskId, int>> flat;
    {
        std::vector<std::pair<TaskId, int>> stack;
        for (auto it = forest_.doneRoots.rbegin(); it != forest_.doneRoots.rend(); ++it)
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

    const float pad = 12.f, gap = 8.f, indent = 16.f;
    auto rowX = [&](int depth) { return L.panel.x + pad + depth * indent; };
    auto textW = [&](int depth) { return std::max(30.f, L.panel.right() - 8.f - (rowX(depth) + 16.f)); };

    // Measure heights.
    std::vector<float> heights;
    heights.reserve(flat.size());
    float totalH = 0.f;
    for (const auto& [id, depth] : flat) {
        const Task* t = forest_.get(id);
        float h = renderer_.measureTextHeight(t ? t->text : std::string{}, textW(depth)) + 12.f;
        h = std::max(h, 28.f);
        heights.push_back(h);
        totalH += h + gap;
    }
    const float visibleH = L.contentClipBottom - L.contentClipTop;
    doneMaxScroll_ = std::max(0.f, totalH - visibleH);
    scrollY_ = std::max(0.f, std::min(scrollY_, doneMaxScroll_));

    // Position rows.
    doneRows_.clear();
    doneRows_.reserve(flat.size());
    float y = L.contentClipTop - scrollY_;
    for (std::size_t i = 0; i < flat.size(); ++i) {
        const TaskId id = flat[i].first;
        const int depth = flat[i].second;
        const Task* t = forest_.get(id);
        DoneRow row;
        row.id = id;
        row.depth = depth;
        row.hasChildren = t && !t->children.empty();
        row.expanded = doneExpanded_.count(id) != 0;
        row.rect = Rect{rowX(depth), y, L.panel.right() - 8.f - rowX(depth), heights[i]};
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
        if (id != 0 && forest_.restoreFromDone(id)) {
            doneExpanded_.erase(id);
            forceRelayout();
            save();
        }
    } else {
        const TaskId id = hitTest(worldMouse());
        if (id != 0) {
            if (drag_.active()) drag_.cancel();
            if (forest_.markDone(id)) { forceRelayout(); save(); }
        } else {
            pan_ = {0.f, 0.f}; // double-click empty canvas recenters the view
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
    if (mode_ != Mode::Hidden) return 0.5; // caret blink while visible
    return -1.0;                           // block until an event/hotkey
}

void App::save() { store::save(forest_, tasksPath_); }

} // namespace tt
