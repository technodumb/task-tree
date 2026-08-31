// Verification for the `tt` CLI. Built by CMake as the `cli_tests` CTest target. Drives
// tt::cli::run in-process (out/err captured into stringstreams) against temp stores — most
// via `--store <path>`, one via XDG_DATA_HOME to prove default store resolution.
#include "cli/Cli.hpp"
#include "model/Store.hpp"
#include "model/Task.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sqlite3.h>

using namespace tt;
namespace fs = std::filesystem;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) { ++g_fail; std::printf("  FAIL: %s\n", msg); }                \
    } while (0)

static fs::path tmpFile(const char* name) {
    return fs::temp_directory_path() / (std::string("tasktree_cli_") + name);
}
static void removeDb(const std::string& path) {
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove(path + "-wal", ec);
    fs::remove(path + "-shm", ec);
}

struct Res { int code; std::string out, err; };
static Res call(std::vector<std::string> args) {
    std::ostringstream out, err;
    const int code = cli::run(args, out, err);
    return {code, out.str(), err.str()};
}

// `tt add ... --store db`, returning the new id it printed on stdout.
static TaskId add(const std::string& db, const std::string& text, TaskId parent = 0) {
    std::vector<std::string> a = {"add", text, "--store", db};
    if (parent) { a.push_back("--parent"); a.push_back(std::to_string(parent)); }
    Res r = call(a);
    if (r.code != cli::kOk) return 0;
    return static_cast<TaskId>(std::stoull(r.out));
}

// Raw row count so we can see soft-deleted rows the public API hides. -1 on failure.
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

int main() {
    std::printf("cli_tests\n");

    // ---- add / tree round-trip, and ids chain ----------------------------------
    {
        const std::string db = tmpFile("roundtrip.db").string();
        removeDb(db);

        const TaskId root = add(db, "root task");
        CHECK(root != 0, "add prints a usable id");
        const TaskId child = add(db, "child task", root);
        CHECK(child != 0, "add under a parent works");

        Forest f;
        CHECK(store::load(f, db), "store loads after CLI writes");
        CHECK(f.exists(root) && f.exists(child), "both tasks persisted");
        CHECK(f.get(child)->parent == root, "the child is under its parent");

        Res tree = call({"tree", "--json", "--store", db});
        CHECK(tree.code == cli::kOk, "tree --json exits 0");
        CHECK(tree.out.find("\"created_at\"") != std::string::npos, "tree exposes created_at");
        CHECK(tree.out.find("child task") != std::string::npos, "tree includes the child");
        CHECK(tree.err.empty(), "tree writes no diagnostics on success");
        removeDb(db);
    }

    // ---- empty store is exit 0, not an error -----------------------------------
    {
        const std::string db = tmpFile("empty.db").string();
        removeDb(db);
        add(db, "seed");                       // create the store
        Forest f; store::load(f, db);
        for (TaskId r : std::vector<TaskId>(f.roots)) call({"rm", std::to_string(r), "--store", db});
        Res tree = call({"tree", "--json", "--store", db});
        CHECK(tree.code == cli::kOk, "tree on an all-deleted store is exit 0");
        CHECK(tree.out.find('[') != std::string::npos, "and prints an (empty) JSON array");
        removeDb(db);
    }

    // ---- node addressing: id, unique text, ambiguous, no-match -----------------
    {
        const std::string db = tmpFile("address.db").string();
        removeDb(db);
        const TaskId one = add(db, "alpha one");
        add(db, "alpha two");

        CHECK(call({"show", std::to_string(one), "--store", db}).code == cli::kOk,
              "addressing by id resolves");
        CHECK(call({"show", "alpha one", "--store", db}).code == cli::kOk,
              "a unique text match resolves");

        Res amb = call({"show", "alpha", "--store", db});
        CHECK(amb.code == cli::kAmbiguous, "an ambiguous query is kAmbiguous");
        CHECK(amb.out.empty(), "ambiguous: stdout stays clean");
        CHECK(amb.err.find("ambiguous") != std::string::npos, "ambiguous: candidates on stderr");

        Res miss = call({"show", "nonexistent-xyz", "--store", db});
        CHECK(miss.code == cli::kNotFound, "no text match is kNotFound");
        Res badId = call({"show", "999999", "--store", db});
        CHECK(badId.code == cli::kNotFound, "an unknown id is kNotFound");
        removeDb(db);
    }

    // ---- done / undone keep the task's slot; status; edit ----------------------
    {
        const std::string db = tmpFile("mutate.db").string();
        removeDb(db);
        const TaskId a = add(db, "first");
        const TaskId b = add(db, "second");

        CHECK(call({"done", std::to_string(a), "--store", db}).code == cli::kOk, "done ok");
        Forest f1; store::load(f1, db);
        CHECK(f1.get(a)->isDone(), "task is done");
        CHECK(f1.roots.size() == 2 && f1.roots[0] == a, "done task keeps its root slot");

        CHECK(call({"undone", std::to_string(a), "--store", db}).code == cli::kOk, "undone ok");
        Forest f2; store::load(f2, db);
        CHECK(!f2.get(a)->isDone(), "task is live again");

        CHECK(call({"done", std::to_string(a), "--store", db}).code == cli::kOk, "done again");
        CHECK(call({"done", std::to_string(a), "--store", db}).code == cli::kUsage,
              "done on an already-done task is a usage error");

        CHECK(call({"status", std::to_string(b), "priority", "--store", db}).code == cli::kOk, "status ok");
        Forest f3; store::load(f3, db);
        CHECK(f3.get(b)->status == 2, "priority maps to status 2");
        CHECK(call({"status", std::to_string(b), "bogus", "--store", db}).code == cli::kUsage,
              "an unknown status word is a usage error");

        CHECK(call({"edit", std::to_string(b), "renamed", "--store", db}).code == cli::kOk, "edit ok");
        Forest f4; store::load(f4, db);
        CHECK(f4.get(b)->text == "renamed", "edit changed the text");
        removeDb(db);
    }

    // ---- reparent, and cycle refusal -------------------------------------------
    {
        const std::string db = tmpFile("reparent.db").string();
        removeDb(db);
        const TaskId a = add(db, "aaa");
        const TaskId b = add(db, "bbb");
        const TaskId c = add(db, "ccc", b);   // c under b

        CHECK(call({"parent", std::to_string(b), std::to_string(a), "--store", db}).code == cli::kOk,
              "reparent b under a");
        Forest f1; store::load(f1, db);
        CHECK(f1.get(b)->parent == a, "b is now under a");
        CHECK(f1.get(c)->parent == b, "c came along under b");

        // a is now under b's old... no: a is root, b under a, c under b. Making a a child of
        // c would cycle (c is a's descendant).
        Res cyc = call({"parent", std::to_string(a), std::to_string(c), "--store", db});
        CHECK(cyc.code == cli::kUsage, "a cycling reparent is refused");
        Forest f2; store::load(f2, db);
        CHECK(f2.get(a)->parent == kNoParent, "and the store was not modified");
        removeDb(db);
    }

    // ---- rm is a soft delete: gone from the tree, present with deleted_at -------
    {
        const std::string db = tmpFile("softdelete.db").string();
        removeDb(db);
        const TaskId a = add(db, "keepme");
        const TaskId b = add(db, "removeme");

        CHECK(call({"rm", std::to_string(b), "--store", db}).code == cli::kOk, "rm ok");
        Forest f; store::load(f, db);
        CHECK(f.exists(a) && !f.exists(b), "the removed task no longer loads");
        CHECK(rawCount(db, "deleted_at!=0") == 1, "its row survives with deleted_at stamped");

        Res del = call({"deleted", "--json", "--store", db});
        CHECK(del.code == cli::kOk, "deleted exits 0");
        CHECK(del.out.find("removeme") != std::string::npos, "and lists the retired task");
        removeDb(db);
    }

    // ---- the guarantee every write rests on: a concurrent writer's rows survive -
    // This mirrors exactly what the CLI does — store::save(work, path, &baseline) — and
    // proves a task the app commits between our load and our save is not clobbered.
    {
        const std::string db = tmpFile("concurrent.db").string();
        removeDb(db);
        Forest seed;
        const TaskId a = seed.addTask("was here first");
        CHECK(store::save(seed, db), "seed the store");

        Forest baseline;                       // what the CLI loads
        CHECK(store::load(baseline, db), "CLI loads the baseline");
        Forest work = baseline;
        work.get(a)->text = "edited by tt";     // the CLI's pending mutation

        // The app commits a new task on its own connection, after our load.
        Forest appView;
        CHECK(store::load(appView, db), "app loads");
        const TaskId b = appView.addTask("added by the app");
        CHECK(store::save(appView, db, &baseline), "app commits its new task");

        // Now the CLI saves against the baseline it loaded — b is in neither baseline nor
        // work, so it must be left alone rather than deleted.
        CHECK(store::save(work, db, &baseline), "CLI saves with its baseline");

        Forest after; CHECK(store::load(after, db), "reload");
        CHECK(after.get(a) && after.get(a)->text == "edited by tt", "the CLI's edit landed");
        CHECK(after.exists(b), "the app's concurrently-added task SURVIVED");
        removeDb(db);
    }

    // ---- safety gate: a newer-schema store is refused, untouched ---------------
    {
        const std::string db = tmpFile("toonew.db").string();
        removeDb(db);
        add(db, "present");
        {   // stamp a schema version this build cannot read
            sqlite3* h = nullptr;
            sqlite3_open(db.c_str(), &h);
            const std::string bump =
                "PRAGMA user_version=" + std::to_string(store::supportedDbSchemaVersion() + 7);
            sqlite3_exec(h, bump.c_str(), nullptr, nullptr, nullptr);
            sqlite3_close(h);
        }
        Res r = call({"tree", "--store", db});
        CHECK(r.code == cli::kTooNew, "a newer store is refused with kTooNew");
        CHECK(r.out.empty(), "and nothing goes to stdout");
        Res w = call({"add", "nope", "--store", db});
        CHECK(w.code == cli::kTooNew, "and no write is attempted");
        removeDb(db);
    }

    // ---- safety: unreadable store is reported, never quarantined ----------------
    {
        const std::string db = tmpFile("corrupt.db").string();
        removeDb(db);
        { std::ofstream o(db, std::ios::binary); o << "this is not a sqlite database"; }
        Res r = call({"tree", "--store", db});
        CHECK(r.code == cli::kUnreadable, "an unreadable store is kUnreadable");
        CHECK(fs::exists(db), "and is left in place (not quarantined)");
        removeDb(db);
    }

    // ---- JSON store: readable, never migrated to a sibling .db ------------------
    {
        const std::string js = tmpFile("legacy.json").string();
        fs::remove(js);
        Forest f; f.addTask("in json");
        CHECK(store::saveJson(f, js), "seed a JSON store");
        Res tree = call({"tree", "--json", "--store", js});
        CHECK(tree.code == cli::kOk && tree.out.find("in json") != std::string::npos,
              "a JSON store reads fine");
        CHECK(!fs::exists(tmpFile("legacy.db")), "no tasks.db is ever created from JSON");
        fs::remove(js);
    }

    // ---- default store resolution via XDG_DATA_HOME ----------------------------
    {
        const fs::path xdg = tmpFile("xdg_home");
        fs::remove_all(xdg);
        fs::create_directories(xdg / "tasktree");   // paths::dataDir() == $XDG_DATA_HOME/tasktree
#ifdef _WIN32
        _putenv_s("XDG_DATA_HOME", xdg.string().c_str());
#else
        setenv("XDG_DATA_HOME", xdg.string().c_str(), 1);
#endif
        // No store yet -> a read must fail non-zero rather than inventing one.
        CHECK(call({"tree"}).code == cli::kUnreadable, "no store -> non-zero, names the path");
        // Seed a db at the resolved default path and confirm the CLI finds it with no --store.
        Forest f; f.addTask("from xdg default");
        CHECK(store::save(f, (xdg / "tasktree" / "tasks.db").string()), "seed the default db");
        Res tree = call({"tree", "--json"});
        CHECK(tree.code == cli::kOk && tree.out.find("from xdg default") != std::string::npos,
              "the default store is resolved from XDG_DATA_HOME");
        fs::remove_all(xdg);
    }

    std::printf("  %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
