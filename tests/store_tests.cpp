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
        CHECK(g.doneRoots.size() == 1 && g.doneRoots[0] == d, "doneRoots persisted");
        CHECK(g.get(d) && g.get(d)->done, "done flag persisted");
        CHECK(std::find(g.roots.begin(), g.roots.end(), d) == g.roots.end(),
              "done task is not a canvas root");
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
        CHECK(g.roots == f.roots, "canvas root order preserved");
        CHECK(g.doneRoots == f.doneRoots, "DONE root order preserved");
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
        std::printf("  real-data check: %zu tasks, %zu roots, %zu done roots\n",
                    live.size(), live.roots.size(), live.doneRoots.size());
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
