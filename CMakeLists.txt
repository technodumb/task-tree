cmake_minimum_required(VERSION 3.24)   # FetchContent + FIND_PACKAGE_ARGS + SOURCE_SUBDIR
project(tasktree LANGUAGES CXX C)       # C is needed for nanovg.c / glad
if(APPLE)
    # OBJCXX: our Cocoa/Carbon platform backend (.mm). OBJC: GLFW's own cocoa_*.m —
    # without it CMake maps .m to OBJCXX and GLFW fails to compile as Objective-C++.
    enable_language(OBJC OBJCXX)
endif()

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
    if(APPLE)
        # Apple's linker spells this differently and has no -s (strip with `strip -x`).
        add_link_options(-Wl,-dead_strip)
    else()
        add_link_options(-Wl,--gc-sections)
        add_link_options($<$<CONFIG:Release>:-s>)
    endif()
endif()

option(TASKTREE_BUILD_TESTS "Build unit tests" ON)
# Development-only features (LLM request/response log, and future dev diagnostics).
# ON  -> development build (this is the default)
# OFF -> production build; dev code compiles to nothing and its .cpp files are excluded.
# Use the presets: `cmake --preset dev` / `cmake --preset prod`.
option(TASKTREE_DEV "Include development-only features" ON)

# ---- System deps ------------------------------------------------------------
find_package(Threads REQUIRED)

# macOS has no system OpenSSL headers; point CMake at the Homebrew keg so HTTPS
# (and therefore the cloud LLMs) works out of the box when it is installed.
if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
    find_program(BREW_EXECUTABLE brew)
    if(BREW_EXECUTABLE)
        execute_process(COMMAND "${BREW_EXECUTABLE}" --prefix openssl@3
                        OUTPUT_VARIABLE _brew_openssl
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET RESULT_VARIABLE _brew_rc)
        if(_brew_rc EQUAL 0 AND EXISTS "${_brew_openssl}")
            set(OPENSSL_ROOT_DIR "${_brew_openssl}")
        endif()
    endif()
endif()
find_package(OpenSSL) # enables HTTPS in cpp-httplib for cloud LLMs (Groq/OpenAI)

# X11 is the Linux/BSD window server; on macOS the platform backend is Cocoa/Carbon
# (see src/platform/NativeWindowMac.mm + HotkeysMac.mm) and needs no system package.
if(UNIX AND NOT APPLE)
    find_package(X11)   # provides X11::X11 for the global-hotkey XGrabKey code
    if(NOT X11_FOUND)
        message(FATAL_ERROR
            "X11 development headers not found. Install them first:\n"
            "    sudo apt install xorg-dev libgl1-mesa-dev\n"
            "(xorg-dev gives X11/Xlib.h etc., libgl1-mesa-dev gives the GLX headers).")
    endif()
    # GLib/GIO drives the StatusNotifierItem tray icon (src/platform/TrayGio.cpp).
    # The runtime is already loaded by every desktop session; only the headers are new.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gobject-2.0 glib-2.0)
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

# ---- CLI library + `tt` executable -----------------------------------------
# A headless client so an agent or the ttd loop can read and mutate the store with the app
# running or stopped. Links tt_core + tt_io only — no GLFW/GL/X11/GIO — so it builds
# anywhere the toolchain does, and is produced by both presets (it uses no dev-only
# feature). sqlite3 is PUBLIC here: tt_io links it PRIVATE, so a consumer of this static
# lib must still carry sqlite3 to the final link (as store_tests does).
add_library(tt_cli STATIC src/cli/Cli.cpp)
target_include_directories(tt_cli PUBLIC src)
target_link_libraries(tt_cli PUBLIC tt_core tt_io sqlite3 nlohmann_json::nlohmann_json)

add_executable(tt src/cli/main.cpp)
target_link_libraries(tt PRIVATE tt_cli)

# ---- Application executable -------------------------------------------------
add_executable(tasktree
    src/main.cpp
    src/app/App.cpp
    src/platform/PlatformGlfw.cpp
    src/render/Renderer.cpp
    src/render/nanovg_gl_impl.cpp
    src/ui/TextInput.cpp
    src/ui/DragController.cpp
    src/llm/OpenAiClassifier.cpp)

# Platform backends: the overlay window hints and the global hotkey grabber are the
# only two things GLFW cannot do portably. Exactly one pair is compiled in.
if(APPLE)
    target_sources(tasktree PRIVATE
        src/platform/NativeWindowMac.mm
        src/platform/HotkeysMac.mm
        src/platform/TrayNull.cpp)   # NSStatusItem backend not written yet
else()
    target_sources(tasktree PRIVATE
        src/platform/NativeWindowX11.cpp
        src/platform/HotkeysX11.cpp
        src/platform/TrayGio.cpp)
endif()
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
    Threads::Threads)

if(APPLE)
    # Cocoa for the NSWindow overlay hints, Carbon for RegisterEventHotKey.
    target_link_libraries(tasktree PRIVATE "-framework Cocoa" "-framework Carbon")
else()
    target_link_libraries(tasktree PRIVATE X11::X11 PkgConfig::GIO)
endif()

if(OpenSSL_FOUND)
    target_compile_definitions(tasktree PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT TASKTREE_HAVE_SSL)
    target_link_libraries(tasktree PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "TaskTree: HTTPS enabled (OpenSSL) — cloud LLMs (Groq/OpenAI) supported.")
else()
    message(WARNING "TaskTree: OpenSSL not found — HTTPS disabled, so cloud LLMs (Groq) "
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

    # Hotkey parsing + the macOS key-name table. Both are header-only and pure, so
    # this runs on every platform (the mac table is just data).
    add_executable(hotkey_tests tests/hotkey_tests.cpp)
    target_link_libraries(hotkey_tests PRIVATE tt_core)
    add_test(NAME hotkey_tests COMMAND hotkey_tests)

    # Tray menu id model (TrayMenu.hpp). Header-only and pure, so this runs on
    # every platform; the live DBus/StatusNotifierItem wiring is verified by hand.
    add_executable(tray_tests tests/tray_tests.cpp)
    target_link_libraries(tray_tests PRIVATE tt_core)
    add_test(NAME tray_tests COMMAND tray_tests)

    # Input-bar editing + selection model. TextInput.cpp maps GLFW key constants, so
    # the test links glfw only for those (header-only) macros — no window is created.
    add_executable(textinput_tests tests/textinput_tests.cpp src/ui/TextInput.cpp)
    target_include_directories(textinput_tests PRIVATE src)
    target_link_libraries(textinput_tests PRIVATE glfw)
    add_test(NAME textinput_tests COMMAND textinput_tests)

    # The `tt` CLI, driven in-process against temp stores (see tests/cli_tests.cpp).
    add_executable(cli_tests tests/cli_tests.cpp)
    target_link_libraries(cli_tests PRIVATE tt_cli sqlite3)
    add_test(NAME cli_tests COMMAND cli_tests)
endif()
