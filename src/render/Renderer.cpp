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
                        bool highlight, float alphaMul) const {
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
}

void Renderer::drawTree(const Forest& f, const std::unordered_map<TaskId, Rect>& rects,
                        const Config& cfg, const DragVisual& dv) {
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
            drawNode(*r, t.text, cfg, hi, 1.f);
        }
    }

    // Ghost of the dragged node, following the cursor.
    if (dv.active) {
        const Task* t = f.get(dv.dragged);
        drawNode(dv.ghost, t ? t->text : std::string{}, cfg, false, 0.85f);
    }
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

} // namespace tt
