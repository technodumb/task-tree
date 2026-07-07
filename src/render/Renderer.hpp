#pragma once
// NanoVG-based scene renderer. Owns no window/GL-context lifetime (main creates the
// NVGcontext); it only issues draw calls and measures node sizes. All coordinates
// are logical units; HiDPI is handled by the devicePixelRatio passed to beginFrame.

#include <string>
#include <unordered_map>
#include <vector>

#include "app/Config.hpp"
#include "layout/Geometry.hpp"
#include "model/Task.hpp"

struct NVGcontext;

namespace tt {

// Everything the renderer needs to draw the in-progress drag.
struct DragVisual {
    bool   active = false;
    TaskId dragged = 0;       // node drawn as a ghost at the cursor
    TaskId target = 0;        // hovered drop target (0 = root area)
    bool   validTarget = false;
    Rect   ghost;             // where to draw the dragged node (follows cursor)
    Vec2   fromPoint;         // preview edge start (target bottom-centre)
    Vec2   toPoint;           // preview edge end (insertion slot top-centre)
    bool   showPreviewEdge = false;
};

// Geometry for the DONE side panel (screen coordinates), computed by App.
struct DonePanelLayout {
    bool  visible = false;
    bool  pinned = false;
    Rect  panel;              // full panel rectangle
    Rect  titleBar;           // top bar holding the title + pin button
    Rect  pinButton;          // autohide toggle
    float contentClipTop = 0.f;
    float contentClipBottom = 0.f;
};

// One visible row in the DONE panel's expandable, indented tree.
struct DoneRow {
    TaskId id = 0;
    Rect   rect;              // full clickable row rect (already indented + scrolled)
    int    depth = 0;
    bool   hasChildren = false;
    bool   expanded = false;
};

class Renderer {
public:
    bool init(NVGcontext* vg, const std::string& fontPath);

    // Measure each node's box (width clamped to cfg.maxNodeWidth, height grown to
    // fit wrapped text) into `out`. Must use the same font/size as drawing.
    void measureSizes(const Forest& f, const Config& cfg,
                      std::unordered_map<TaskId, Size>& out) const;

    void beginFrame(int winW, int winH, float devicePixelRatio);
    void endFrame();

    void drawScrim(float w, float h, const Config& cfg);

    // Draw all edges then all nodes, translated by the canvas `pan` offset. The
    // dragged node (dv.dragged) is skipped here and drawn as a ghost; the drop target
    // is highlighted.
    void drawTree(const Forest& f, const std::unordered_map<TaskId, Rect>& rects,
                  const Config& cfg, const DragVisual& dv, Vec2 pan);

    // The input widget. In quick-add mode it's a centred box with a drop shadow;
    // otherwise a bar at the bottom of the overlay.
    void drawInput(float screenW, float screenH, const std::string& text,
                   std::size_t caretByte, bool caretOn, const Config& cfg,
                   bool quickAddMode);

    // The DONE side panel: translucent green tint, "DONE" title, autohide toggle, and
    // a scissor-clipped, indented, expandable list of done rows (no card borders).
    void drawDonePanel(const DonePanelLayout& layout, const Forest& f,
                       const std::vector<DoneRow>& rows, const Config& cfg);

    // Wrapped height of `text` at the given content width (for card sizing).
    float measureTextHeight(const std::string& text, float width) const;

    float fontSize() const { return fontSize_; }

private:
    void drawNode(const Rect& r, const std::string& text, const Config& cfg,
                  bool highlight, float alphaMul, TaskId id) const;
    void drawEdge(const Rect& parent, const Rect& child, const Config& cfg) const;

    // Reserved top band inside a node for the id badge (so text never overlaps it).
    float idBandHeight() const { return fontSize_ * 0.62f + 12.f; }

    NVGcontext* vg_ = nullptr;
    int   font_ = -1;
    float fontSize_ = 18.f;
    float padX_ = 14.f;
    float padY_ = 10.f;
    float minNodeW_ = 90.f;
};

} // namespace tt
