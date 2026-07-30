// SQLite backend for the task forest (declared in model/Store.hpp).
//
// Schema (one row per task; `roots` / `doneRoots` / `children` are NOT stored as lists):
//
//   tasks(id, parent, ord, text, collapsed, status, created_at, done_at, deleted_at)
//   meta(key, value)          -- next_id
//
// TWO TIMESTAMPS, NO BOOLEANS. `done_at` and `deleted_at` each carry both the state and
// its date, so the pair can never disagree the way a flag beside a timestamp can:
//
//   done_at = 0    not done          deleted_at = 0    live
//   done_at > 0    done, at that ms  deleted_at > 0    deleted, at that ms
//   done_at = -1   done, date unknown (completed before the date was recorded)
//
// NOTHING IS EVER HARD-DELETED. There is no DELETE statement in this file: removing a
// task stamps `deleted_at` and the row stays forever. Loads filter `deleted_at = 0`, so
// the Forest only ever holds live tasks — the trash is deliberately outside the model,
// readable via deletedRows(). An id that comes back (an undone delete) has `deleted_at`
// cleared to 0 by the upsert.
//
// `ord` is a task's position among its siblings, or among the top-level tasks (parent = 0).
// A done task keeps its parent and its `ord` — completion is a timestamp, not a move — so:
//
//   top-level tasks   parent = 0, ordered by ord     (done ones included)
//   children of P     parent = P, ordered by ord     (ditto)
//   canvas            tasks with no done_at anywhere on their ancestor chain
//   DONE section      done_at != 0, and its top entries are those whose parent chain has
//                     no other done task — so a done child of a live parent heads its own
//                     entry while staying in place under that parent
//
// Un-doing therefore needs no stored memory of where a task used to be: it never left.
//
// This is the whole-forest path (load once at startup, save the lot in one transaction),
// which is parity with the JSON store, not yet the incremental/concurrent story that
// motivated SQLite — see docs/FUTURE.md.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <sqlite3.h>

#include "app/Paths.hpp"
#include "model/Store.hpp"

namespace tt::store {
namespace {

namespace fs = std::filesystem;

// 1 = original, 2 = + deleted_at, 3 = `done` boolean folded into done_at
constexpr int kSchemaVersion = 3;

const char* const kSchema =
    "CREATE TABLE IF NOT EXISTS tasks("
    "  id         INTEGER PRIMARY KEY,"
    "  parent     INTEGER NOT NULL DEFAULT 0,"
    "  ord        INTEGER NOT NULL DEFAULT 0,"
    "  text       TEXT    NOT NULL DEFAULT '',"
    "  collapsed  INTEGER NOT NULL DEFAULT 0,"
    "  status     INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL DEFAULT 0,"
    "  done_at    INTEGER NOT NULL DEFAULT 0,"    // 0 = not done; -1 = done, date unknown
    "  deleted_at INTEGER NOT NULL DEFAULT 0);"   // 0 = live; epoch ms = when it was deleted
    "CREATE INDEX IF NOT EXISTS idx_tasks_parent ON tasks(parent, ord);"
    "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value NOT NULL);";

std::int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Owning handles: every early return below closes/finalizes without a goto ladder.
struct Db {
    sqlite3* h = nullptr;
    ~Db() { if (h) sqlite3_close(h); }
    bool exec(const char* sql) const {
        return sqlite3_exec(h, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    }
};

struct Stmt {
    sqlite3_stmt* h = nullptr;
    ~Stmt() { if (h) sqlite3_finalize(h); }
    bool prepare(sqlite3* db, const char* sql) {
        return sqlite3_prepare_v2(db, sql, -1, &h, nullptr) == SQLITE_OK;
    }
};

int userVersion(const Db& db) {
    Stmt q;
    if (!q.prepare(db.h, "PRAGMA user_version") || sqlite3_step(q.h) != SQLITE_ROW) return -1;
    return sqlite3_column_int(q.h, 0);
}

// Bring an older file up to kSchemaVersion, one step at a time. No row is ever dropped,
// and no state is lost: a step that removes a column must first move its information
// somewhere else.
bool upgradeSchema(const Db& db) {
    const int from = userVersion(db);
    if (from < 0) return false;
    if (from == kSchemaVersion) return true;
    if (from > kSchemaVersion) return false;   // written by a newer build: refuse, don't corrupt
    // 0 means this process just created the file, so kSchema already made it current.
    if (from >= 1) {
        if (from < 2 &&
            !db.exec("ALTER TABLE tasks ADD COLUMN deleted_at INTEGER NOT NULL DEFAULT 0"))
            return false;
        if (from < 3) {
            // Folding `done` into done_at. Tasks completed before the date was recorded are
            // done with done_at = 0, so they MUST be stamped kDoneAtUnknown first — dropping
            // the column without this would silently un-complete every one of them.
            if (!db.exec("UPDATE tasks SET done_at=-1 WHERE done!=0 AND done_at=0")) return false;
            // Symmetrically, a done_at on a not-done row was always meaningless; clear it so
            // done_at != 0 is exactly "done".
            if (!db.exec("UPDATE tasks SET done_at=0 WHERE done=0 AND done_at!=0")) return false;
            if (!db.exec("ALTER TABLE tasks DROP COLUMN done")) return false;
        }
    }
    return db.exec(("PRAGMA user_version=" + std::to_string(kSchemaVersion)).c_str());
}

bool openDb(Db& db, const std::string& path, bool create) {
    const int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
    if (sqlite3_open_v2(path.c_str(), &db.h, flags, nullptr) != SQLITE_OK) return false;
    // A second process (a CLI, an agent) may hold the write lock briefly; wait rather
    // than fail. WAL is what lets that reader work while the app has the DB open.
    sqlite3_busy_timeout(db.h, 3000);
    db.exec("PRAGMA journal_mode=WAL");
    db.exec("PRAGMA synchronous=NORMAL");
    if (!db.exec(kSchema)) return false;
    return upgradeSchema(db);
}

// Position of every node within its sibling list (or among the top-level tasks).
std::unordered_map<TaskId, int> siblingOrder(const Forest& f) {
    std::unordered_map<TaskId, int> ord;
    ord.reserve(f.nodes.size());
    for (std::size_t i = 0; i < f.roots.size(); ++i) ord[f.roots[i]] = static_cast<int>(i);
    for (const auto& [id, t] : f.nodes)
        for (std::size_t i = 0; i < t.children.size(); ++i)
            ord[t.children[i]] = static_cast<int>(i);
    return ord;
}

int ordOf(const std::unordered_map<TaskId, int>& ord, TaskId id) {
    const auto it = ord.find(id);
    return it == ord.end() ? 0 : it->second;
}

// Do these two tasks produce identical columns? `children` is deliberately excluded —
// it is stored as the children's own parent+ord, not on this row.
bool sameRow(const Task& a, const Task& b) {
    return a.parent == b.parent && a.text == b.text &&
           a.collapsed == b.collapsed && a.status == b.status &&
           a.createdAt == b.createdAt && a.doneAt == b.doneAt;
}

} // namespace

bool loadDb(Forest& f, const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;   // absent -> empty forest, as with JSON

    Db db;
    if (!openDb(db, path, false)) return false;

    f.nodes.clear();
    f.roots.clear();
    f.nextId = 1;

    // Grouped by parent and in sibling order, so a single appending pass rebuilds
    // `children` and `roots` in exactly the order they were written.
    std::vector<TaskId> rowOrder;
    {
        Stmt q;
        if (!q.prepare(db.h,
                       "SELECT id,parent,text,collapsed,status,created_at,done_at"
                       " FROM tasks WHERE deleted_at=0 ORDER BY parent, ord, id"))
            return false;
        for (;;) {
            const int rc = sqlite3_step(q.h);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) return false;

            Task t;
            t.id = static_cast<TaskId>(sqlite3_column_int64(q.h, 0));
            if (t.id == 0) continue;      // 0 means "no parent"; never a real id
            t.parent = static_cast<TaskId>(sqlite3_column_int64(q.h, 1));
            if (const unsigned char* s = sqlite3_column_text(q.h, 2))
                t.text = reinterpret_cast<const char*>(s);
            t.collapsed = sqlite3_column_int(q.h, 3) != 0;
            t.status = sqlite3_column_int(q.h, 4);
            t.createdAt = sqlite3_column_int64(q.h, 5);
            t.doneAt = sqlite3_column_int64(q.h, 6);

            rowOrder.push_back(t.id);
            f.nextId = std::max<TaskId>(f.nextId, t.id + 1);
            f.nodes.emplace(t.id, std::move(t));
        }
    }

    for (TaskId id : rowOrder) {
        Task* t = f.get(id);
        if (!t) continue;
        // Defensive, matching the JSON loader: a dangling parent becomes a root rather
        // than an unreachable node.
        if (t->parent != kNoParent && !f.exists(t->parent)) t->parent = kNoParent;
        if (t->parent == kNoParent) f.roots.push_back(id);   // done or not: same list
        else                        f.get(t->parent)->children.push_back(id);
    }

    Stmt m;
    if (m.prepare(db.h, "SELECT value FROM meta WHERE key='next_id'") &&
        sqlite3_step(m.h) == SQLITE_ROW)
        f.nextId = std::max<TaskId>(f.nextId, static_cast<TaskId>(sqlite3_column_int64(m.h, 0)));
    return true;
}

bool saveDb(const Forest& f, const std::string& path, const Forest* baseline) {
    const fs::path p(path);
    if (auto dir = p.parent_path(); !dir.empty()) paths::ensureDir(dir);

    Db db;
    if (!openDb(db, path, true)) return false;
    // One transaction per save: a reader sees the previous state or the new one, never a
    // half-written tree. IMMEDIATE takes the write lock up front.
    if (!db.exec("BEGIN IMMEDIATE")) return false;

    bool ok = true;
    const std::unordered_map<TaskId, int> ord = siblingOrder(f);

    // Work out the minimum set of statements. `deletes` are SOFT: see the file header.
    std::vector<TaskId> upserts;
    std::vector<TaskId> deletes;
    if (baseline) {
        // Incremental. `ord` is compared too: removing a middle child renumbers its later
        // siblings without changing a single field on their rows.
        const std::unordered_map<TaskId, int> was = siblingOrder(*baseline);
        for (const auto& [id, t] : f.nodes) {
            const Task* b = baseline->get(id);
            if (!b || !sameRow(t, *b) || ordOf(ord, id) != ordOf(was, id)) upserts.push_back(id);
        }
        for (const auto& [id, t] : baseline->nodes)
            if (!f.exists(id)) deletes.push_back(id);   // WE deleted it; absence alone is not proof
    } else {
        // Full rewrite: every node, and retire whatever else is still live.
        for (const auto& [id, t] : f.nodes) upserts.push_back(id);
        Stmt q;
        if (q.prepare(db.h, "SELECT id FROM tasks WHERE deleted_at=0")) {
            while (sqlite3_step(q.h) == SQLITE_ROW) {
                const TaskId id = static_cast<TaskId>(sqlite3_column_int64(q.h, 0));
                if (!f.exists(id)) deletes.push_back(id);
            }
        } else {
            ok = false;
        }
    }

    if (ok && !upserts.empty()) {
        Stmt up;
        // deleted_at=0 on both paths: a task present in the forest is live by definition,
        // so writing one un-deletes its row (this is what makes undo-of-a-delete work).
        ok = up.prepare(db.h,
                        "INSERT INTO tasks(id,parent,ord,text,collapsed,status,"
                        "created_at,done_at,deleted_at) VALUES(?,?,?,?,?,?,?,?,0)"
                        " ON CONFLICT(id) DO UPDATE SET parent=excluded.parent,"
                        " ord=excluded.ord, text=excluded.text,"
                        " collapsed=excluded.collapsed, status=excluded.status,"
                        " created_at=excluded.created_at, done_at=excluded.done_at,"
                        " deleted_at=0");
        for (TaskId id : upserts) {
            if (!ok) break;
            const Task* t = f.get(id);
            if (!t) continue;
            sqlite3_reset(up.h);
            sqlite3_bind_int64(up.h, 1, static_cast<sqlite3_int64>(t->id));
            sqlite3_bind_int64(up.h, 2, static_cast<sqlite3_int64>(t->parent));
            sqlite3_bind_int(up.h, 3, ordOf(ord, id));
            // SQLITE_STATIC: `*t` lives in the forest, which outlives this step().
            sqlite3_bind_text(up.h, 4, t->text.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(up.h, 5, t->collapsed ? 1 : 0);
            sqlite3_bind_int(up.h, 6, t->status);
            sqlite3_bind_int64(up.h, 7, t->createdAt);
            sqlite3_bind_int64(up.h, 8, t->doneAt);
            ok = sqlite3_step(up.h) == SQLITE_DONE;
        }
    }

    if (ok && !deletes.empty()) {
        // Soft delete: stamp the row and keep it. `AND deleted_at=0` preserves the FIRST
        // deletion's timestamp if this ever runs twice for the same id.
        Stmt del;
        ok = del.prepare(db.h, "UPDATE tasks SET deleted_at=? WHERE id=? AND deleted_at=0");
        const std::int64_t when = nowMs();
        for (TaskId id : deletes) {
            if (!ok) break;
            sqlite3_reset(del.h);
            sqlite3_bind_int64(del.h, 1, when);
            sqlite3_bind_int64(del.h, 2, static_cast<sqlite3_int64>(id));
            ok = sqlite3_step(del.h) == SQLITE_DONE;
        }
    }

    if (ok) {
        // next_id never moves backwards: another writer may have taken ids beyond ours,
        // and lowering it would hand out an id that is already in use.
        Stmt m;
        ok = m.prepare(db.h, "INSERT INTO meta(key,value) VALUES('next_id',?)"
                             " ON CONFLICT(key) DO UPDATE SET"
                             " value=MAX(CAST(value AS INTEGER),CAST(excluded.value AS INTEGER))");
        if (ok) {
            sqlite3_bind_int64(m.h, 1, static_cast<sqlite3_int64>(f.nextId));
            ok = sqlite3_step(m.h) == SQLITE_DONE;
        }
    }

    if (!ok) {
        db.exec("ROLLBACK");
        return false;
    }
    return db.exec("COMMIT");
}

std::vector<DeletedRow> deletedRows(const std::string& path) {
    std::vector<DeletedRow> out;
    std::error_code ec;
    if (!fs::exists(path, ec)) return out;
    Db db;
    if (!openDb(db, path, false)) return out;

    Stmt q;
    if (!q.prepare(db.h, "SELECT id,parent,text,deleted_at FROM tasks WHERE deleted_at!=0"
                         " ORDER BY deleted_at DESC, id"))
        return out;
    while (sqlite3_step(q.h) == SQLITE_ROW) {
        DeletedRow r;
        r.id = static_cast<TaskId>(sqlite3_column_int64(q.h, 0));
        r.parent = static_cast<TaskId>(sqlite3_column_int64(q.h, 1));
        if (const unsigned char* s = sqlite3_column_text(q.h, 2))
            r.text = reinterpret_cast<const char*>(s);
        r.deletedAt = sqlite3_column_int64(q.h, 3);
        out.push_back(std::move(r));
    }
    return out;
}

bool migrateJsonToDb(const std::string& jsonPath, const std::string& dbPath) {
    std::error_code ec;
    if (fs::exists(dbPath, ec)) return false;   // an existing store is never overwritten

    Forest from;
    if (!loadJson(from, jsonPath)) return false;

    // Any failure past this point leaves nothing behind: the JSON file was only read,
    // and a partial DB is removed so the next run retries from scratch.
    const auto scrub = [&dbPath] {
        std::error_code e;
        fs::remove(dbPath, e);
        fs::remove(dbPath + "-wal", e);
        fs::remove(dbPath + "-shm", e);
        return false;
    };

    if (!saveDb(from, dbPath)) return scrub();

    Forest back;
    if (!loadDb(back, dbPath)) return scrub();
    if (!equivalent(from, back)) return scrub();   // proof, not optimism
    return true;
}

} // namespace tt::store
