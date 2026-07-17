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
#include "model/History.hpp"
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
    void onCursorEnter(bool entered);
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
    // Current canvas tree (excluding DONE and `exclude`) as an indented "[id] text"
    // outline for the classifier.
    std::string buildTreeOutline(TaskId exclude) const;
    void save();
    void undo();   // Ctrl+Z: restore the previous forest state
    void redo();   // Ctrl+Shift+Z / Ctrl+Y: reapply an undone state
    void afterHistoryChange();  // shared relayout/cleanup after an undo or redo
    TaskId hitTest(Vec2 p) const;
    bool caretOn() const;
    DragVisual buildDragVisual();

    // Screen cursor -> canvas/world coordinates (accounts for pan + zoom).
    Vec2 worldMouse() const {
        return {(mouse_.x - pan_.x) / zoom_, (mouse_.y - pan_.y) / zoom_};
    }
    void moveOverlayToNextMonitor();

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
    History history_;   // undo/redo snapshots of forest_

    // Search (Ctrl+F in the full overlay): live-highlight matching canvas nodes.
    bool searching_ = false;
    TextInput search_;
    std::unordered_set<TaskId> searchHits_;
    void toggleSearch();
    void exitSearch();
    void updateSearchMatches();
    TaskId nearestSearchHit(int winW, int winH) const; // match closest to the view centre
    double searchPanDue_ = 0.0; // debounce: auto-pan to nearest match at this time (0 = none)

    std::unordered_map<TaskId, Size> sizes_;
    std::unordered_map<TaskId, Rect> rects_;        // current layout
    std::unordered_map<TaskId, Rect> previewRects_; // layout during a drag
    Vec2 mouse_;
    bool needsRelayout_ = true;
    TaskId selected_ = 0;   // click-selected canvas node (0 = none); drawn with a ring

    // Transient "path to the new task" flash (root -> leaf), fades over ~1.6 s.
    std::unordered_set<TaskId> highlightSet_;
    double highlightUntil_ = 0.0;

    // Canvas view transform (screen = pan + zoom * world). Screen-space UI is unaffected.
    Vec2  pan_;
    float zoom_ = 1.f;
    TaskId focusNode_ = 0;   // centre this node on the next frame (0 = none); glides there

    // Smooth camera glide shared by the search auto-pan and the new-node pan: pan_ eases
    // from panFrom_ to panTo_ over kPanAnimDur (ease-out cubic); the loop wakes ~60fps
    // meanwhile. Any manual pan/zoom pre-empts it via cancelPanAnim().
    void  startPanTo(Vec2 target);        // begin a glide (snaps instead if already ~there)
    void  cancelPanAnim() { panAnimActive_ = false; }
    bool  panAnimActive_ = false;
    Vec2  panFrom_, panTo_;
    double panAnimStart_ = 0.0;

    bool  panning_ = false;
    Vec2  panGrab_;     // screen cursor at pan start
    Vec2  panOrigin_;   // pan_ at pan start

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
