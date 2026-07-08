#pragma once
// Optional request/response log for the LLM classifier, for debugging why a task
// landed where it did. Thread-safe (classifiers run on worker threads). No-op unless
// configured with enabled=true. Written as timestamped, human-readable blocks.

#include <string>

namespace tt::llmlog {

void configure(bool enabled, std::string path);
bool enabled();
const std::string& path();

// Append a timestamped block. Safe to call from any thread; no-op when disabled.
void write(const std::string& body);

} // namespace tt::llmlog
