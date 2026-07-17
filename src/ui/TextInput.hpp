#pragma once
// Single-line UTF-8 text field. Caret is a byte offset that always sits on a UTF-8
// code-point boundary. GLFW-agnostic API (onKey takes an int keycode) so the model
// is testable; the .cpp maps GLFW key constants internally.

#include <cstddef>
#include <string>

namespace tt {

class TextInput {
public:
    enum class Action { None, Submit, Cancel };

    void clear() { text_.clear(); caret_ = 0; }
    void setText(const std::string& s) { text_ = s; caret_ = s.size(); } // seed + caret to end
    bool focused() const { return focused_; }
    void setFocused(bool f) { focused_ = f; }

    void onChar(unsigned int codepoint);          // printable input
    Action onKey(int glfwKey, int glfwMods);      // editing / submit / cancel keys
    void insert(const std::string& utf8);         // e.g. clipboard paste

    const std::string& text() const { return text_; }
    std::size_t caret() const { return caret_; }

private:
    std::size_t prevBoundary(std::size_t i) const;
    std::size_t nextBoundary(std::size_t i) const;
    std::size_t prevWordStart(std::size_t i) const;  // Ctrl+Backspace / Ctrl+Left
    std::size_t nextWordEnd(std::size_t i) const;     // Ctrl+Delete
    std::size_t nextWordStart(std::size_t i) const;   // Ctrl+Right

    std::string text_;
    std::size_t caret_ = 0;
    bool focused_ = false;
};

} // namespace tt
