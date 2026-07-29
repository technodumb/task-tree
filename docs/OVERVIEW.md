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

Status: proof-of-concept, **X11 only** (Wayland is future work). The app is being
dogfood-developed *through itself* — see the "ttd" workflow in `CLAUDE.md`.

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
platform/  X11 window + global hotkeys      ← behind IPlatform (Wayland can slot in)
render/    NanoVG drawing (nodes/edges/UI)
ui/        TextInput (line editor) + DragController
layout/    PURE tidy-tree engine + geometry  ← no GL, unit-tested
model/     Task/Forest + JSON persistence     ← PURE, unit-tested
app/       state machine, config, Palette     ← Palette (bar grammar) is PURE, unit-tested
llm/       Pluggable classifier               ← behind IClassifier
app/       App state machine, Config, paths, ttd routing
```

Two deliberate **seams** (single-impl today, exist for extension — do not delete as
"dead code"):
- **`IPlatform`** (`platform/IPlatform.hpp`) — window + hotkeys. `PlatformX11` implements
  it; a `PlatformWayland`/Win/Mac would slot in here.
- **`IClassifier`** (`llm/IClassifier.hpp`) — task classification. `NullClassifier`
  (default, everything standalone) and `OpenAiClassifier` (any OpenAI-compatible
  endpoint) implement it.

The **pure layers** (`model/`, `layout/`) have zero external deps and are unit-tested
(`tests/`), so they're stack-agnostic and fast to reason about. `App` is the orchestrator
that ties model → layout → render and routes input.

**Data model** (`model/Task.hpp`): `Task{id, parent, text, children[], done, collapsed,
status, createdAt, doneAt}`; `Forest{nodes, roots[], doneRoots[], nextId}`. Key ops:
`addTask`, `reparent` (with cycle prevention), `markDone`/`restoreFromDone`,
`isInDoneSection`, `removeSubtree`. Sibling order = children-vector order = layout order.
Undo/redo is a separate pure `model/History` — a bounded stack of whole-`Forest`
snapshots, so any mutation is reversible without per-op inverse logic (v2).

---

## 4. Feature catalog

**Overlay & capture**
- Two customizable global hotkeys: **toggle full overlay** (`Ctrl+Alt+Space`) and
  **quick-add** (`Ctrl+Alt+Return`). Grabbed via Xlib `XGrabKey` on a dedicated thread
  (handles nuisance modifiers; warns if a chord is already taken).
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
- **Command palette (the input bar)**: a leading symbol re-purposes the one text field
  instead of opening another (grammar + ranking: `app/Palette.hpp`, unit-tested).

  | typed | mode | Enter |
  |---|---|---|
  | `task text` | add (unchanged) | add the task, LLM-classified as before |
  | `?query` | **find** (amber) | jump to the active match, stay in find mode |
  | `:12` | **select** (blue) | select node 12 (the number is its id badge) |
  | `:?query` / `:query` | **select by text** | select the active match |
  | `>12` | **parent** (green) | make node 12 the parent of the selection |
  | `>?query` / `>query` | **parent by text** | make the active match the parent |

  After `:` / `>` the `?` is optional — a non-numeric tail is a text query anyway. A
  **leading space escapes** the grammar, so `" :literal"` adds a task named `:literal`.
  Text modes show a **drop-up list** above the bar (up to 4 rows of `[id] text` + "+N
  more"); `↑`/`↓` walk it (wrapping), the bar's **border and the drop-up are tinted by
  mode** and the active candidate is ringed on canvas — blue for select/find (what Enter
  acts on), green for parent (where the selection lands), while every match keeps its amber
  ring. The bar also shows a status: `7 matches`, `no match`, `node 12`, `→ child of 12`,
  `can't move there`, `select a node first`. Targets hidden under a collapsed ancestor are
  **revealed** (ancestors expanded) when committed; the canvas only pans if the target is
  **off screen**, and never for a `>` move onto a visible node (that one anchors instead).
  `Esc` backs out of the command, leaving the overlay open.
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
- Backend = **any OpenAI-compatible endpoint**. **Cerebras** is auto-selected when
  `CEREBRAS_API_KEY` is set; a local server (e.g. Ollama at `.../v1`) via `[llm]` config.
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

- **Prereqs (one-time):** `sudo apt install xorg-dev libgl1-mesa-dev libssl-dev`
  (X11 + GL headers; OpenSSL enables HTTPS for cloud LLMs). Deps (GLFW/NanoVG/glad/
  nlohmann-json/toml++/httplib) are fetched by CMake.
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
- **X11-only.** Global hotkeys (`XGrabKey`) and the overlay assume X11 + a compositor.
  `Alt`+drag is *not* used for panning — the window manager grabs it.
- **Hotkey grabs** can collide with the desktop (GNOME/mutter reserves many `Super`
  combos); the app logs any chord it fails to grab.

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
