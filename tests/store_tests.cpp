// Verification for persistence (Store) + config (Config). Built by CMake as the
// `store_tests` CTest target.
#include "app/Config.hpp"
#include "app/Paths.hpp"
#include "model/Store.hpp"
#include "model/Task.hpp"
#include "platform/Hotkey.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

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

int main() {
    // ---- Store round-trip ------------------------------------------------------
    {
        Forest f;
        TaskId a = f.addTask("alpha — éà");   // UTF-8 content
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
