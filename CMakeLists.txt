cmake_minimum_required(VERSION 3.24)   # FetchContent + FIND_PACKAGE_ARGS + SOURCE_SUBDIR
project(tasktree LANGUAGES CXX C)       # C is needed for nanovg.c / glad

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Size: put each function/data item in its own section and let the linker drop the
# unused ones (trims the header-only deps, esp. httplib). Strip symbols in Release
# only, so RelWithDebInfo stays debuggable. Ship with -DCMAKE_BUILD_TYPE=Release.
if(NOT MSVC)
    add_compile_options(-ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections)
    add_link_options($<$<CONFIG:Release>:-s>)
endif()

option(TASKTREE_BUILD_TESTS "Build unit tests" ON)
# Development-only features (LLM request/response log, and future dev diagnostics).
# ON  -> development build (this is the default)
# OFF -> production build; dev code compiles to nothing and its .cpp files are excluded.
# Use the presets: `cmake --preset dev` / `cmake --preset prod`.
option(TASKTREE_DEV "Include development-only features" ON)

# ---- System deps ------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(OpenSSL) # enables HTTPS in cpp-httplib for cloud LLMs (Cerebras/OpenAI)
find_package(X11)   # provides X11::X11 for the global-hotkey XGrabKey code
if(NOT X11_FOUND)
    message(FATAL_ERROR
        "X11 development headers not found. Install them first:\n"
        "    sudo apt install xorg-dev libgl1-mesa-dev\n"
        "(xorg-dev gives X11/Xlib.h etc., libgl1-mesa-dev gives the GLX headers).")
endif()

include(cmake/deps.cmake)

# ---- Pure core library (no external deps -> fast, unit-testable) ------------
add_library(tt_core STATIC
    src/model/Forest.cpp
    src/layout/TidyLayout.cpp
    src/app/Palette.cpp)
target_include_directories(tt_core PUBLIC src)

# ---- I/O library (persistence + config) ------------------------------------
# Two store backends live here: SQLite (primary) and JSON (export/import + the
# pre-SQLite data files). sqlite3 is PRIVATE — sqlite3.h stays out of Store.hpp.
add_library(tt_io STATIC
    src/model/Store.cpp
    src/model/SqliteStore.cpp
    src/app/Config.cpp)
target_include_directories(tt_io PUBLIC src)
target_link_libraries(tt_io
    PUBLIC tt_core
        nlohmann_json::nlohmann_json
        tomlplusplus::tomlplusplus
    PRIVATE sqlite3)

# ---- Application executable -------------------------------------------------
add_executable(tasktree
    src/main.cpp
    src/app/App.cpp
    src/platform/PlatformX11.cpp
    src/platform/Hotkeys.cpp
    src/render/Renderer.cpp
    src/render/nanovg_gl_impl.cpp
    src/ui/TextInput.cpp
    src/ui/DragController.cpp
    src/llm/OpenAiClassifier.cpp)
target_include_directories(tasktree PRIVATE src)
target_compile_definitions(tasktree PRIVATE
    GLFW_INCLUDE_NONE           # let glad own the GL headers
    TASKTREE_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")

# ---- Development-only features (gated by TASKTREE_DEV) ----------------------
# The macro flips the header stubs (see src/llm/LlmLog.hpp): in dev the real logger
# is declared + compiled; in prod it becomes inline no-ops and the .cpp is dropped.
# Add future dev-only code the same way: guard with `#if TASKTREE_DEV` and add its
# sources here.
if(TASKTREE_DEV)
    target_compile_definitions(tasktree PRIVATE TASKTREE_DEV=1)
    target_sources(tasktree PRIVATE src/llm/LlmLog.cpp)
    message(STATUS "TaskTree: DEVELOPMENT build (dev features ON — LLM request logging).")
else()
    target_compile_definitions(tasktree PRIVATE TASKTREE_DEV=0)
    message(STATUS "TaskTree: PRODUCTION build (dev features stripped).")
endif()
target_link_libraries(tasktree PRIVATE
    tt_core tt_io
    glfw glad_gl_core_33 nanovg
    nlohmann_json::nlohmann_json
    httplib::httplib
    X11::X11
    Threads::Threads)

if(OpenSSL_FOUND)
    target_compile_definitions(tasktree PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT TASKTREE_HAVE_SSL)
    target_link_libraries(tasktree PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "TaskTree: HTTPS enabled (OpenSSL) — cloud LLMs (Cerebras/OpenAI) supported.")
else()
    message(WARNING "TaskTree: OpenSSL not found — HTTPS disabled, so cloud LLMs (Cerebras) "
                    "won't work. Install libssl-dev and reconfigure. Local Ollama (http) is unaffected.")
endif()

# ---- Tests (dependency-free runners registered with CTest) ------------------
if(TASKTREE_BUILD_TESTS)
    enable_testing()

    add_executable(core_tests tests/core_tests.cpp)
    target_link_libraries(core_tests PRIVATE tt_core)
    add_test(NAME core_tests COMMAND core_tests)

    # sqlite3 directly, so the tests can build an old-schema fixture and inspect rows the
    # public API hides on purpose (soft-deleted ones).
    add_executable(store_tests tests/store_tests.cpp)
    target_link_libraries(store_tests PRIVATE tt_core tt_io sqlite3)
    add_test(NAME store_tests COMMAND store_tests)
endif()
