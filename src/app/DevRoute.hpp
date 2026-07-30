#pragma once
// Dev-task fast path. Tasks typed with a "ttd>" marker are TaskTree's own dev
// to-dos: they are routed straight under a dedicated "tasktree dev" canvas node
// and skip LLM classification entirely. Keeping them in one predictable place
// also lets an external dev loop pick them up by their prefix.
//
// Pure + header-only (depends only on the model), so it is unit-testable without
// pulling in the GLFW / render stack.

#include <cctype>
#include <string>

#include "model/Task.hpp"

namespace tt {

// Marker that flags a task as TaskTree-dev work. Matched case-insensitively and
// ignoring leading whitespace; it is NOT stripped from the text — the task keeps
// reading "ttd> ..." on the canvas (and stays discoverable by that prefix).
inline constexpr const char* kDevTaskMarker = "ttd>";

// Title of the canvas node that collects dev tasks.
inline constexpr const char* kDevNodeTitle = "tasktree dev";

// Case-insensitive ASCII string equality.
inline bool iequalsAscii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

// True when `text` begins with the dev marker (case-insensitive, leading
// whitespace ignored). The marker only counts at the start of the text.
inline bool isDevTask(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    const std::string marker = kDevTaskMarker;
    if (text.size() - i < marker.size()) return false;
    for (std::size_t j = 0; j < marker.size(); ++j) {
        if (std::tolower(static_cast<unsigned char>(text[i + j])) !=
            std::tolower(static_cast<unsigned char>(marker[j]))) return false;
    }
    return true;
}

// Id of the canvas dev node, creating it as a root if none exists. Matches an
// existing *canvas* root titled "tasktree dev" (case-insensitive); it never scans
// or reuses DONE nodes, so a done dev node won't be resurrected. `roots` holds done
// top-level tasks too, so the done flag has to be checked explicitly — for a root it
// is exactly "is in the DONE section", since it has no ancestors.
inline TaskId ensureDevRoot(Forest& forest, std::int64_t createdAt) {
    for (TaskId r : forest.roots) {
        const Task* t = forest.get(r);
        if (t && !t->isDone() && iequalsAscii(t->text, kDevNodeTitle)) return r;
    }
    return forest.addTask(kDevNodeTitle, kNoParent, createdAt);
}

} // namespace tt
