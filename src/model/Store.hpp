#pragma once
// Persistence for the task forest. Two backends behind one seam:
//
//   *.db / *.sqlite  -> SQLite (model/SqliteStore.cpp) — the primary store
//   anything else    -> JSON   (model/Store.cpp)       — export/import, backups,
//                                                        and pre-SQLite data files
//
// load()/save() dispatch on the path's extension, so callers (App) never pick a
// backend: they hold a path and the path decides. See docs/FUTURE.md "SQLite store"
// for why SQLite (row-level writes) and what it is expected to unlock.

#include <string>

#include "model/Task.hpp"

namespace tt::store {

// True when `path` names a SQLite database by extension (.db / .sqlite / .sqlite3).
bool isDbPath(const std::string& path);

// Backend-dispatching load/save. Load returns false if the file is absent or
// unreadable, leaving `f` empty. Save is atomic in both backends (temp+rename for
// JSON, a transaction for SQLite).
bool load(Forest& f, const std::string& path);
bool save(const Forest& f, const std::string& path);

// The backends explicitly, for the migration, export/import and the tests.
// Malformed JSON entries are handled defensively (orphans promoted to roots).
bool loadJson(Forest& f, const std::string& path);
bool saveJson(const Forest& f, const std::string& path);
bool loadDb(Forest& f, const std::string& path);
bool saveDb(const Forest& f, const std::string& path);

// One-time JSON -> SQLite migration. Non-destructive and self-verifying:
//   - refuses (false) if `dbPath` already exists — an existing store is never touched
//   - writes the DB, reads it straight back, and requires tt::equivalent() with the
//     forest loaded from JSON: same ids, order, nextId and every field
//   - on ANY failure deletes the half-written DB (plus its -wal/-shm) and returns
//     false, so the caller can keep using the JSON store
// `jsonPath` is only ever read — this function never writes, renames or removes it.
bool migrateJsonToDb(const std::string& jsonPath, const std::string& dbPath);

} // namespace tt::store
