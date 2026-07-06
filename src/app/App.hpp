#pragma once
// Application state machine + orchestration. Owns the interaction state (mode,
// text input, drag) and drives model -> layout -> render. GLFW input is forwarded
// in via the on*() methods (main installs the trampolines).

#include <mutex>
#include <string>
#include <unordered_map>
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
    void save();
    TaskId hitTest(Vec2 p) const;
    bool caretOn() const;
    DragVisual buildDragVisual();

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

    std::mutex pendingMutex_;
    std::vector<std::pair<TaskId, ClassifyResult>> pending_;
};

} // namespace tt
