#pragma once
// Command palette grammar for the input bar.
//
// The bottom input bar is the only text field in the app. A leading symbol switches what
// it does; anything else is still just a task to add, so the default flow is untouched:
//
//   task text     AddTask      add a task (LLM classification as before)
//   ?query        Find         highlight matches, walk them, jump to one
//   :12           SelectId     select node 12 (the id badge on the node)
//   :?query       SelectText   pick a node by text, then select it
//   >12           ParentId     make node 12 the parent of the selected node
//   >?query       ParentText   pick the new parent by text
//
// After ':' or '>' the '?' is optional — a non-numeric tail is a text query either way, so
// ':foo' means ':?foo'. A leading space escapes the grammar: ' :not a command' adds a task
// literally called ':not a command'.
//
// Pure + dependency-free (parsing and match ranking are unit-tested in
// tests/core_tests.cpp); the App layer owns all the side effects.

#include <cstddef>
#include <string>
#include <vector>

#include "model/Task.hpp"

namespace tt::palette {

enum class Kind { AddTask, Find, SelectId, SelectText, ParentId, ParentText };

struct Command {
    Kind kind = Kind::AddTask;
    TaskId id = 0;         // SelectId / ParentId — 0 when none was given
    std::string query;     // Find / SelectText / ParentText, trimmed
    std::string body;      // AddTask — the task text, trimmed

    bool isCommand() const { return kind != Kind::AddTask; }
    // Modes whose target comes from the ranked match list (so ↑/↓ and the drop-up apply).
    bool picksByText() const {
        return kind == Kind::Find || kind == Kind::SelectText || kind == Kind::ParentText;
    }
    bool selects() const { return kind == Kind::SelectId || kind == Kind::SelectText; }
    bool reparents() const { return kind == Kind::ParentId || kind == Kind::ParentText; }
};

Command parse(const std::string& raw);

// Case-insensitive substring matches over canvas nodes (the DONE section is excluded —
// it isn't on the canvas), best first: earliest match position, then shorter text (a
// tighter match), then smaller id so the order is stable across runs. `limit` 0 = all.
std::vector<TaskId> rankMatches(const Forest& f, const std::string& query,
                                std::size_t limit = 0);

} // namespace tt::palette
