#pragma once
// Persistence for the task forest: JSON on disk, written atomically.

#include <string>

#include "model/Task.hpp"

namespace tt::store {

// Load the forest from `path`. Returns false if the file is absent or unreadable
// (leaving `f` empty). Malformed entries are handled defensively (orphans promoted
// to roots via Forest::reindexRootsAfterLoad).
bool load(Forest& f, const std::string& path);

// Serialize `f` to `path` atomically (write to a temp file, then rename). Creates
// parent directories as needed. Returns false on I/O failure.
bool save(const Forest& f, const std::string& path);

} // namespace tt::store
