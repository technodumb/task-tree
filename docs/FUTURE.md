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
  collapsed node as a leaf (subtree hidden, no overlap); a chevron affordance on nodes with
  children toggles it, with a small "+N" badge showing hidden descendant count.

---

## Deferred — interaction & motion
- **Keyboard-first operation (the big interaction item).** Full mouse-free flow needs a
  proper *canvas focus* vs *input focus* model (today the input bar always holds keyboard
  focus in the overlay). Once that exists:
  - **Select** a node by typing its id (vimium-style), or step focus with arrows —
    **up/down climb the tree** (to parent / first child), **left/right cycle siblings**.
  - **Reparent by id**: a command like "move N under M" (type the two ids) that calls
    `Forest::reparent`, then relayout.
  - **Move the selected node** with modified arrows (Shift+arrows) — promote to the parent's
    level, demote under a sibling, or reorder among siblings.
  - **Keyboard-only add**: Enter to add a child of the focused/selected node.
  v2 ships single-node click-selection as the first step toward this.
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

## Deferred — data & polish
- **Export / import** (Markdown outline, OPML, JSON).
- **Debounced / journaled saves** instead of save-on-every-change; crash-safe history.
- **Config hot-reload** (watch the config file, re-apply without restart).
