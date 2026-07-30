// SQLite backend for the task forest (declared in model/Store.hpp).
//
// Schema (one row per task; `roots` / `doneRoots` / `children` are NOT stored as lists):
//
//   tasks(id, parent, ord, text, done, collapsed, status, created_at, done_at)
//   meta(key, value)          -- next_id
//
// The forest's three orderings all collapse into `ord` = position among siblings, or
// among top-level nodes. Top-level nodes are those with parent = 0 (kNoParent), split
// into canvas roots vs DONE roots by the `done` flag — exactly the rule
// Forest::reindexRootsAfterLoad already uses, so the derivation is not a new invariant.
// Roots and doneRoots number their `ord` independently; the done flag keeps them apart.
//
// This is the whole-forest path (load once at startup, save the lot in one transaction),
// which is parity with the JSON store, not yet the incremental/concurrent story that
// motivated SQLite — see docs/FUTURE.md.

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sqlite3.h>

#include "app/Paths.hpp"
#include "model/Store.hpp"

namespace tt::store {
namespace {

namespace fs = std::filesystem;

constexpr int kSchemaVersion = 1;

const char* const kSchema =
    "CREATE TABLE IF NOT EXISTS tasks("
    "  id         INTEGER PRIMARY KEY,"
    "  parent     INTEGER NOT NULL DEFAULT 0,"
    "  ord        INTEGER NOT NULL DEFAULT 0,"
    "  text       TEXT    NOT NULL DEFAULT '',"
    "  done       INTEGER NOT NULL DEFAULT 0,"
    "  collapsed  INTEGER NOT NULL DEFAULT 0,"
    "  status     INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL DEFAULT 0,"
    "  done_at    INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS idx_tasks_parent ON tasks(parent, ord);"
    "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value NOT NULL);";

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

bool openDb(Db& db, const std::string& path, bool create) {
    const int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
    if (sqlite3_open_v2(path.c_str(), &db.h, flags, nullptr) != SQLITE_OK) return false;
    // A second process (a CLI, an agent) may hold the write lock briefly; wait rather
    // than fail. WAL is what lets that reader work while the app has the DB open.
    sqlite3_busy_timeout(db.h, 3000);
    db.exec("PRAGMA journal_mode=WAL");
    db.exec("PRAGMA synchronous=NORMAL");
    if (!db.exec(kSchema)) return false;
    return db.exec(("PRAGMA user_version=" + std::to_string(kSchemaVersion)).c_str());
}

// Position of every node within its sibling list (or within roots / doneRoots).
std::unordered_map<TaskId, int> siblingOrder(const Forest& f) {
    std::unordered_map<TaskId, int> ord;
    ord.reserve(f.nodes.size());
    for (std::size_t i = 0; i < f.roots.size(); ++i) ord[f.roots[i]] = static_cast<int>(i);
    for (std::size_t i = 0; i < f.doneRoots.size(); ++i) ord[f.doneRoots[i]] = static_cast<int>(i);
    for (const auto& [id, t] : f.nodes)
        for (std::size_t i = 0; i < t.children.size(); ++i)
            ord[t.children[i]] = static_cast<int>(i);
    return ord;
}

} // namespace

bool loadDb(Forest& f, const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;   // absent -> empty forest, as with JSON

    Db db;
    if (!openDb(db, path, false)) return false;

    f.nodes.clear();
    f.roots.clear();
    f.doneRoots.clear();
    f.nextId = 1;

    // Grouped by parent and in sibling order, so a single appending pass rebuilds
    // children / roots / doneRoots in exactly the order they were written.
    std::vector<TaskId> rowOrder;
    {
        Stmt q;
        if (!q.prepare(db.h,
                       "SELECT id,parent,text,done,collapsed,status,created_at,done_at"
                       " FROM tasks ORDER BY parent, ord, id"))
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
            t.done = sqlite3_column_int(q.h, 3) != 0;
            t.collapsed = sqlite3_column_int(q.h, 4) != 0;
            t.status = sqlite3_column_int(q.h, 5);
            t.createdAt = sqlite3_column_int64(q.h, 6);
            t.doneAt = sqlite3_column_int64(q.h, 7);

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
        if (t->parent == kNoParent) (t->done ? f.doneRoots : f.roots).push_back(id);
        else                        f.get(t->parent)->children.push_back(id);
    }

    Stmt m;
    if (m.prepare(db.h, "SELECT value FROM meta WHERE key='next_id'") &&
        sqlite3_step(m.h) == SQLITE_ROW)
        f.nextId = std::max<TaskId>(f.nextId, static_cast<TaskId>(sqlite3_column_int64(m.h, 0)));
    return true;
}

bool saveDb(const Forest& f, const std::string& path) {
    const fs::path p(path);
    if (auto dir = p.parent_path(); !dir.empty()) paths::ensureDir(dir);

    Db db;
    if (!openDb(db, path, true)) return false;
    // One transaction for the whole forest: a reader sees the previous state or the new
    // one, never a half-written tree. IMMEDIATE takes the write lock up front.
    if (!db.exec("BEGIN IMMEDIATE")) return false;

    bool ok = true;
    const std::unordered_map<TaskId, int> ord = siblingOrder(f);

    // Rows currently on disk, so tasks deleted from the forest are deleted here too.
    std::unordered_set<TaskId> onDisk;
    {
        Stmt q;
        if (q.prepare(db.h, "SELECT id FROM tasks")) {
            while (sqlite3_step(q.h) == SQLITE_ROW)
                onDisk.insert(static_cast<TaskId>(sqlite3_column_int64(q.h, 0)));
        } else {
            ok = false;
        }
    }

    if (ok) {
        Stmt up;
        ok = up.prepare(db.h,
                        "INSERT INTO tasks(id,parent,ord,text,done,collapsed,status,"
                        "created_at,done_at) VALUES(?,?,?,?,?,?,?,?,?)"
                        " ON CONFLICT(id) DO UPDATE SET parent=excluded.parent,"
                        " ord=excluded.ord, text=excluded.text, done=excluded.done,"
                        " collapsed=excluded.collapsed, status=excluded.status,"
                        " created_at=excluded.created_at, done_at=excluded.done_at");
        for (const auto& [id, t] : f.nodes) {
            if (!ok) break;
            const auto it = ord.find(id);
            sqlite3_reset(up.h);
            sqlite3_bind_int64(up.h, 1, static_cast<sqlite3_int64>(t.id));
            sqlite3_bind_int64(up.h, 2, static_cast<sqlite3_int64>(t.parent));
            sqlite3_bind_int(up.h, 3, it == ord.end() ? 0 : it->second);
            // SQLITE_STATIC: `t` lives in the forest, which outlives this step().
            sqlite3_bind_text(up.h, 4, t.text.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(up.h, 5, t.done ? 1 : 0);
            sqlite3_bind_int(up.h, 6, t.collapsed ? 1 : 0);
            sqlite3_bind_int(up.h, 7, t.status);
            sqlite3_bind_int64(up.h, 8, t.createdAt);
            sqlite3_bind_int64(up.h, 9, t.doneAt);
            ok = sqlite3_step(up.h) == SQLITE_DONE;
        }
    }

    if (ok) {
        Stmt del;
        ok = del.prepare(db.h, "DELETE FROM tasks WHERE id=?");
        for (TaskId id : onDisk) {
            if (!ok) break;
            if (f.exists(id)) continue;
            sqlite3_reset(del.h);
            sqlite3_bind_int64(del.h, 1, static_cast<sqlite3_int64>(id));
            ok = sqlite3_step(del.h) == SQLITE_DONE;
        }
    }

    if (ok) {
        Stmt m;
        ok = m.prepare(db.h, "INSERT INTO meta(key,value) VALUES('next_id',?)"
                             " ON CONFLICT(key) DO UPDATE SET value=excluded.value");
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
