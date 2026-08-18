#pragma once
// Single-line UTF-8 text field. Caret is a byte offset that always sits on a UTF-8
// code-point boundary. GLFW-agnostic API (onKey takes an int keycode) so the model
// is testable; the .cpp maps GLFW key constants internally.
//
// Selection: a second byte offset, the anchor, marks the other end of the highlighted
// range. The selection is [selBegin(), selEnd()); it is empty when anchor == caret.
// Shift+motion extends it, plain motion collapses it, and typing / paste / Backspace /
// Delete all replace it — i.e. the field behaves like a normal OS text box.

#include <cstddef>
#include <string>
#include <utility>

namespace tt {

class TextInput {
public:
    enum class Action { None, Submit, Cancel };

    void clear() { text_.clear(); caret_ = 0; anchor_ = 0; }
    void setText(const std::string& s) { text_ = s; caret_ = s.size(); anchor_ = caret_; } // seed + caret to end
    bool focused() const { return focused_; }
    void setFocused(bool f) { focused_ = f; }

    void onChar(unsigned int codepoint);          // printable input (replaces any selection)
    Action onKey(int glfwKey, int glfwMods);      // editing / motion / submit / cancel keys
    void insert(const std::string& utf8);         // e.g. clipboard paste (replaces any selection)

    const std::string& text() const { return text_; }
    std::size_t caret() const { return caret_; }

    // Selection.
    bool hasSelection() const { return anchor_ != caret_; }
    std::size_t selBegin() const { return anchor_ < caret_ ? anchor_ : caret_; }
    std::size_t selEnd()   const { return anchor_ < caret_ ? caret_ : anchor_; }
    std::string selectedText() const { return text_.substr(selBegin(), selEnd() - selBegin()); }
    void selectAll() { anchor_ = 0; caret_ = text_.size(); }
    void deleteSelection();                            // erase it; caret ends at its start

    // Mouse-driven caret placement. `byte` is snapped to a code-point boundary. When
    // `extend` is true the anchor is kept (drag / shift-click), otherwise it collapses.
    void moveCaretTo(std::size_t byte, bool extend);
    void selectWordAt(std::size_t byte);               // double-click: the run around `byte`

private:
    std::size_t prevBoundary(std::size_t i) const;
    std::size_t nextBoundary(std::size_t i) const;
    std::size_t prevWordStart(std::size_t i) const;  // Ctrl+Backspace / Ctrl+Left
    std::size_t nextWordEnd(std::size_t i) const;     // Ctrl+Delete
    std::size_t nextWordStart(std::size_t i) const;   // Ctrl+Right
    std::size_t alignBoundary(std::size_t i) const;   // round down to a code-point boundary
    std::pair<std::size_t, std::size_t> wordBounds(std::size_t i) const;

    std::string text_;
    std::size_t caret_ = 0;
    std::size_t anchor_ = 0;   // other end of the selection; == caret_ means no selection
    bool focused_ = false;
};

} // namespace tt
