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

#include <cstdint>
#include <string>
#include <vector>

#include "model/Task.hpp"

namespace tt::store {

// A retired task, as kept by the SQLite backend. Deleting a task NEVER removes its row:
// `deleted_at` is stamped and the row stays. Loads filter those rows out, so a Forest
// only ever holds live tasks — the trash stays out of the model and is read through here
// (the foundation for a future "restore deleted" view).
struct DeletedRow {
    TaskId id = 0;
    TaskId parent = 0;
    std::string text;
    std::int64_t deletedAt = 0;   // epoch ms
};

// Retired rows, most recently deleted first. Empty for a JSON store or a missing file.
std::vector<DeletedRow> deletedRows(const std::string& path);

// True when `path` names a SQLite database by extension (.db / .sqlite / .sqlite3).
bool isDbPath(const std::string& path);

// Backend-dispatching load/save. Load returns false if the file is absent or
// unreadable, leaving `f` empty. Save is atomic in both backends (temp+rename for
// JSON, a transaction for SQLite).
//
// `baseline` is the state this process last successfully wrote or loaded. Given one,
// the SQLite backend writes ONLY the difference, and — the important part — infers a
// deletion from "present in baseline, gone from `f`" instead of "absent from `f`".
// Rows in neither are then left alone rather than deleted, so a task another writer
// added while we were running survives our next save. Pass nullptr (the default) for
// a full rewrite, which does delete anything not in `f`. JSON ignores it: rewriting
// the whole file is inherent to that format.
bool load(Forest& f, const std::string& path);
bool save(const Forest& f, const std::string& path, const Forest* baseline = nullptr);

// The backends explicitly, for the migration, export/import and the tests.
// Malformed JSON entries are handled defensively (orphans promoted to roots).
bool loadJson(Forest& f, const std::string& path);
bool saveJson(const Forest& f, const std::string& path);
bool loadDb(Forest& f, const std::string& path);
bool saveDb(const Forest& f, const std::string& path, const Forest* baseline = nullptr);

// How many tasks a JSON file *declares*, counted from the file itself rather than from a
// loaded Forest — a loader's own output cannot be used to check that loader. `objects` is
// task entries with a usable (positive integer) id; `distinct` is how many unique ids those
// are. They differ only when a file has duplicate ids, which cannot be represented at all.
// Returns false if the file is missing or unparseable.
bool jsonTaskCount(const std::string& path, std::size_t& objects, std::size_t& distinct);

// One-time JSON -> SQLite migration. Non-destructive and self-verifying:
//   - refuses (false) if `dbPath` already exists — an existing store is never touched
//   - writes the DB, reads it straight back, and requires tt::equivalent() with the
//     forest loaded from JSON: same ids, order, nextId and every field
//   - on ANY failure deletes the half-written DB (plus its -wal/-shm) and returns
//     false, so the caller can keep using the JSON store
// `jsonPath` is only ever read — this function never writes, renames or removes it.
bool migrateJsonToDb(const std::string& jsonPath, const std::string& dbPath);

} // namespace tt::store
