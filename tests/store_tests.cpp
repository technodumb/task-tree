// Verification for persistence (Store) + config (Config). Built by CMake as the
// `store_tests` CTest target.
#include "app/Config.hpp"
#include "app/Paths.hpp"
#include "model/Store.hpp"
#include "model/Task.hpp"
#include "platform/Hotkey.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <sqlite3.h>

using namespace tt;
namespace fs = std::filesystem;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                            \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) { ++g_fail; std::printf("  FAIL: %s\n", msg); }               \
    } while (0)

static fs::path tmpFile(const char* name) {
    return fs::temp_directory_path() / (std::string("tasktree_test_") + name);
}

// A SQLite DB is up to three files in WAL mode; leave none of them behind.
static void removeDb(const std::string& path) {
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove(path + "-wal", ec);
    fs::remove(path + "-shm", ec);
}

// Raw SQL, to see what the store API deliberately hides (soft-deleted rows) and to
// build an old-schema fixture. -1 on any failure.
static int rawCount(const std::string& db, const char* where) {
    sqlite3* h = nullptr;
    if (sqlite3_open_v2(db.c_str(), &h, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return -1;
    const std::string sql = std::string("SELECT count(*) FROM tasks WHERE ") + where;
    sqlite3_stmt* s = nullptr;
    int n = -1;
    if (sqlite3_prepare_v2(h, sql.c_str(), -1, &s, nullptr) == SQLITE_OK &&
        sqlite3_step(s) == SQLITE_ROW)
        n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    sqlite3_close(h);
    return n;
}

static int rawUserVersion(const std::string& db) {
    sqlite3* h = nullptr;
    if (sqlite3_open_v2(db.c_str(), &h, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return -1;
    sqlite3_stmt* s = nullptr;
    int v = -1;
    if (sqlite3_prepare_v2(h, "PRAGMA user_version", -1, &s, nullptr) == SQLITE_OK &&
        sqlite3_step(s) == SQLITE_ROW)
        v = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    sqlite3_close(h);
    return v;
}

static std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

int main() {
    // ---- Store round-trip ------------------------------------------------------
    {
        Forest f;
        TaskId a = f.addTask("alpha — éà");   // UTF-8 content
        f.get(a)->status = 2;                 // priority
        f.get(a)->collapsed = true;           // collapsed subtree
        TaskId b = f.addTask("beta", a);
        f.addTask("gamma", a);
        TaskId c = f.addTask("second tree");
        f.addTask("child", c);

        const std::string path = tmpFile("roundtrip.json").string();
        CHECK(store::save(f, path), "save succeeds");
        CHECK(!fs::exists(path + ".tmp"), "atomic write leaves no temp file");

        Forest g;
        CHECK(store::load(g, path), "load succeeds");
        CHECK(g.size() == f.size(), "same node count");
        CHECK(g.nextId == f.nextId, "nextId preserved");
        CHECK(g.roots == f.roots, "roots order preserved");
        CHECK(g.get(a) && g.get(a)->text == "alpha — éà", "UTF-8 text preserved");
        CHECK(g.get(a) && g.get(a)->status == 2, "status preserved");
        CHECK(g.get(a) && g.get(a)->collapsed, "collapsed flag preserved");
        CHECK(g.get(a)->children == f.get(a)->children, "children order preserved");
        CHECK(g.get(b) && g.get(b)->parent == a, "parent link preserved");
        fs::remove(path);
    }

    // ---- Missing file loads as empty ------------------------------------------
    {
        Forest g;
        CHECK(!store::load(g, tmpFile("does_not_exist.json").string()), "missing file -> false");
        CHECK(g.size() == 0, "forest stays empty");
    }

    // ---- Orphan promotion (parent id absent, no roots array) ------------------
    {
        const std::string path = tmpFile("orphan.json").string();
        {
            std::ofstream o(path);
            o << R"({"version":1,"nextId":10,"tasks":[
                    {"id":5,"parent":99,"text":"orphan","children":[]}]})";
        }
        Forest g;
        CHECK(store::load(g, path), "load orphan file");
        CHECK(g.get(5) && g.get(5)->parent == kNoParent, "orphan promoted to root");
        CHECK(g.roots.size() == 1 && g.roots[0] == 5, "orphan is a root");
        fs::remove(path);
    }

    // ---- DONE section persistence ---------------------------------------------
    {
        Forest f;
        f.addTask("keep me");
        TaskId d = f.addTask("finished");
        f.addTask("subtask", d);
        f.markDone(d);

        const std::string path = tmpFile("done.json").string();
        CHECK(store::save(f, path), "save with a done task");
        Forest g;
        CHECK(store::load(g, path), "load with a done task");
        CHECK(g.doneSectionRoots().size() == 1 && g.doneSectionRoots()[0] == d,
              "the DONE section survives the round-trip");
        CHECK(g.get(d) && g.get(d)->isDone(), "completion persisted");
        CHECK(std::find(g.roots.begin(), g.roots.end(), d) != g.roots.end(),
              "a done top-level task is still a root — the flag decides the view");
        CHECK(g.get(d)->children.size() == 1, "done subtree intact");
        fs::remove(path);
    }

    // ---- SQLite round-trip -----------------------------------------------------
    // Every field, every ordering. `equivalent()` is exact, so this fails on any drift.
    {
        Forest f;
        TaskId a = f.addTask("alpha — éà", kNoParent, 1700000000000);  // UTF-8 + timestamp
        TaskId b = f.addTask("beta", a);
        TaskId c = f.addTask("gamma", a);
        f.addTask("delta", b);
        f.get(a)->collapsed = true;
        f.get(c)->status = 2;
        TaskId second = f.addTask("second root");
        TaskId third = f.addTask("third root");
        // Two DONE roots as well, so the shared parent=0 rows have to stay in their own
        // ord sequences (roots vs doneRoots both start at 0).
        TaskId d1 = f.addTask("finished one");
        f.addTask("its child", d1);
        TaskId d2 = f.addTask("finished two");
        f.markDone(d1);
        f.markDone(d2);
        f.get(d1)->doneAt = 1700000009999;

        const std::string db = tmpFile("roundtrip.db").string();
        removeDb(db);
        CHECK(store::saveDb(f, db), "sqlite save succeeds");

        Forest g;
        CHECK(store::loadDb(g, db), "sqlite load succeeds");
        CHECK(equivalent(f, g), "sqlite round-trip is field-for-field identical");
        CHECK(g.roots == f.roots, "top-level order preserved (done tasks included)");
        CHECK(g.doneSectionRoots() == f.doneSectionRoots(), "DONE section preserved");
        CHECK(g.get(a) && g.get(a)->children == f.get(a)->children, "sibling order preserved");
        CHECK(g.get(a) && g.get(a)->text == "alpha — éà", "UTF-8 text preserved");
        CHECK(g.get(a) && g.get(a)->collapsed, "collapsed preserved");
        CHECK(g.get(c) && g.get(c)->status == 2, "status preserved");
        CHECK(g.get(a) && g.get(a)->createdAt == 1700000000000, "createdAt preserved");
        CHECK(g.get(d1) && g.get(d1)->doneAt == 1700000009999, "doneAt preserved");
        CHECK(g.nextId == f.nextId, "nextId preserved");
        CHECK(g.get(d1) && g.get(d1)->children.size() == 1, "DONE subtree intact");
        CHECK(g.get(second) && g.get(third), "all roots present");

        // Saving again over the same DB must not duplicate or drop anything.
        CHECK(store::saveDb(f, db), "re-save over an existing DB");
        Forest h;
        CHECK(store::loadDb(h, db) && equivalent(f, h), "re-save is idempotent");

        // A deletion in the forest must delete the row, not leave an orphan behind.
        Forest less = f;
        less.removeSubtree(a);
        CHECK(store::saveDb(less, db), "save after removing a subtree");
        Forest k;
        CHECK(store::loadDb(k, db), "load after removing a subtree");
        CHECK(!k.exists(a) && !k.exists(b) && !k.exists(c), "removed rows are gone");
        CHECK(equivalent(less, k), "post-delete state round-trips");
        removeDb(db);
    }

    // ---- Missing DB / backend dispatch ----------------------------------------
    {
        Forest g;
        CHECK(!store::loadDb(g, tmpFile("nope.db").string()), "missing DB -> false");
        CHECK(g.size() == 0, "forest stays empty");

        CHECK(store::isDbPath("/tmp/tasks.db"), ".db is a DB path");
        CHECK(store::isDbPath("/tmp/tasks.SQLite"), ".sqlite is a DB path (any case)");
        CHECK(!store::isDbPath("/tmp/tasks.json"), ".json is not a DB path");

        // load()/save() must pick the backend from the extension.
        Forest f;
        f.addTask("dispatch me");
        const std::string db = tmpFile("dispatch.db").string();
        removeDb(db);
        CHECK(store::save(f, db), "save() dispatches to SQLite");
        Forest viaFacade;
        CHECK(store::load(viaFacade, db) && equivalent(f, viaFacade), "load() dispatches too");
        removeDb(db);
    }

    // ---- Incremental writes ----------------------------------------------------
    {
        Forest f;
        TaskId a = f.addTask("root");
        TaskId b = f.addTask("first", a);
        TaskId c = f.addTask("middle", a);
        TaskId d = f.addTask("last", a);

        const std::string db = tmpFile("incremental.db").string();
        removeDb(db);
        CHECK(store::saveDb(f, db), "seed with a full write");

        // Edit one field; the rest of the tree must be untouched and still correct.
        Forest base = f;                       // what disk holds now
        f.get(b)->text = "first, edited";
        f.get(b)->status = 1;
        CHECK(store::saveDb(f, db, &base), "incremental save of one edit");
        Forest g;
        CHECK(store::loadDb(g, db) && equivalent(f, g), "edit landed, nothing else moved");

        // Removing a middle child renumbers its later sibling's `ord` without changing a
        // single column on that sibling's row — the diff has to notice.
        base = f;
        f.removeSubtree(c);
        CHECK(store::saveDb(f, db, &base), "incremental save of a deletion");
        Forest h;
        CHECK(store::loadDb(h, db), "load after incremental delete");
        CHECK(!h.exists(c), "deleted row is gone");
        CHECK(h.get(a) && h.get(a)->children.size() == 2, "two children left");
        CHECK(h.get(a) && h.get(a)->children[0] == b && h.get(a)->children[1] == d,
              "surviving siblings keep their order after the ord shift");
        CHECK(equivalent(f, h), "post-delete state round-trips");

        // A new node arrives through the incremental path too.
        base = f;
        TaskId e = f.addTask("added later", a);
        CHECK(store::saveDb(f, db, &base), "incremental save of an insert");
        Forest k;
        CHECK(store::loadDb(k, db) && equivalent(f, k), "insert landed");
        CHECK(k.exists(e), "new row present");
        removeDb(db);
    }

    // ---- A second writer's task is not clobbered -------------------------------
    // The whole point of row-level writes. Two Forests stand in for two processes: each
    // has its own baseline, and neither may delete what it never knew about.
    {
        Forest seed;
        TaskId keep = seed.addTask("shared task");
        seed.addTask("another", keep);

        const std::string db = tmpFile("two_writers.db").string();
        removeDb(db);
        CHECK(store::saveDb(seed, db), "seed the shared DB");

        // Both "processes" load the same state.
        Forest appSide, cliSide;
        CHECK(store::loadDb(appSide, db) && store::loadDb(cliSide, db), "both load");
        const Forest appBaseline = appSide;    // the app's view of disk, now frozen
        const Forest cliBaseline = cliSide;

        // The other writer adds a task and commits it.
        TaskId external = cliSide.addTask("added by the CLI");
        CHECK(store::saveDb(cliSide, db, &cliBaseline), "external writer inserts");

        // The app, which has never heard of that task, saves an unrelated edit.
        appSide.get(keep)->text = "edited by the app";
        CHECK(store::saveDb(appSide, db, &appBaseline), "app saves its own edit");

        Forest after;
        CHECK(store::loadDb(after, db), "reload the shared DB");
        CHECK(after.exists(external), "the external task SURVIVED the app's save");
        CHECK(after.get(keep) && after.get(keep)->text == "edited by the app",
              "the app's edit landed");
        CHECK(after.size() == 3, "three tasks: both writers' work is present");
        CHECK(after.nextId >= cliSide.nextId, "next_id did not move backwards");

        // Contrast, so the mechanism is load-bearing rather than incidental: the same
        // save WITHOUT a baseline is a full rewrite, and does drop the external task.
        const std::string db2 = tmpFile("two_writers_full.db").string();
        removeDb(db2);
        CHECK(store::saveDb(cliSide, db2), "seed a second DB including the external task");
        CHECK(store::saveDb(appSide, db2), "app full-rewrites it (baseline = nullptr)");
        Forest wiped;
        CHECK(store::loadDb(wiped, db2), "reload after the full rewrite");
        CHECK(!wiped.exists(external), "full rewrite drops it — which is why baselines exist");
        removeDb(db);
        removeDb(db2);
    }

    // ---- Soft delete: nothing is ever removed from the DB ----------------------
    {
        Forest f;
        TaskId keep = f.addTask("survivor");
        TaskId doomed = f.addTask("doomed parent");
        TaskId child = f.addTask("doomed child", doomed);

        const std::string db = tmpFile("soft_delete.db").string();
        removeDb(db);
        CHECK(store::saveDb(f, db), "seed");
        CHECK(rawCount(db, "deleted_at=0") == 3, "three live rows");

        Forest base = f;
        f.removeSubtree(doomed);
        CHECK(store::saveDb(f, db, &base), "save after removing a subtree");

        // The rows are still there — that is the whole point.
        CHECK(rawCount(db, "1=1") == 3, "NO row was removed from the table");
        CHECK(rawCount(db, "deleted_at=0") == 1, "one row still live");
        CHECK(rawCount(db, "deleted_at!=0") == 2, "the subtree is stamped, not gone");

        // …but a load only ever sees live tasks.
        Forest g;
        CHECK(store::loadDb(g, db), "load after soft delete");
        CHECK(g.size() == 1 && g.exists(keep), "load hides deleted rows");
        CHECK(!g.exists(doomed) && !g.exists(child), "deleted tasks are not in the forest");
        CHECK(equivalent(f, g), "live state round-trips");

        // The trash is readable, newest first, with a real timestamp.
        auto trash = store::deletedRows(db);
        CHECK(trash.size() == 2, "deletedRows() returns both");
        CHECK(trash[0].deletedAt > 0, "deleted_at is stamped");
        bool sawChild = false, sawParent = false;
        for (const auto& r : trash) {
            if (r.id == child) { sawChild = true; CHECK(r.parent == doomed, "child keeps its parent"); }
            if (r.id == doomed) { sawParent = true; CHECK(r.text == "doomed parent", "text kept"); }
        }
        CHECK(sawChild && sawParent, "both retired tasks are recoverable from the row data");

        // Undo of a delete: the same ids come back, and their rows go live again.
        const std::int64_t stamp = trash[0].deletedAt;
        Forest afterDelete = f;
        CHECK(store::saveDb(base, db, &afterDelete), "re-save the pre-delete forest (undo)");
        CHECK(rawCount(db, "deleted_at=0") == 3, "all three rows live again");
        CHECK(store::deletedRows(db).empty(), "trash is empty after the undo");
        Forest h;
        CHECK(store::loadDb(h, db) && equivalent(base, h), "undone state round-trips");
        CHECK(stamp > 0, "the earlier stamp was real");

        // Deleting again, twice, must not overwrite the first deletion's timestamp.
        Forest base2 = base;
        Forest gone = base;
        gone.removeSubtree(doomed);
        CHECK(store::saveDb(gone, db, &base2), "delete again");
        const auto first = store::deletedRows(db);
        CHECK(first.size() == 2, "stamped again");
        CHECK(store::saveDb(gone, db, &base2), "repeat the same save");
        const auto second = store::deletedRows(db);
        CHECK(second.size() == 2 && second[0].deletedAt == first[0].deletedAt,
              "a repeated delete keeps the ORIGINAL deleted_at");
        removeDb(db);
    }

    // ---- Schema upgrade 1 -> 3 (no deleted_at; `done` boolean still present) -----
    // The dangerous step is folding `done` into done_at: tasks completed before the date
    // was recorded are done=1 with done_at=0, and must stay done.
    {
        const std::string db = tmpFile("schema_v1.db").string();
        removeDb(db);
        {
            sqlite3* h = nullptr;
            CHECK(sqlite3_open(db.c_str(), &h) == SQLITE_OK, "create a v1 DB");
            const char* v1 =
                "CREATE TABLE tasks(id INTEGER PRIMARY KEY, parent INTEGER NOT NULL DEFAULT 0,"
                " ord INTEGER NOT NULL DEFAULT 0, text TEXT NOT NULL DEFAULT '',"
                " done INTEGER NOT NULL DEFAULT 0, collapsed INTEGER NOT NULL DEFAULT 0,"
                " status INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL DEFAULT 0,"
                " done_at INTEGER NOT NULL DEFAULT 0);"
                "CREATE TABLE meta(key TEXT PRIMARY KEY, value NOT NULL);"
                "INSERT INTO tasks(id,parent,ord,text,done,done_at) VALUES"
                "  (1,0,0,'old root',0,0),"          // live
                "  (2,1,0,'old child',0,0),"         // live child
                "  (3,0,1,'dated done',1,1700000000000),"   // done, with a date
                "  (4,0,2,'undated done',1,0),"      // done, no date  <-- the risky one
                "  (5,0,3,'stale done_at',0,1699999999999);"  // not done, junk date
                "INSERT INTO meta VALUES('next_id',6);"
                "PRAGMA user_version=1;";
            CHECK(sqlite3_exec(h, v1, nullptr, nullptr, nullptr) == SQLITE_OK, "seed v1 rows");
            sqlite3_close(h);
        }
        Forest g;
        CHECK(store::loadDb(g, db), "a v1 DB still loads");
        CHECK(g.size() == 5, "every old row survived the upgrade");
        CHECK(g.get(1) && g.get(1)->text == "old root", "old data intact");
        CHECK(g.get(2) && g.get(2)->parent == 1, "old parent link intact");
        CHECK(g.nextId == 6, "old next_id intact");
        CHECK(rawUserVersion(db) == 3, "user_version bumped to 3");
        CHECK(rawCount(db, "deleted_at=0") == 5, "existing rows are marked live");

        CHECK(g.get(3) && g.get(3)->isDone(), "a dated done task is still done");
        CHECK(g.get(3)->doneAt == 1700000000000, "and keeps its real date");
        CHECK(g.get(4) && g.get(4)->isDone(), "an UNDATED done task is still done");
        CHECK(g.get(4)->doneAt == kDoneAtUnknown, "recorded as done-at-unknown, not fabricated");
        CHECK(g.get(5) && !g.get(5)->isDone(), "a not-done row with a stale date is not done");
        CHECK(g.get(5)->doneAt == 0, "its meaningless timestamp was cleared");
        CHECK(g.doneSectionRoots().size() == 2, "both done tasks head DONE entries");
        removeDb(db);
    }

    // ---- JSON -> SQLite migration: verified and non-destructive ----------------
    {
        Forest f;
        TaskId r = f.addTask("keep me");
        f.addTask("child", r);
        TaskId d = f.addTask("done root");
        f.markDone(d);

        const std::string js = tmpFile("migrate.json").string();
        const std::string db = tmpFile("migrate.db").string();
        removeDb(db);
        CHECK(store::saveJson(f, js), "seed a JSON store");
        const std::string before = readAll(js);

        CHECK(store::migrateJsonToDb(js, db), "migration succeeds");
        CHECK(fs::exists(db), "DB created");
        CHECK(fs::exists(js), "JSON file still exists after migration");
        CHECK(readAll(js) == before, "JSON file is byte-for-byte untouched");

        Forest fromJson, fromDb;
        CHECK(store::loadJson(fromJson, js), "reload the JSON");
        CHECK(store::loadDb(fromDb, db), "load the migrated DB");
        CHECK(equivalent(fromJson, fromDb), "migrated DB matches the JSON exactly");

        // Re-running must refuse rather than rewrite a store that already exists.
        CHECK(!store::migrateJsonToDb(js, db), "migration refuses an existing DB");
        Forest again;
        CHECK(store::loadDb(again, db) && equivalent(fromJson, again), "existing DB intact");

        // A missing source leaves no DB behind at all.
        const std::string db2 = tmpFile("migrate_missing.db").string();
        removeDb(db2);
        CHECK(!store::migrateJsonToDb(tmpFile("no_such.json").string(), db2),
              "migration fails on a missing JSON");
        CHECK(!fs::exists(db2), "failed migration leaves no DB behind");

        removeDb(db);
        fs::remove(js);
    }

    // ---- Hostile / foreign JSON: never crash, never silently lose a task ---------
    // These are shapes another person's file can have but mine does not. The loader used to
    // throw an uncaught type_error on the first two, which kills the app at startup.
    {
        auto write = [](const std::string& p, const char* body) {
            std::ofstream o(p);
            o << body;
        };

        // Nulls where numbers/booleans belong.
        const std::string nulls = tmpFile("hostile_nulls.json").string();
        write(nulls, R"({"version":1,"nextId":5,"roots":[1],"tasks":[
          {"id":1,"parent":null,"text":"nulls","children":[],"done":null,
           "doneAt":null,"createdAt":null,"status":null,"collapsed":null}]})");
        Forest a;
        CHECK(store::loadJson(a, nulls), "a file full of nulls loads");
        CHECK(a.size() == 1 && a.get(1) && a.get(1)->text == "nulls", "the task survives");
        CHECK(!a.get(1)->isDone() && a.get(1)->status == 0, "null fields fall back to defaults");

        // "done": null must NOT un-complete a task that carries a real doneAt.
        const std::string nullDone = tmpFile("hostile_nulldone.json").string();
        write(nullDone, R"({"version":1,"nextId":3,"roots":[1],"tasks":[
          {"id":1,"parent":0,"text":"done, null flag","children":[],"done":null,
           "doneAt":1700000000000}]})");
        Forest b;
        CHECK(store::loadJson(b, nullDone), "null done flag loads");
        CHECK(b.get(1) && b.get(1)->isDone() && b.get(1)->doneAt == 1700000000000,
              "a real doneAt wins over a meaningless done flag");

        // Strings where numbers belong.
        const std::string types = tmpFile("hostile_types.json").string();
        write(types, R"({"version":1,"nextId":"nine","roots":[1],"tasks":[
          {"id":1,"parent":"zero","text":"bad types","children":["x"],"done":"yes"}]})");
        Forest c;
        CHECK(store::loadJson(c, types), "wrong types load instead of throwing");
        CHECK(c.size() == 1 && c.get(1)->parent == kNoParent, "unparseable parent -> top level");
        CHECK(c.get(1)->children.empty(), "unparseable child ids are dropped");

        // parent links with no children arrays: the child must still be reachable.
        const std::string bare = tmpFile("hostile_bare.json").string();
        write(bare, R"({"tasks":[{"id":1,"text":"p"},{"id":2,"parent":1,"text":"kid"}]})");
        Forest d;
        CHECK(store::loadJson(d, bare), "a file with no roots/children lists loads");
        CHECK(d.size() == 2, "both tasks load");
        CHECK(d.get(1) && d.get(1)->children == std::vector<TaskId>{2},
              "the child is reachable from its parent");
        const std::string bareDb = tmpFile("hostile_bare.db").string();
        removeDb(bareDb);
        CHECK(store::migrateJsonToDb(bare, bareDb), "and such a file still migrates");
        removeDb(bareDb);

        // Duplicate ids cannot be represented, so migration must REFUSE rather than freeze
        // a lossy copy — the whole point of counting the raw file.
        const std::string dup = tmpFile("hostile_dup.json").string();
        write(dup, R"({"version":1,"nextId":3,"roots":[1],"tasks":[
          {"id":1,"parent":0,"text":"first","children":[]},
          {"id":1,"parent":0,"text":"same id","children":[]}]})");
        std::size_t objects = 0, distinct = 0;
        CHECK(store::jsonTaskCount(dup, objects, distinct), "counting the raw file works");
        CHECK(objects == 2 && distinct == 1, "two entries, one id");
        const std::string dupDb = tmpFile("hostile_dup.db").string();
        removeDb(dupDb);
        CHECK(!store::migrateJsonToDb(dup, dupDb), "duplicate ids refuse migration");
        CHECK(!fs::exists(dupDb), "and leave no DB behind");

        // The real file count must match what the loader produced, for a sane file.
        CHECK(store::jsonTaskCount(bare, objects, distinct) && objects == 2 && distinct == 2,
              "a good file counts straight");

        for (const auto& p : {nulls, nullDone, types, bare, dup}) fs::remove(p);
    }

    // ---- Migration leaves a frozen copy of the source ---------------------------
    {
        Forest f;
        f.addTask("backed up");
        const std::string js = tmpFile("backup_src.json").string();
        const std::string db = tmpFile("backup_src.db").string();
        const std::string bak = js + ".pre-sqlite.bak";
        removeDb(db);
        fs::remove(bak);
        CHECK(store::saveJson(f, js), "seed");
        CHECK(store::migrateJsonToDb(js, db), "migrate");
        CHECK(fs::exists(bak), "a .pre-sqlite.bak copy is left next to the source");
        CHECK(readAll(bak) == readAll(js), "and it is byte-identical to the source");
        removeDb(db);
        fs::remove(bak);
        fs::remove(js);
    }

    // ---- An unreadable store is moved aside, never written over ------------------
    {
        const std::string js = tmpFile("corrupt.json").string();
        { std::ofstream o(js); o << "this is not json {{{"; }
        const std::string body = readAll(js);

        Forest g;
        CHECK(!store::loadJson(g, js), "a corrupt file fails to load");
        CHECK(fs::exists(js), "and is still on disk at that point");

        const std::string kept = store::quarantine(js);
        CHECK(!kept.empty(), "quarantine returns the new path");
        CHECK(!fs::exists(js), "the original path is now free for a fresh store");
        CHECK(fs::exists(kept), "the unreadable file still exists under its new name");
        CHECK(readAll(kept) == body, "byte-for-byte — nothing was rewritten");

        // A second failure must not clobber the first rescue.
        { std::ofstream o(js); o << "also broken"; }
        const std::string kept2 = store::quarantine(js);
        CHECK(!kept2.empty() && kept2 != kept, "a second rescue picks a fresh name");
        CHECK(readAll(kept) == body, "the first rescue is untouched");

        fs::remove(kept);
        fs::remove(kept2);

        // A DB and its WAL sidecars travel together.
        Forest f;
        f.addTask("in a db");
        const std::string db = tmpFile("quarantine.db").string();
        removeDb(db);
        CHECK(store::saveDb(f, db), "seed a DB");
        { std::ofstream o(db + "-wal", std::ios::binary); o << "sidecar"; }
        const std::string keptDb = store::quarantine(db);
        CHECK(!keptDb.empty() && !fs::exists(db), "the DB moved aside");
        CHECK(fs::exists(keptDb + "-wal"), "its -wal came along");
        removeDb(keptDb);

        CHECK(store::quarantine(tmpFile("never_existed.json").string()).empty(),
              "nothing to quarantine -> empty result");
    }

    // ---- A store from a newer build is identified, not touched -------------------
    {
        Forest f;
        f.addTask("from the future");
        const std::string db = tmpFile("future.db").string();
        removeDb(db);
        CHECK(store::saveDb(f, db), "seed");
        CHECK(store::dbSchemaVersion(db) == store::supportedDbSchemaVersion(),
              "a DB we just wrote reports our own schema version");

        {   // stamp it as newer than this build understands
            sqlite3* h = nullptr;
            CHECK(sqlite3_open(db.c_str(), &h) == SQLITE_OK, "reopen to bump the version");
            const std::string bump =
                "PRAGMA user_version=" + std::to_string(store::supportedDbSchemaVersion() + 7);
            CHECK(sqlite3_exec(h, bump.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK, "bump");
            sqlite3_close(h);
        }
        CHECK(store::dbSchemaVersion(db) > store::supportedDbSchemaVersion(),
              "a newer store is recognised as newer");
        const std::string before = readAll(db);
        Forest g;
        CHECK(!store::loadDb(g, db), "and refuses to load rather than misread it");
        CHECK(fs::exists(db), "the file is left exactly where it was");
        CHECK(readAll(db) == before,
              "and byte-for-byte unchanged — refusing must not write pragmas or an index");
        CHECK(!fs::exists(db + "-wal") && !fs::exists(db + "-shm"),
              "no sidecars created either");
        CHECK(store::dbSchemaVersion(tmpFile("no_such.db").string()) == -1,
              "a missing file has no schema version");
        removeDb(db);
    }

    // ---- Real data check (opt-in): TASKTREE_TEST_JSON=<a copy of tasks.json> ---
    // Proves the migration on the actual file rather than on synthetic fixtures.
    if (const char* real = std::getenv("TASKTREE_TEST_JSON")) {
        Forest live;
        CHECK(store::loadJson(live, real), "real tasks.json loads");
        const std::string db = tmpFile("real_data.db").string();
        removeDb(db);
        CHECK(store::migrateJsonToDb(real, db), "real tasks.json migrates + verifies");
        Forest back;
        CHECK(store::loadDb(back, db), "migrated real DB loads");
        CHECK(equivalent(live, back), "every real task survives byte-for-byte");
        std::printf("  real-data check: %zu tasks, %zu top-level, %zu DONE entries\n",
                    live.size(), live.roots.size(), live.doneSectionRoots().size());
        removeDb(db);
    } else {
        std::printf("  real-data check skipped (set TASKTREE_TEST_JSON to a tasks.json copy)\n");
    }

    // ---- Config round-trip -----------------------------------------------------
    {
        Config c;
        c.toggleHotkey = "Ctrl+Shift+T";
        c.maxNodeWidth = 333.f;
        c.overlayOpacity = 0.5f;
        c.llmEnabled = true;
        c.llmModel = "qwen2.5";

        const std::string path = tmpFile("config.toml").string();
        CHECK(saveConfig(c, path), "config save succeeds");
        Config d = loadConfig(path);
        CHECK(d.toggleHotkey == "Ctrl+Shift+T", "toggle hotkey preserved");
        CHECK(std::abs(d.maxNodeWidth - 333.f) < 0.01f, "max node width preserved");
        CHECK(std::abs(d.overlayOpacity - 0.5f) < 0.01f, "opacity preserved");
        CHECK(d.llmEnabled == true, "llm enabled preserved");
        CHECK(d.llmModel == "qwen2.5", "llm model preserved");
        fs::remove(path);

        // missing config -> defaults
        Config e = loadConfig(tmpFile("no_config.toml").string());
        CHECK(e.toggleHotkey == "Ctrl+Alt+Space", "missing config -> defaults");
    }

    // ---- Hotkey parsing --------------------------------------------------------
    {
        HotkeySpec s = parseHotkey("Ctrl+Alt+Space");
        CHECK((s.mods & Mod_Ctrl) && (s.mods & Mod_Alt), "ctrl+alt parsed");
        CHECK(!(s.mods & Mod_Shift), "no shift");
        CHECK(s.key == "Space", "key token captured");

        HotkeySpec r = parseHotkey("ctrl + shift + Return");
        CHECK((r.mods & Mod_Ctrl) && (r.mods & Mod_Shift), "case-insensitive mods + spaces");
        CHECK(r.key == "Return", "return key");

        HotkeySpec bare = parseHotkey("F1");
        CHECK(bare.mods == Mod_None && bare.key == "F1", "modifier-less hotkey");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
