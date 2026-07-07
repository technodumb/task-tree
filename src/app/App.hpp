#pragma once
// Application state machine + orchestration. Owns the interaction state (mode,
// text input, drag) and drives model -> layout -> render. GLFW input is forwarded
// in via the on*() methods (main installs the trampolines).

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "app/Config.hpp"
#include "layout/Geometry.hpp"
#include "layout/TidyLayout.hpp"
#include "llm/IClassifier.hpp"
#include "model/Task.hpp"
#include "platform/IPlatform.hpp"
#include "render/Renderer.hpp"
#include "ui/DragController.hpp"
#include "ui/TextInput.hpp"

namespace tt {

class App {
public:
    App(IPlatform& platform, Renderer& renderer, IClassifier& classifier,
        Config& cfg, Forest& forest, std::string tasksPath);

    // Hotkey actions.
    void toggleOverlay();   // show/hide the full overlay
    void showQuickAdd();    // show just the input box
    void hide();

    // GLFW input (forwarded by main).
    void onChar(unsigned int codepoint);
    void onKey(int key, int action, int mods);
    void onMouseButton(int button, int action, int mods);
    void onCursorPos(double x, double y);
    void onScroll(double dx, double dy);

    // Called from a classifier worker thread; enqueues a result and wakes the loop.
    void pushClassification(TaskId newTask, ClassifyResult result);

    // Main-loop hooks.
    void applyPendingClassifications();
    void drawScene(int winW, int winH, float devicePixelRatio);
    bool visible() const { return mode_ != Mode::Hidden; }
    double desiredTimeout() const;   // <0 => block indefinitely

private:
    enum class Mode { Hidden, Full, QuickAdd };

    void relayoutIfNeeded();
    void forceRelayout() { needsRelayout_ = true; }
    void commitInput();
    void flashPath(TaskId leaf);   // briefly highlight root -> leaf after an add/classify
    void save();
    TaskId hitTest(Vec2 p) const;
    bool caretOn() const;
    DragVisual buildDragVisual();

    // Screen cursor -> canvas/world coordinates (accounts for the pan offset).
    Vec2 worldMouse() const { return {mouse_.x - pan_.x, mouse_.y - pan_.y}; }

    // DONE panel helpers.
    void layoutDonePanel(int winW, int winH);
    bool pointInPanel(Vec2 p) const;
    TaskId hitTestDone(Vec2 p) const;
    void handleDoubleClick();

    IPlatform&    platform_;
    Renderer&     renderer_;
    IClassifier&  classifier_;
    Config&       cfg_;
    Forest&       forest_;
    std::string   tasksPath_;

    Mode mode_ = Mode::Hidden;
    TextInput input_;
    DragController drag_;
    LayoutParams params_;

    std::unordered_map<TaskId, Size> sizes_;
    std::unordered_map<TaskId, Rect> rects_;        // current layout
    std::unordered_map<TaskId, Rect> previewRects_; // layout during a drag
    Vec2 mouse_;
    bool needsRelayout_ = true;

    // Transient "path to the new task" flash (root -> leaf), fades over ~1.6 s.
    std::unordered_set<TaskId> highlightSet_;
    double highlightUntil_ = 0.0;

    // Canvas panning (view offset applied to the tree; screen-space UI is unaffected).
    Vec2 pan_;
    bool panning_ = false;
    Vec2 panGrab_;     // screen cursor at pan start
    Vec2 panOrigin_;   // pan_ at pan start

    // DONE side panel state.
    bool  pinned_ = false;           // autohide off when true
    bool  doneHover_ = false;        // mouse in the reveal/keep zone (autohide)
    float scrollY_ = 0.f;
    float doneMaxScroll_ = 0.f;
    int   lastWinW_ = 0, lastWinH_ = 0;
    DonePanelLayout donePanel_;
    std::vector<DoneRow> doneRows_;               // visible, indented DONE rows
    std::unordered_set<TaskId> doneExpanded_;     // which DONE nodes show their children
    double lastClickTime_ = 0.0;
    Vec2   lastClickPos_;

    std::mutex pendingMutex_;
    std::vector<std::pair<TaskId, ClassifyResult>> pending_;
};

} // namespace tt
