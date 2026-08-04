#include "model/Store.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "app/Paths.hpp"

namespace tt::store {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// Tolerant field readers. A missing, null, or wrong-typed value falls back to the default
// instead of throwing: files in the wild carry `"doneAt": null`, and a hand-edited one can
// have a string where a number belongs. A store that dies on either takes the whole app
// down at startup, which is a far worse outcome than a defaulted field.
std::int64_t numField(const json& j, const char* key, std::int64_t fallback = 0) {
    const auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<std::int64_t>() : fallback;
}

bool boolField(const json& j, const char* key, bool fallback = false) {
    const auto it = j.find(key);
    if (it == j.end()) return fallback;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0.0;   // 0/1 from another writer
    return fallback;
}

std::string strField(const json& j, const char* key) {
    const auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

// A task id from a bare JSON value / a named field. Anything not a positive integer is 0,
// which every caller treats as "no id".
TaskId asId(const json& v) {
    if (!v.is_number_integer() && !v.is_number_unsigned()) return 0;
    const std::int64_t n = v.get<std::int64_t>();
    return n > 0 ? static_cast<TaskId>(n) : 0;
}

TaskId idField(const json& j, const char* key) {
    const auto it = j.find(key);
    return it == j.end() ? 0 : asId(*it);
}

} // namespace

bool isDbPath(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".db" || ext == ".sqlite" || ext == ".sqlite3";
}

bool load(Forest& f, const std::string& path) {
    return isDbPath(path) ? loadDb(f, path) : loadJson(f, path);
}

bool save(const Forest& f, const std::string& path, const Forest* baseline) {
    return isDbPath(path) ? saveDb(f, path, baseline) : saveJson(f, path);
}

bool loadJson(Forest& f, const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;

    json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        return false; // corrupt file -> treat as empty rather than crash
    }

    f.nodes.clear();
    f.roots.clear();
    f.nextId = 1;

    const json* tasks = j.contains("tasks") && j["tasks"].is_array() ? &j["tasks"] : nullptr;
    if (tasks) for (const auto& jt : *tasks) {
        if (!jt.is_object()) continue;
        Task t;
        t.id = idField(jt, "id");
        if (t.id == 0) continue;              // 0 is "no parent"; never a usable id
        t.parent = idField(jt, "parent");
        t.text = strField(jt, "text");
        t.collapsed = boolField(jt, "collapsed");
        t.status = static_cast<int>(numField(jt, "status"));
        t.createdAt = numField(jt, "createdAt");
        t.doneAt = numField(jt, "doneAt");
        // "done" is a legacy key: completion used to be a boolean beside the timestamp, and
        // tasks finished before the timestamp existed have done=true with doneAt=0. Those
        // must become kDoneAtUnknown, or reading an old file would un-complete every one of
        // them. A doneAt on a not-done row was always meaningless, so it is cleared. Only a
        // *meaningful* done key counts: `"done": null` must not silently un-complete a task
        // that carries a real doneAt.
        if (const auto it = jt.find("done");
            it != jt.end() && (it->is_boolean() || it->is_number())) {
            const bool wasDone = boolField(jt, "done");
            if (wasDone && t.doneAt == 0)  t.doneAt = kDoneAtUnknown;
            if (!wasDone && t.doneAt != 0) t.doneAt = 0;
        }
        if (const auto it = jt.find("children"); it != jt.end() && it->is_array())
            for (const auto& c : *it)
                if (const TaskId cid = asId(c); cid != 0) t.children.push_back(cid);
        f.nextId = std::max<TaskId>(f.nextId, t.id + 1);
        f.nodes.emplace(t.id, std::move(t));   // a duplicate id keeps the first; see jsonTaskCount
    }
    f.nextId = std::max<TaskId>(f.nextId, static_cast<TaskId>(numField(j, "nextId")));

    // Prefer the explicit top-level order if present and still valid. "doneRoots" is a
    // legacy key: DONE tasks used to live in a second root list, and files written then
    // still have one. They are ordinary top-level tasks now (their done flag is what puts
    // them in the DONE section), so both lists append into `roots`.
    bool ok = j.contains("roots") && j["roots"].is_array();
    auto readList = [&](const char* key, std::vector<TaskId>& out) {
        if (!j[key].is_array()) return false;
        for (const auto& r : j[key]) {
            const TaskId id = asId(r);
            const Task* t = f.get(id);
            if (!t || t->parent != kNoParent) return false;
            out.push_back(id);
        }
        return true;
    };
    if (ok) ok = readList("roots", f.roots);
    if (ok && j.contains("doneRoots")) ok = readList("doneRoots", f.roots);
    // Couldn't read an explicit roots order: drop the partially-filled list so
    // repairAfterLoad() below rebuilds it from the parent-less nodes (id-sorted).
    if (!ok) f.roots.clear();
    // The file can disagree with itself — `parent` and `children` are two encodings of one
    // edge, and an older or hand-written file may have only the former. Reconcile them so no
    // task is left existing-but-unreachable.
    f.repairAfterLoad();
    return true;
}

std::string quarantine(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return {};
    for (int n = 0; n < 1000; ++n) {
        const std::string dest = path + ".unreadable" + (n == 0 ? "" : "-" + std::to_string(n));
        if (fs::exists(dest, ec)) continue;          // never clobber an earlier rescue
        fs::rename(path, dest, ec);
        if (ec) return {};
        // A SQLite store is up to three files; its sidecars belong with it.
        for (const char* suffix : {"-wal", "-shm"}) {
            std::error_code side;
            if (fs::exists(path + suffix, side)) fs::rename(path + suffix, dest + suffix, side);
        }
        return dest;
    }
    return {};
}

bool jsonTaskCount(const std::string& path, std::size_t& objects, std::size_t& distinct) {
    objects = distinct = 0;
    std::ifstream in(path);
    if (!in) return false;
    json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        return false;
    }
    if (!j.contains("tasks") || !j["tasks"].is_array()) return true;   // no tasks = 0 of them
    std::unordered_set<TaskId> ids;
    for (const auto& jt : j["tasks"]) {
        if (!jt.is_object()) continue;
        const TaskId id = idField(jt, "id");
        if (id == 0) continue;
        ++objects;
        ids.insert(id);
    }
    distinct = ids.size();
    return true;
}

bool saveJson(const Forest& f, const std::string& path) {
    fs::path p(path);
    if (auto dir = p.parent_path(); !dir.empty()) paths::ensureDir(dir);

    json tasks = json::array();
    for (const auto& [id, t] : f.nodes) {
        tasks.push_back({
            {"id", t.id},
            {"parent", t.parent},
            {"text", t.text},
            {"collapsed", t.collapsed},
            {"status", t.status},
            {"createdAt", t.createdAt},
            {"doneAt", t.doneAt},
            {"children", t.children},
        });
    }
    // No "doneRoots": every top-level task is in `roots` now, done or not (loadJson still
    // reads the old key so pre-existing files keep working).
    json j = {
        {"version", 2},
        {"nextId", f.nextId},
        {"roots", f.roots},
        {"tasks", std::move(tasks)},
    };

    // Atomic: write a sibling temp file, then rename over the target.
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << j.dump(2);
        out.flush();
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace tt::store
