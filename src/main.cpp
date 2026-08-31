// TaskTree entry point: initialise platform + GL + NanoVG, load config/tasks, wire
// input callbacks, and run the on-demand event loop.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <glad/gl.h>

#include <GLFW/glfw3.h>

#include <nanovg.h>
#define NANOVG_GL3   // select the GL3 backend so nvgCreateGL3/nvgDeleteGL3 are declared
#include <nanovg_gl.h>

#include "app/App.hpp"
#include "app/Config.hpp"
#include "app/Paths.hpp"
#include "llm/LlmLog.hpp"
#include "llm/NullClassifier.hpp"
#include "llm/OpenAiClassifier.hpp"
#include "model/Store.hpp"
#include "platform/PlatformGlfw.hpp"
#include "render/Renderer.hpp"

#ifndef TASKTREE_ASSETS_DIR
#define TASKTREE_ASSETS_DIR "assets"
#endif

using namespace tt;

namespace {

App* appOf(GLFWwindow* w) { return static_cast<App*>(glfwGetWindowUserPointer(w)); }

std::string pickFont() {
    std::vector<std::string> candidates = {
        std::string(TASKTREE_ASSETS_DIR) + "/fonts/Inter-Regular.ttf",
        std::string(TASKTREE_ASSETS_DIR) + "/fonts/UI.ttf",
    };
#ifdef __APPLE__
    // System .ttf files only: NanoVG loads faces with stb_truetype at offset 0, so
    // the TrueType *collections* macOS ships its UI fonts in (Helvetica.ttc, and
    // SFNS) would be misparsed.
    candidates.insert(candidates.end(), {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/System/Library/Fonts/Supplemental/Verdana.ttf",
        "/System/Library/Fonts/Geneva.ttf",
    });
#else
    candidates.insert(candidates.end(), {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    });
#endif
    std::error_code ec;
    for (const auto& p : candidates)
        if (std::filesystem::exists(p, ec)) return p;
    return {};
}

} // namespace

int main() {
    PlatformGlfw platform;
    if (!platform.init("TaskTree")) {
        std::fprintf(stderr, "Failed to create the overlay window.\n");
        return 1;
    }

    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) {
        std::fprintf(stderr, "Failed to load OpenGL functions.\n");
        platform.shutdown();
        return 1;
    }

    NVGcontext* vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!vg) {
        std::fprintf(stderr, "Failed to create the NanoVG context.\n");
        platform.shutdown();
        return 1;
    }

    const std::string fontPath = pickFont();
    if (fontPath.empty()) {
        std::fprintf(stderr, "No usable font found. Install a TTF font or bundle one in "
                             "%s/fonts/.\n", TASKTREE_ASSETS_DIR);
        nvgDeleteGL3(vg);
        platform.shutdown();
        return 1;
    }

    Renderer renderer;
    if (!renderer.init(vg, fontPath)) {
        std::fprintf(stderr, "Failed to load font: %s\n", fontPath.c_str());
        nvgDeleteGL3(vg);
        platform.shutdown();
        return 1;
    }

    Config cfg = loadOrCreateDefaultConfig();

    // Store: SQLite (docs/FUTURE.md "SQLite store"). A pre-SQLite tasks.json is migrated
    // once, and only if the migration can prove it round-tripped exactly — otherwise we
    // stay on JSON. Either way tasks.json is never written, renamed or removed here, so
    // the old file remains a complete backup of the state before the switch.
    Forest forest;
    const std::string jsonPath = paths::tasksFile().string();
    std::string tasksPath = paths::dbFile().string();
    if (!std::filesystem::exists(tasksPath) && std::filesystem::exists(jsonPath)) {
        if (store::migrateJsonToDb(jsonPath, tasksPath)) {
            std::fprintf(stderr, "Store: migrated to %s (verified); %s is now an untouched "
                                 "backup and is no longer read or written\n",
                         tasksPath.c_str(), jsonPath.c_str());
        } else {
            tasksPath = jsonPath;
            std::fprintf(stderr, "Store: SQLite migration did not verify — staying on %s. "
                                 "Nothing was changed.\n", jsonPath.c_str());
        }
    }
    // One Session for the app's whole life. Its held connection is what makes the
    // external-change poll work: the app's own saves go through it (and so never look
    // like news), while any other process's commit moves PRAGMA data_version.
    store::Session session(tasksPath);
    // A load that fails on a file that EXISTS means the data is unreadable, not absent — and
    // carrying on would let the first save replace it with an empty store. So: never write
    // over a store we could not read. (A failed load drops the session's connection, so
    // quarantine below can move the file out from under it.)
    if (!session.load(forest) && std::filesystem::exists(tasksPath)) {
        const int have = store::isDbPath(tasksPath) ? store::dbSchemaVersion(tasksPath) : -1;
        if (have > store::supportedDbSchemaVersion()) {
            // Not damaged — just newer than this binary understands. Touching it (even to
            // move it aside) would be wrong; the user wants their newer build back.
            std::fprintf(stderr,
                         "Store: %s was written by a newer TaskTree (schema %d, this build "
                         "reads %d). Refusing to touch it. Run the newer build, or move the "
                         "file aside yourself.\n",
                         tasksPath.c_str(), have, store::supportedDbSchemaVersion());
            nvgDeleteGL3(vg);
            platform.shutdown();
            return 1;
        }
        const std::string kept = store::quarantine(tasksPath);
        if (kept.empty()) {
            std::fprintf(stderr,
                         "Store: %s is unreadable and could not be moved aside. Refusing to "
                         "start, so nothing overwrites it.\n", tasksPath.c_str());
            nvgDeleteGL3(vg);
            platform.shutdown();
            return 1;
        }
        std::fprintf(stderr,
                     "Store: %s was unreadable — kept as %s and starting with an empty tree. "
                     "Nothing was discarded.\n", tasksPath.c_str(), kept.c_str());
        forest = Forest{};
    }

    // Classifier selection (all cloud/local LLMs go through the one OpenAI-compatible
    // client): GROQ_API_KEY env -> Groq; else config's OpenAI-compatible endpoint
    // if enabled (e.g. a local Ollama at http://localhost:11434/v1); else no-op.
    std::unique_ptr<IClassifier> classifier;
    bool llmActive = false;
    const char* groqKey = std::getenv("GROQ_API_KEY");
    if (groqKey && *groqKey) {
        std::string model = (cfg.llmModel.empty() || cfg.llmModel == "llama3.2")
                                ? std::string("openai/gpt-oss-120b") : cfg.llmModel;
        classifier = std::make_unique<OpenAiClassifier>("https://api.groq.com/openai/v1",
                                                        groqKey, model,
                                                        cfg.llmConfidenceThreshold, cfg.llmTimeoutMs);
        llmActive = true;
        std::fprintf(stderr, "LLM: Groq (model %s)\n", model.c_str());
#ifndef TASKTREE_HAVE_SSL
        std::fprintf(stderr, "WARNING: built without HTTPS support (OpenSSL). Groq will not "
                             "connect — install OpenSSL (libssl-dev / `brew install openssl@3`) "
                             "and reconfigure/rebuild.\n");
#endif
    } else if (cfg.llmEnabled) {
        // Local / self-hosted OpenAI-compatible server (empty key; Ollama etc. ignore it).
        classifier = std::make_unique<OpenAiClassifier>(cfg.llmEndpoint, "", cfg.llmModel,
                                                        cfg.llmConfidenceThreshold, cfg.llmTimeoutMs);
        llmActive = true;
        std::fprintf(stderr, "LLM: OpenAI-compatible (model %s @ %s)\n",
                     cfg.llmModel.c_str(), cfg.llmEndpoint.c_str());
    } else {
        classifier = std::make_unique<NullClassifier>();
    }

    // Request/response log (for debugging task placement).
    llmlog::configure(cfg.llmLogRequests && llmActive, (paths::dataDir() / "llm.log").string());
    if (llmlog::enabled())
        std::fprintf(stderr, "LLM request log: %s\n", llmlog::path().c_str());

    App app(platform, renderer, *classifier, cfg, forest, session);

    // Global hotkeys.
    platform.registerHotkey(cfg.toggleSpec(), [&app]() { app.toggleOverlay(); });
    platform.registerHotkey(cfg.quickAddSpec(), [&app]() { app.showQuickAdd(); });
    if (!platform.startHotkeys())
        std::fprintf(stderr, "Warning: could not install the global hotkey listener.\n");
    for (const std::string& chord : platform.failedChords())
        std::fprintf(stderr, "Warning: hotkey '%s' could not be grabbed (already in use?). "
                             "Change it in %s\n", chord.c_str(), paths::configFile().string().c_str());

    // System-tray icon: a reliable click target for show/hide even when the global
    // hotkey is swallowed (as an X11 grab can be under Wayland). A no-op where there
    // is no tray host — the app just carries on without an icon.
    platform.registerTrayActivate([&app]() { app.toggleOverlay(); });  // left-click
    {
        std::vector<TrayItem> menu;
        menu.push_back(TrayItem{"Show/Hide TaskTree", [&app]() { app.toggleOverlay(); }, false});
        menu.push_back(TrayItem::sep());
        menu.push_back(TrayItem{"Quit", [&platform]() {
                                    glfwSetWindowShouldClose(platform.window(), GLFW_TRUE);
                                }, false});
        platform.setTrayMenu(std::move(menu));
    }
    platform.startTray();

    // Input callbacks -> App.
    GLFWwindow* win = platform.window();
    glfwSetWindowUserPointer(win, &app);
    glfwSetCharCallback(win, [](GLFWwindow* w, unsigned int cp) { appOf(w)->onChar(cp); });
    glfwSetKeyCallback(win, [](GLFWwindow* w, int key, int, int action, int mods) {
        appOf(w)->onKey(key, action, mods);
    });
    glfwSetMouseButtonCallback(win, [](GLFWwindow* w, int b, int action, int mods) {
        appOf(w)->onMouseButton(b, action, mods);
    });
    glfwSetCursorPosCallback(win, [](GLFWwindow* w, double x, double y) {
        appOf(w)->onCursorPos(x, y);
    });
    glfwSetCursorEnterCallback(win, [](GLFWwindow* w, int entered) {
        appOf(w)->onCursorEnter(entered != 0);
    });
    glfwSetScrollCallback(win, [](GLFWwindow* w, double dx, double dy) {
        appOf(w)->onScroll(dx, dy);
    });

    std::fprintf(stderr, "TaskTree running. Toggle: %s   Quick-add: %s\n",
                 cfg.toggleHotkey.c_str(), cfg.quickAddHotkey.c_str());
    std::fprintf(stderr, "Pan: middle-drag or drag empty canvas. Wheel: %s (config [input] "
                         "scroll_mode). Next monitor: Ctrl+M. Double-click empty to recenter.\n",
                 cfg.scrollMode.c_str());
    std::fprintf(stderr, "Select a node, then Ctrl+click another to make it that node's "
                         "child. F2 edits, Delete removes, Ctrl+Z undoes.\n");
    std::fprintf(stderr, "Input bar doubles as a command palette — type '/' on the empty bar "
                         "to list modes: '?' find, ':' select (id or text), '>' reparent the "
                         "selection. Esc or Backspace leaves a mode; Ctrl+F = '?'.\n");

    // On-demand event loop: blocks (~0 CPU) when idle, wakes on input/hotkey.
    while (!glfwWindowShouldClose(win)) {
        const double timeout = app.desiredTimeout();
        if (timeout < 0.0) glfwWaitEvents();
        else               glfwWaitEventsTimeout(timeout);

        platform.pumpPlatformEvents();       // run queued hotkey callbacks
        app.applyPendingClassifications();
        app.pollStore();                     // pick up another process's store writes

        if (app.visible()) {
            int fbw = 0, fbh = 0, ww = 0, wh = 0;
            glfwGetFramebufferSize(win, &fbw, &fbh);
            glfwGetWindowSize(win, &ww, &wh);
            const float px = (ww > 0) ? static_cast<float>(fbw) / static_cast<float>(ww) : 1.f;

            glViewport(0, 0, fbw, fbh);
            glClearColor(0.f, 0.f, 0.f, 0.f); // transparent
            glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            app.drawScene(ww, wh, px);
            glfwSwapBuffers(win);
        }
    }

    nvgDeleteGL3(vg);
    platform.shutdown();
    return 0;
}
