# TaskTree — Project Overview (for agents & contributors)

High-level map of what TaskTree is, how it's built, and every feature it has. Read this
first, then `CLAUDE.md` (session-start / ttd workflow), `docs/FUTURE.md` (roadmap), and
`docs/AGENTS.md` (iterative-plan protocol).

---

## 1. What it is

A **fast, low-memory graphical scratchpad for organizing tasks as a tree.** You summon a
borderless, semi-transparent full-screen **overlay** with a global hotkey; tasks appear
as auto-arranged **squircle nodes** joined by curved edges; drag a node onto another to
make it a child. A second hotkey pops just a **quick-add box** to capture a task without
the full overlay.

Design intent: a keyboard-summoned **HUD** that reduces context-switching — flick it
open, dump/organize a thought, flick it away. Native **C++ + GLFW + OpenGL + NanoVG**,
deliberately *not* a browser/Electron stack. A resident background process at ~0% CPU
when idle (on-demand rendering); a stripped production binary is ~1.5 MB.

Status: proof-of-concept, running on **Linux/X11 and macOS** (Wayland is future work).
The app is being dogfood-developed *through itself* — see the "ttd" workflow in
`CLAUDE.md`.

---

## 2. How it runs (the loop)

`main.cpp` wires everything and runs an **on-demand event loop**:
`glfwWaitEvents()` blocks (~0% CPU) until input, a global hotkey, or an LLM result wakes
it; then it renders one frame. The overlay window is created **hidden and resident** — a
global hotkey maps/unmaps it, so summoning is instant. It polls faster only while
dragging (smooth) or during the new-task path flash.

Threads: main (GL + everything), a **hotkey listener** (its own X connection), and
detached **LLM worker** threads. Workers marshal results back to the main thread via a
mutex-guarded queue + `glfwPostEmptyEvent()`.

---

## 3. Architecture (layers + seams)

```
platform/  Overlay window + global hotkeys  ← behind IPlatform (X11 + macOS backends)
render/    NanoVG drawing (nodes/edges/UI)
ui/        TextInput (line editor) + DragController
layout/    PURE tidy-tree engine + geometry  ← no GL, unit-tested
model/     Task/Forest + SQLite/JSON store    ← Task/Forest PURE, unit-tested
app/       state machine, config, Palette     ← Palette (bar grammar) is PURE, unit-tested
llm/       Pluggable classifier               ← behind IClassifier
cli/       `tt` headless read/write CLI        ← tt_core + tt_io only, no GL
app/       App state machine, Config, paths, ttd routing
```

Two deliberate **seams** (exist for extension — do not delete as "dead code"):
- **`IPlatform`** (`platform/IPlatform.hpp`) — window + hotkeys. `PlatformGlfw`
  implements it for every OS; GLFW already abstracts X11 vs Cocoa, so only the two
  things it cannot express portably are split per-OS:
  - `native::applyOverlayHints` / `activateForInput` (`platform/NativeWindow.hpp`) —
    "always-on-top utility window, off the taskbar" and "take the keyboard now".
    `NativeWindowX11.cpp` sets EWMH properties; `NativeWindowMac.mm` sets the NSWindow
    level/collection behaviour and an accessory activation policy.
  - `Hotkeys` (`platform/Hotkeys.hpp`) — the queue plumbing is shared; the grabbing is
    `HotkeysX11.cpp` (`XGrabKey`) or `HotkeysMac.mm` (`RegisterEventHotKey`).
  A `PlatformWayland` needs only a third pair of these files.
- **`IClassifier`** (`llm/IClassifier.hpp`) — task classification. `NullClassifier`
  (default, everything standalone) and `OpenAiClassifier` (any OpenAI-compatible
  endpoint) implement it.

The **pure layers** (`model/`, `layout/`) have zero external deps and are unit-tested
(`tests/`), so they're stack-agnostic and fast to reason about. `App` is the orchestrator
that ties model → layout → render and routes input.

**`cli/`** is the `tt` command-line client — the second writer that v3's store layer (WAL,
`busy_timeout`, `BEGIN IMMEDIATE`, baseline-diffed saves, soft delete, and the
external-change reload) was built for. It links only `tt_core` + `tt_io`, so it runs with
the overlay open or closed, and every write passes the loaded forest as the save baseline,
so a task the app commits concurrently is never clobbered. It is also what the ttd loop and
the `SessionStart` hook now use instead of hand-written SQL. See `src/cli/Cli.cpp` and
`openspec/changes/add-tt-cli`.

**Data model** (`model/Task.hpp`): `Task{id, parent, text, children[], collapsed, status,
createdAt, doneAt}`; `Forest{nodes, roots[], nextId}`. Key ops: `addTask`,
`reparent` (with cycle prevention), `markDone`/`restoreFromDone`, `isInDoneSection`,
`removeSubtree`. Sibling order = children-vector order = layout order.
**Done is a timestamp, not a move** (v3): there is no `done` boolean — `doneAt` carries both
the state and the date (`0` not done, `>0` done then, `kDoneAtUnknown` done at an unknown
time), exactly as `deleted_at` does in the store, so the two can never disagree.
`markDone(id, whenMs)` only sets that field, so the task keeps its parent and its slot; the
canvas layout skips done subtrees and the DONE panel is derived via `doneSectionRoots()`. That is why un-doing restores a task to exactly where it was, including
a done child of a live parent — and why `roots[]` holds done top-level tasks too, which any
canvas-walking code must filter (see `DevRoute::ensureDevRoot`, `App::buildTreeOutline`).
Undo/redo is a separate pure `model/History` — a bounded stack of whole-`Forest`
snapshots, so any mutation is reversible without per-op inverse logic (v2).

---

## 4. Feature catalog

**Overlay & capture**
- Two customizable global hotkeys: **toggle full overlay** (`Ctrl+Alt+Space`) and
  **quick-add** (`Ctrl+Alt+Return`). On X11, grabbed via Xlib `XGrabKey` on a dedicated
  thread (handles nuisance modifiers); on macOS via Carbon `RegisterEventHotKey`, which
  needs no Accessibility permission. Either way a matched press is queued and run on the
  main thread, and any chord already taken is warned about.
- Transparent, borderless, always-on-top, monitor-covering window; instant show/hide.
- **Quick-add box** (grows *downward*) vs **full-overlay input bar** (grows *upward*) —
  both wrap and expand as you type, capped at 10 lines with caret-follow scroll.

**Text input** (`ui/TextInput`)
- Single-line-logical UTF-8 editor: caret on code-point boundaries, Backspace/Delete,
  arrows/Home/End, clipboard paste, **Ctrl+Backspace/Delete** (word delete),
  **Ctrl+Left/Right** (word move). Caret height matches the text glyphs.

**Layout** (`layout/TidyLayout`, pure)
- Reingold–Tilford-style **tidy tree**: variable node heights, every node on a depth
  shares the same top-y, parents centered over children, subtrees never overlap
  (min `hGap`). Multiple trees (a forest) are packed side by side. The whole forest is
  centered horizontally in the window (roots at top-center).

**Nodes & edges** (`render/Renderer`)
- Squircle (rounded-rect) nodes with border; **max width**, text wraps, height grows to
  fit (overflow never clipped). Wrapped line-breaks are **cached per node** so wrapping
  is identical at every zoom (no reflow/clipping).
- Small **id badge** (top-left, its own color) = the node's stable `TaskId`.
- **Status color** cycled by **right-click**: default → yellow (in-progress) → orange
  (priority) → default; text flips to dark on the bright fills.
- Edges: cubic bezier parent-bottom → child-top (straight when vertically aligned).

**Interaction**
- **Drag-drop reparent**: drop region = a node's box **plus a band below it**; the
  target's children reflow (re-centered under the *fixed* target) to open a gap; a
  preview edge shows the slot; cycle-prevented; snaps on drop. Currently **append-only**
  (a dropped node goes to the rightmost slot). A click-vs-drag threshold prevents
  accidental reparents.
- **Double-click** a canvas node → move it (+ subtree) to DONE; double-click a DONE row →
  restore it to the canvas.
- **Pan**: middle-drag, or drag empty canvas, or scroll (Shift+scroll = horizontal).
  **Zoom**: `Ctrl+scroll` (or plain scroll — config `scroll_mode` = `zoom`/`pan`/`off`),
  about the cursor. **Double-click empty space** recenters + resets zoom.
- **Multi-monitor**: `Ctrl+M` moves the overlay to the next monitor.
- **Search**: part of the command palette below — `?query` in the input bar (`Ctrl+F` is
  a shortcut for typing that `?`). There is only ever **one** text field.
- **Path flash**: adding a task briefly highlights root→new-node; the canvas also **pans
  to bring the new node into view**.

**Direct manipulation & history** (v2 — theme: editable in place, every edit reversible)
- **Select**: a left click that doesn't cross the drag threshold selects a canvas node
  (blue selection ring); `Esc` or an empty-canvas click clears it. The handle the id
  badge always hinted at, and the base for edit + delete.
- **Edit node text**: `F2` (or `Enter` on a selected node with the input bar empty) seeds
  the input bar with the node's text (blue border cue); commit updates the node in place
  and re-measures/relayouts instead of adding a new task.
- **Delete subtree**: `Delete` / `Backspace` with a node selected and the input bar empty
  removes that subtree. No confirm dialog — undo covers it.
- **Ctrl+click reparent**: with a node selected, `Ctrl`+click another canvas node to move
  the selection (and its subtree) in as that node's **last child** — the drag-free way to
  restructure, and the only way to reparent across a long distance without dragging.
  Holding `Ctrl` rings the node under the cursor green (the drop-hint colour) when it's a
  valid target; invalid ones (the selection itself, its own descendants, its current
  parent) show no ring and fall through to normal click behaviour. A collapsed target is
  opened so the arriving node is visible, and the move flashes root→node. **The clicked
  parent is held still** (unlike add/search, which glide the camera to the node): the move
  re-packs the tree, so `anchorNode_`/`anchorScreen_` record its pre-move screen position
  and the next relayout in `drawScene` shifts `pan_` to put it back exactly there. The
  cursor therefore still points at it — and the selection is kept — so Ctrl+clicks chain
  without re-aiming. Undo-backed.
- **Undo / redo**: `Ctrl+Z` / `Ctrl+Shift+Z` (also `Ctrl+Y`) step through the `History`
  snapshot stack. Every structural mutation snapshots first; the input bar ignores these
  chords so there's no collision.
- **Command palette (the input bar)**: a symbol typed into the *empty* bar switches its
  **mode** instead of opening another field. The symbol is **consumed** — it never appears
  in the text; the bar grows a tinted `find:` / `select:` / `parent:` **section** at its left
  end (full height, left corners sharing the box radius so it sits flush inside the border,
  straight right edge with a hairline divider, and one width for every mode so the text
  never shifts when the mode changes) and
  what you type after it is just that mode's argument. Grammar + ranking live in
  `app/Palette.hpp` (pure, unit-tested); `App` owns the side effects.

  | prefix | mode (accent) | argument | Enter |
  |---|---|---|---|
  | — | add (unchanged) | task text | add the task, LLM-classified as before |
  | `/` | **mode menu** (violet) | filter text | switch to the highlighted mode |
  | `?` | **find** (amber) | text | jump to the active match, stay in find |
  | `:` | **select** (blue) | `12` or text | select node 12 / the active match |
  | `>` | **parent** (green) | `12` or text | make it the parent of the selection |

  In select/parent, digits mean *by id* (the node's id badge) and anything else is a text
  query; a leading `?` in the argument forces text (`:?12` looks for the text "12"), so the
  `:?query` / `>?query` spellings work exactly as typed. `/` is a **picker, not a filter**:
  the bar shows only the highlighted mode's *name* in its section (`find`, not `mode: find`;
  caret hidden, no argument), each drop-up row is a **symbol + what it does** (no names —
  the bar has that), and the whole bar takes that mode's colour so `↑`/`↓` previews what
  you're switching into. Typing a mode's symbol jumps straight to it. A prefix typed into an empty argument **switches** modes
  directly, so `:` → `>` needs no exit. **Leaving a mode**: `Esc`, or `Backspace` on an empty argument
  (the prefix isn't in the text, so that's what deleting it means) — both drop back to plain
  add without closing the overlay. To add a task that *starts* with a prefix symbol, type a
  space first (commit trims it).

  Text modes show a **drop-up list** above the bar (up to 4 rows of `[id] text` + "+N
  more"), windowed to keep the `↑`/`↓` cursor visible; the cursor wraps. Every row is
  (lead, rest) — id badge or mode symbol, then its text — and the lead column is one shared
  width, so ids/symbols of different widths still leave the text aligned. Mode is legible
  without reading the prefix because the section, the border, the drop-up tint and the canvas
  ring share one vocabulary: blue = what Enter acts on, green = where the selection lands,
  amber = every match. The bar's right edge shows status: `7 matches`, `no match`,
  `node 12`, `→ child of 12`, `can't move there`, `select a node first`. Targets hidden
  under a collapsed ancestor are **revealed** (ancestors expanded) on commit; the canvas
  pans only if the target is **off screen**, and never for a `>` onto a visible node (that
  one anchors instead). `Ctrl+F` is exactly "type `?`" (carrying any text already typed
  into the query).
- **Collapse / expand**: nodes with children show a small disc handle on the bottom edge —
  a minus when expanded, the hidden-descendant count when collapsed. Clicking it toggles
  the node's `collapsed` flag; layout then treats it as a leaf (subtree hidden, no
  overlap). View-only (no undo entry) but persisted in `tasks.json`.

**DONE panel** (right edge)
- Autohides: reveals when the cursor is within the right ~15%, hides when it moves ~17%
  out; a **PIN** button disables autohide. Translucent green.
- Expandable **indented tree** of completed items: single-click a row to show its
  children; scrollable (mouse wheel).

**LLM auto-classification** (`llm/`, optional, off by default)
- On task creation, a local/cloud LLM can place the task in the tree. Runs **async, off
  the UI thread**; the task is created standalone immediately and reparented when/if a
  result arrives (never blocks).
- Backend = **any OpenAI-compatible endpoint**. **Groq** is auto-selected when
  `GROQ_API_KEY` is set; a local server (e.g. Ollama at `.../v1`) via `[llm]` config.
- Prompt sends the current canvas tree as `\[id] parent=<pid|none>: text` lines (DONE
  tasks excluded); model returns `{relation: standalone|child_of|parent_of, targetId,
  confidence}`. `parent_of` inserts the new task **between** the target and its parent.
  Confidence threshold, timeouts, and transport retries; any failure → standalone.
- **DONE tasks are never sent** to the classifier and never chosen as targets.

**ttd> routing** (`app/DevRoute.hpp`) — tasks whose text starts with `ttd>` park under a
"tasktree dev" node and **skip** LLM classification (they're dev to-dos for agents, not
real tasks).

**Persistence & config**
- Tasks → `${XDG_DATA_HOME}/tasktree/tasks.json` (atomic write). Config →
  `${XDG_CONFIG_HOME}/tasktree/config.toml` (hand-editable TOML: hotkeys, colors, node
  width, `scroll_mode`, LLM settings, `log_requests`).

---

## 5. Build, run, variants

- **Prereqs (one-time):** Linux — `sudo apt install xorg-dev libgl1-mesa-dev libssl-dev`
  (X11 + GL headers; OpenSSL enables HTTPS for cloud LLMs). macOS — Xcode Command Line
  Tools plus `brew install cmake ninja` and optionally `openssl@3` (CMake finds the keg
  via `brew --prefix`). Deps (GLFW/NanoVG/glad/nlohmann-json/toml++/httplib) are fetched
  by CMake. glad generates its loader with a Python script that needs Jinja2; if the
  Python on `PATH` can't import it, `cmake/deps.cmake` provisions a private venv under
  the build dir rather than touching the system Python.
- **Dev build** (default, dev features on, debuggable): `cmake --preset dev` then
  `cmake --build build`. → `build/tasktree`.
- **Production build** (dev-only code stripped, small + optimized): `cmake --preset prod`
  then `cmake --build build-prod`.
- **Tests:** `ctest --test-dir build` (pure model/layout + store/config round-trips).
- **Run/manage:** `~/.init-scripts/tasktree.sh {start|stop|restart|status}` (aliased
  `tasktree`, auto-starts at login).

**Dev vs production**: the `TASKTREE_DEV` CMake flag gates development-only features. When
off, gated code compiles to nothing and its `.cpp` is dropped. Current dev-only feature:
the **LLM request/response log** (`llm/LlmLog`, to `<data>/llm.log`) — the pattern to
follow for any future dev instrumentation: guard with `#if TASKTREE_DEV`, add sources in
the `TASKTREE_DEV` branch of `CMakeLists.txt`, provide inline no-op stubs in the header
for the off case (see `LlmLog.hpp`).

---

## 6. Coordinate systems & gotchas

- **World vs screen.** The canvas has a view transform `screen = pan + zoom * world`.
  Node rects (`rects_`) are world coordinates; the renderer applies `pan`/`zoom` via
  `nvgTranslate`/`nvgScale`. Hit-testing/dragging convert with
  `worldMouse = (cursor - pan) / zoom`. Screen-space UI (DONE panel, input bar, palette
  drop-up) is drawn without the transform.
- **The running app owns `tasks.json`.** It rewrites the file on every change, so editing
  the file externally while the app runs gets clobbered. To edit safely: stop the app,
  re-read, edit, restart (`tasktree stop`/`start`).
- **No Wayland.** On Linux the overlay and its hotkeys assume X11 + a compositor.
  `Alt`+drag is *not* used for panning — the window manager grabs it.
- **Hotkey grabs** can collide with the desktop: GNOME/mutter reserves many `Super`
  combos, and macOS reserves `Cmd+Space` (Spotlight) and `Ctrl+Space` (input source).
  The app logs any chord it fails to grab.
- **Modifier names are shared, meanings are not.** One `config.toml` works on both
  platforms: `Super` is the Windows/Super key on X11 and **Command** on macOS, `Alt` is
  **Option** on macOS. Chords name *physical* keys (X11 keycodes / `kVK_*` codes), so
  they don't move with the keyboard layout.

---

## 7. Extending — where to add things

- New **model/layout** logic → `model/` or `layout/` (keep them GL-free; extend the
  tests in `tests/`).
- New **interaction** → `App` (state + input routing in `on*` handlers) +
  `ui/DragController` or `render/Renderer` primitives.
- New **LLM backend** → implement `IClassifier`; select it in `main.cpp`.
- New **platform** (Wayland/Win/Mac) → implement `IPlatform`.
- New **dev-only instrumentation** → gate with `#if TASKTREE_DEV` (see §5).
- Colors/sizes/hotkeys are config-driven (`app/Config`), not hard-coded — thread new
  knobs through there.

## 8. Related docs
- `CLAUDE.md` — session-start behavior + the **ttd** self-development workflow (how agents
  pick up `ttd>` tasks and file them to `ttd ✓ done`).
- `docs/AGENTS.md` — the iterative one-version-at-a-time planning loop.
- `docs/FUTURE.md` — roadmap / deferred ideas (keyboard-first operation, auto-align
  animation, true superellipse, richer LLM UX, multiple graphs, Wayland, …); its "v2 —
  in this release" section is the source of truth for what §4's v2 features cover.
- `README.md` — user-facing quick start.
