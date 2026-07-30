#include "model/Store.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "app/Paths.hpp"

namespace tt::store {

using json = nlohmann::json;
namespace fs = std::filesystem;

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

    for (const auto& jt : j.value("tasks", json::array())) {
        Task t;
        t.id = jt.value("id", TaskId{0});
        if (t.id == 0) continue;
        t.parent = jt.value("parent", kNoParent);
        t.text = jt.value("text", std::string{});
        t.done = jt.value("done", false);
        t.collapsed = jt.value("collapsed", false);
        t.status = jt.value("status", 0);
        t.createdAt = jt.value("createdAt", std::int64_t{0});
        t.doneAt = jt.value("doneAt", std::int64_t{0});   // 0/null for pre-existing tasks
        for (const auto& c : jt.value("children", json::array()))
            t.children.push_back(c.get<TaskId>());
        f.nextId = std::max<TaskId>(f.nextId, t.id + 1);
        f.nodes.emplace(t.id, std::move(t));
    }
    f.nextId = std::max<TaskId>(f.nextId, j.value("nextId", f.nextId));

    // Prefer the explicit top-level order if present and still valid. "doneRoots" is a
    // legacy key: DONE tasks used to live in a second root list, and files written then
    // still have one. They are ordinary top-level tasks now (their done flag is what puts
    // them in the DONE section), so both lists append into `roots`.
    bool ok = j.contains("roots");
    auto readList = [&](const char* key, std::vector<TaskId>& out) {
        for (const auto& r : j[key]) {
            TaskId id = r.get<TaskId>();
            const Task* t = f.get(id);
            if (!t || t->parent != kNoParent) return false;
            out.push_back(id);
        }
        return true;
    };
    if (ok) ok = readList("roots", f.roots);
    if (ok && j.contains("doneRoots")) ok = readList("doneRoots", f.roots);
    if (!ok) {
        f.roots.clear();
        f.reindexRootsAfterLoad();
    }
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
            {"done", t.done},
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
