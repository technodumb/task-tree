#include "ui/TextInput.hpp"

#include <GLFW/glfw3.h>

namespace tt {
namespace {

// Append the UTF-8 encoding of `cp` to `s`.
void appendUtf8(std::string& s, unsigned int cp) {
    if (cp < 0x80) {
        s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool isContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

} // namespace

std::size_t TextInput::prevBoundary(std::size_t i) const {
    if (i == 0) return 0;
    --i;
    while (i > 0 && isContinuation(static_cast<unsigned char>(text_[i]))) --i;
    return i;
}

std::size_t TextInput::nextBoundary(std::size_t i) const {
    if (i >= text_.size()) return text_.size();
    ++i;
    while (i < text_.size() && isContinuation(static_cast<unsigned char>(text_[i]))) ++i;
    return i;
}

void TextInput::onChar(unsigned int codepoint) {
    std::string enc;
    appendUtf8(enc, codepoint);
    text_.insert(caret_, enc);
    caret_ += enc.size();
}

void TextInput::insert(const std::string& utf8) {
    // Drop control characters (e.g. newlines from a multi-line clipboard).
    std::string clean;
    clean.reserve(utf8.size());
    for (char c : utf8)
        if (static_cast<unsigned char>(c) >= 0x20 || (c & 0x80)) clean.push_back(c);
    text_.insert(caret_, clean);
    caret_ += clean.size();
}

TextInput::Action TextInput::onKey(int key, int /*mods*/) {
    switch (key) {
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER:
            return Action::Submit;
        case GLFW_KEY_ESCAPE:
            return Action::Cancel;
        case GLFW_KEY_BACKSPACE:
            if (caret_ > 0) {
                const std::size_t from = prevBoundary(caret_);
                text_.erase(from, caret_ - from);
                caret_ = from;
            }
            break;
        case GLFW_KEY_DELETE:
            if (caret_ < text_.size()) {
                const std::size_t to = nextBoundary(caret_);
                text_.erase(caret_, to - caret_);
            }
            break;
        case GLFW_KEY_LEFT:  caret_ = prevBoundary(caret_); break;
        case GLFW_KEY_RIGHT: caret_ = nextBoundary(caret_); break;
        case GLFW_KEY_HOME:  caret_ = 0; break;
        case GLFW_KEY_END:   caret_ = text_.size(); break;
        default: break;
    }
    return Action::None;
}

} // namespace tt
