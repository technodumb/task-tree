#include "render/Renderer.hpp"

#include <algorithm>
#include <cmath>

#include <nanovg.h>

namespace tt {
namespace {

NVGcolor col(const Color& c, float alphaMul = 1.f) {
    return nvgRGBAf(c.r, c.g, c.b, c.a * alphaMul);
}

const char* end(const std::string& s) { return s.c_str() + s.size(); }

} // namespace

bool Renderer::init(NVGcontext* vg, const std::string& fontPath) {
    vg_ = vg;
    if (!vg_) return false;
    font_ = nvgCreateFont(vg_, "ui", fontPath.c_str());
    return font_ >= 0;
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

        float tb[4] = {0, 0, 0, 0};
        float textH = fontSize_;
        if (!t.text.empty()) {
            nvgTextBoxBounds(vg_, 0, 0, contentW, s, e, tb);
            textH = tb[3] - tb[1];
        }
        Size sz;
        sz.w = contentW + 2 * padX_;
        sz.h = std::max(fontSize_ + 2 * padY_, textH + 2 * padY_);
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
                        bool highlight, float alphaMul, TaskId id) const {
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, r.x, r.y, r.w, r.h, cfg.cornerRadius);
    nvgFillColor(vg_, col(cfg.nodeFill, alphaMul));
    nvgFill(vg_);
    nvgStrokeColor(vg_, col(highlight ? cfg.dropHint : cfg.nodeBorder, alphaMul));
    nvgStrokeWidth(vg_, cfg.borderWidth * (highlight ? 2.2f : 1.f));
    nvgStroke(vg_);

    if (!text.empty()) {
        nvgFontFaceId(vg_, font_);
        nvgFontSize(vg_, fontSize_);
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, col(cfg.nodeText, alphaMul));
        nvgTextBox(vg_, r.x + padX_, r.y + padY_, r.w - 2 * padX_, text.c_str(), end(text));
    }

    // Small id label (top-right), drawn on top with a faint pill so it stays legible
    // over wrapped text. Used later for keyboard selection/navigation (see docs/FUTURE.md).
    const std::string label = std::to_string(id);
    const float idFs = fontSize_ * 0.62f;
    nvgFontFaceId(vg_, font_);
    nvgFontSize(vg_, idFs);
    nvgTextAlign(vg_, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
    float lb[4] = {0, 0, 0, 0};
    nvgTextBounds(vg_, 0, 0, label.c_str(), nullptr, lb);
    const float lw = lb[2] - lb[0];
    const float lx = r.right() - 8.f;
    const float ly = r.y + 4.f;
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, lx - lw - 4.f, ly - 2.f, lw + 8.f, idFs + 4.f, 4.f);
    nvgFillColor(vg_, col(cfg.nodeFill, alphaMul));
    nvgFill(vg_);
    nvgFillColor(vg_, col(cfg.nodeBorder, alphaMul * 0.9f));
    nvgText(vg_, lx, ly, label.c_str(), nullptr);
}

void Renderer::drawTree(const Forest& f, const std::unordered_map<TaskId, Rect>& rects,
                        const Config& cfg, const DragVisual& dv, Vec2 pan) {
    nvgSave(vg_);
    nvgTranslate(vg_, pan.x, pan.y);
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

    // Nodes.
    for (const auto& [id, t] : f.nodes) {
        if (dv.active && id == dv.dragged) continue;
        if (const Rect* r = rectOf(id)) {
            const bool hi = dv.active && dv.validTarget && id == dv.target;
            drawNode(*r, t.text, cfg, hi, 1.f, id);
        }
    }

    // Ghost of the dragged node, following the cursor.
    if (dv.active) {
        const Task* t = f.get(dv.dragged);
        drawNode(dv.ghost, t ? t->text : std::string{}, cfg, false, 0.85f, dv.dragged);
    }
    nvgRestore(vg_);
}

void Renderer::drawInput(float screenW, float screenH, const std::string& text,
                         std::size_t caretByte, bool caretOn, const Config& cfg,
                         bool quickAddMode) {
    const float boxW = std::min(620.f, std::max(360.f, screenW * 0.5f));
    const float boxH = fontSize_ + 2 * padY_ + 10.f;
    const float bx = (screenW - boxW) * 0.5f;
    // Full overlay: input pinned to the bottom-centre. Quick-add: floating box ~centre.
    const float by = quickAddMode ? (screenH * 0.42f) : (screenH - boxH - 40.f);
    const float r = cfg.cornerRadius;

    // Drop shadow (mainly for quick-add, where there is no scrim behind it).
    NVGpaint shadow = nvgBoxGradient(vg_, bx, by + 4, boxW, boxH, r * 1.5f, 22.f,
                                     nvgRGBA(0, 0, 0, 150), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg_);
    nvgRect(vg_, bx - 40, by - 40, boxW + 80, boxH + 80);
    nvgRoundedRect(vg_, bx, by, boxW, boxH, r);
    nvgPathWinding(vg_, NVG_HOLE);
    nvgFillPaint(vg_, shadow);
    nvgFill(vg_);

    // Box.
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, bx, by, boxW, boxH, r);
    nvgFillColor(vg_, col(cfg.quickAddFill));
    nvgFill(vg_);
    nvgStrokeColor(vg_, col(cfg.nodeBorder));
    nvgStrokeWidth(vg_, 1.f);
    nvgStroke(vg_);

    // Text (or placeholder).
    const float tx = bx + padX_;
    const float ty = by + boxH * 0.5f;
    nvgFontFaceId(vg_, font_);
    nvgFontSize(vg_, fontSize_);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    if (text.empty()) {
        nvgFillColor(vg_, col(cfg.nodeText, 0.4f));
        nvgText(vg_, tx, ty, "Type a task, press Enter…", nullptr);
    } else {
        nvgFillColor(vg_, col(cfg.nodeText));
        nvgText(vg_, tx, ty, text.c_str(), end(text));
    }

    // Caret.
    if (caretOn) {
        const std::string prefix = text.substr(0, std::min(caretByte, text.size()));
        const float adv = prefix.empty() ? 0.f
                        : nvgTextBounds(vg_, tx, ty, prefix.c_str(), end(prefix), nullptr);
        const float caretX = tx + adv;
        nvgBeginPath(vg_);
        nvgMoveTo(vg_, caretX, by + 8);
        nvgLineTo(vg_, caretX, by + boxH - 8);
        nvgStrokeColor(vg_, col(cfg.nodeText));
        nvgStrokeWidth(vg_, 1.4f);
        nvgStroke(vg_);
    }
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

    // Just a translucent green tint (no border) + a subtle brighter left edge.
    nvgBeginPath(vg_);
    nvgRect(vg_, p.x, p.y, p.w, p.h);
    nvgFillColor(vg_, col(cfg.donePanelBg));
    nvgFill(vg_);
    nvgBeginPath(vg_);
    nvgRect(vg_, p.x, p.y, 2.f, p.h);
    nvgFillColor(vg_, col(cfg.doneTitle, 0.55f));
    nvgFill(vg_);

    // Title.
    nvgFontFaceId(vg_, font_);
    nvgFontSize(vg_, fontSize_ + 6.f);
    nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg_, col(cfg.doneTitle));
    nvgText(vg_, L.titleBar.x + 16.f, L.titleBar.cy(), "DONE", nullptr);

    // Autohide toggle button (kept as a small pill so it reads as clickable).
    const Rect& b = L.pinButton;
    nvgBeginPath(vg_);
    nvgRoundedRect(vg_, b.x, b.y, b.w, b.h, 6.f);
    nvgFillColor(vg_, col(cfg.doneCardFill, L.pinned ? 0.95f : 0.4f));
    nvgFill(vg_);
    nvgFontSize(vg_, fontSize_ - 5.f);
    nvgTextAlign(vg_, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg_, col(cfg.doneText));
    nvgText(vg_, b.cx(), b.cy(), L.pinned ? "PINNED" : "PIN", nullptr);

    // Rows: chevron (if it has children) + indented text. No boxes, no borders.
    nvgSave(vg_);
    nvgScissor(vg_, p.x, L.contentClipTop, p.w, L.contentClipBottom - L.contentClipTop);
    for (const DoneRow& row : rows) {
        const Task* t = f.get(row.id);
        if (!t) continue;
        const Rect& r = row.rect;
        const float chevX = r.x + 3.f;
        const float cy = r.y + 12.f;
        if (row.hasChildren) {
            nvgBeginPath(vg_);
            if (row.expanded) { // down-pointing
                nvgMoveTo(vg_, chevX, cy - 3.f);
                nvgLineTo(vg_, chevX + 8.f, cy - 3.f);
                nvgLineTo(vg_, chevX + 4.f, cy + 3.f);
            } else {            // right-pointing
                nvgMoveTo(vg_, chevX, cy - 4.f);
                nvgLineTo(vg_, chevX + 6.f, cy);
                nvgLineTo(vg_, chevX, cy + 4.f);
            }
            nvgFillColor(vg_, col(cfg.doneTitle));
            nvgFill(vg_);
        }
        const float textX = r.x + 16.f;
        nvgFontFaceId(vg_, font_);
        nvgFontSize(vg_, fontSize_);
        nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg_, col(cfg.doneText, row.depth == 0 ? 1.f : 0.85f));
        nvgTextBox(vg_, textX, r.y + 6.f, std::max(20.f, r.right() - textX - 8.f),
                   t->text.c_str(), end(t->text));
    }
    nvgRestore(vg_);
}

} // namespace tt
