// The `tt` CLI: read and mutate the task store from outside the overlay, with the app
// running or stopped. See openspec/changes/add-tt-cli for the design; the short version:
//
//   * Direct store access, made safe by v3's store layer (WAL, BEGIN IMMEDIATE, a busy
//     timeout, and baseline-diffed saves). The app polls PRAGMA data_version and reloads
//     within ~1s, so a write here shows up live with no restart.
//   * EVERY write passes the loaded forest as `baseline` to store::save, so a deletion is
//     "present in baseline, gone from the forest" — never "absent from the forest". Rows a
//     concurrent writer (the app) added while we ran are in neither, so they survive. This
//     is the single most important rule here; there is exactly one save call site below.
//   * An unprivileged writer: it honours the schema-version guard, and never quarantines a
//     damaged store or migrates JSON→SQLite. Those are the app's calls, not an agent's.

#include "cli/Cli.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <limits>
#include <ostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/Palette.hpp"
#include "app/Paths.hpp"
#include "model/Store.hpp"
#include "model/Task.hpp"

namespace tt::cli {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// ---- argument model ---------------------------------------------------------

struct Args {
    std::string command;             // first positional, "" if none
    std::vector<std::string> rest;   // positionals after the command
    bool json = false;               // --json
    std::string store;               // --store <path>, empty = default resolution
    std::string parent;              // --parent <spec> (add)
    bool hasParent = false;
};

// Parse global + known flags out of argv, leaving positionals in order. Returns false and
// writes the offending flag to `err` on a malformed flag (a missing value, or an unknown
// --flag), so the caller can exit kUsage.
bool parseArgs(const std::vector<std::string>& argv, Args& a, std::ostream& err) {
    std::vector<std::string> pos;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string& s = argv[i];
        auto needValue = [&](const char* name, std::string& out) -> bool {
            if (i + 1 >= argv.size()) { err << name << " needs a value\n"; return false; }
            out = argv[++i];
            return true;
        };
        if (s == "--json") a.json = true;
        else if (s == "--store") { if (!needValue("--store", a.store)) return false; }
        else if (s == "--parent") { a.hasParent = true; if (!needValue("--parent", a.parent)) return false; }
        else if (s.size() > 2 && s[0] == '-' && s[1] == '-') { err << "unknown flag " << s << "\n"; return false; }
        else pos.push_back(s);
    }
    if (!pos.empty()) { a.command = pos.front(); a.rest.assign(pos.begin() + 1, pos.end()); }
    return true;
}

// ---- store resolution + safety gate ----------------------------------------

// The store the app would use: tasks.db if it exists, else tasks.json, honouring
// XDG_DATA_HOME (paths::* already does). Empty when neither exists — we never create one.
std::string defaultStore() {
    const std::string db = paths::dbFile().string();
    if (fs::exists(db)) return db;
    const std::string js = paths::tasksFile().string();
    if (fs::exists(js)) return js;
    return "";
}

// Refuse a SQLite store whose schema is newer than this build, BEFORE any read or write —
// naming both versions. Returns kOk for JSON stores and for a DB this build understands.
Exit schemaGate(const std::string& path, std::ostream& err) {
    if (!store::isDbPath(path)) return kOk;
    const int have = store::dbSchemaVersion(path);            // -1 if missing / not a DB
    const int supported = store::supportedDbSchemaVersion();
    if (have > supported) {
        err << "store " << path << " was written by a newer TaskTree (schema " << have
            << ", this build reads " << supported << "). Refusing to touch it — run the "
               "newer build.\n";
        return kTooNew;
    }
    return kOk;
}

// Load the store into `f`. A load that fails on a file that EXISTS means unreadable data,
// not an absent store: we report and stop rather than letting a later save replace it with
// an empty tree, and we never quarantine (the app owns recovery). A genuinely absent file
// leaves `f` empty and succeeds — an empty tree is a valid read, not an error.
Exit loadStore(const std::string& path, Forest& f, std::ostream& err) {
    if (const Exit e = schemaGate(path, err); e != kOk) return e;
    if (!store::load(f, path) && fs::exists(path)) {
        err << "store " << path << " exists but could not be read. Run TaskTree to recover "
               "it; this tool will not touch a store it cannot read.\n";
        return kUnreadable;
    }
    return kOk;
}

// THE one and only call site of store::save — always with a baseline (see the file header).
Exit saveStore(const Forest& work, const std::string& path, const Forest& baseline,
               std::ostream& err) {
    if (!store::save(work, path, &baseline)) {
        err << "could not write " << path << " — it may be locked by the app past the busy "
               "timeout, or unwritable.\n";
        return kBusy;
    }
    return kOk;
}

// ---- node addressing --------------------------------------------------------

bool allDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

// Resolve a node spec to a live task id: a number is an id, anything else a text query
// resolved with palette::rankMatches (so the CLI and the overlay agree on what a name
// means). Ambiguity is an error listing the candidates, never a silent guess.
Exit resolveNode(const Forest& f, const std::string& spec, TaskId& out, std::ostream& err) {
    if (allDigits(spec)) {
        TaskId id = 0;
        const char* end = spec.data() + spec.size();
        if (std::from_chars(spec.data(), end, id).ec == std::errc{} && f.exists(id)) {
            out = id;
            return kOk;
        }
        err << "no live task with id " << spec << "\n";
        return kNotFound;
    }
    const std::vector<TaskId> matches = palette::rankMatches(f, spec);
    if (matches.empty()) {
        err << "no live task matches \"" << spec << "\"\n";
        return kNotFound;
    }
    if (matches.size() > 1) {
        err << "\"" << spec << "\" is ambiguous — " << matches.size() << " matches:\n";
        for (std::size_t i = 0; i < matches.size() && i < 12; ++i)
            err << "  #" << matches[i] << " " << f.get(matches[i])->text << "\n";
        err << "address it by id, or narrow the query.\n";
        return kAmbiguous;
    }
    out = matches.front();
    return kOk;
}

// ---- serialisation ----------------------------------------------------------

int ordOf(const Forest& f, const Task& t) {
    const std::vector<TaskId>& sibs =
        (t.parent == kNoParent) ? f.roots
                                : (f.get(t.parent) ? f.get(t.parent)->children : f.roots);
    for (std::size_t i = 0; i < sibs.size(); ++i)
        if (sibs[i] == t.id) return static_cast<int>(i);
    return -1;
}

// The JSON shape for a task — the documented, stable field set (snake_case, distinct from
// the internal JSON store's camelCase). `ord` is the task's slot among its siblings.
json taskFields(const Forest& f, const Task& t, int ord) {
    return json{
        {"id", t.id},
        {"text", t.text},
        {"parent", t.parent},
        {"ord", ord},
        {"status", t.status},
        {"collapsed", t.collapsed},
        {"created_at", t.createdAt},
        {"done_at", t.doneAt},
    };
}

// A task and, recursively, its whole subtree — `children` in sibling order, each with its
// own `ord`. Done tasks are included and carry done_at, because a done task keeps its slot.
json subtreeJson(const Forest& f, TaskId id, int ord) {
    const Task& t = *f.get(id);
    json j = taskFields(f, t, ord);
    json kids = json::array();
    for (std::size_t i = 0; i < t.children.size(); ++i)
        kids.push_back(subtreeJson(f, t.children[i], static_cast<int>(i)));
    j["children"] = std::move(kids);
    return j;
}

std::string doneTag(const Task& t) {
    std::string s;
    if (t.isDone()) s += " ✓";
    if (t.status == 1) s += " [in-progress]";
    else if (t.status == 2) s += " [priority]";
    return s;
}

void printOutline(const Forest& f, TaskId id, int depth, std::ostream& out) {
    const Task& t = *f.get(id);
    out << std::string(static_cast<std::size_t>(depth) * 2, ' ')
        << "#" << t.id << " " << t.text << doneTag(t) << "\n";
    for (TaskId c : t.children) printOutline(f, c, depth + 1, out);
}

// ---- commands ---------------------------------------------------------------

void printHelp(std::ostream& out) {
    out <<
        "tt — read and mutate the TaskTree store (app running or stopped)\n"
        "\n"
        "Read:\n"
        "  tree [<node>]            whole forest, or a subtree, as an indented outline\n"
        "  show <node>              one task and its immediate children\n"
        "  find <query>             ranked text matches, best first\n"
        "  deleted                  soft-deleted rows, newest first\n"
        "Write:\n"
        "  add <text> [--parent <node>]   append a task (prints the new id)\n"
        "  edit <node> <text>             replace a task's text\n"
        "  done <node> | undone <node>    set / clear completion (keeps its slot)\n"
        "  status <node> <normal|in-progress|priority>\n"
        "  parent <child> <parent>        reparent a subtree (refuses cycles)\n"
        "  rm <node>                      soft-delete a subtree (recoverable; see `deleted`)\n"
        "\n"
        "A <node> is a numeric id or a text query (unique match required; ambiguity is an error).\n"
        "\n"
        "Global flags:\n"
        "  --json                   emit one JSON document on stdout (diagnostics stay on stderr)\n"
        "  --store <path>           operate on this store instead of the default\n"
        "\n"
        "Exit codes: 0 ok · 1 usage/refused · 2 not-found · 3 ambiguous · "
        "4 store-unreadable · 5 store-too-new · 6 store-busy\n";
}

// Status word <-> stored int (0 default, 1 in progress, 2 priority; see Task::status).
bool statusFromWord(const std::string& w, int& out) {
    if (w == "normal" || w == "default") { out = 0; return true; }
    if (w == "in-progress" || w == "in_progress") { out = 1; return true; }
    if (w == "priority") { out = 2; return true; }
    return false;
}

int cmdTree(const Args& a, const Forest& f, std::ostream& out, std::ostream& err) {
    if (!a.rest.empty()) {
        TaskId id = 0;
        if (const Exit e = resolveNode(f, a.rest.front(), id, err); e != kOk) return e;
        if (a.json) out << subtreeJson(f, id, ordOf(f, *f.get(id))).dump(2) << "\n";
        else printOutline(f, id, 0, out);
        return kOk;
    }
    if (a.json) {
        json arr = json::array();
        for (std::size_t i = 0; i < f.roots.size(); ++i)
            arr.push_back(subtreeJson(f, f.roots[i], static_cast<int>(i)));
        out << arr.dump(2) << "\n";
    } else {
        for (TaskId r : f.roots) printOutline(f, r, 0, out);
    }
    return kOk;
}

int cmdShow(const Args& a, const Forest& f, std::ostream& out, std::ostream& err) {
    if (a.rest.empty()) { err << "show needs a node\n"; return kUsage; }
    TaskId id = 0;
    if (const Exit e = resolveNode(f, a.rest.front(), id, err); e != kOk) return e;
    const Task& t = *f.get(id);
    if (a.json) {
        json j = taskFields(f, t, ordOf(f, t));
        json kids = json::array();
        for (std::size_t i = 0; i < t.children.size(); ++i)
            kids.push_back(taskFields(f, *f.get(t.children[i]), static_cast<int>(i)));
        j["children"] = std::move(kids);
        out << j.dump(2) << "\n";
    } else {
        out << "#" << t.id << " " << t.text << doneTag(t) << "\n";
        out << "  parent " << t.parent << "  ord " << ordOf(f, t)
            << "  created_at " << t.createdAt << "  done_at " << t.doneAt << "\n";
        for (TaskId c : t.children) out << "  #" << c << " " << f.get(c)->text << doneTag(*f.get(c)) << "\n";
    }
    return kOk;
}

int cmdFind(const Args& a, const Forest& f, std::ostream& out, std::ostream& err) {
    if (a.rest.empty()) { err << "find needs a query\n"; return kUsage; }
    const std::vector<TaskId> matches = palette::rankMatches(f, a.rest.front());
    if (a.json) {
        json arr = json::array();
        for (TaskId id : matches) arr.push_back(taskFields(f, *f.get(id), ordOf(f, *f.get(id))));
        out << arr.dump(2) << "\n";
    } else {
        for (TaskId id : matches) out << "#" << id << " " << f.get(id)->text << doneTag(*f.get(id)) << "\n";
    }
    return kOk;   // no match is still success — an empty result, not an error
}

int cmdDeleted(const Args& a, const std::string& path, std::ostream& out) {
    const std::vector<store::DeletedRow> rows = store::deletedRows(path);   // empty for JSON/missing
    if (a.json) {
        json arr = json::array();
        for (const auto& r : rows)
            arr.push_back(json{{"id", r.id}, {"parent", r.parent}, {"text", r.text}, {"deleted_at", r.deletedAt}});
        out << arr.dump(2) << "\n";
    } else {
        for (const auto& r : rows)
            out << "#" << r.id << " " << r.text << "  (deleted " << r.deletedAt << ")\n";
    }
    return kOk;
}

int cmdAdd(const Args& a, const std::string& path, Forest& baseline, std::ostream& out, std::ostream& err) {
    if (a.rest.empty()) { err << "add needs text\n"; return kUsage; }
    Forest work = baseline;
    TaskId parentId = kNoParent;
    if (a.hasParent) {
        if (const Exit e = resolveNode(work, a.parent, parentId, err); e != kOk) return e;
    }
    const TaskId id = work.addTask(a.rest.front(), parentId, nowMs());
    if (const Exit e = saveStore(work, path, baseline, err); e != kOk) return e;
    if (a.json) out << json{{"id", id}}.dump() << "\n";
    else out << id << "\n";
    return kOk;
}

int cmdEdit(const Args& a, const std::string& path, Forest& baseline, std::ostream& err) {
    if (a.rest.size() < 2) { err << "edit needs a node and text\n"; return kUsage; }
    Forest work = baseline;
    TaskId id = 0;
    if (const Exit e = resolveNode(work, a.rest[0], id, err); e != kOk) return e;
    work.get(id)->text = a.rest[1];
    return saveStore(work, path, baseline, err);
}

int cmdDone(const Args& a, bool done, const std::string& path, Forest& baseline, std::ostream& err) {
    if (a.rest.empty()) { err << (done ? "done" : "undone") << " needs a node\n"; return kUsage; }
    Forest work = baseline;
    TaskId id = 0;
    if (const Exit e = resolveNode(work, a.rest.front(), id, err); e != kOk) return e;
    const bool changed = done ? work.markDone(id, nowMs()) : work.restoreFromDone(id);
    if (!changed) {
        err << "#" << id << " is already " << (done ? "done" : "not done") << "\n";
        return kUsage;
    }
    return saveStore(work, path, baseline, err);
}

int cmdStatus(const Args& a, const std::string& path, Forest& baseline, std::ostream& err) {
    if (a.rest.size() < 2) { err << "status needs a node and one of normal|in-progress|priority\n"; return kUsage; }
    int s = 0;
    if (!statusFromWord(a.rest[1], s)) { err << "unknown status \"" << a.rest[1] << "\"\n"; return kUsage; }
    Forest work = baseline;
    TaskId id = 0;
    if (const Exit e = resolveNode(work, a.rest[0], id, err); e != kOk) return e;
    work.get(id)->status = s;
    return saveStore(work, path, baseline, err);
}

int cmdParent(const Args& a, const std::string& path, Forest& baseline, std::ostream& err) {
    if (a.rest.size() < 2) { err << "parent needs a child and a parent\n"; return kUsage; }
    Forest work = baseline;
    TaskId child = 0, parent = 0;
    if (const Exit e = resolveNode(work, a.rest[0], child, err); e != kOk) return e;
    if (const Exit e = resolveNode(work, a.rest[1], parent, err); e != kOk) return e;
    // INT_MAX index => append as the last child (reparent clamps). false => the move would
    // cycle (onto itself or a descendant), which the model refuses; we do too, untouched.
    if (!work.reparent(child, parent, std::numeric_limits<int>::max())) {
        err << "cannot make #" << child << " a child of #" << parent
            << " — that would create a cycle\n";
        return kUsage;
    }
    return saveStore(work, path, baseline, err);
}

int cmdRm(const Args& a, const std::string& path, Forest& baseline, std::ostream& err) {
    if (a.rest.empty()) { err << "rm needs a node\n"; return kUsage; }
    Forest work = baseline;
    TaskId id = 0;
    if (const Exit e = resolveNode(work, a.rest.front(), id, err); e != kOk) return e;
    work.removeSubtree(id);
    // Save with the loaded baseline: the removed rows are "in baseline, gone from work", so
    // the SQLite backend stamps deleted_at rather than dropping them — a recoverable delete.
    return saveStore(work, path, baseline, err);
}

} // namespace

int run(const std::vector<std::string>& argv, std::ostream& out, std::ostream& err) {
    // --help / --version are answered before any store work, and even with no store present.
    for (const std::string& s : argv) {
        if (s == "--help" || s == "-h") { printHelp(out); return kOk; }
        if (s == "--version") { out << "tt (TaskTree CLI) 0.1\n"; return kOk; }
    }

    Args a;
    if (!parseArgs(argv, a, err)) return kUsage;
    if (a.command.empty()) { printHelp(err); return kUsage; }

    const std::string path = a.store.empty() ? defaultStore() : a.store;
    if (path.empty()) {
        err << "no task store found. Expected " << paths::dbFile().string()
            << " (run TaskTree once to create it).\n";
        return kUnreadable;
    }

    // `deleted` reads retired rows directly and needs no loaded forest; everything else
    // wants the live tree. All commands honour the schema gate first.
    if (a.command == "deleted") {
        if (const Exit e = schemaGate(path, err); e != kOk) return e;
        return cmdDeleted(a, path, out);
    }

    Forest forest;   // the baseline: the state as loaded, passed to every save unchanged.
    if (const Exit e = loadStore(path, forest, err); e != kOk) return e;

    if (a.command == "tree")   return cmdTree(a, forest, out, err);
    if (a.command == "show")   return cmdShow(a, forest, out, err);
    if (a.command == "find")   return cmdFind(a, forest, out, err);
    if (a.command == "add")    return cmdAdd(a, path, forest, out, err);
    if (a.command == "edit")   return cmdEdit(a, path, forest, err);
    if (a.command == "done")   return cmdDone(a, true, path, forest, err);
    if (a.command == "undone") return cmdDone(a, false, path, forest, err);
    if (a.command == "status") return cmdStatus(a, path, forest, err);
    if (a.command == "parent") return cmdParent(a, path, forest, err);
    if (a.command == "rm")     return cmdRm(a, path, forest, err);

    err << "unknown command \"" << a.command << "\" (try --help)\n";
    return kUsage;
}

} // namespace tt::cli
