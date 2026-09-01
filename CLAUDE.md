# CLAUDE.md — repo guide

## Default branch: `v3`

Active development happens on **`v3`**, checked out in this folder (the main repo
checkout, `Out/TaskTree`). At session start, check you're on `v3` and switch to it if
not — don't start work on `main`, `v1` or `v2`. `v1`/`v2` are maintenance lines; `main`
only tracks whatever the current line has published. Commit ttd work to `v3`;
**never push**.

## ttd dev workflow — run this at the START of every session

The user develops TaskTree *through* their running TaskTree app: they plan in the task
tree and mark dev work with a `ttd>` prefix. A read-only `SessionStart` hook
(`scripts/ttd-scan.sh`, wired in `.claude/settings.json`) prints the pending `ttd>`
tasks into your context at startup — so this works from **any** Claude instance in this
folder. When you start a session here, **proactively** pick up that work — don't wait to
be asked:

1. Read the store in `~/.local/share/tasktree/`: **`tasks.db` if it exists, otherwise
   `tasks.json`** — the DB wins once created, the rule `src/main.cpp`, the `tt` CLI and
   `scripts/ttd-scan.sh` all use. Collect **canvas** (non-DONE) tasks whose text starts
   with `ttd>` (case-insensitive, optional space). These are dev tasks for you. Easiest:
   `build/tt find "ttd>"` lists them and `build/tt tree --json` is the raw walk — the
   `SessionStart` hook is now just a thin wrapper over `tt tree --json`.
   In the DB there are no `roots`/`children` lists: a task is top-level when `parent = 0`
   and `ord` is its position among its siblings. **There are no booleans: a timestamp is the
   state.** `done_at = 0` is not done, `> 0` is done at that epoch-ms, and `-1` is done at an
   unknown time (finished before the date was recorded — 43 of the existing tasks). Likewise
   `deleted_at != 0` means retired, and those rows must be excluded from every query.
   **Completion is not a move** — a done task keeps its parent and slot — so a task is off
   the canvas when *any* task on its parent chain (itself included) has `done_at != 0`, and
   the DONE section's top entries are the done tasks whose parent chain holds no other.
2. If there are any clearly-scoped ones, **start implementing them now**. For each:
   implement → build (`cmake --build build -j`) → `ctest` → commit (one focused commit
   per task; **never push**).
3. File each completed task into the DONE section under a done-root titled **`ttd ✓ done`**
   (create it if missing), using the **`tt` CLI** (`build/tt`, from `cmake --build build`) —
   no more hand-written SQL. `tt` loads → mutates → saves in one shot, passing the loaded
   forest as the save *baseline*, so it preserves every other task exactly (ids, order,
   parents, status, done state, and `next_id`) on its own — that is the whole reason it
   exists. Verbs: `tt find "ttd>"` (pending), `tt tree --json` (full read), `tt done <id>`,
   `tt parent <child-id> <parent-id>`, `tt add "text" [--parent <id>]`, `tt rm <id>` (soft).
   Address the `ttd ✓ done` root — and any done task — **by id**: text search (`find`,
   name-queries) covers only the canvas, so a done node won't match; read its id once from
   `tt tree --json`. **You no longer need to stop the app.** It holds one connection and
   polls `PRAGMA data_version` (~1 s), so it notices `tt`'s commit and reloads — the change
   shows on the live canvas within a second. Confirm in `/tmp/tasktree.log`
   (`Store: another writer changed … — reloaded (N tasks)`). Two caveats: each external
   commit clears the app's undo stack, so keep `tt` writes few (a planned `tt batch` will
   fold several into one reload); and don't edit a row the user is actively editing/dragging
   that same second (the app defers its reload during a drag or in-bar edit, then wins any
   same-row conflict). (App stopped, or on a pre-Session binary? The old `tasktree.sh stop`
   → edit → `start` dance still works.)
4. If a task is **ambiguous, risky, or destructive, don't implement it** — surface the
   doubt (tell the user, and/or flag it orange `status=2` / add a `❓ <question>` child
   in the tree) and move on. You're in a live session, so just ask the user directly
   when it's quick.
5. If the user's message is about something else, **do that first** — the ttd sweep is a
   proactive default, not a hijack.

There are normal permission prompts (this is an attended session); the user approves
edits/commits as they come. There is no unattended loop and no standing allowlist.

## Build / test
- Dev build (default, dev features on): `cmake --preset dev` then `cmake --build build`,
  or just `cmake --build build`.
- Production build (dev-only features like the LLM log stripped, small + optimized):
  `cmake --preset prod` then `cmake --build build-prod`.
- Tests: `ctest --test-dir build`. Pure logic (model, layout, store, config) is
  dependency-free and unit-tested; extend those with any model/layout change.
- Dev-only code is gated by `TASKTREE_DEV` (see `src/llm/LlmLog.hpp` for the pattern).
- **Always restart the running app after modifying it.** Once a build (and any tests)
  succeed, run `~/.init-scripts/tasktree.sh restart` (aliased `tasktree restart`) so the
  live instance picks up the new binary and the user can see the change immediately.
  Do this once per finished set of changes — after the successful build — not after each
  individual edit. Restart is safe: tasks persist in the store across it.

## Layout of the code
`src/model` (Task/Forest + SQLite/JSON store), `src/layout` (pure tidy-tree engine),
`src/render` (NanoVG), `src/ui` (text input, drag), `src/platform` (overlay window +
global hotkeys behind `IPlatform`; `PlatformGlfw` is shared, with X11 and macOS backends
for the window hints and the hotkey grabber), `src/llm` (pluggable classifier),
`src/cli` (the `tt` CLI — headless read/write over the store, app running or not),
`src/app` (state machine, config).
Roadmap in `docs/FUTURE.md`; iterative-plan protocol in `docs/AGENTS.md`.
