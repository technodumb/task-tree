#include "app/App.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <GLFW/glfw3.h>

#include "app/DevRoute.hpp"
#include "llm/LlmLog.hpp"
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
constexpr double kFlashDuration = 1.6; // seconds the new-task path stays highlighted
constexpr double kSearchDebounce = 0.2; // seconds after typing settles before auto-panning
constexpr double kPanAnimDur = 0.28;    // seconds for the search / new-node camera glide

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
        editingNode_ = 0;
        clearPalette();          // empty bar, no stale command state
        input_.setFocused(true);
        platform_.showOverlay();
    }
}

void App::showQuickAdd() {
    mode_ = Mode::QuickAdd;
    editingNode_ = 0;
    clearPalette();
    input_.setFocused(true);
    platform_.showOverlay();
}

void App::hide() {
    mode_ = Mode::Hidden;
    input_.setFocused(false);
    input_.clear();
    editingNode_ = 0;
    selected_ = 0;
    reparentTarget_ = 0;
    anchorNode_ = 0;
    drag_.cancel();
    cancelPanAnim();
    clearPalette();
    platform_.hideOverlay();
}

// ---- input -----------------------------------------------------------------

void App::onChar(unsigned int codepoint) {
    if (mode_ == Mode::Hidden) return;
    if (tryEnterMode(codepoint)) return;   // ? : > / on an empty bar: mode, not text
    input_.onChar(codepoint);
    updatePalette();
}

void App::onKey(int key, int action, int mods) {
    // Ctrl is the reparent modifier: while it's held with a node selected, the node under
    // the cursor is ringed green as the pending new parent. Handled before the release
    // guard below so the cue also clears when Ctrl comes back up.
    if (key == GLFW_KEY_LEFT_CONTROL || key == GLFW_KEY_RIGHT_CONTROL)
        updateReparentCue(action != GLFW_RELEASE);

    if (action == GLFW_RELEASE || mode_ == Mode::Hidden) return;

    if (key == GLFW_KEY_ESCAPE && drag_.active()) { drag_.cancel(); return; }
    if (key == GLFW_KEY_ESCAPE && editingNode_ != 0) { cancelEditing(); return; }
    // Esc backs out of a palette mode (leaving the overlay open) before it does anything
    // else — the bar is how you got into the mode, so it's how you leave.
    if (key == GLFW_KEY_ESCAPE && pmode_ != palette::Mode::Add) { clearPalette(); return; }
    // Backspace with the argument already empty is the other way out: the prefix isn't in
    // the text, so this is what "deleting" it means. Checked before the delete-subtree
    // shortcut below, which also fires on Backspace with an empty bar.
    if (key == GLFW_KEY_BACKSPACE && pmode_ != palette::Mode::Add && input_.text().empty()) {
        clearPalette();
        return;
    }
    // Esc then clears a selection: a selected node can be dismissed without also closing
    // the overlay (a third Esc, with the bar empty, hides it).
    if (key == GLFW_KEY_ESCAPE && selected_ != 0 && input_.text().empty()) {
        selected_ = 0;
        reparentTarget_ = 0;
        return;
    }
    // F2, or Enter on a selected node with an empty input bar, edits that node's text.
    if (editingNode_ == 0 && selected_ != 0 && forest_.exists(selected_)) {
        const bool enterEmpty = (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) &&
                                input_.text().empty();
        if (key == GLFW_KEY_F2 || enterEmpty) { startEditing(selected_); return; }
    }

    if (mods & GLFW_MOD_CONTROL) {
        if (key == GLFW_KEY_V) {
            if (const char* clip = glfwGetClipboardString(platform_.window())) {
                input_.insert(clip);
                updatePalette();
            }
            return;
        }
        if (key == GLFW_KEY_M) { moveOverlayToNextMonitor(); return; }          // next monitor
        if (key == GLFW_KEY_F && mode_ == Mode::Full) { enterFindMode(); return; } // find
        if (mode_ == Mode::Full && key == GLFW_KEY_Z) {   // undo / redo
            if (mods & GLFW_MOD_SHIFT) redo(); else undo();
            return;
        }
        if (mode_ == Mode::Full && key == GLFW_KEY_Y) { redo(); return; }
    }

    // ↑/↓ walk whichever drop-up is open — node candidates, or the '/' mode menu. (They do
    // nothing in the single-line field otherwise, so nothing is shadowed.)
    if ((key == GLFW_KEY_UP || key == GLFW_KEY_DOWN) &&
        (!candidates_.empty() || !menuItems_.empty())) {
        movePaletteCursor(key == GLFW_KEY_DOWN ? 1 : -1);
        return;
    }

    // Delete / Backspace removes the selected subtree — but only when the input bar is
    // empty (otherwise those keys edit the text being typed). Undo-backed, so no confirm.
    if ((key == GLFW_KEY_DELETE || key == GLFW_KEY_BACKSPACE) &&
        selected_ != 0 && editingNode_ == 0 && input_.text().empty()) {
        deleteSelected();
        return;
    }

    switch (input_.onKey(key, mods)) {
        case TextInput::Action::Submit: commitInput(); break;
        case TextInput::Action::Cancel: hide(); break;
        case TextInput::Action::None:   updatePalette(); break;  // editing keys change the parse
    }
}

// ---- command palette -------------------------------------------------------

bool App::tryEnterMode(unsigned int codepoint) {
    // Prefixes only bite on an empty bar, so anything typed after a character is literal
    // (type a space first to add a task that starts with '?', ':' or '>').
    // Quick-add is add-only (no tree on screen for a command to act on).
    if (mode_ != Mode::Full) return false;
    if (codepoint > 0x7f || !input_.text().empty()) return false;
    const char c = static_cast<char>(codepoint);
    // Inside select/parent, '?' means "force a text query" (':?12'), so it stays literal.
    if (c == '?' && (pmode_ == palette::Mode::Select || pmode_ == palette::Mode::Parent))
        return false;

    const palette::Mode m = palette::modeForPrefix(c);
    if (m == palette::Mode::Add) return false;
    setPaletteMode(m);
    return true;
}

void App::setPaletteMode(palette::Mode m) {
    pmode_ = m;
    input_.clear();          // the prefix is the mode now; the bar holds only its argument
    candidateIdx_ = 0;
    updatePalette();
}

void App::updatePalette() {
    cmd_ = palette::interpret(pmode_, input_.text());

    // The ranking shifts as the query changes, so the ↑/↓ cursor only survives while the
    // query text is identical (e.g. moving the caret, or switching : <-> >).
    if (cmd_.query != lastQuery_) { candidateIdx_ = 0; lastQuery_ = cmd_.query; }

    candidates_.clear();
    menuItems_.clear();
    searchHits_.clear();
    if (pmode_ == palette::Mode::Menu) {
        // The menu is a pure picker: every mode, always, in display order.
        for (const palette::ModeInfo& i : palette::modes()) menuItems_.push_back(i.mode);
    } else if (cmd_.picksByText() && !cmd_.query.empty()) {
        candidates_ = palette::rankMatches(forest_, cmd_.query);
        searchHits_.insert(candidates_.begin(), candidates_.end());
    }
    const std::size_t rows = candidates_.empty() ? menuItems_.size() : candidates_.size();
    if (candidateIdx_ >= rows) candidateIdx_ = (rows == 0) ? 0 : rows - 1;

    // Preview rings, reusing the canvas vocabulary: blue = what Enter will select/jump to,
    // green = where the selection will land (same cue as a Ctrl+click reparent).
    const TaskId target = commandTarget();
    previewSelect_ = (cmd_.selects() || cmd_.kind == palette::Kind::Find) ? target : 0;
    reparentTarget_ = (cmd_.reparents() && canReparent(selected_, target)) ? target : 0;

    // Bring the target into view — but only if it isn't already on screen, and only after
    // typing settles, so the canvas doesn't lurch on every keystroke.
    searchPanDue_ = (target != 0 && !nodeOnScreen(target)) ? glfwGetTime() + kSearchDebounce : 0.0;
}

void App::clearPalette() {
    pmode_ = palette::Mode::Add;   // back to plain "type a task"
    input_.clear();
    candidateIdx_ = 0;
    updatePalette();
}

void App::enterFindMode() {
    // Ctrl+F converts THIS bar into a search bar rather than opening another field. Text
    // already typed becomes the query, so Ctrl+F after typing searches for what you typed.
    const std::string carried = (pmode_ == palette::Mode::Add) ? input_.text() : std::string{};
    setPaletteMode(palette::Mode::Find);
    if (!carried.empty()) { input_.setText(carried); updatePalette(); }
    input_.setFocused(true);
}

void App::movePaletteCursor(int delta) {
    const std::size_t rows = candidates_.empty() ? menuItems_.size() : candidates_.size();
    if (rows == 0) return;
    const int n = static_cast<int>(rows);
    int i = static_cast<int>(candidateIdx_) + delta;
    while (i < 0) i += n;          // wrap both ways: the list is short
    candidateIdx_ = static_cast<std::size_t>(i % n);
    const TaskId target = commandTarget();
    previewSelect_ = (cmd_.selects() || cmd_.kind == palette::Kind::Find) ? target : 0;
    reparentTarget_ = (cmd_.reparents() && canReparent(selected_, target)) ? target : 0;
    // Stepping is a deliberate move, so follow it immediately (no debounce) — but stay put
    // when the candidate is already on screen.
    searchPanDue_ = 0.0;
    if (target != 0 && !nodeOnScreen(target)) focusNode_ = target;
}

TaskId App::activeCandidate() const {
    return candidates_.empty() ? 0 : candidates_[candidateIdx_];
}

TaskId App::commandTarget() const {
    if (cmd_.kind == palette::Kind::SelectId || cmd_.kind == palette::Kind::ParentId)
        return (forest_.exists(cmd_.id) && !forest_.isInDoneSection(cmd_.id)) ? cmd_.id : 0;
    return activeCandidate();
}

void App::runCommand() {
    // The '/' menu doesn't act on the tree: Enter just switches the bar into the mode the
    // ↑/↓ cursor is on.
    if (pmode_ == palette::Mode::Menu) {
        if (candidateIdx_ < menuItems_.size()) setPaletteMode(menuItems_[candidateIdx_]);
        return;
    }
    const TaskId target = commandTarget();
    switch (cmd_.kind) {
        case palette::Kind::AddTask:
            return;
        case palette::Kind::Find:
            // Jump to the active match and stay in find mode (the query is kept, so ↑/↓
            // can keep walking) — the same feel as the old Ctrl+F bar.
            if (target != 0) {
                revealNode(target);
                focusNode_ = target;
            }
            searchPanDue_ = 0.0;
            return;
        case palette::Kind::SelectId:
        case palette::Kind::SelectText:
            if (target == 0) return;          // no such node / no match: leave the bar alone
            selected_ = target;
            revealNode(target);
            if (!nodeOnScreen(target)) focusNode_ = target;
            clearPalette();
            return;
        case palette::Kind::ParentId:
        case palette::Kind::ParentText:
            if (!canReparent(selected_, target)) return;   // inert rather than surprising
            revealNode(target);
            reparentSelected(target);         // undo + anchor + flash all live in there
            clearPalette();
            return;
    }
}

void App::revealNode(TaskId id) {
    // Expand every collapsed ancestor, otherwise the node we just selected/moved to would
    // sit inside a hidden subtree with no rect — no ring, nothing to pan to.
    bool changed = false;
    const Task* t = forest_.get(id);
    for (std::size_t steps = 0; t && t->parent != kNoParent && steps <= forest_.size(); ++steps) {
        Task* p = forest_.get(t->parent);
        if (!p) break;
        if (p->collapsed) { p->collapsed = false; changed = true; }
        t = p;
    }
    if (changed) { forceRelayout(); save(); }
}

palette::Mode App::menuHighlight() const {
    return (candidateIdx_ < menuItems_.size()) ? menuItems_[candidateIdx_] : palette::Mode::Add;
}

Color App::paletteTint() const {
    // Same vocabulary as the canvas: amber = search rings, blue = selection ring,
    // green = drop hint. In the '/' menu the bar takes the highlighted mode's colour, so
    // ↑/↓ previews what you're about to switch into.
    switch (pmode_ == palette::Mode::Menu ? menuHighlight() : pmode_) {
        case palette::Mode::Find:   return {245 / 255.f, 200 / 255.f, 70 / 255.f, 0.86f};
        case palette::Mode::Select: return {120 / 255.f, 175 / 255.f, 255 / 255.f, 1.f};
        case palette::Mode::Parent: return cfg_.dropHint;
        case palette::Mode::Menu:
        case palette::Mode::Add:    break;
    }
    return cfg_.nodeBorder;
}

std::string App::paletteStatus() const {
    // In the menu the bar names the highlighted mode, so the status explains it.
    if (pmode_ == palette::Mode::Menu) {
        const palette::ModeInfo* i = palette::infoFor(menuHighlight());
        return i ? i->blurb : std::string{};
    }
    const auto matches = [this] {
        if (cmd_.query.empty()) return std::string{};
        if (candidates_.empty()) return std::string("no match");
        return std::to_string(candidates_.size()) +
               (candidates_.size() == 1 ? " match" : " matches");
    };
    switch (cmd_.kind) {
        case palette::Kind::Find:
            return matches();
        case palette::Kind::SelectText:
            return matches();
        case palette::Kind::SelectId:
            if (cmd_.id == 0) return "id?";
            return commandTarget() != 0 ? "node " + std::to_string(cmd_.id)
                                        : "no node " + std::to_string(cmd_.id);
        case palette::Kind::ParentId:
        case palette::Kind::ParentText: {
            if (selected_ == 0) return "select a node first";
            const TaskId target = commandTarget();
            if (cmd_.kind == palette::Kind::ParentId && cmd_.id == 0) return "id?";
            if (target == 0) return cmd_.kind == palette::Kind::ParentId
                                  ? "no node " + std::to_string(cmd_.id) : matches();
            if (!canReparent(selected_, target)) return "can't move there";
            return "→ child of " + std::to_string(target);
        }
        case palette::Kind::AddTask:
            break;
    }
    return {};
}

void App::drawPaletteDropUp(const Rect& inputBox) {
    constexpr std::size_t kRows = 4;   // then a "+N more" footer

    // The '/' menu: symbol + what it does. The names stay out of the list — the bar shows
    // the highlighted one, so repeating it in every row is noise.
    if (pmode_ == palette::Mode::Menu) {
        if (menuItems_.empty()) return;
        std::vector<std::string> rows;
        rows.reserve(menuItems_.size());
        for (palette::Mode m : menuItems_) {
            const palette::ModeInfo* i = palette::infoFor(m);
            if (i) rows.push_back(std::string(1, i->prefix) + "   " + i->blurb);
        }
        renderer_.drawPalette(inputBox, rows, static_cast<int>(candidateIdx_), 0,
                              "↑↓ pick · Enter use · Esc cancel", paletteTint(), cfg_);
        return;
    }

    if (candidates_.empty()) return;
    // Window the list so the ↑/↓ cursor stays visible, with one row of lead-in above it.
    const std::size_t n = candidates_.size();
    std::size_t first = (candidateIdx_ > 0) ? candidateIdx_ - 1 : 0;
    if (n > kRows) first = std::min(first, n - kRows);
    else           first = 0;
    const std::size_t shown = std::min(kRows, n - first);

    std::vector<std::string> rows;
    rows.reserve(shown);
    for (std::size_t i = first; i < first + shown; ++i) {
        const Task* t = forest_.get(candidates_[i]);
        rows.push_back("[" + std::to_string(candidates_[i]) + "]  " + (t ? t->text : std::string{}));
    }

    const char* hint = cmd_.reparents() ? "↑↓ pick · Enter make parent · Esc cancel"
                     : cmd_.selects()   ? "↑↓ pick · Enter select · Esc cancel"
                                        : "↑↓ walk · Enter jump · Esc close";
    renderer_.drawPalette(inputBox, rows, static_cast<int>(candidateIdx_ - first),
                          static_cast<int>(n - (first + shown)), hint, paletteTint(), cfg_);
}

bool App::nodeOnScreen(TaskId id) const {
    auto it = rects_.find(id);
    if (it == rects_.end()) return false;   // hidden under a collapsed ancestor
    const Rect& r = it->second;
    const float x0 = pan_.x + zoom_ * r.x, y0 = pan_.y + zoom_ * r.y;
    const float x1 = x0 + zoom_ * r.w, y1 = y0 + zoom_ * r.h;
    return x1 > 0.f && y1 > 0.f && x0 < static_cast<float>(lastWinW_) &&
           y0 < static_cast<float>(lastWinH_);
}

void App::onMouseButton(int button, int action, int mods) {
    if (mode_ != Mode::Full) return;

    // Clicking anywhere while editing commits the pending text edit first, then the click
    // is handled normally (select another node, pan, etc.).
    if (editingNode_ != 0 && action == GLFW_PRESS) commitEdit(trim(input_.text()));

    // Right-click a canvas node to cycle its status colour: default -> yellow
    // (in progress) -> orange (priority) -> default.
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        if (action == GLFW_PRESS && !pointInPanel(mouse_)) {
            const TaskId id = hitTest(worldMouse());
            if (Task* t = forest_.get(id)) { history_.snapshot(forest_); t->status = (t->status + 1) % 3; save(); }
        }
        return;
    }

    // Pan the canvas with middle-drag. (Alt+drag is intentionally NOT used — the window
    // manager grabs Alt+drag to move the whole window; left-drag on empty canvas pans.)
    if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        if (action == GLFW_PRESS) { cancelPanAnim(); panning_ = true; panGrab_ = mouse_; panOrigin_ = pan_; }
        else if (action == GLFW_RELEASE) panning_ = false;
        return;
    }
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;

    if (action == GLFW_PRESS) {
        // Autohide toggle button.
        if (donePanel_.visible && donePanel_.pinButton.contains(mouse_.x, mouse_.y)) {
            pinned_ = !pinned_;
            return;
        }
        // Ctrl+click a canvas node: move the selected subtree in as that node's last
        // child. Tested before the double-click and collapse-handle paths so a quick
        // chain of Ctrl+clicks can't be read as a double-click (which marks a node DONE).
        if ((mods & GLFW_MOD_CONTROL) && selected_ != 0 && !pointInPanel(mouse_)) {
            const TaskId target = reparentTargetAt(worldMouse());
            if (target != 0) {
                reparentSelected(target);
                lastClickTime_ = 0.0;   // this click doesn't arm a double-click
                lastClickPos_ = mouse_;
                return;
            }
        }
        // Double-click (same spot, within 400 ms).
        const double t = glfwGetTime();
        const bool dbl = (t - lastClickTime_ < 0.40) &&
                         std::fabs(mouse_.x - lastClickPos_.x) < 8.f &&
                         std::fabs(mouse_.y - lastClickPos_.y) < 8.f;
        lastClickTime_ = dbl ? 0.0 : t;
        lastClickPos_ = mouse_;
        if (dbl) { handleDoubleClick(); return; }
        // Collapse/expand handle (world space) wins over selecting/dragging the node.
        if (!pointInPanel(mouse_)) {
            const TaskId cId = collapseHandleHit(worldMouse());
            if (cId != 0) { toggleCollapse(cId); return; }
        }
        // Single press: DONE panel toggles a row's children (screen space); on the
        // canvas, a node starts a drag, empty space starts a pan (world space).
        if (pointInPanel(mouse_)) {
            const TaskId id = hitTestDone(mouse_);
            if (id != 0) {
                if (doneExpanded_.count(id)) doneExpanded_.erase(id);
                else                         doneExpanded_.insert(id);
            }
        } else {
            const TaskId id = hitTest(worldMouse());
            if (id != 0) {
                auto it = rects_.find(id);
                if (it != rects_.end()) { cancelPanAnim(); drag_.begin(id, worldMouse(), it->second); }
            } else {
                cancelPanAnim();
                selected_ = 0;   // pressing empty canvas clears the selection
                reparentTarget_ = 0;
                panning_ = true; panGrab_ = mouse_; panOrigin_ = pan_; // drag empty bg to pan
            }
        }
    } else if (action == GLFW_RELEASE) {
        if (panning_) { panning_ = false; return; }
        if (drag_.active()) {
            const TaskId node = drag_.dragged();
            Forest before = forest_;   // snapshot only if the drop actually reparents
            if (drag_.drop(forest_)) { history_.record(std::move(before)); forceRelayout(); save(); }
            selected_ = node;          // clicking (or dragging) a node selects it
            // A pending '>' command targets the selection, so its preview needs the new one.
            if (cmd_.isCommand()) updatePalette();
        }
    }
}

void App::onCursorPos(double x, double y) {
    mouse_ = {static_cast<float>(x), static_cast<float>(y)};
    if (panning_) {
        pan_ = {panOrigin_.x + (mouse_.x - panGrab_.x), panOrigin_.y + (mouse_.y - panGrab_.y)};
        return;
    }
    // Autohide reveal with hysteresis. Only within the overlay's own bounds: if the
    // cursor is reported past the right edge it's on a monitor to the right, not asking
    // for the panel — hide rather than pin (paired with onCursorEnter's leave handling).
    if (mode_ == Mode::Full && lastWinW_ > 0) {
        const float w = static_cast<float>(lastWinW_);
        if (mouse_.x > w) doneHover_ = false;                // on a monitor to the right
        else if (mouse_.x >= w * 0.85f) doneHover_ = true;   // within the right 15%
        else if (mouse_.x < w * 0.83f && !pointInPanel(mouse_))
            doneHover_ = false;                              // 17% out AND off the panel
    }
    if (drag_.active()) drag_.update(worldMouse(), forest_, rects_, params_);
    else                updateReparentCue(ctrlHeld());
}

void App::onCursorEnter(bool entered) {
    // The overlay only covers the primary monitor. When the cursor leaves it (e.g. moving
    // onto a monitor to the right), we stop getting move events, so the autohide reveal
    // would otherwise stay stuck open. Drop it on leave — PIN still overrides autohide.
    if (!entered) doneHover_ = false;
}

void App::onScroll(double dx, double dy) {
    if (mode_ != Mode::Full) return;
    // Over the DONE panel: scroll its list.
    if (donePanel_.visible && pointInPanel(mouse_)) {
        scrollY_ -= static_cast<float>(dy) * 48.f;
        scrollY_ = std::max(0.f, std::min(scrollY_, doneMaxScroll_));
        return;
    }
    // Canvas wheel behaviour is configurable (config [input] scroll_mode).
    if (cfg_.scrollMode == "off") return;

    GLFWwindow* w = platform_.window();
    const bool ctrl = glfwGetKey(w, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                      glfwGetKey(w, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    const bool shift = glfwGetKey(w, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                       glfwGetKey(w, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;

    // "zoom": wheel zooms. "pan": wheel pans, but Ctrl+wheel still zooms.
    const bool doZoom = (cfg_.scrollMode == "zoom") || (cfg_.scrollMode == "pan" && ctrl);
    if (doZoom) {
        if (dy == 0.0) return;
        cancelPanAnim();
        const float factor = (dy > 0.0) ? 1.1f : 1.f / 1.1f;
        const float nz = std::max(0.3f, std::min(3.0f, zoom_ * factor));
        const float wx = (mouse_.x - pan_.x) / zoom_; // keep the world point under the cursor fixed
        const float wy = (mouse_.y - pan_.y) / zoom_;
        pan_.x = mouse_.x - nz * wx;
        pan_.y = mouse_.y - nz * wy;
        zoom_ = nz;
        return;
    }

    if (cfg_.scrollMode == "pan") {
        cancelPanAnim();
        const float step = 48.f;
        pan_.x += static_cast<float>(dx) * step;
        if (shift) pan_.x += static_cast<float>(dy) * step;
        else       pan_.y += static_cast<float>(dy) * step;
    }
}

void App::moveOverlayToNextMonitor() {
    platform_.moveToNextMonitor();
    if (mode_ == Mode::Hidden) {
        mode_ = Mode::Full;
        input_.clear();
        input_.setFocused(true);
        platform_.showOverlay();
    }
}

// ---- task creation + classification ----------------------------------------

void App::commitInput() {
    if (editingNode_ != 0) { commitEdit(trim(input_.text())); return; }

    // In a palette mode the bar isn't a task at all — Enter runs the command (app/Palette.hpp).
    updatePalette();
    if (pmode_ != palette::Mode::Add) { runCommand(); return; }

    const std::string txt = cmd_.body;
    if (txt.empty()) { if (mode_ == Mode::QuickAdd) hide(); return; }

    history_.snapshot(forest_);   // undo checkpoint before the add
    TaskId id;
    if (isDevTask(txt)) {
        // Dev fast path: TaskTree's own to-dos ("ttd> ...") park directly under the
        // "tasktree dev" node and skip LLM classification entirely.
        id = forest_.addTask(txt, ensureDevRoot(forest_, nowMs()), nowMs());
    } else {
        id = forest_.addTask(txt, kNoParent, nowMs());
        if (classifier_.enabled()) {
            classifier_.classify(txt, buildTreeOutline(id),
                                  [this, id](ClassifyResult r) { pushClassification(id, r); });
        }
    }

    input_.clear();
    forceRelayout();
    flashPath(id);   // briefly show where the new node landed (standalone -> just itself)
    focusNode_ = id; // pan the canvas to bring the new node into view
    save();
    if (mode_ == Mode::QuickAdd) hide();
}

void App::flashPath(TaskId leaf) {
    highlightSet_.clear();
    TaskId cur = leaf;
    for (std::size_t i = 0; cur != kNoParent && i <= forest_.size(); ++i) {
        const Task* t = forest_.get(cur);
        if (!t) break;
        highlightSet_.insert(cur);
        cur = t->parent;
    }
    highlightUntil_ = glfwGetTime() + kFlashDuration;
}

std::string App::buildTreeOutline(TaskId exclude) const {
    // One line per canvas node: "[id] parent=<pid|none>: text". Explicit parent ids
    // (not indentation) so the model reconstructs the tree unambiguously. Pre-order
    // over canvas roots (DONE roots live in a separate list, so they're excluded).
    std::string out;
    std::vector<TaskId> stack;
    for (auto it = forest_.roots.rbegin(); it != forest_.roots.rend(); ++it)
        stack.push_back(*it);
    while (!stack.empty()) {
        const TaskId id = stack.back();
        stack.pop_back();
        if (id == exclude) continue; // skip the just-created node
        const Task* t = forest_.get(id);
        if (!t) continue;
        out += "[" + std::to_string(id) + "] parent=";
        out += (t->parent == kNoParent) ? "none" : std::to_string(t->parent);
        out += ": " + t->text + "\n";
        for (auto cit = t->children.rbegin(); cit != t->children.rend(); ++cit)
            stack.push_back(*cit);
    }
    return out;
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
    if (local.empty()) return;
    Forest before = forest_;   // pre-batch undo checkpoint (recorded only if something moves)
    const bool L = llmlog::enabled();
    auto textOf = [&](TaskId t) {
        const Task* n = forest_.get(t);
        return n ? n->text : std::string("<gone>");
    };
    bool changed = false;
    TaskId flashLeaf = 0;
    for (const auto& [id, r] : local) {
        if (r.relation == Relation::Standalone || r.targetId == 0) continue; // classifier logged it
        if (!forest_.exists(id)) {
            if (L) llmlog::write("APPLY skipped: new task " + std::to_string(id) + " no longer exists");
            continue;
        }
        if (!forest_.exists(r.targetId)) {
            if (L) llmlog::write("APPLY rejected: target id " + std::to_string(r.targetId) +
                                 " does not exist (kept '" + textOf(id) + "' standalone)");
            continue;
        }
        if (forest_.isInDoneSection(r.targetId)) { // never attach to / resurrect DONE tasks
            if (L) llmlog::write("APPLY rejected: target id " + std::to_string(r.targetId) +
                                 " is in the DONE section (kept '" + textOf(id) + "' standalone)");
            continue;
        }
        const bool childOf = (r.relation == Relation::ChildOf);
        bool c = false;
        if (childOf) {
            c = forest_.reparent(id, r.targetId, kAppendIndex);
        } else {
            // parent_of: insert the new task as target's parent — it takes target's
            // current slot (under target's old parent) and target becomes its child.
            // This is how a node lands BETWEEN an existing task and its parent.
            const TaskId oldParent = forest_.get(r.targetId)->parent;
            c = forest_.reparent(id, oldParent, kAppendIndex) &&
                forest_.reparent(r.targetId, id, kAppendIndex);
        }
        if (c) {
            changed = true;
            flashLeaf = id;
            if (L) llmlog::write("APPLY: '" + textOf(id) + "' (id " + std::to_string(id) + ") " +
                                 (childOf ? "-> child of '" : "-> parent of '") + textOf(r.targetId) +
                                 "' (id " + std::to_string(r.targetId) + ")");
        } else if (L) {
            llmlog::write("APPLY rejected: reparent failed (cycle / invalid) new=" +
                          std::to_string(id) + " target=" + std::to_string(r.targetId));
        }
    }
    if (changed) {
        history_.record(std::move(before));
        forceRelayout(); save(); flashPath(flashLeaf); focusNode_ = flashLeaf;
    }
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
    if (!drag_.active() || !drag_.moved()) return dv;
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

void App::startPanTo(Vec2 target) {
    const float dx = target.x - pan_.x, dy = target.y - pan_.y;
    if (dx * dx + dy * dy < 1.f) { pan_ = target; panAnimActive_ = false; return; } // already there
    panFrom_ = pan_;
    panTo_ = target;
    panAnimStart_ = glfwGetTime();
    panAnimActive_ = true;
}

void App::drawScene(int winW, int winH, float dpr) {
    lastWinW_ = winW;
    lastWinH_ = winH;
    // Centre the forest across the full window (roots -> true top-centre). The DONE
    // panel is translucent and autohides, so it overlays rather than reserving space.
    const float cw = static_cast<float>(winW);
    if (params_.centerWidth != cw) { params_.centerWidth = cw; needsRelayout_ = true; }
    relayoutIfNeeded();

    // Anchored relayout (Ctrl+click reparent): the fresh rects have re-packed the tree, so
    // put the anchored node back where it was on screen by shifting the camera the same
    // amount the node moved. The cursor keeps pointing at it, ready for the next Ctrl+click.
    if (anchorNode_ != 0) {
        if (auto it = rects_.find(anchorNode_); it != rects_.end()) {
            cancelPanAnim();   // an instant correction, not a glide
            pan_ = {anchorScreen_.x - zoom_ * it->second.cx(),
                    anchorScreen_.y - zoom_ * it->second.cy()};
        }
        anchorNode_ = 0;
    }

    // Debounced palette pan: once typing settles, bring the command's target into view.
    if (searchPanDue_ != 0.0 && glfwGetTime() >= searchPanDue_) {
        searchPanDue_ = 0.0;
        const TaskId target = commandTarget();
        if (target != 0 && !nodeOnScreen(target)) focusNode_ = target;
    }

    // Pan to bring a just-added / just-reparented / searched node into view (centre-ish,
    // above the input bar). Done after relayout so its rect is current. Rather than
    // snapping, start a smooth glide — the search auto-pan and the new-node pan both
    // land here, so both animate.
    if (focusNode_ != 0) {
        auto it = rects_.find(focusNode_);
        if (it != rects_.end())
            startPanTo({winW * 0.5f - zoom_ * it->second.cx(),
                        winH * 0.4f - zoom_ * it->second.cy()});
        focusNode_ = 0;
    }

    // Advance the camera glide (ease-out cubic). The loop wakes at ~60fps while it runs
    // (desiredTimeout), then drops back to idle once pan_ reaches the target.
    if (panAnimActive_) {
        const double t = (glfwGetTime() - panAnimStart_) / kPanAnimDur;
        if (t >= 1.0) { pan_ = panTo_; panAnimActive_ = false; }
        else {
            const float e = static_cast<float>(1.0 - std::pow(1.0 - t, 3.0));
            pan_.x = panFrom_.x + (panTo_.x - panFrom_.x) * e;
            pan_.y = panFrom_.y + (panTo_.y - panFrom_.y) * e;
        }
    }

    layoutDonePanel(winW, winH);

    renderer_.beginFrame(winW, winH, dpr);
    if (mode_ == Mode::Full) {
        renderer_.drawScrim(static_cast<float>(winW), static_cast<float>(winH), cfg_);
        DragVisual dv = buildDragVisual();
        const auto& drawRects = dv.active ? previewRects_ : rects_;
        const double now = glfwGetTime();
        float hi = 0.f;
        if (!highlightSet_.empty()) {
            if (now < highlightUntil_) hi = static_cast<float>((highlightUntil_ - now) / kFlashDuration);
            else highlightSet_.clear();
        }
        // While a palette command is pending, the blue ring previews what Enter will act on
        // rather than the current selection (for '>' the selection keeps its own ring and
        // the target gets the green one).
        const TaskId ringSel = previewSelect_ != 0 ? previewSelect_ : selected_;
        renderer_.drawTree(forest_, drawRects, cfg_, dv, pan_, zoom_, highlightSet_, hi, searchHits_,
                           ringSel, reparentTarget_);
        if (donePanel_.visible)
            renderer_.drawDonePanel(donePanel_, forest_, doneRows_, cfg_);
        InputStyle style;
        style.editing = editingNode_ != 0;
        // In the '/' menu the bar isn't an editor: it just names the highlighted mode (no
        // caret, no argument — the drop-up carries the explanations).
        const bool menu = pmode_ == palette::Mode::Menu;
        std::string barText = input_.text();
        if (pmode_ != palette::Mode::Add) {
            const palette::ModeInfo* info = palette::infoFor(menu ? menuHighlight() : pmode_);
            style.tinted = true;
            style.border = paletteTint();
            style.chip = info ? (menu ? std::string(info->name) : std::string(info->name) + ":")
                              : std::string{};
            if (menu) {
                barText.clear();
            } else {
                style.placeholder = info ? info->hint : std::string{};
                style.status = paletteStatus();
            }
        }
        const Rect box = renderer_.drawInput(winW, winH, barText, barText.size(),
                                             caretOn() && !menu, cfg_, style);
        drawPaletteDropUp(box);
    } else if (mode_ == Mode::QuickAdd) {
        InputStyle style;
        style.quickAdd = true;
        renderer_.drawInput(winW, winH, input_.text(), input_.caret(), caretOn(), cfg_, style);
    }
    renderer_.endFrame();
}

void App::layoutDonePanel(int winW, int winH) {
    DonePanelLayout L;
    L.pinned = pinned_;
    L.visible = (mode_ == Mode::Full) && (pinned_ || doneHover_);
    // Floating rounded card: inset from the screen edges so it reads as a raised
    // surface (with a drop shadow), consistent with the input/search boxes.
    const float margin = 10.f;
    const float pw = std::max(winW * 0.17f, 312.f);
    L.panel = {winW - pw - margin, margin, pw, winH - 2 * margin};
    const float titleH = 46.f;
    L.titleBar = {L.panel.x, L.panel.y, pw, titleH};
    const float btnW = 70.f, btnH = 24.f;
    L.pinButton = {L.panel.right() - btnW - 14.f, L.panel.y + (titleH - btnH) * 0.5f, btnW, btnH};
    L.contentClipTop = L.panel.y + titleH;
    L.contentClipBottom = L.panel.bottom() - 8.f;
    for (const auto& [id, t] : forest_.nodes)
        if (t.done) ++L.itemCount;
    donePanel_ = L;

    // Measure card heights, clamp scroll, then position (screen coords, scrolled).
    // Flatten the expanded DONE tree in display order (pre-order) with depth. Top-level
    // roots are shown latest-completed first (display-only sort; the persisted doneRoots
    // order is left untouched). Primary key is doneAt (newest first); items with an
    // unknown doneAt (0, completed before the field existed) fall back to completion
    // order — markDone appends, so reversing doneRoots puts the most recent first, and a
    // stable sort keeps that fallback order within the undated group.
    std::vector<TaskId> order(forest_.doneRoots.rbegin(), forest_.doneRoots.rend());
    std::stable_sort(order.begin(), order.end(), [&](TaskId a, TaskId b) {
        const Task* ta = forest_.get(a);
        const Task* tb = forest_.get(b);
        return (ta ? ta->doneAt : 0) > (tb ? tb->doneAt : 0);
    });
    std::vector<std::pair<TaskId, int>> flat;
    {
        std::vector<std::pair<TaskId, int>> stack;
        for (auto it = order.rbegin(); it != order.rend(); ++it)
            stack.emplace_back(*it, 0);
        while (!stack.empty()) {
            const auto [id, depth] = stack.back();
            stack.pop_back();
            const Task* t = forest_.get(id);
            if (!t) continue;
            flat.emplace_back(id, depth);
            if (doneExpanded_.count(id))
                for (auto cit = t->children.rbegin(); cit != t->children.rend(); ++cit)
                    stack.emplace_back(*cit, depth + 1);
        }
    }

    const float pad = 14.f, gap = 9.f, indent = 18.f;
    const float glyphGutter = 18.f;   // space before text for the chevron/check
    auto rowX = [&](int depth) { return L.panel.x + pad + depth * indent; };
    auto textW = [&](int depth) {
        return std::max(30.f, L.panel.right() - pad - (rowX(depth) + glyphGutter));
    };

    // Measure heights (roomier vertical padding per row -> calmer, more modern spacing).
    std::vector<float> heights;
    heights.reserve(flat.size());
    float totalH = 0.f;
    for (const auto& [id, depth] : flat) {
        const Task* t = forest_.get(id);
        float h = renderer_.measureTextHeight(t ? t->text : std::string{}, textW(depth)) + 22.f;
        h = std::max(h, 40.f);
        heights.push_back(h);
        totalH += h + gap;
    }
    const float visibleH = L.contentClipBottom - L.contentClipTop;
    doneMaxScroll_ = std::max(0.f, totalH - visibleH);
    scrollY_ = std::max(0.f, std::min(scrollY_, doneMaxScroll_));

    // Position rows.
    const bool canHover = donePanel_.visible && pointInPanel(mouse_) &&
                          mouse_.y >= L.contentClipTop && mouse_.y <= L.contentClipBottom;
    doneRows_.clear();
    doneRows_.reserve(flat.size());
    float y = L.contentClipTop + 6.f - scrollY_;
    for (std::size_t i = 0; i < flat.size(); ++i) {
        const TaskId id = flat[i].first;
        const int depth = flat[i].second;
        const Task* t = forest_.get(id);
        DoneRow row;
        row.id = id;
        row.depth = depth;
        row.hasChildren = t && !t->children.empty();
        row.expanded = doneExpanded_.count(id) != 0;
        row.rect = Rect{rowX(depth), y, L.panel.right() - pad - rowX(depth), heights[i]};
        row.hovered = canHover && mouse_.y >= y && mouse_.y < y + heights[i];
        doneRows_.push_back(row);
        y += heights[i] + gap;
    }
}

bool App::pointInPanel(Vec2 p) const {
    return donePanel_.visible && donePanel_.panel.contains(p.x, p.y);
}

TaskId App::hitTestDone(Vec2 p) const {
    if (!pointInPanel(p)) return 0;
    if (p.y < donePanel_.contentClipTop || p.y > donePanel_.contentClipBottom) return 0;
    for (const DoneRow& row : doneRows_)
        if (row.rect.contains(p.x, p.y)) return row.id;
    return 0;
}

void App::handleDoubleClick() {
    if (pointInPanel(mouse_)) {
        const TaskId id = hitTestDone(mouse_);
        if (id != 0) {
            Forest before = forest_;
            if (forest_.restoreFromDone(id)) {
                history_.record(std::move(before));
                doneExpanded_.erase(id);
                forceRelayout();
                save();
            }
        }
    } else {
        const TaskId id = hitTest(worldMouse());
        if (id != 0) {
            if (drag_.active()) drag_.cancel();
            Forest before = forest_;
            if (forest_.markDone(id)) {
                history_.record(std::move(before));
                if (Task* t = forest_.get(id)) t->doneAt = nowMs();  // record done date
                forceRelayout();
                save();
            }
        } else {
            cancelPanAnim();
            pan_ = {0.f, 0.f}; // double-click empty canvas recenters + resets zoom
            zoom_ = 1.f;
        }
    }
}

TaskId App::hitTest(Vec2 p) const {
    for (const auto& [id, r] : rects_)
        if (pointInRoundedRect(r, cfg_.cornerRadius, p.x, p.y)) return id;
    return 0;
}

TaskId App::collapseHandleHit(Vec2 p) const {
    for (const auto& [id, r] : rects_) {
        const Task* t = forest_.get(id);
        if (!t || t->children.empty()) continue;   // handle only shown on nodes with children
        if (collapseHandle(r).contains(p.x, p.y)) return id;
    }
    return 0;
}

void App::toggleCollapse(TaskId id) {
    Task* t = forest_.get(id);
    if (!t || t->children.empty()) return;
    t->collapsed = !t->collapsed;   // a view toggle, not a structural edit -> no undo entry
    forceRelayout();
    save();                         // collapsed state is persisted in the model
}

bool App::caretOn() const {
    return mode_ != Mode::Hidden && std::fmod(glfwGetTime(), 1.0) < 0.5;
}

double App::desiredTimeout() const {
    if (drag_.active()) return 0.0;        // poll for smooth drag
    if (panAnimActive_) return 1.0 / 60.0; // ~60fps while the camera glides to a node
    if (searchPanDue_ != 0.0)              // wake exactly when the debounced pan is due
        return std::max(0.0, searchPanDue_ - glfwGetTime());
    if (!highlightSet_.empty() && glfwGetTime() < highlightUntil_) return 0.03; // animate flash
    if (mode_ != Mode::Hidden) return 0.5; // caret blink while visible
    return -1.0;                           // block until an event/hotkey
}

void App::save() { store::save(forest_, tasksPath_); }

void App::undo() {
    if (!history_.undo(forest_)) return;
    afterHistoryChange();
}

void App::redo() {
    if (!history_.redo(forest_)) return;
    afterHistoryChange();
}

bool App::ctrlHeld() const {
    GLFWwindow* w = platform_.window();
    return glfwGetKey(w, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
           glfwGetKey(w, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
}

// Shared by the Ctrl+click cue and the palette's '>' commands, so mouse and keyboard
// reject exactly the same moves.
bool App::canReparent(TaskId child, TaskId newParent) const {
    if (child == 0 || newParent == 0 || child == newParent) return false;
    if (!forest_.exists(child) || !forest_.exists(newParent)) return false;
    if (forest_.isInDoneSection(newParent)) return false;        // never attach to DONE
    if (forest_.get(child)->parent == newParent) return false;   // already this node's child
    return !forest_.isDescendantOf(newParent, child);            // no cycles
}

TaskId App::reparentTargetAt(Vec2 world) const {
    const TaskId id = hitTest(world);
    return canReparent(selected_, id) ? id : 0;
}

void App::updateReparentCue(bool ctrl) {
    reparentTarget_ = (ctrl && mode_ == Mode::Full && !drag_.active() && !pointInPanel(mouse_))
                          ? reparentTargetAt(worldMouse())
                          : 0;
}

void App::reparentSelected(TaskId newParent) {
    const TaskId child = selected_;
    if (!canReparent(child, newParent)) return;
    if (drag_.active()) drag_.cancel();
    const int index = static_cast<int>(forest_.get(newParent)->children.size()); // append last
    // Pin the new parent to where it is right now (pre-move screen position). The relayout
    // this move triggers re-packs the tree; drawScene shifts the camera to compensate so
    // this node — the one under the cursor — does not visibly move. If the parent isn't on
    // screen at all (a '>' palette command can name a far-away node) there's nothing to hold
    // still, so glide to it instead: otherwise the move happens out of sight.
    if (nodeOnScreen(newParent)) {
        const Rect& r = rects_.at(newParent);
        anchorNode_ = newParent;
        anchorScreen_ = {pan_.x + zoom_ * r.cx(), pan_.y + zoom_ * r.cy()};
    } else {
        focusNode_ = newParent;
    }
    Forest before = forest_;
    if (!forest_.reparent(child, newParent, index)) return;  // self/cycle rejected by the model
    // A collapsed parent would swallow the node it just received: open it so the move shows.
    forest_.get(newParent)->collapsed = false;
    history_.record(std::move(before));
    reparentTarget_ = 0;      // the target is now the parent -> no longer a valid target
    forceRelayout();
    flashPath(child);         // flash root -> moved node so the new position is obvious
    save();
}

void App::startEditing(TaskId id) {
    const Task* t = forest_.get(id);
    if (!t) return;
    editingNode_ = id;
    selected_ = id;
    input_.setText(t->text);   // seed with current text, caret at end
    input_.setFocused(true);
}

void App::commitEdit(const std::string& txt) {
    const TaskId id = editingNode_;
    editingNode_ = 0;
    input_.clear();
    Task* t = forest_.get(id);
    if (!t) return;
    if (txt.empty() || txt == t->text) return;  // empty or unchanged -> leave the node as-is
    history_.snapshot(forest_);                  // undo checkpoint before the text change
    t->text = txt;
    forceRelayout();  // text changed -> re-measure box + relayout
    save();
}

void App::cancelEditing() {
    editingNode_ = 0;
    input_.clear();  // node text untouched; selection stays
}

void App::deleteSelected() {
    if (selected_ == 0 || !forest_.exists(selected_)) { selected_ = 0; return; }
    if (drag_.active()) drag_.cancel();
    history_.snapshot(forest_);          // undo checkpoint before removal
    const TaskId victim = selected_;
    selected_ = 0;
    reparentTarget_ = 0;
    doneExpanded_.erase(victim);
    forest_.removeSubtree(victim);       // removes the node and its whole subtree
    forceRelayout();
    updatePalette();                     // candidates may name nodes that just vanished
    save();
}

void App::afterHistoryChange() {
    drag_.cancel();
    focusNode_ = 0;
    reparentTarget_ = 0;
    if (!forest_.exists(selected_)) selected_ = 0;  // don't keep a ring on a vanished node
    forceRelayout();
    updatePalette();   // candidates/previews may reference now-removed or restored nodes
    save();
}

} // namespace tt
