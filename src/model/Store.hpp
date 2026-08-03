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
#include <memory>
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

// One live handle on the store, held for the app's whole run. Two jobs the free
// functions above cannot do:
//
//   - reuse a single SQLite connection, instead of re-running the open/pragma/
//     CREATE TABLE/upgrade prologue on every save;
//   - notice OTHER writers. `PRAGMA data_version` moves only when a DIFFERENT
//     connection commits, so polling it on the same connection the app saves through
//     makes the app's own writes invisible to the poll and everyone else's visible.
//     Through a fresh-connection-per-save (the free functions), every save — our own
//     included — would look like news.
//
// JSON paths degrade gracefully: load/save dispatch exactly as store::load/save do,
// and changedExternally() is simply always false.
class Session {
public:
    explicit Session(std::string path);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    const std::string& path() const { return path_; }

    // Same contracts as store::load / store::save, `baseline` included.
    bool load(Forest& f);
    bool save(const Forest& f, const Forest* baseline = nullptr);

    // True ONCE when the store changed under us since this session last read, wrote or
    // polled it. Reload promptly after a true: the next save from a stale forest would
    // overwrite the other writer's rows. A store appearing after a failed load counts
    // as a change; polling never creates the file.
    bool changedExternally();

private:
    std::string path_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Move an unreadable store aside instead of writing over it, returning the new path (empty
// if it could not be moved). A load that fails on a file that EXISTS means the data is
// unreadable, not absent — and the app's next save would otherwise replace it with an empty
// store, destroying whatever was still recoverable. Picks an unused `.unreadable[-N]` suffix
// so successive rescues never overwrite each other, and carries a SQLite store's
// `-wal`/`-shm` sidecars along with it.
std::string quarantine(const std::string& path);

// The schema version stamped in a SQLite store, or -1 if the file is missing or not readable
// as a DB. Greater than supportedDbSchemaVersion() means it was written by a NEWER build:
// that data is fine, this binary simply cannot read it, so it must be left strictly alone
// rather than quarantined or overwritten.
int dbSchemaVersion(const std::string& path);
int supportedDbSchemaVersion();

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
