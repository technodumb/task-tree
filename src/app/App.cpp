#include "app/App.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

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

void App::onMouseButton(int button, int action, int /*mods*/) {
    if (mode_ != Mode::Full || button != GLFW_MOUSE_BUTTON_LEFT) return;
    if (action == GLFW_PRESS) {
        const TaskId id = hitTest(mouse_);
        if (id != 0) {
            auto it = rects_.find(id);
            if (it != rects_.end()) drag_.begin(id, mouse_, it->second);
        }
    } else if (action == GLFW_RELEASE) {
        if (drag_.active() && drag_.drop(forest_)) { forceRelayout(); save(); }
    }
}

void App::onCursorPos(double x, double y) {
    mouse_ = {static_cast<float>(x), static_cast<float>(y)};
    if (drag_.active()) drag_.update(mouse_, forest_, rects_, params_);
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
    if (!drag_.active()) return dv;
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
    // Centre the forest horizontally within the window (roots -> top-centre).
    if (params_.centerWidth != static_cast<float>(winW)) {
        params_.centerWidth = static_cast<float>(winW);
        needsRelayout_ = true;
    }
    relayoutIfNeeded();
    renderer_.beginFrame(winW, winH, dpr);
    if (mode_ == Mode::Full) {
        renderer_.drawScrim(static_cast<float>(winW), static_cast<float>(winH), cfg_);
        DragVisual dv = buildDragVisual();
        const auto& drawRects = dv.active ? previewRects_ : rects_;
        renderer_.drawTree(forest_, drawRects, cfg_, dv);
        renderer_.drawInput(winW, winH, input_.text(), input_.caret(), caretOn(), cfg_, false);
    } else if (mode_ == Mode::QuickAdd) {
        renderer_.drawInput(winW, winH, input_.text(), input_.caret(), caretOn(), cfg_, true);
    }
    renderer_.endFrame();
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
