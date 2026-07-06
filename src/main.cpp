// TaskTree entry point: initialise platform + GL + NanoVG, load config/tasks, wire
// input callbacks, and run the on-demand event loop.
#include <cstdio>
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
#include "llm/NullClassifier.hpp"
#include "llm/OllamaClassifier.hpp"
#include "model/Store.hpp"
#include "platform/PlatformX11.hpp"
#include "render/Renderer.hpp"

#ifndef TASKTREE_ASSETS_DIR
#define TASKTREE_ASSETS_DIR "assets"
#endif

using namespace tt;

namespace {

App* appOf(GLFWwindow* w) { return static_cast<App*>(glfwGetWindowUserPointer(w)); }

std::string pickFont() {
    const std::vector<std::string> candidates = {
        std::string(TASKTREE_ASSETS_DIR) + "/fonts/Inter-Regular.ttf",
        std::string(TASKTREE_ASSETS_DIR) + "/fonts/UI.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    std::error_code ec;
    for (const auto& p : candidates)
        if (std::filesystem::exists(p, ec)) return p;
    return {};
}

} // namespace

int main() {
    PlatformX11 platform;
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

    Forest forest;
    const std::string tasksPath = paths::tasksFile().string();
    store::load(forest, tasksPath); // ok if absent -> empty forest

    std::unique_ptr<IClassifier> classifier;
    if (cfg.llmEnabled)
        classifier = std::make_unique<OllamaClassifier>(cfg.llmEndpoint, cfg.llmModel,
                                                        cfg.llmConfidenceThreshold, cfg.llmTimeoutMs);
    else
        classifier = std::make_unique<NullClassifier>();

    App app(platform, renderer, *classifier, cfg, forest, tasksPath);

    // Global hotkeys.
    platform.registerHotkey(cfg.toggleSpec(), [&app]() { app.toggleOverlay(); });
    platform.registerHotkey(cfg.quickAddSpec(), [&app]() { app.showQuickAdd(); });
    if (!platform.startHotkeys())
        std::fprintf(stderr, "Warning: could not open an X connection for hotkeys.\n");
    for (const std::string& chord : platform.failedChords())
        std::fprintf(stderr, "Warning: hotkey '%s' could not be grabbed (already in use?). "
                             "Change it in %s\n", chord.c_str(), paths::configFile().string().c_str());

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

    std::fprintf(stderr, "TaskTree running. Toggle: %s   Quick-add: %s\n",
                 cfg.toggleHotkey.c_str(), cfg.quickAddHotkey.c_str());

    // On-demand event loop: blocks (~0 CPU) when idle, wakes on input/hotkey.
    while (!glfwWindowShouldClose(win)) {
        const double timeout = app.desiredTimeout();
        if (timeout < 0.0) glfwWaitEvents();
        else               glfwWaitEventsTimeout(timeout);

        platform.pumpPlatformEvents();       // run queued hotkey callbacks
        app.applyPendingClassifications();

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
