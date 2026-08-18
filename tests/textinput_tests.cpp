// Dependency-free verification of the single-line text field's editing + selection
// model (src/ui/TextInput.cpp). Built by CMake as the `textinput_tests` CTest target.
// Uses the real GLFW key constants (header-only) so the codes match the app exactly.
#include "ui/TextInput.hpp"

#include <cstdio>
#include <string>

#include <GLFW/glfw3.h>

using namespace tt;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) { ++g_fail; std::printf("  FAIL: %s\n", msg); }                \
    } while (0)

// Convenience: press `key` with modifiers, type text, etc.
static void key(TextInput& t, int k, int mods = 0) { t.onKey(k, mods); }
static void type(TextInput& t, const std::string& s) { for (char c : s) t.onChar((unsigned char)c); }

static TextInput seeded(const std::string& s) {
    TextInput t;
    t.setText(s);   // caret at end, no selection
    return t;
}

static void testCaretMotion() {
    std::printf("[textinput] caret motion\n");
    TextInput t = seeded("hello world");
    CHECK(t.caret() == 11, "setText puts caret at end");
    CHECK(!t.hasSelection(), "setText clears selection");

    key(t, GLFW_KEY_HOME);
    CHECK(t.caret() == 0, "Home -> 0");
    key(t, GLFW_KEY_RIGHT);
    CHECK(t.caret() == 1, "Right -> 1");
    key(t, GLFW_KEY_RIGHT, GLFW_MOD_CONTROL);
    CHECK(t.caret() == 6, "Ctrl+Right jumps to start of 'world'");
    key(t, GLFW_KEY_LEFT, GLFW_MOD_CONTROL);
    CHECK(t.caret() == 0, "Ctrl+Left jumps back to start of 'hello'");
    key(t, GLFW_KEY_END);
    CHECK(t.caret() == 11, "End -> size");
    CHECK(!t.hasSelection(), "plain motion never selects");
}

static void testSelectAllAndCopyText() {
    std::printf("[textinput] select all\n");
    TextInput t = seeded("abcdef");
    t.selectAll();
    CHECK(t.hasSelection(), "selectAll selects");
    CHECK(t.selBegin() == 0 && t.selEnd() == 6, "selectAll spans whole text");
    CHECK(t.selectedText() == "abcdef", "selectedText is the whole string");
    // caret at end, plain Left collapses to the near (left) edge.
    key(t, GLFW_KEY_LEFT);
    CHECK(!t.hasSelection() && t.caret() == 0, "plain Left collapses to selBegin");

    t.selectAll();
    key(t, GLFW_KEY_RIGHT);
    CHECK(!t.hasSelection() && t.caret() == 6, "plain Right collapses to selEnd");
}

static void testShiftExtend() {
    std::printf("[textinput] shift extends\n");
    TextInput t = seeded("hello world");
    key(t, GLFW_KEY_HOME);
    key(t, GLFW_KEY_RIGHT, GLFW_MOD_SHIFT);
    key(t, GLFW_KEY_RIGHT, GLFW_MOD_SHIFT);
    CHECK(t.hasSelection(), "shift+right selects");
    CHECK(t.selBegin() == 0 && t.selEnd() == 2, "two shift+rights -> [0,2)");
    CHECK(t.selectedText() == "he", "selected 'he'");
    key(t, GLFW_KEY_RIGHT, GLFW_MOD_CONTROL | GLFW_MOD_SHIFT);
    CHECK(t.selEnd() == 6, "Ctrl+Shift+Right extends by word to 6");
    CHECK(t.selectedText() == "hello ", "selected up to start of 'world'");
    key(t, GLFW_KEY_LEFT, GLFW_MOD_SHIFT);
    CHECK(t.selEnd() == 5, "shift+left shrinks selection back to 5");

    // Anchor stays fixed while the caret sweeps past it and back.
    TextInput u = seeded("abcdef");
    key(u, GLFW_KEY_HOME);
    key(u, GLFW_KEY_RIGHT); key(u, GLFW_KEY_RIGHT); key(u, GLFW_KEY_RIGHT); // caret at 3, no sel
    key(u, GLFW_KEY_LEFT, GLFW_MOD_SHIFT);   // anchor 3, caret 2
    key(u, GLFW_KEY_LEFT, GLFW_MOD_SHIFT);   // caret 1
    CHECK(u.selBegin() == 1 && u.selEnd() == 3, "leftward selection [1,3)");
    CHECK(u.selectedText() == "bc", "selected 'bc' backwards");
}

static void testReplaceOnType() {
    std::printf("[textinput] typing replaces selection\n");
    TextInput t = seeded("hello world");
    key(t, GLFW_KEY_HOME);
    for (int i = 0; i < 5; ++i) key(t, GLFW_KEY_RIGHT, GLFW_MOD_SHIFT); // select "hello"
    CHECK(t.selectedText() == "hello", "selected the first word");
    type(t, "Hi");
    CHECK(t.text() == "Hi world", "typing replaced the selection");
    CHECK(!t.hasSelection() && t.caret() == 2, "caret after inserted text, no selection");
}

static void testDeleteBackspaceSelection() {
    std::printf("[textinput] delete/backspace remove selection\n");
    TextInput t = seeded("hello world");
    t.selectAll();
    key(t, GLFW_KEY_BACKSPACE);
    CHECK(t.text().empty(), "backspace deletes whole selection");
    CHECK(!t.hasSelection() && t.caret() == 0, "empty, caret 0");

    TextInput u = seeded("hello world");
    u.moveCaretTo(0, false);
    u.moveCaretTo(5, true);   // select "hello"
    key(u, GLFW_KEY_DELETE);
    CHECK(u.text() == " world", "delete removes selection only");
    CHECK(u.caret() == 0, "caret at deletion point");
}

static void testPasteReplacesSelection() {
    std::printf("[textinput] paste replaces selection\n");
    TextInput t = seeded("hello world");
    t.moveCaretTo(0, false);
    t.moveCaretTo(5, true);   // select "hello"
    t.insert("goodbye");
    CHECK(t.text() == "goodbye world", "paste replaced selection");
    CHECK(!t.hasSelection(), "no selection after paste");
    // Control chars in a paste are still stripped.
    TextInput u = seeded("");
    u.insert("a\nb\tc");
    CHECK(u.text() == "abc", "paste strips control chars");
}

static void testMouseCaretAndWord() {
    std::printf("[textinput] mouse caret + word select\n");
    TextInput t = seeded("hello world");
    t.moveCaretTo(3, false);
    CHECK(t.caret() == 3 && !t.hasSelection(), "click places caret, clears selection");
    t.moveCaretTo(8, true);
    CHECK(t.selBegin() == 3 && t.selEnd() == 8, "drag extends [3,8)");
    CHECK(t.selectedText() == "lo wo", "dragged selection text");

    t.selectWordAt(7);   // inside "world"
    CHECK(t.selectedText() == "world", "double-click selects the word");
    t.selectWordAt(2);   // inside "hello"
    CHECK(t.selectedText() == "hello", "double-click selects a different word");

    // Clamping past the end.
    t.moveCaretTo(999, false);
    CHECK(t.caret() == 11, "moveCaretTo clamps to size");
}

static void testUtf8Boundaries() {
    std::printf("[textinput] utf-8 boundaries\n");
    // "é" is 2 bytes (0xC3 0xA9); "…" is 3 bytes. String: "aé…b" -> bytes: a(1) é(2) …(3) b(1) = 7 bytes.
    TextInput t = seeded("a\xC3\xA9\xE2\x80\xA6"
                         "b");
    CHECK(t.text().size() == 7, "byte length 7");
    // A click landing mid-codepoint snaps to the boundary (start of "é" at byte 1).
    t.moveCaretTo(2, false);
    CHECK(t.caret() == 1, "click inside 'é' snaps to its start");
    // Word-select over the whole non-space run.
    t.selectWordAt(0);
    CHECK(t.selectedText() == "a\xC3\xA9\xE2\x80\xA6" "b", "word covers the multibyte run");
}

int main() {
    testCaretMotion();
    testSelectAllAndCopyText();
    testShiftExtend();
    testReplaceOnType();
    testDeleteBackspaceSelection();
    testPasteReplacesSelection();
    testMouseCaretAndWord();
    testUtf8Boundaries();

    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
