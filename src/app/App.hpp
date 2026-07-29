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
#include "app/Palette.hpp"
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
    void deleteSelected();      // Delete/Backspace: remove the selected subtree (undoable)
    // Ctrl+click reparent: select a node, then Ctrl+click another to move the selection
    // (and its subtree) in as that node's last child. Drag-free alternative to dragging.
    void   reparentSelected(TaskId newParent);   // undoable move; no-op if invalid
    TaskId reparentTargetAt(Vec2 world) const;   // valid target under `world` (0 = none)
    void   updateReparentCue(bool ctrlHeld);     // refresh the green target ring
    bool   ctrlHeld() const;                     // either Ctrl key down right now
    // In-place text editing: the input bar is reused to edit an existing node's text.
    // editingNode_ != 0 routes a commit to update that node instead of adding a task.
    void startEditing(TaskId id);          // seed the input bar with the node's text
    void commitEdit(const std::string& txt); // apply the edit (or cancel if empty/unchanged)
    void cancelEditing();                  // Esc: abandon the edit, leave the node as-is
    TaskId hitTest(Vec2 p) const;
    TaskId collapseHandleHit(Vec2 p) const;  // node whose collapse handle contains p (0 = none)
    void   toggleCollapse(TaskId id);        // flip a node's collapsed flag + relayout
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

    // Command palette: the input bar itself, re-purposed by a leading ? / : / > (grammar +
    // match ranking in app/Palette.hpp). Ctrl+F is just "prefix the bar with ?" — there is
    // no second field. Highlights every match on canvas and previews the pending target.
    palette::Mode pmode_ = palette::Mode::Add;  // what the bar is doing (prefix consumed)
    palette::Command cmd_;                // resolved action for (pmode_, bar text)
    std::vector<TaskId> candidates_;      // ranked matches for a text-picking command
    std::vector<palette::Mode> menuItems_;   // rows of the '/' mode menu
    std::size_t candidateIdx_ = 0;        // which row ↑/↓ has landed on (nodes or modes)
    std::string lastQuery_;               // query the cursor above belongs to
    std::unordered_set<TaskId> searchHits_;  // every match (amber rings)
    TaskId previewSelect_ = 0;            // ring previewing what Enter would select
    double searchPanDue_ = 0.0;           // debounced jump to the active candidate (0 = none)
    void   updatePalette();               // re-resolve the bar; refresh candidates + previews
    void   clearPalette();                // empty the bar, back to plain add mode
    void   setPaletteMode(palette::Mode m);  // switch mode, clearing the argument
    bool   tryEnterMode(unsigned int codepoint);  // a prefix typed into an empty bar
    void   enterFindMode();               // Ctrl+F: the same as typing '?'
    void   movePaletteCursor(int delta);  // ↑/↓ through the candidate list
    void   runCommand();                  // Enter on a palette command
    TaskId activeCandidate() const;       // candidates_[candidateIdx_] (0 if none)
    TaskId commandTarget() const;         // the node the current command points at (0 = none)
    bool   canReparent(TaskId child, TaskId newParent) const;  // shared move validity
    void   revealNode(TaskId id);         // un-collapse every ancestor so `id` is on canvas
    bool   nodeOnScreen(TaskId id) const; // its rect is at least partly inside the window
    Color  paletteTint() const;           // per-mode accent (matches the canvas rings)
    std::string paletteStatus() const;    // "7 matches" / "node 12" / "no match" …
    void   drawPaletteDropUp(const Rect& inputBox);  // candidate list above the bar

    std::unordered_map<TaskId, Size> sizes_;
    std::unordered_map<TaskId, Rect> rects_;        // current layout
    std::unordered_map<TaskId, Rect> previewRects_; // layout during a drag
    Vec2 mouse_;
    bool needsRelayout_ = true;
    TaskId selected_ = 0;   // click-selected canvas node (0 = none); drawn with a ring
    TaskId editingNode_ = 0; // node whose text the input bar is editing (0 = adding, not editing)
    TaskId reparentTarget_ = 0; // node a Ctrl+click would reparent the selection under (0 = none)

    // "Hold this node still" anchor, consumed by the next relayout in drawScene: pan_ is
    // shifted so `anchorNode_` lands back on `anchorScreen_`. A Ctrl+click reparent re-packs
    // the tree, which would otherwise slide the clicked parent out from under the cursor.
    TaskId anchorNode_ = 0;
    Vec2   anchorScreen_;

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
