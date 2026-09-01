# TaskTree

A fast, low-memory graphical scratchpad for organizing tasks as a **tree**. Summon a
borderless, semi-transparent full-screen overlay with a global hotkey; type tasks that
appear as auto-arranged **squircle nodes** joined by curved edges; drag any node onto
another to make it a child. Multiple independent trees can coexist. A second hotkey
pops up just a quick-add box so you can capture a task without the full overlay.

Built in C++ with **GLFW + OpenGL + NanoVG** — a resident background process that sits
at ~0% CPU when idle (on-demand rendering) and a small memory footprint, deliberately
avoiding a browser/Electron stack.

> Status: **proof of concept (v1).** Linux/X11 and macOS. See `docs/FUTURE.md` for the
> roadmap and `docs/AGENTS.md` for how the project advances one reviewable version at a
> time.

## Requirements

Both platforms need a C++20 compiler, CMake ≥ 3.24, Ninja, and network access for the
first build (deps are fetched via CMake `FetchContent`).

**Linux** — an **X11 session** (`echo $XDG_SESSION_TYPE` should print `x11`). Wayland is
not yet supported; see `docs/FUTURE.md`. On GNOME, choose "Ubuntu on Xorg" at login.
X11 + GL development headers, one-time:

```sh
sudo apt install xorg-dev libgl1-mesa-dev
```

**macOS** — the Xcode Command Line Tools (`xcode-select --install`) are enough; the
Cocoa/Carbon backend needs no extra packages. With Homebrew:

```sh
brew install cmake ninja
brew install openssl@3   # optional: enables HTTPS, i.e. the cloud LLM classifier
```

Fonts: a TTF is auto-detected — DejaVu / Liberation / Ubuntu on Linux, Arial / Verdana /
Geneva on macOS. To use a specific one, drop `Inter-Regular.ttf` (or `UI.ttf`) into
`assets/fonts/`.

## Build & run

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build          # first run fetches GLFW, NanoVG, json, toml++, httplib
./build/tasktree
```

The process starts hidden and resident. Default hotkeys:

| Action              | Default chord      |
|---------------------|--------------------|
| Toggle full overlay | `Ctrl+Alt+Space`   |
| Quick-add box       | `Ctrl+Alt+Return`  |

In the overlay: type a task and press **Enter** to create it; **drag** a node onto
another node (or the area just below it) to reparent — siblings reflow to open a gap
and the tree snaps to its new shape. **Esc** hides the overlay.

> If a hotkey doesn't fire, something else already owns it: on Linux the compositor
> (GNOME/mutter reserves most `Super` combos), on macOS the system or another app
> (`Cmd+Space` is Spotlight, `Ctrl+Space` switches input source). The app prints a
> warning naming any chord it couldn't grab — change it in the config file.
>
> On macOS, `Super` in a chord means **Command** and `Alt` means **Option**; the config
> also accepts `cmd` and `option` as spellings. The global hotkeys use Carbon's
> `RegisterEventHotKey`, so TaskTree needs **no Accessibility permission**.

## Configuration

Hand-editable TOML at `${XDG_CONFIG_HOME:-~/.config}/tasktree/config.toml` (written
with defaults on first run). Controls hotkeys, overlay opacity, node max width,
colours, and the optional LLM endpoint. Restart to apply.

Tasks are stored at `${XDG_DATA_HOME:-~/.local/share}/tasktree/tasks.db` (SQLite, written
incrementally per row). A pre-SQLite `tasks.json` at the same path is still read when no
`tasks.db` exists yet; JSON is otherwise for export/import.

## Optional: local LLM auto-classification

When enabled, a new task is checked against existing tasks by a local LLM and may be
auto-placed as a child/parent. It runs off the UI thread and never blocks task
creation; **disabled by default** (every task is standalone). To enable:

```sh
# install ollama from https://ollama.com, then:
ollama pull llama3.2
```
Then in `config.toml` set:
```toml
[llm]
enabled = true
endpoint = "http://localhost:11434"
model = "llama3.2"
```

## Command line (`tt`)

`tt` is a headless client for the same task store — read or change the tree from a script
or an agent, with the app running or closed. It links only the pure model + I/O libraries
(no GLFW/GL/X11), and `cmake --build build` produces it at `build/tt`.

```sh
tt tree [--json]                       # whole forest (subtree: tt tree <id|query>)
tt find <query>                        # ranked text matches
tt show <id|query>                     # one task + its children
tt add "text" [--parent <id|query>]    # prints the new id
tt edit <id|query> "text"
tt done <id|query> | tt undone <id|query>
tt status <id|query> <normal|in-progress|priority>
tt parent <child> <parent>             # reparent (refuses cycles)
tt rm <id|query>                       # soft delete (recoverable; see `tt deleted`)
```

A node is addressed by numeric id or a unique text match (ambiguity is an error, never a
guess). Every command takes `--json` (one JSON document on stdout; diagnostics stay on
stderr) and a `--store <path>` override. Writes cooperate with the running app — each is a
single transaction it notices and reloads within ~1 s. Exit codes distinguish usage (1),
not-found (2), ambiguous (3), store-unreadable (4), store-too-new (5), store-busy (6).

## Tests

The pure logic (task model, tidy-tree layout, persistence, config, hotkey parsing) plus
the `tt` CLI have dependency-free tests:

```sh
ctest --test-dir build --output-on-failure
```

## Project layout

```
src/model/    Task + Forest data model, SQLite + JSON persistence
src/layout/   Pure tidy-tree layout engine + geometry (unit tested)
src/render/   NanoVG renderer (squircles, curved edges, input box)
src/ui/       Single-line UTF-8 text input, drag & drop controller
src/platform/ Overlay window + global hotkeys (behind IPlatform; X11 + macOS backends)
src/llm/      Pluggable classifier seam (Null default, OpenAI-compatible impl)
src/cli/      `tt` — headless read/write CLI over the store (tt_core + tt_io only)
src/app/      App state machine, config, XDG paths
docs/         FUTURE.md (roadmap), AGENTS.md (iterative dev loop), plans/
```

The layout and model layers are GL-free and stack-agnostic, so the alternative-stack
plans on the `plan/qt6-fallback` and `plan/imgui-alt` branches reuse them verbatim.
