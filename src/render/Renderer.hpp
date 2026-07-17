#pragma once
// NanoVG-based scene renderer. Owns no window/GL-context lifetime (main creates the
// NVGcontext); it only issues draw calls and measures node sizes. All coordinates
// are logical units; HiDPI is handled by the devicePixelRatio passed to beginFrame.

#include <string>
#include <unordered_map>
#include <unordered_set>
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
    int   itemCount = 0;      // total completed tasks (shown as a count badge)
};

// One visible row in the DONE panel's expandable, indented tree.
struct DoneRow {
    TaskId id = 0;
    Rect   rect;              // full clickable row rect (already indented + scrolled)
    int    depth = 0;
    bool   hasChildren = false;
    bool   expanded = false;
    bool   hovered = false;   // cursor is over this row (subtle highlight)
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
    // is highlighted. `pathHi` nodes (and edges between them) get a fading highlight
    // at intensity `pathStrength` (0 = none) — used to flash the path to a new task.
    void drawTree(const Forest& f, const std::unordered_map<TaskId, Rect>& rects,
                  const Config& cfg, const DragVisual& dv, Vec2 pan, float zoom,
                  const std::unordered_set<TaskId>& pathHi, float pathStrength,
                  const std::unordered_set<TaskId>& searchHits);

    // Search bar (top-centre) shown while Ctrl+F search is active.
    void drawSearchBar(float screenW, const std::string& query, std::size_t caretByte,
                       bool caretOn, int matchCount, const Config& cfg);

    // The input widget. In quick-add mode it's a centred box with a drop shadow;
    // otherwise a bar at the bottom of the overlay.
    void drawInput(float screenW, float screenH, const std::string& text,
                   std::size_t caretByte, bool caretOn, const Config& cfg,
                   bool quickAddMode);

    // The DONE side panel: a floating rounded card (drop shadow + subtle border, dark
    // surface with green accents), a header with title + count badge + pin toggle, and a
    // scissor-clipped, indented, expandable list of done rows (top-level items as cards,
    // children with indent guides, hover highlight, chevrons/checks).
    void drawDonePanel(const DonePanelLayout& layout, const Forest& f,
                       const std::vector<DoneRow>& rows, const Config& cfg);

    // Wrapped height of `text` at the given content width (for card sizing).
    float measureTextHeight(const std::string& text, float width) const;

    float fontSize() const { return fontSize_; }

private:
    void drawNode(const Rect& r, const std::string& text, const Config& cfg,
                  bool highlight, float alphaMul, TaskId id, int status) const;
    void drawEdge(const Rect& parent, const Rect& child, const Config& cfg) const;

    // Shared chrome for the input + search fields (drop shadow, rounded fill, border).
    // They are the same component; only the border colour differs.
    void drawFieldChrome(float bx, float by, float w, float h,
                         const Config& cfg, const Color& border) const;

    // Blinking text caret: a vertical bar of caretHeight(), centred on centerY at x.
    void drawCaret(float x, float centerY, const Config& cfg) const;
    // Caret height, sized between the text glyphs and the field box (used by both fields).
    float caretHeight() const { return fontSize_ * 1.5f; }

    // Reserved top band inside a node for the id badge (so text never overlaps it).
    float idBandHeight() const { return fontSize_ * 0.62f + 12.f; }

    // Per-node wrapped-text cache. Node text never changes, so the line breaks are
    // computed once (at scale 1) and reused for both sizing and drawing — this keeps
    // wrapping identical at every zoom level (no reflow -> no clipping).
    struct NodeTextCache {
        std::string text;
        float width = -1.f;
        float lineH = 0.f;
        std::vector<std::string> lines;
    };
    const NodeTextCache& layoutText(TaskId id, const std::string& text, float contentW) const;

    NVGcontext* vg_ = nullptr;
    int   font_ = -1;
    float fontSize_ = 18.f;
    float padX_ = 14.f;
    float padY_ = 10.f;
    float minNodeW_ = 90.f;
    mutable std::unordered_map<TaskId, NodeTextCache> textCache_;
};

} // namespace tt
