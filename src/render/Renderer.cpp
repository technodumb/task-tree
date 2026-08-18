#include "render/Renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <nanovg.h>

#include "model/DateText.hpp"

namespace tt {
namespace {

NVGcolor col(const Color& c, float alphaMul = 1.f) {
    return nvgRGBAf(c.r, c.g, c.b, c.a * alphaMul);
}

const char* end(const std::string& s) { return s.c_str() + s.size(); }

// Total number of nodes beneath `id` (excluding `id` itself).
int countDescendants(const Forest& f, TaskId id) {
    const Task* t = f.get(id);
    if (!t) return 0;
    int n = 0;
    for (TaskId c : t->children) n += 1 + countDescendants(f, c);
    return n;
}

} // namespace

bool Renderer::init(NVGcontext* vg, const std::string& fontPath) {
    vg_ = vg;
    if (!vg_) return false;
    font_ = nvgCreateFont(vg_, "ui", fontPath.c_str());
    return font_ >= 0;
}

const Renderer::NodeTextCache& Renderer::layoutText(TaskId id, const std::string& text,
                                                    float contentW) const {
    NodeTextCache& e = textCache_[id];
    if (e.width == contentW && e.text == text) return e; // cache hit (node text never changes)

    e.text = text;
    e.width = contentW;
    e.lines.clear();
    nvgFontFaceId(vg_, font_);
    nvgFontSize(vg_, fontSize_);
    float asc = 0, desc = 0, lineh = 0;
    nvgTextMetrics(vg_, &asc, &desc, &lineh);
    e.lineH = lineh;

    if (!text.empty()) {
        const char* start = text.c_str();
        const char* stop = text.c_str() + text.size();
        NVGtextRow rows[4];
        int n = 0;
        while ((n = nvgTextBreakLines(vg_, start, stop, contentW, rows, 4)) > 0) {
            for (int i = 0; i < n; ++i) e.lines.emplace_back(rows[i].start, rows[i].end);
            start = rows[n - 1].next;
        }
    }
    return e;
}

void Renderer::measureSizes(const Forest& f, const Config& cfg,
                            std::unordered_map<TaskId, Size>& out) const {
    out.clear();
    const float maxContentW = std::max(20.f, cfg.maxNodeWidth - 2 * padX_);
    nvgFontFaceId(vg_, font_);
    nvgFontSize(vg_, fontSize_);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);

    for (const auto& [id, t] : f.nodes) {
        const char* s = t.text.c_str();
        const char* e = end(t.text);

        float b[4] = {0, 0, 0, 0};
        const float singleW = t.text.empty() ? minNodeW_ : nvgTextBounds(vg_, 0, 0, s, e, b);
        const float contentW = std::min(maxContentW, std::max(minNodeW_ - 2 * padX_, singleW));

        // Wrap once (at scale 1) and size from the cached line count, so the box always
        // fits the exact lines that will be drawn — at any zoom level.
        const NodeTextCache& L = layoutText(id, t.text, contentW);
        const float textH = t.text.empty() ? fontSize_ : L.lines.size() * L.lineH;

        Size sz;
        sz.w = contentW + 2 * padX_;
        // Top band for the id badge + the text height + bottom padding.
        sz.h = idBandHeight() + std::max(fontSize_, textH) + padY_;
        out[id] = sz;
    }
}

void Renderer::beginFrame(int winW, int winH, float dpr) {
    nvgBeginFrame(vg_, static_cast<float>(winW), static_cast<float>(winH), dpr);
}

void Renderer::endFrame() { nvgEndFrame(vg_); }

void Renderer::drawScrim(float w, float h, const Config& cfg) {
    nvgBeginPath(vg_);
    nvgRect(vg_, 0, 0, w, h);
    nvgFillColor(vg_, col(cfg.scrim, cfg.overlayOpacity));
    nvgFill(vg_);
}

void Renderer::drawEdge(const Rect& parent, const Rect& child, const Config& cfg) const {
    const float x0 = parent.cx(), y0 = parent.bottom();
    const float x1 = child.cx(), y1 = child.top();
    nvgBeginPath(vg_);
    nvgMoveTo(vg_, x0, y0);
    if (std::fabs(x0 - x1) < 1.0f) {
        nvgLineTo(vg_, x1, y1);
    } else {
        const float k = (y1 - y0) * 0.4f;
        nvgBezierTo(vg_, x0, y0 + k, x1, y1 - k, x1, y1);
    }
    nvgStrokeColor(vg_, col(cfg.edgeColor));
    nvgStrokeWidth(vg_, 1.8f);
    nvgStroke(vg_);
}

void Renderer::drawNode(const Rect& r, const std::string& text, const Config& cfg,
                        bool highlight, float alphaMul, TaskId id, int status,
                        const std::string& dateChip) const {
    // Fill + text colour depend on the status (right-click cycles it).
    const Color fillC = (status == 1) ? cfg.nodeFillInProgress
                      : (status == 2) ? cfg.nodeFillPriority
                                      : cfg.nodeFill;
    const Color textC = (status == 0) ? cfg.nodeText : cfg.nodeTextDark;

    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, r.x, r.y, r.w, r.h, cfg.cornerRadius);
    nvgFillColor(vg_, col(fillC, alphaMul));
    nvgFill(vg_);
    nvgStrokeColor(vg_, col(highlight ? cfg.dropHint : cfg.nodeBorder, alphaMul));
    nvgStrokeWidth(vg_, cfg.borderWidth * (highlight ? 2.2f : 1.f));
    nvgStroke(vg_);

    // Task text sits below the reserved id band. Draw the pre-wrapped cached lines
    // explicitly (no re-wrapping) so zoom never reflows or clips the text.
    if (!text.empty()) {
        nvgFontFaceId(vg_, font_);
        nvgFontSize(vg_, fontSize_);
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, col(textC, alphaMul));
        const NodeTextCache& L = layoutText(id, text, r.w - 2 * padX_);
        float ty = r.y + idBandHeight();
        for (const std::string& line : L.lines) {
            nvgText(vg_, r.x + padX_, ty, line.c_str(), line.c_str() + line.size());
            ty += L.lineH;
        }
    }

    // Id badge (top-left), a small pill in its own colour. Handle for future keyboard
    // selection/navigation (see docs/FUTURE.md).
    const std::string label = std::to_string(id);
    const float idFs = fontSize_ * 0.62f;
    nvgFontFaceId(vg_, font_);
    nvgFontSize(vg_, idFs);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    float lb[4] = {0, 0, 0, 0};
    nvgTextBounds(vg_, 0, 0, label.c_str(), nullptr, lb);
    const float lw = lb[2] - lb[0];
    const float bx = r.x + 8.f, by = r.y + 5.f;
    const float bw = lw + 12.f, bh = idFs + 6.f;
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, bx, by, bw, bh, 5.f);
    nvgFillColor(vg_, col(cfg.idBadgeBg, alphaMul));
    nvgFill(vg_);
    nvgFillColor(vg_, col(cfg.idBadgeText, alphaMul));
    nvgText(vg_, bx + 6.f, by + 3.f, label.c_str(), nullptr);

    // Creation-date chip (ttd 145): right end of the same reserved band, in the id
    // badge's style (font face/size/align still set from it). Dropped, not squeezed,
    // when the band is too narrow — and empty when the date is unknown, because no
    // chip beats a guessed one.
    if (!dateChip.empty()) {
        float db[4] = {0, 0, 0, 0};
        nvgTextBounds(vg_, 0, 0, dateChip.c_str(), end(dateChip), db);
        const float dw = (db[2] - db[0]) + 12.f;
        const float dx = r.x + r.w - 8.f - dw;
        if (dx >= bx + bw + 6.f) {
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, dx, by, dw, bh, 5.f);
            nvgFillColor(vg_, col(cfg.idBadgeBg, alphaMul));
            nvgFill(vg_);
            nvgFillColor(vg_, col(cfg.idBadgeText, alphaMul));
            nvgText(vg_, dx + 6.f, by + 3.f, dateChip.c_str(), end(dateChip));
        }
    }
}

void Renderer::drawCollapseHandle(const Rect& node, bool collapsed, int hiddenCount,
                                  const Config& cfg) const {
    const Rect h = collapseHandle(node);
    const float cx = h.cx(), cy = h.cy(), rad = h.w * 0.5f;
    nvgBeginPath(vg_);
    nvgCircle(vg_, cx, cy, rad);
    nvgFillColor(vg_, col(cfg.idBadgeBg));
    nvgFill(vg_);
    nvgStrokeColor(vg_, col(cfg.nodeBorder));
    nvgStrokeWidth(vg_, 1.f);
    nvgStroke(vg_);

    if (collapsed) {
        // Show how many descendants are hidden (also the "expand me" affordance).
        const std::string n = std::to_string(hiddenCount);
        nvgFontFaceId(vg_, font_);
        nvgFontSize(vg_, rad * 1.3f);
        nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, col(cfg.idBadgeText));
        nvgText(vg_, cx, cy + 0.5f, n.c_str(), nullptr);
    } else {
        // Minus sign -> click to collapse.
        nvgBeginPath(vg_);
        nvgMoveTo(vg_, cx - rad * 0.45f, cy);
        nvgLineTo(vg_, cx + rad * 0.45f, cy);
        nvgStrokeColor(vg_, col(cfg.idBadgeText));
        nvgStrokeWidth(vg_, 1.7f);
        nvgLineCap(vg_, NVG_ROUND);
        nvgStroke(vg_);
    }
}

void Renderer::drawTree(const Forest& f, const std::unordered_map<TaskId, Rect>& rects,
                        const Config& cfg, const DragVisual& dv, Vec2 pan, float zoom,
                        const std::unordered_set<TaskId>& pathHi, float pathStrength,
                        const std::unordered_set<TaskId>& searchHits, TaskId selected,
                        TaskId reparentTarget) {
    nvgSave(vg_);
    nvgTranslate(vg_, pan.x, pan.y);  // screen = pan + zoom * world
    nvgScale(vg_, zoom, zoom);
    auto rectOf = [&](TaskId id) -> const Rect* {
        auto it = rects.find(id);
        return it == rects.end() ? nullptr : &it->second;
    };

    // Edges (skip any touching the dragged node — it's shown as a ghost instead).
    for (const auto& [id, t] : f.nodes) {
        if (dv.active && id == dv.dragged) continue;
        const Rect* pr = rectOf(id);
        if (!pr) continue;
        for (TaskId c : t.children) {
            if (dv.active && c == dv.dragged) continue;
            if (const Rect* cr = rectOf(c)) drawEdge(*pr, *cr, cfg);
        }
    }

    // Preview edge from the hovered target down to the insertion slot.
    if (dv.active && dv.validTarget && dv.showPreviewEdge) {
        const float k = (dv.toPoint.y - dv.fromPoint.y) * 0.4f;
        nvgBeginPath(vg_);
        nvgMoveTo(vg_, dv.fromPoint.x, dv.fromPoint.y);
        nvgBezierTo(vg_, dv.fromPoint.x, dv.fromPoint.y + k,
                    dv.toPoint.x, dv.toPoint.y - k, dv.toPoint.x, dv.toPoint.y);
        nvgStrokeColor(vg_, col(cfg.dropHint));
        nvgStrokeWidth(vg_, 2.4f);
        nvgStroke(vg_);
    }

    // Nodes. One clock read per frame; the per-node chip text is derived from it.
    const std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
    for (const auto& [id, t] : f.nodes) {
        if (dv.active && id == dv.dragged) continue;
        if (const Rect* r = rectOf(id)) {
            const bool hi = dv.active && dv.validTarget && id == dv.target;
            drawNode(*r, t.text, cfg, hi, 1.f, id, t.status, shortDate(t.createdAt, nowMs));
            // A node with children gets a collapse/expand handle on its bottom edge.
            if (!t.children.empty())
                drawCollapseHandle(*r, t.collapsed, t.collapsed ? countDescendants(f, id) : 0, cfg);
        }
    }

    // Ghost of the dragged node, following the cursor.
    if (dv.active) {
        const Task* t = f.get(dv.dragged);
        drawNode(dv.ghost, t ? t->text : std::string{}, cfg, false, 0.85f, dv.dragged,
                 t ? t->status : 0, t ? shortDate(t->createdAt, nowMs) : std::string{});
    }

    // Fading path flash (root -> new node): highlight the path edges and node outlines.
    if (pathStrength > 0.f && !pathHi.empty()) {
        const NVGcolor hc = col(cfg.dropHint, pathStrength);
        for (const auto& [id, t] : f.nodes) {
            if (!pathHi.count(id)) continue;
            const Rect* pr = rectOf(id);
            if (!pr) continue;
            for (TaskId c : t.children) {
                if (!pathHi.count(c)) continue;
                const Rect* cr = rectOf(c);
                if (!cr) continue;
                const float x0 = pr->cx(), y0 = pr->bottom(), x1 = cr->cx(), y1 = cr->top();
                nvgBeginPath(vg_);
                nvgMoveTo(vg_, x0, y0);
                if (std::fabs(x0 - x1) < 1.f) nvgLineTo(vg_, x1, y1);
                else { const float k = (y1 - y0) * 0.4f; nvgBezierTo(vg_, x0, y0 + k, x1, y1 - k, x1, y1); }
                nvgStrokeColor(vg_, hc);
                nvgStrokeWidth(vg_, 3.5f);
                nvgStroke(vg_);
            }
        }
        for (TaskId id : pathHi) {
            const Rect* r = rectOf(id);
            if (!r) continue;
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, r->x - 2.f, r->y - 2.f, r->w + 4.f, r->h + 4.f, cfg.cornerRadius + 2.f);
            nvgStrokeColor(vg_, hc);
            nvgStrokeWidth(vg_, 3.f);
            nvgStroke(vg_);
        }
    }

    // Search matches: amber ring around each matching node.
    if (!searchHits.empty()) {
        for (TaskId id : searchHits) {
            const Rect* r = rectOf(id);
            if (!r) continue;
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, r->x - 3.f, r->y - 3.f, r->w + 6.f, r->h + 6.f, cfg.cornerRadius + 3.f);
            nvgStrokeColor(vg_, nvgRGBA(245, 200, 70, 255));
            nvgStrokeWidth(vg_, 3.f);
            nvgStroke(vg_);
        }
    }

    // Selection ring: the click-selected node, a distinct blue so it reads apart from the
    // amber search ring and the green path flash. Drawn slightly outside the search ring
    // so both can show at once. Skipped while the selected node is being dragged (it's the
    // ghost then).
    if (selected != 0 && !(dv.active && selected == dv.dragged)) {
        if (const Rect* r = rectOf(selected)) {
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, r->x - 5.f, r->y - 5.f, r->w + 10.f, r->h + 10.f, cfg.cornerRadius + 5.f);
            nvgStrokeColor(vg_, nvgRGBA(120, 175, 255, 255));
            nvgStrokeWidth(vg_, 2.5f);
            nvgStroke(vg_);
        }
    }

    // Ctrl+click reparent cue: ring the node the selection would move under, in the same
    // green as the drag drop-hint so "this is where it lands" reads the same either way.
    if (reparentTarget != 0 && reparentTarget != selected) {
        if (const Rect* r = rectOf(reparentTarget)) {
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, r->x - 5.f, r->y - 5.f, r->w + 10.f, r->h + 10.f, cfg.cornerRadius + 5.f);
            nvgStrokeColor(vg_, col(cfg.dropHint));
            nvgStrokeWidth(vg_, 3.f);
            nvgStroke(vg_);
        }
    }
    nvgRestore(vg_);
}

void Renderer::drawFieldChrome(float bx, float by, float w, float h,
                               const Config& cfg, const Color& border) const {
    const float r = cfg.cornerRadius;
    // Drop shadow.
    NVGpaint shadow = nvgBoxGradient(vg_, bx, by + 4, w, h, r * 1.5f, 22.f,
                                     nvgRGBA(0, 0, 0, 150), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg_);
    nvgRect(vg_, bx - 40, by - 40, w + 80, h + 80);
    nvgRoundedRect(vg_, bx, by, w, h, r);
    nvgPathWinding(vg_, NVG_HOLE);
    nvgFillPaint(vg_, shadow);
    nvgFill(vg_);
    // Box fill + border (the border colour is the only per-field difference).
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, bx, by, w, h, r);
    nvgFillColor(vg_, col(cfg.quickAddFill));
    nvgFill(vg_);
    nvgStrokeColor(vg_, col(border));
    nvgStrokeWidth(vg_, 1.5f);
    nvgStroke(vg_);
}

void Renderer::drawToast(float cx, float cyBottom, const std::string& text,
                         const Config& cfg) const {
    nvgFontFaceId(vg_, font_);
    nvgFontSize(vg_, fontSize_);
    float asc = 0, desc = 0, lh = 0;
    nvgTextMetrics(vg_, &asc, &desc, &lh);
    const float tw = nvgTextBounds(vg_, 0, 0, text.c_str(), end(text), nullptr);
    const float padX = 14.f, padY = 6.f;
    const float w = tw + 2 * padX;
    const float h = lh + 2 * padY;
    const float x = cx - w * 0.5f;
    const float y = cyBottom - h;      // pill bottom rests at cyBottom (just above the node)
    const float r = h * 0.5f;          // fully rounded ends

    NVGpaint shadow = nvgBoxGradient(vg_, x, y + 3, w, h, r, 16.f,
                                     nvgRGBA(0, 0, 0, 140), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg_);
    nvgRect(vg_, x - 30, y - 30, w + 60, h + 60);
    nvgRoundedRect(vg_, x, y, w, h, r);
    nvgPathWinding(vg_, NVG_HOLE);
    nvgFillPaint(vg_, shadow);
    nvgFill(vg_);

    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, x, y, w, h, r);
    nvgFillColor(vg_, col(cfg.quickAddFill));
    nvgFill(vg_);
    nvgStrokeColor(vg_, col(cfg.nodeBorder));
    nvgStrokeWidth(vg_, 1.f);
    nvgStroke(vg_);

    nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg_, col(cfg.nodeText));
    nvgText(vg_, cx, y + h * 0.5f, text.c_str(), end(text));
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
}

void Renderer::drawCaret(float x, float centerY, const Config& cfg) const {
    const float half = caretHeight() * 0.5f;
    nvgBeginPath(vg_);
    nvgMoveTo(vg_, x, centerY - half);
    nvgLineTo(vg_, x, centerY + half);
    nvgStrokeColor(vg_, col(cfg.nodeText));
    nvgStrokeWidth(vg_, 1.4f);
    nvgStroke(vg_);
}

Renderer::InputLayout Renderer::layoutInput(float screenW, float screenH,
                                            const std::string& text, std::size_t caretByte,
                                            const InputStyle& style) const {
    InputLayout L;
    const float boxW = std::min(620.f, std::max(360.f, screenW * 0.5f));
    const float bx = (screenW - boxW) * 0.5f;
    const float pad = padY_;

    nvgFontFaceId(vg_, font_);
    nvgFontSize(vg_, fontSize_);
    nvgTextMetrics(vg_, &L.asc, &L.desc, &L.lineH);

    // A palette mode claims a section at the bar's left end; the text — and the wrap width
    // with it — starts after it, so the two never collide. `chipWidth` lets the caller give
    // every mode the same section width so the text doesn't shift when the mode changes.
    const float chipTextW = style.chip.empty()
                              ? 0.f
                              : nvgTextBounds(vg_, 0, 0, style.chip.c_str(), end(style.chip), nullptr);
    L.chipW = style.chip.empty() ? 0.f : std::max(chipTextW + 24.f, style.chipWidth);
    L.contentW = boxW - 2 * padX_ - L.chipW;

    // Wrap the input into visual lines so the box grows as text is typed.
    if (!text.empty()) {
        const char* s = text.c_str();
        const char* e = s + text.size();
        const char* p = s;
        NVGtextRow tr[8];
        int n = 0;
        while ((n = nvgTextBreakLines(vg_, p, e, L.contentW, tr, 8)) > 0) {
            for (int i = 0; i < n; ++i)
                L.rows.push_back({(int)(tr[i].start - s), (int)(tr[i].end - s), (int)(tr[i].next - s)});
            p = tr[n - 1].next;
        }
    }
    if (L.rows.empty()) L.rows.push_back({0, 0, 0});

    // Which visual line holds the caret?
    const int cb = (int)std::min(caretByte, text.size());
    L.caretRow = (int)L.rows.size() - 1;
    for (int i = 0; i < (int)L.rows.size(); ++i)
        if (cb >= L.rows[i].start && cb <= L.rows[i].next) { L.caretRow = i; if (cb < L.rows[i].next) break; }

    const int maxLines = 10;                 // cap growth; scroll to keep the caret in view
    const int total = (int)L.rows.size();
    L.firstRow = 0;
    if (total > maxLines)
        L.firstRow = std::max(0, std::min(L.caretRow - maxLines + 1, total - maxLines));
    L.shown = std::min(total, maxLines);

    const float boxH = L.shown * L.lineH + 2 * pad + 4.f;
    // Quick-add (Ctrl+Alt+Enter): centred on screen. Full overlay (Ctrl+Alt+Space):
    // bottom anchored -> grows UP.
    const float by = style.quickAdd ? (screenH - boxH) * 0.5f : (screenH - 40.f - boxH);

    L.box = {bx, by, boxW, boxH};
    L.tx = bx + L.chipW + padX_;
    L.textTop = by + (boxH - L.shown * L.lineH) * 0.5f;
    return L;
}

Rect Renderer::drawInput(float screenW, float screenH, const std::string& text,
                         std::size_t caretByte, std::size_t selBegin, std::size_t selEnd,
                         bool caretOn, const Config& cfg, const InputStyle& style) {
    const bool editing = style.editing;
    const InputLayout L = layoutInput(screenW, screenH, text, caretByte, style);
    const float bx = L.box.x, by = L.box.y, boxW = L.box.w, boxH = L.box.h;
    const float tx = L.tx, textTop = L.textTop;
    selBegin = std::min(selBegin, text.size());
    selEnd = std::min(selEnd, text.size());

    // One field, several jobs — only the border colour says which: subtle node border when
    // adding, selection-blue when editing a node, and the palette tint in command modes.
    const Color editBorder{120 / 255.f, 175 / 255.f, 255 / 255.f, 1.f};
    const Color border = style.tinted ? style.border : (editing ? editBorder : cfg.nodeBorder);
    drawFieldChrome(bx, by, boxW, boxH, cfg, border);

    // Mode section: a full-height band at the left end of the bar. Its left corners share
    // the box's radius so it sits flush inside the border; the right edge is straight, with
    // a hairline rule separating it from the text.
    if (!style.chip.empty()) {
        const float inset = 1.5f;                      // stay inside the border stroke
        const float r = std::max(0.f, cfg.cornerRadius - inset);
        const float x0 = bx + inset, x1 = bx + L.chipW;
        const float y0 = by + inset, y1 = by + boxH - inset;
        nvgBeginPath(vg_);
        nvgMoveTo(vg_, x1, y0);
        nvgLineTo(vg_, x0 + r, y0);
        nvgArcTo(vg_, x0, y0, x0, y0 + r, r);          // top-left, same curve as the box
        nvgLineTo(vg_, x0, y1 - r);
        nvgArcTo(vg_, x0, y1, x0 + r, y1, r);          // bottom-left
        nvgLineTo(vg_, x1, y1);
        nvgClosePath(vg_);
        nvgFillColor(vg_, col(border, 0.18f));
        nvgFill(vg_);

        nvgBeginPath(vg_);                             // divider
        nvgMoveTo(vg_, x1, y0);
        nvgLineTo(vg_, x1, y1);
        nvgStrokeColor(vg_, col(border, 0.55f));
        nvgStrokeWidth(vg_, 1.f);
        nvgStroke(vg_);

        nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, col(border, 0.95f));
        nvgText(vg_, (x0 + x1) * 0.5f, by + boxH * 0.5f, style.chip.c_str(), end(style.chip));
    }

    // Selection: a translucent block behind the selected glyphs on each visible row (drawn
    // under the text so the glyphs stay legible). The advance of the row prefix up to each
    // edge places it, matching the caret's own measurement.
    if (selEnd > selBegin) {
        const Color selCol{120 / 255.f, 175 / 255.f, 255 / 255.f, 1.f};
        for (int i = L.firstRow; i < L.firstRow + L.shown; ++i) {
            const int rs = L.rows[i].start, re = L.rows[i].end;
            const int a = std::max((int)selBegin, rs);
            const int b = std::min((int)selEnd, re);
            if (b <= a) continue;
            const float xa = a > rs ? nvgTextBounds(vg_, 0, 0, text.c_str() + rs, text.c_str() + a, nullptr) : 0.f;
            const float xb = nvgTextBounds(vg_, 0, 0, text.c_str() + rs, text.c_str() + b, nullptr);
            const float y0 = textTop + (i - L.firstRow) * L.lineH;
            nvgBeginPath(vg_);
            nvgRect(vg_, tx + xa, y0, xb - xa, L.lineH);
            nvgFillColor(vg_, col(selCol, 0.30f));
            nvgFill(vg_);
        }
    }

    // Text (or placeholder): the visible rows form a block centred vertically in the
    // box (the search field's balanced look), one row per wrapped line.
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    if (text.empty()) {
        // In a palette mode the style's placeholder is authoritative — empty means show
        // nothing (the mode section already says what the bar is for).
        const char* ph = !style.placeholder.empty() ? style.placeholder.c_str()
                       : style.tinted ? ""
                       : editing ? "Edit text — Enter to save, Esc to cancel"
                                 : "Type a task, or / for other modes…";
        nvgFillColor(vg_, col(cfg.nodeText, 0.4f));
        nvgText(vg_, tx, textTop, ph, nullptr);
    } else {
        nvgFillColor(vg_, col(cfg.nodeText));
        float ty = textTop;
        for (int i = L.firstRow; i < L.firstRow + L.shown; ++i) {
            nvgText(vg_, tx, ty, text.c_str() + L.rows[i].start, text.c_str() + L.rows[i].end);
            ty += L.lineH;
        }
    }

    // Caret on its wrapped line, centred on the row's glyph box (drawCaret sizes it).
    if (caretOn) {
        const int cb = (int)std::min(caretByte, text.size());
        const float lineTop = textTop + std::max(0, L.caretRow - L.firstRow) * L.lineH;
        float adv = 0.f;
        if (cb > L.rows[L.caretRow].start)
            adv = nvgTextBounds(vg_, 0, 0, text.c_str() + L.rows[L.caretRow].start,
                                text.c_str() + cb, nullptr);
        drawCaret(tx + adv, lineTop + (L.asc - L.desc) * 0.5f, cfg);
    }

    // Palette status ("7 matches", "node 12", "no match"), right-aligned on the first row.
    // Commands are short, but skip it if this row's text would run into it anyway.
    if (!style.status.empty()) {
        const int fr = L.firstRow;
        const float sw = nvgTextBounds(vg_, 0, 0, style.status.c_str(), end(style.status), nullptr);
        const float used = L.rows[fr].end > L.rows[fr].start
                             ? nvgTextBounds(vg_, 0, 0, text.c_str() + L.rows[fr].start,
                                             text.c_str() + L.rows[fr].end, nullptr)
                             : 0.f;
        if (used + sw + 24.f < L.contentW) {
            nvgTextAlign(vg_, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
            nvgFillColor(vg_, col(cfg.nodeText, 0.5f));
            nvgText(vg_, bx + boxW - padX_, textTop, style.status.c_str(), end(style.status));
            nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        }
    }
    return L.box;
}

std::size_t Renderer::inputByteAt(float screenW, float screenH, const std::string& text,
                                  std::size_t caretByte, const InputStyle& style,
                                  Vec2 point) const {
    if (text.empty()) return 0;
    const InputLayout L = layoutInput(screenW, screenH, text, caretByte, style);

    // Clamp the click to a visible row, then walk that row's code-point boundaries and
    // stop at the first whose glyph midpoint the click is left of (the usual caret rule).
    int rel = (int)std::floor((point.y - L.textTop) / L.lineH);
    rel = std::max(0, std::min(rel, L.shown - 1));
    const int row = L.firstRow + rel;
    const int rs = L.rows[row].start, re = L.rows[row].end;
    const float localX = point.x - L.tx;
    if (localX <= 0.f) return static_cast<std::size_t>(rs);

    int i = rs;
    float prevAdv = 0.f;
    while (i < re) {
        int j = i + 1;
        while (j < re && (static_cast<unsigned char>(text[j]) & 0xC0) == 0x80) ++j;
        const float adv = nvgTextBounds(vg_, 0, 0, text.c_str() + rs, text.c_str() + j, nullptr);
        if (localX < (prevAdv + adv) * 0.5f) return static_cast<std::size_t>(i);
        prevAdv = adv;
        i = j;
    }
    return static_cast<std::size_t>(re);
}

std::string Renderer::fitText(const std::string& s, float maxW) const {
    if (nvgTextBounds(vg_, 0, 0, s.c_str(), end(s), nullptr) <= maxW) return s;
    std::string out = s;
    while (!out.empty()) {
        // Trim whole code points so a multi-byte glyph is never cut in half.
        do { out.pop_back(); } while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80);
        const std::string probe = out + "…";
        if (nvgTextBounds(vg_, 0, 0, probe.c_str(), end(probe), nullptr) <= maxW) return probe;
    }
    return out;
}

void Renderer::drawPalette(const Rect& inputBox,
                           const std::vector<std::pair<std::string, std::string>>& rows,
                           int activeRow, int moreCount, const std::string& hint,
                           const Color& tint, const Config& cfg) {
    if (rows.empty()) return;

    nvgFontFaceId(vg_, font_);
    nvgFontSize(vg_, fontSize_);
    const float hintSize = fontSize_ * 0.72f;   // the key legend is a footnote, not content
    const float rowH = fontSize_ + 12.f;
    const float footerH = hintSize + 12.f;
    const float pad = 8.f;
    const float w = inputBox.w;
    const float h = rows.size() * rowH + footerH + 2 * pad;
    const float x = inputBox.x;
    const float y = inputBox.y - h - 10.f;   // drops UP out of the bar

    drawFieldChrome(x, y, w, h, cfg, tint);

    const float contentW = w - 2 * padX_;
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    // One shared width for the lead column: '?' and '>' (or "[7]" and "[130]") advance
    // differently, so without this the descriptions would step in and out.
    float leadW = 0.f;
    for (const auto& [lead, rest] : rows) {
        (void)rest;
        leadW = std::max(leadW, nvgTextBounds(vg_, 0, 0, lead.c_str(), end(lead), nullptr));
    }
    leadW += 14.f;

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const float ry = y + pad + i * rowH;
        const bool active = static_cast<int>(i) == activeRow;
        if (active) {
            // The ↑/↓ cursor: a tinted band, same hue as the border and the canvas ring.
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, x + 4.f, ry, w - 8.f, rowH, cfg.cornerRadius * 0.6f);
            nvgFillColor(vg_, col(tint, 0.16f));
            nvgFill(vg_);
        }
        const float ty = ry + rowH * 0.5f;
        const std::string& lead = rows[i].first;
        nvgFillColor(vg_, col(tint, active ? 1.f : 0.7f));
        nvgText(vg_, x + padX_, ty, lead.c_str(), end(lead));
        const std::string line = fitText(rows[i].second, contentW - leadW);
        nvgFillColor(vg_, col(cfg.nodeText, active ? 1.f : 0.62f));
        nvgText(vg_, x + padX_ + leadW, ty, line.c_str(), end(line));
    }

    const float fy = y + pad + rows.size() * rowH + footerH * 0.5f;
    nvgFontSize(vg_, hintSize);
    nvgFillColor(vg_, col(cfg.nodeText, 0.4f));
    nvgText(vg_, x + padX_, fy, hint.c_str(), end(hint));
    if (moreCount > 0) {
        const std::string more = "+" + std::to_string(moreCount) + " more";
        nvgTextAlign(vg_, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgText(vg_, x + w - padX_, fy, more.c_str(), end(more));
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    }
    nvgFontSize(vg_, fontSize_);   // leave the shared context as we found it
}

float Renderer::measureTextWidth(const std::string& text) const {
    if (text.empty()) return 0.f;
    nvgFontFaceId(vg_, font_);
    nvgFontSize(vg_, fontSize_);
    return nvgTextBounds(vg_, 0, 0, text.c_str(), end(text), nullptr);
}

float Renderer::measureTextHeight(const std::string& text, float width) const {
    if (text.empty()) return fontSize_;
    nvgFontFaceId(vg_, font_);
    nvgFontSize(vg_, fontSize_);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    float b[4] = {0, 0, 0, 0};
    nvgTextBoxBounds(vg_, 0, 0, std::max(10.f, width), text.c_str(), end(text), b);
    return b[3] - b[1];
}

void Renderer::drawDonePanel(const DonePanelLayout& L, const Forest& f,
                             const std::vector<DoneRow>& rows, const Config& cfg) {
    const Rect& p = L.panel;
    const float radius = cfg.cornerRadius;

    // Floating card: drop shadow, then the rounded dark surface + subtle green border.
    NVGpaint shadow = nvgBoxGradient(vg_, p.x, p.y + 5.f, p.w, p.h, radius * 1.4f, 26.f,
                                     nvgRGBA(0, 0, 0, 140), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg_);
    nvgRect(vg_, p.x - 50.f, p.y - 50.f, p.w + 100.f, p.h + 100.f);
    nvgRoundedRect(vg_, p.x, p.y, p.w, p.h, radius);
    nvgPathWinding(vg_, NVG_HOLE);
    nvgFillPaint(vg_, shadow);
    nvgFill(vg_);

    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, p.x, p.y, p.w, p.h, radius);
    nvgFillColor(vg_, col(cfg.donePanelBg));
    nvgFill(vg_);
    nvgStrokeColor(vg_, col(cfg.donePanelBorder));
    nvgStrokeWidth(vg_, 1.f);
    nvgStroke(vg_);

    // Header: "Done" title + a small count pill.
    nvgFontFaceId(vg_, font_);
    nvgFontSize(vg_, fontSize_ + 3.f);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg_, col(cfg.doneTitle));
    const float titleX = L.titleBar.x + 16.f;
    const float titleY = L.titleBar.cy();
    nvgText(vg_, titleX, titleY, "Done", nullptr);
    if (L.itemCount > 0) {
        float tb[4] = {0, 0, 0, 0};
        nvgTextBounds(vg_, 0, 0, "Done", nullptr, tb);
        const std::string cnt = std::to_string(L.itemCount);
        const float badgeFs = fontSize_ - 5.f;
        nvgFontSize(vg_, badgeFs);
        float cb[4] = {0, 0, 0, 0};
        nvgTextBounds(vg_, 0, 0, cnt.c_str(), nullptr, cb);
        const float bw = std::max(badgeFs + 8.f, (cb[2] - cb[0]) + 12.f);
        const float bh = badgeFs + 7.f;
        const float bx = titleX + (tb[2] - tb[0]) + 10.f;
        const float by = titleY - bh * 0.5f;
        nvgBeginPath(vg_);
        nvgRoundedRect(vg_, bx, by, bw, bh, bh * 0.5f);
        nvgFillColor(vg_, col(cfg.doneTitle, 0.20f));
        nvgFill(vg_);
        nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, col(cfg.doneTitle));
        nvgText(vg_, bx + bw * 0.5f, titleY + 0.5f, cnt.c_str(), nullptr);
    }

    // Autohide toggle: filled accent pill when pinned, outlined when not.
    const Rect& b = L.pinButton;
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, b.x, b.y, b.w, b.h, b.h * 0.5f);
    if (L.pinned) {
        nvgFillColor(vg_, col(cfg.doneCardFill));
        nvgFill(vg_);
    } else {
        nvgStrokeColor(vg_, col(cfg.doneTitle, 0.55f));
        nvgStrokeWidth(vg_, 1.f);
        nvgStroke(vg_);
    }
    nvgFontSize(vg_, fontSize_ - 6.f);
    nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg_, col(L.pinned ? cfg.doneText : cfg.doneTitle, L.pinned ? 1.f : 0.85f));
    nvgText(vg_, b.cx(), b.cy() + 0.5f, L.pinned ? "PINNED" : "PIN", nullptr);

    // Divider under the header.
    nvgBeginPath(vg_);
    nvgRect(vg_, p.x + 14.f, L.contentClipTop - 0.5f, p.w - 28.f, 1.f);
    nvgFillColor(vg_, col(cfg.donePanelBorder));
    nvgFill(vg_);

    if (rows.empty()) {
        nvgFontFaceId(vg_, font_);
        nvgFontSize(vg_, fontSize_ - 1.f);
        nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg_, col(cfg.doneText, 0.4f));
        nvgText(vg_, p.cx(), (L.contentClipTop + L.contentClipBottom) * 0.5f,
                "Double-click a task to complete it", nullptr);
        return;
    }

    // Rows: top-level items sit on a subtle card; children are indented with a guide
    // line. Hovered row gets a faint highlight. Chevron if expandable, else a check.
    nvgSave(vg_);
    nvgScissor(vg_, p.x, L.contentClipTop, p.w, L.contentClipBottom - L.contentClipTop);
    const float bandX = p.x + 8.f, bandW = p.w - 16.f;
    for (const DoneRow& row : rows) {
        const Task* t = f.get(row.id);
        if (!t) continue;
        const Rect& r = row.rect;

        // Card behind top-level items; hover highlight on any row.
        if (row.depth == 0) {
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, bandX, r.y, bandW, r.h, 9.f);
            nvgFillColor(vg_, col(cfg.doneRowCard));
            nvgFill(vg_);
        }
        if (row.hovered) {
            nvgBeginPath(vg_);
            nvgRoundedRect(vg_, bandX, r.y, bandW, r.h, 9.f);
            nvgFillColor(vg_, col(cfg.doneRowHover));
            nvgFill(vg_);
        }

        // Indent guide for nested items.
        if (row.depth > 0) {
            const float gx = r.x - 9.f;
            nvgBeginPath(vg_);
            nvgRect(vg_, gx, r.y + 3.f, 1.f, r.h - 6.f);
            nvgFillColor(vg_, col(cfg.doneCardBorder, 0.22f));
            nvgFill(vg_);
        }

        const float glyphX = r.x + 3.f;
        const float cy = r.y + 20.f;   // aligned with the first text line
        if (row.hasChildren) {
            nvgBeginPath(vg_);
            if (row.expanded) { // down-pointing
                nvgMoveTo(vg_, glyphX, cy - 3.f);
                nvgLineTo(vg_, glyphX + 8.f, cy - 3.f);
                nvgLineTo(vg_, glyphX + 4.f, cy + 3.f);
            } else {            // right-pointing
                nvgMoveTo(vg_, glyphX, cy - 4.f);
                nvgLineTo(vg_, glyphX + 6.f, cy);
                nvgLineTo(vg_, glyphX, cy + 4.f);
            }
            nvgFillColor(vg_, col(cfg.doneTitle));
            nvgFill(vg_);
        } else {                // leaf: a small check mark reinforces "completed"
            nvgBeginPath(vg_);
            nvgMoveTo(vg_, glyphX, cy);
            nvgLineTo(vg_, glyphX + 3.f, cy + 3.f);
            nvgLineTo(vg_, glyphX + 8.f, cy - 4.f);
            nvgStrokeColor(vg_, col(cfg.doneTitle, 0.8f));
            nvgStrokeWidth(vg_, 1.6f);
            nvgLineCap(vg_, NVG_ROUND);
            nvgLineJoin(vg_, NVG_ROUND);
            nvgStroke(vg_);
        }

        const float textX = r.x + 18.f;
        nvgFontFaceId(vg_, font_);
        nvgFontSize(vg_, fontSize_);
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, col(cfg.doneText, row.depth == 0 ? 1.f : 0.8f));
        nvgTextBox(vg_, textX, r.y + 11.f, std::max(20.f, r.right() - textX - 2.f),
                   t->text.c_str(), end(t->text));
    }
    nvgRestore(vg_);
}

} // namespace tt
