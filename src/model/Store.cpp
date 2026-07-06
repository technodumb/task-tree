#include "model/Store.hpp"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "app/Paths.hpp"

namespace tt::store {

using json = nlohmann::json;
namespace fs = std::filesystem;

bool load(Forest& f, const std::string& path) {
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
        t.createdAt = jt.value("createdAt", std::int64_t{0});
        for (const auto& c : jt.value("children", json::array()))
            t.children.push_back(c.get<TaskId>());
        f.nextId = std::max<TaskId>(f.nextId, t.id + 1);
        f.nodes.emplace(t.id, std::move(t));
    }
    f.nextId = std::max<TaskId>(f.nextId, j.value("nextId", f.nextId));

    // Prefer the explicit top-level order if present and still valid.
    bool rootsOk = j.contains("roots");
    if (rootsOk) {
        for (const auto& r : j["roots"]) {
            TaskId id = r.get<TaskId>();
            const Task* t = f.get(id);
            if (!t || t->parent != kNoParent) { rootsOk = false; break; }
            f.roots.push_back(id);
        }
    }
    if (!rootsOk) {
        f.roots.clear();
        f.reindexRootsAfterLoad();
    }
    return true;
}

bool save(const Forest& f, const std::string& path) {
    fs::path p(path);
    if (auto dir = p.parent_path(); !dir.empty()) paths::ensureDir(dir);

    json tasks = json::array();
    for (const auto& [id, t] : f.nodes) {
        tasks.push_back({
            {"id", t.id},
            {"parent", t.parent},
            {"text", t.text},
            {"done", t.done},
            {"createdAt", t.createdAt},
            {"children", t.children},
        });
    }
    json j = {
        {"version", 1},
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
