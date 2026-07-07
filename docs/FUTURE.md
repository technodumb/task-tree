# TaskTree — Future Innovations Backlog

Ideas deliberately deferred from the POC. The seams to add them are already in
place (`IPlatform`, `IClassifier`, the pure `TidyLayout`/`Forest` modules, the
`Renderer` primitives). Each item notes where it plugs in. `AGENTS.md` describes
how these get pulled into the next version's plan.

## Interaction & motion
- **Auto-align animation.** Tween node positions on relayout with easing instead of
  snapping. Keep the previous `rects_` and the new target `rects_`, interpolate over
  ~150–200 ms, drive continuous frames while animating (the loop already supports a
  0-timeout "poll" mode; `App::desiredTimeout()` returns 0 during a drag — extend
  that to an `animating_` flag). Ease-out cubic is a good default.
- **Drag reflow polish.** The gap already opens via `previewLayout`; add a subtle
  scale/opacity on the ghost, snap-back animation on invalid drop, and a horizontal
  insertion caret between siblings.
- **Node text editing.** Double-click a node to edit its text in place (reuse
  `TextInput`), re-measure + relayout on commit.
- **Collapse / expand subtrees.** A per-node `collapsed` flag (already room in the
  model) that hides descendants; layout skips collapsed subtrees. Chevron affordance.
- **Multi-select + bulk reparent**, **undo/redo** (command stack over `Forest` ops),
  **delete key** to remove the hovered subtree (with confirm).
- **Keyboard navigation via node ids.** Each node already renders a small id label
  (its stable `TaskId`, top-right). Build on it:
  - **Select** a node by typing its id (vimium-style), or step focus with arrows —
    **up/down climb the tree** (to parent / first child), **left/right cycle siblings**.
  - **Reparent by id**: a command like "move N under M" (type the two ids) that calls
    `Forest::reparent`, then relayout.
  - **Move the selected node** with modified arrows (e.g. Shift+arrows) — promote to
    the parent's level, demote under a sibling, or reorder among siblings.
  - Goal: full mouse-free operation (add, select, move, complete) driven by the ids.
- **Keyboard-only add.** Enter to add a child of the focused/selected node.
- **Pan & zoom** the canvas for large forests; **search / filter** to highlight or
  isolate matching nodes.

## Nodes & text
- **True superellipse squircle.** Replace `nvgRoundedRect` in `Renderer::drawNode`
  with a path built from parametric superellipse samples (`nvgMoveTo`/`nvgBezierTo`)
  for the genuine "squircle" silhouette.
- **Link highlighting + browser redirect.** Detect URLs in node text, render those
  runs in an accent colour (NanoVG per-run colouring), and open them with `xdg-open`
  on click. Store click-target rects during draw for hit-testing.
- **Themes.** More than the current single dark theme; light theme; per-tree accent
  colours. All colours already flow from `Config`.
- **Done state / progress.** The `done` flag exists in the model — render a checkbox,
  strike-through, and roll up completion counts to parents.

## Classifier (LLM)
- **Generic provider system + connection modal (planned).** Today: `NullClassifier`
  (off), `OllamaClassifier` (local HTTP), and `OpenAiClassifier` (any OpenAI-compatible
  endpoint — Cerebras is wired via the `CEREBRAS_API_KEY` env var). Generalise this to
  support *any* backend behind a single seam and make it fully user-configurable:
  - An in-app **modal** (no config-file editing) to add/edit connections: pick a
    provider *type* (local / OpenAI-compatible cloud / Ollama / Anthropic / custom
    base-URL / future embedded llama.cpp), enter **base URL + API key** (keys stored
    outside `tasks.json`, e.g. an env var or an OS keyring, never in plaintext config).
  - **Fetch available models** from the endpoint (`GET /v1/models` for OpenAI-compatible,
    `/api/tags` for Ollama) and let the user pick from a dropdown.
  - Expose **configuration options** (model, temperature, timeout, confidence threshold,
    max existing-task context) in the modal.
  - Store multiple named connections; switch the active one; test-connection button.
  - HTTPS requires `libssl-dev` at build time (CMake enables `CPPHTTPLIB_OPENSSL_SUPPORT`);
    document/bundle this. Consider migrating the HTTP layer to something that always has
    TLS to avoid the optional-OpenSSL split.
- **Richer relationships.** Suggest multiple candidate parents with a confirmation UI
  instead of auto-reparenting; show the classifier's confidence.
- **Embedded llama.cpp** option (link `libllama`) for a fully self-contained binary.
- **Debounced re-classification** as the tree grows; **local embeddings** to prefilter
  the nearest existing task cheaply before calling the LLM.

## Platform & portability
- **Wayland support** (the big one). Add a `PlatformWayland` behind `IPlatform`:
  global hotkeys via `org.freedesktop.portal.GlobalShortcuts`, overlay via
  `wlr-layer-shell` where available or a portal surface (note: GNOME/Wayland cannot
  place arbitrary always-on-top overlays the way X11 does — expect a reduced mode).
- **Multi-monitor** placement (follow the monitor with the cursor / focus).
- **Tray icon / D-Bus activation** as a fallback if a chosen global hotkey can't be
  grabbed under the current compositor.
- **macOS / Windows** ports behind `IPlatform` (Carbon/Cocoa hotkeys; Win32
  `RegisterHotKey`).

## Data & polish
- **Export / import** (Markdown outline, OPML, JSON).
- **Debounced / journaled saves** instead of save-on-every-change; crash-safe history.
- **Config hot-reload** (watch the config file, re-apply without restart).
