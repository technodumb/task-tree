# Third-party dependencies, all vendored via FetchContent so the only system
# requirement is the X11 / GL dev headers on Linux (nothing on macOS — the SDK has
# Cocoa and Carbon). See README. Pin tags/commits for reproducible builds.

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---- GLFW 3.4 : windowing, GL context, input -------------------------------
if(UNIX AND NOT APPLE)
    set(GLFW_BUILD_X11     ON  CACHE BOOL "" FORCE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE) # X11-only for now (docs/FUTURE.md)
endif()
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw
    GIT_TAG        3.4)

# ---- glad2 : OpenGL 3.3 core function loader -------------------------------
# SOURCE_SUBDIR cmake exposes glad_add_library().
FetchContent_Declare(glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad
    GIT_TAG        v2.0.6
    SOURCE_SUBDIR  cmake)

# ---- nlohmann/json : task data (de)serialization ---------------------------
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG        v3.11.3)

# ---- toml++ : hand-editable config -----------------------------------------
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus
    GIT_TAG        v3.4.0)

# ---- cpp-httplib : header-only HTTP client for the (optional) LLM ----------
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib
    GIT_TAG        v0.15.3)

FetchContent_MakeAvailable(glfw glad nlohmann_json tomlplusplus httplib)

# glad generates its loader by running its own Python package, which imports Jinja2.
# Distro Pythons usually have it; Homebrew's is PEP-668 "externally managed", so a
# plain `pip install jinja2` into it is refused. Provision a private venv in the build
# tree instead of asking the user to modify their global Python. This is a no-op when
# the interpreter on PATH can already import jinja2 (the usual case on Linux).
#
# Deliberately probed with find_program, not find_package(Python): FindPython caches
# its choice internally on the first call, so calling it here would pin the wrong
# interpreter before glad_add_library() gets to look. We only publish VIRTUAL_ENV and
# let glad's own find_package(Python) pick the venv up. The venv is created with
# --copies so its interpreter is a real binary, not a symlink that FindPython could
# resolve back out of the venv.
find_program(TT_BOOTSTRAP_PYTHON NAMES python3 python REQUIRED)
execute_process(COMMAND "${TT_BOOTSTRAP_PYTHON}" -c "import jinja2"
                RESULT_VARIABLE TT_JINJA_RC OUTPUT_QUIET ERROR_QUIET)
if(NOT TT_JINJA_RC EQUAL 0)
    set(TT_GLAD_VENV "${CMAKE_BINARY_DIR}/glad-venv")
    if(NOT EXISTS "${TT_GLAD_VENV}/bin/python")
        message(STATUS "TaskTree: ${TT_BOOTSTRAP_PYTHON} cannot import jinja2, which glad "
                       "needs — creating a private venv at ${TT_GLAD_VENV}")
        execute_process(COMMAND "${TT_BOOTSTRAP_PYTHON}" -m venv --copies "${TT_GLAD_VENV}"
                        RESULT_VARIABLE TT_VENV_RC)
        if(NOT TT_VENV_RC EQUAL 0)
            message(FATAL_ERROR "Could not create a venv with ${TT_BOOTSTRAP_PYTHON}. "
                                "Install Jinja2 for that interpreter and reconfigure.")
        endif()
        execute_process(COMMAND "${TT_GLAD_VENV}/bin/python" -m pip install --quiet jinja2
                        RESULT_VARIABLE TT_PIP_RC)
        if(NOT TT_PIP_RC EQUAL 0)
            file(REMOVE_RECURSE "${TT_GLAD_VENV}")   # don't leave a venv without Jinja2
            message(FATAL_ERROR "Could not install Jinja2 into ${TT_GLAD_VENV} (offline?). "
                                "Install Jinja2 for a Python on PATH and reconfigure.")
        endif()
    endif()
    # Point FindPython at the venv and nothing else. Python_FIND_FRAMEWORK must be
    # overridden explicitly: it defaults to FIRST on macOS, which makes FindPython
    # prefer Homebrew's Python.framework over any virtual environment.
    set(ENV{VIRTUAL_ENV} "${TT_GLAD_VENV}")
    set(Python_ROOT_DIR "${TT_GLAD_VENV}")
    set(Python_FIND_VIRTUALENV ONLY)
    set(Python_FIND_FRAMEWORK NEVER)
endif()

# GL 3.3 core loader library target: glad_gl_core_33
glad_add_library(glad_gl_core_33 STATIC API gl:core=3.3)

# ---- SQLite 3.53.4 : the task store ----------------------------------------
# The amalgamation (one sqlite3.c) rather than system libsqlite3, so the system
# requirement stays "X11 + GL dev headers" like every other dep here. Pinned by
# SHA256 — sqlite.org URLs are mutable per release.
FetchContent_Declare(sqlite_amalgamation
    URL      https://sqlite.org/2026/sqlite-amalgamation-3530400.zip
    URL_HASH SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d)
FetchContent_MakeAvailable(sqlite_amalgamation)   # sources only: no upstream CMakeLists

add_library(sqlite3 STATIC ${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite_amalgamation_SOURCE_DIR})
# Trim what we don't use; DQS=0 makes "..." a string only (never an identifier), so a
# typo'd column name is an error instead of a silently-quoted string.
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_DQS=0
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_DEPRECATED
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1)
target_link_libraries(sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(NOT MSVC)
    target_compile_options(sqlite3 PRIVATE -w)   # not our warnings to fix
endif()

# ---- NanoVG : anti-aliased vector rendering (no CMake / releases upstream) --
# Pin an exact commit for reproducibility (TODO: replace master with a SHA).
FetchContent_Declare(nanovg
    GIT_REPOSITORY https://github.com/memononen/nanovg
    GIT_TAG        master)
FetchContent_MakeAvailable(nanovg)

add_library(nanovg STATIC ${nanovg_SOURCE_DIR}/src/nanovg.c)
target_include_directories(nanovg PUBLIC ${nanovg_SOURCE_DIR}/src)
# nanovg.c triggers -Wall noise we don't own; keep it quiet.
if(NOT MSVC)
    target_compile_options(nanovg PRIVATE -w)
endif()
