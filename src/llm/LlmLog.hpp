#pragma once
// Optional request/response log for the LLM classifier, for debugging why a task
// landed where it did. Thread-safe (classifiers run on worker threads).
//
// DEVELOPMENT-ONLY: this whole feature is gated by TASKTREE_DEV. In a production
// build (TASKTREE_DEV=0) every entry point below becomes an inline no-op that the
// compiler eliminates, and LlmLog.cpp isn't compiled at all — so callers need no
// #ifdefs and the functionality vanishes cleanly from the final binary.

#include <string>

#ifndef TASKTREE_DEV
#define TASKTREE_DEV 0
#endif

namespace tt::llmlog {

#if TASKTREE_DEV

void configure(bool enabled, std::string path);
bool enabled();
const std::string& path();
// Append a timestamped block. Safe to call from any thread; no-op when disabled.
void write(const std::string& body);

#else // production: no-op stubs, fully inlined away

inline void configure(bool, std::string) {}
inline bool enabled() { return false; }
inline const std::string& path() { static const std::string empty; return empty; }
inline void write(const std::string&) {}

#endif

} // namespace tt::llmlog
