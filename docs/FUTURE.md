# TaskTree — Roadmap & Deferred Backlog

Ideas deferred from the POC. The seams to add them are already in place (`IPlatform`,
`IClassifier`, the pure `TidyLayout`/`Forest` modules, the `Renderer` primitives). Each
item notes where it plugs in. `AGENTS.md` describes how these get pulled into a version's
plan.

---

## v2 — in this release (branch `v2`)

v2's theme is **direct manipulation + safety**: make the tree editable in place and make
every edit reversible. Chosen so each interaction avoids colliding with the always-focused
input bar in the full overlay (bare typing goes to that bar), and so the model/layout
changes stay in the pure, unit-tested core.

- **Undo / redo.** A pure snapshot `History` (over whole `Forest` states) in `model/`.
  Every user mutation (add, reparent, status cycle, done/restore, delete, edit) snapshots
  first; `Ctrl+Z` / `Ctrl+Shift+Z` (also `Ctrl+Y`) step through. Bounded depth. No conflict
  with the input bar (it ignores those chords).
- **Node selection.** A left click that doesn't cross the drag threshold selects a canvas
  node (drawn with a selection ring); `Esc` or an empty-canvas click clears it. This is the
  handle the id badge always hinted at, and the base for edit + delete.
- **Delete subtree.** `Delete` / `Backspace` with a node selected and the input bar empty
  removes that subtree (undo-backed — no confirm dialog needed because undo exists).
- **In-place-ish text editing.** `F2` (or `Enter` with a node selected + empty input) edits
  the selected node: the input bar is seeded with its text and commit updates the node
  instead of adding a new task; re-measures + relayouts.
- **Collapse / expand subtrees.** A per-node `collapsed` flag (persisted). Layout treats a
  collapsed node as a leaf (subtree hidden, no overlap); a small disc handle on the bottom
  edge of any node with children toggles it — a minus sign when expanded, the hidden-
  descendant count when collapsed. View-only (no undo entry) but round-tripped in the store.

---

## Deferred — interaction & motion
- **Keyboard-first operation (the big interaction item).** The **command palette** (`?` /
  `:` / `>` in the input bar — `app/Palette.hpp`) covers most of this *without* needing a
  canvas-focus model: commands are typed in the bar, which already owns keyboard focus, so
  select-by-id (`:12`), select-by-text (`:?foo`) and reparent (`>12`, `>?foo`) are done.
  What's left genuinely needs focus-on-canvas:
  - **Step focus with arrows** — **up/down climb the tree** (to parent / first child),
    **left/right cycle siblings**. (`↑`/`↓` are currently the palette's candidate cursor,
    so a canvas-focus mode would have to claim them only when the bar is empty.)
  - **Move the selected node** with modified arrows (Shift+arrows) — promote to the parent's
    level, demote under a sibling, or reorder among siblings. Note the palette only appends
    (last child); ordering among siblings is still mouse-only, and drag is append-only too.
  - **Keyboard-only add**: Enter to add a child of the focused/selected node (today Enter on
    a selection edits it; a `+` palette prefix would fit the grammar).
  - **More palette verbs** as the need appears: `!` status cycle, `#` done/restore,
    multi-select, `>>` insert-between (the classifier's `parent_of` move).
- **Auto-align animation.** Tween node positions on relayout with easing instead of
  snapping (the camera glide already added for search/new-node pans is the pattern —
  extend it to per-node `rects_` interpolation). Keep previous + target `rects_`, ease-out
  cubic over ~150–200 ms, drive frames via an `animating_` timeout.
- **Drag reflow polish.** Ghost scale/opacity, snap-back animation on invalid drop, a
  horizontal insertion caret between siblings, and in-node ordering (drop is append-only
  today).
- **True in-place text editing.** v2 edits via the input bar; a later pass can draw the
  caret + wrapped editor directly over the node box.
- **Multi-select + bulk reparent.**

## Deferred — multiple graphs (ttd 106)
A dropdown to switch between several independent graphs, each its own forest.
- Model: today `Forest` is a single set of trees. Introduce a `Workspace` = named list of
  `Forest`s + an active index, or keep one `Forest` per file and switch the loaded file.
- Persistence: either one `graphs.json` holding all forests (`{active, graphs:[{name,
  forest}]}`), or a directory of `tasks-<name>.json` with a small index. The latter keeps
  the atomic-save story simple and lets each graph load lazily.
- LLM/classification context stays per-graph (only the active graph's outline is sent).
- UI: a compact dropdown in a screen-space chrome slot (top-left), plus add/rename/delete.
- Open questions: cross-graph drag/move? shared DONE section or per-graph? Deferred until
  the single-graph v2 UX settles.

## Deferred — nodes & text
- **True superellipse squircle.** Replace `nvgRoundedRect` in `Renderer::drawNode` with a
  path built from parametric superellipse samples for the genuine "squircle" silhouette.
- **Link highlighting + browser redirect.** Detect URLs in node text, render those runs in
  an accent colour, and open them with `xdg-open` on click (store click-target rects during
  draw for hit-testing).
- **Themes.** Light theme; per-tree accent colours. All colours already flow from `Config`.
- **Done state / progress.** Beyond the current DONE panel: a checkbox / strike-through on
  canvas nodes and rolled-up completion counts to parents.

## Deferred — classifier (LLM)
- **Generic provider system + connection modal.** Today: `NullClassifier` (off) and
  `OpenAiClassifier` (any OpenAI-compatible endpoint — Cerebras via `CEREBRAS_API_KEY`, or a
  local server like Ollama at `.../v1`). Generalise behind one seam, fully user-configurable:
  - In-app **modal** (no config-file editing) to add/edit connections: pick a provider
    *type* (local / OpenAI-compatible cloud / Ollama / Anthropic / custom base-URL / future
    embedded llama.cpp), enter **base URL + API key** (keys stored outside `tasks.json` —
    env var or OS keyring, never plaintext config).
  - **Fetch available models** (`GET /v1/models`, `/api/tags`) → dropdown.
  - Expose model / temperature / timeout / confidence threshold / max context in the modal.
  - Multiple named connections; switch active; test-connection button.
  - HTTPS needs `libssl-dev` at build (CMake enables `CPPHTTPLIB_OPENSSL_SUPPORT`); consider
    an HTTP layer that always has TLS to avoid the optional-OpenSSL split.
- **Richer relationships.** Suggest multiple candidate parents with a confirmation UI and
  show confidence, instead of auto-reparenting.
- **Embedded llama.cpp** option (link `libllama`) for a fully self-contained binary.
- **Debounced re-classification** as the tree grows; **local embeddings** to prefilter the
  nearest existing task cheaply before calling the LLM.

## Deferred — platform & portability
- **Wayland support** (the big one). `PlatformWayland` behind `IPlatform`: global hotkeys
  via `org.freedesktop.portal.GlobalShortcuts`, overlay via `wlr-layer-shell` or a portal
  surface (GNOME/Wayland can't place arbitrary always-on-top overlays like X11 — expect a
  reduced mode).
- **Multi-monitor** placement (follow the monitor with the cursor / focus).
- **Tray icon / D-Bus activation** as a fallback if a chosen global hotkey can't be grabbed.
- **macOS / Windows** ports behind `IPlatform` (Carbon/Cocoa hotkeys; Win32 `RegisterHotKey`).

## Deferred — SQLite store (ttd 133)
Replace the hand-rolled JSON store (`model/Store.cpp`, 112 lines, dependency-free) with
SQLite behind the same `store::load`/`save` seam.

**Verdict first: not worth doing for storage's sake.** 134 tasks ≈ 35 KB; save-on-every-
change rewrites the whole file atomically (temp + `rename`) from 12 call sites in `App` and
costs nothing measurable — two orders of magnitude more data before that hurts. The one
structural argument: **JSON's unit of write is the whole forest; SQLite's is the row.**
Everything below follows from that, and none of it is reachable without it.

- **Concurrent external edits.** The ttd workflow currently has to stop the app, edit
  `tasks.json`, and restart it, because the running app clobbers the edit on its next save.
  With WAL (one writer, many readers) an agent or a `tt` CLI can write *while the app runs*;
  the app reloads the changed rows instead of overwriting them. This is the actual reason to
  switch.
- **Persistent undo.** `History` holds in-memory whole-`Forest` snapshots, dropped on exit.
  An append-only `ops` table makes undo survive restarts and doubles as an audit log
  (supersedes "crash-safe history" below).
- **Multiple graphs (ttd 106).** One file, N forests, loaded lazily — removes that entry's
  "one JSON per graph plus an index file" awkwardness and its atomic-save open question.
- **Queries.** `created_at`/`done_at` are already persisted but only reachable by walking the
  forest in C++; throughput, aging and "untouched for 30 days" become `SELECT`s.

**Costs to accept explicitly**
- `tt_core` (model + layout + palette) is the dependency-free layer and stays that way.
  `tt_io` already links nlohmann/json and toml++, but those are header-only: SQLite is the
  first *compiled* third-party library in the persistence path, and the
  `plan/qt6-fallback` / `plan/imgui-alt` branches reuse `model/` + `Store` verbatim.
- Loses greppable, diffable, hand-repairable state. `.dump` is not the same thing.
- A schema means migrations. JSON version-stamps (`{"version": 1}`) and never needed one; a
  table does (`PRAGMA user_version`).

**Shape if built**
- `tasks(id, parent, ord, text, done, collapsed, status, created_at, done_at)` +
  `meta(key, value)` for `next_id` / schema version. `roots` and `doneRoots` become derived
  (`parent IS NULL`, plus a done flag) rather than two stored vectors. Sibling order lives in
  `ord` — which finally gives the sibling-reordering item under *interaction & motion* a home,
  since `Forest::reparent` takes an index today that every caller ignores.
- Keep whole-forest `load`/`save` first (load at start, save as one transaction) so `App` and
  its 12 call sites don't change on day one; move to incremental row writes after that works.
- WAL, `synchronous=NORMAL`, single writer.
- **Keep JSON permanently** as export/import (see below): backup, git-diffable snapshot, and
  the ttd fallback if the DB is ever wedged.
- Migration: on first run, if `tasks.json` exists and the DB doesn't, import it and leave the
  JSON in place as `tasks.json.bak`.

**Cheaper alternative if external edits are the only goal.** Watch the data directory with
inotify (save is a `rename`, so watch the dir, not the file) and reload on change — ~60 lines,
no new dependency. It does *not* fix clobbering: the app's next whole-file save still wins. Fits
"pick up my edits", not "edit while I work".

## Deferred — data & polish
- **Export / import** (Markdown outline, OPML, JSON). Becomes load-bearing if the SQLite
  store above lands — JSON export is then the only human-readable form of the data.
- **Debounced / journaled saves** instead of save-on-every-change; crash-safe history
  (largely subsumed by the SQLite `ops` table above).
- **Config hot-reload** (watch the config file, re-apply without restart).
