## Context

v3 built everything a second writer needs, in two waves.

**The store (slices 1–7).** `PRAGMA journal_mode=WAL` plus `sqlite3_busy_timeout(3000)` —
`SqliteStore.cpp` says it plainly: *"A second process (a CLI, an agent) may hold the write
lock briefly; wait rather than fail."* `BEGIN IMMEDIATE` takes the write lock up front, so
no reader sees a half-written tree. `save(f, path, &baseline)` writes only the diff and
infers a deletion from *"present in baseline, gone from `f`"* rather than *"absent from
`f`"*, so rows a concurrent writer added survive. Deletion is soft: `deleted_at` stamped,
no `DELETE` statement anywhere, retired rows readable via `store::deletedRows()`.
`dbSchemaVersion()` refuses a newer store before a single pragma is set, and
`quarantine()` moves an unreadable store aside rather than overwriting it.

**Reload (slice 8, commit `2fe2f5f`).** `store::Session` holds one connection for the app's
life and exposes `changedExternally()`, which polls `PRAGMA data_version` — a value that
moves only when a *different* connection commits, which is exactly why the poll must share
the connection the app saves through. `App::pollStore()` runs it on a 1 s interval,
deferring while a drag or in-bar edit is active, and `App::reloadFromStore()` replaces the
forest, sets `lastSaved_` to the reloaded state, keeps `nextId` monotonic, clears the
selection if its node vanished, and drops the undo history.

So both halves of the hard problem are solved, and **an earlier draft of this design was
wrong about the second one**: it argued app-side reload had to ship *with* the CLI. It
shipped first instead. What is left is a client.

Constraints: `tt_core` links nothing and `tt_io` links only header-only json/toml++ plus
SQLite, so a CLI needs no GLFW/GL/X11/Cocoa. The store is single-user and local — 2.2 ms to
load 1000 tasks makes per-invocation load cost a non-issue.

## Goals / Non-Goals

**Goals:**
- A `tt` binary an agent can drive: read the tree, add, edit, complete, reparent, delete.
- Work identically whether or not the app is running.
- Never corrupt or silently discard data, including under concurrent use.
- Batch many operations into one transaction.
- Output a program can parse without heuristics.
- Remove the hand-written SQL from `CLAUDE.md` step 3.

**Non-Goals:**
- **App-side reload** — already shipped in v3 slice 8. Nothing here re-implements it.
- **No MCP server.** Explicitly rejected: *"i do not need to initialise mcp every time."*
  An MCP wrapper can shell out to `tt` later; nothing here forecloses that.
- **No cross-process undo.** Soft delete makes `rm` recoverable and the other mutations are
  hand-reversible. The `ops` table stays deferred (`docs/plans/v3.md` item 2).
- **No daemon or IPC.** Considered and rejected below.
- **No sibling reordering** (`ord` gives it a home, but it is its own change), **no multiple
  graphs**, **no JSON→DB migration from the CLI**.

## Decisions

### 1. Direct store access, not IPC to the running app

An IPC design (CLI sends ops to the app; the app stays the only writer) is attractive — no
locking by construction, instant overlay update, and the main-thread drain pattern already
exists in `Hotkeys`. Rejected because the CLI must work with the app closed. Supporting both
would mean two mutation paths that must stay behaviourally identical, and the one exercised
least would rot.

Direct access is viable precisely because v3 made it safe: WAL for concurrent readers,
`BEGIN IMMEDIATE` for serialised writers, `busy_timeout` for contention, baseline-diffed
saves so neither writer clobbers the other, and now a reload so the app sees the result.

### 2. Pass the loaded forest as `baseline` on every write

Non-negotiable, and the single most important implementation rule. `tt` loads, mutates, then
calls `save(forest, path, &loaded)`. Passing `nullptr` requests a full rewrite, which *does*
delete anything absent from `f` — destroying every task the app added while `tt` was running.
`App::save` already does this correctly, advancing `lastSaved_` only on success; the CLI
mirrors it.

### 3. Free functions, not `store::Session`

`Session` exists to hold a connection across a long-lived process and to make
`data_version` polling meaningful. A one-shot CLI wants neither: it loads, mutates, saves,
and exits, so the open/pragma prologue happens once either way, and it has nothing to poll
*for*. The free functions are the right seam. `batch` is what makes this efficient under
load — twenty operations in one `load → mutate → save` rather than twenty processes.

### 4. Mirror the palette's vocabulary and reuse `rankMatches`

v2 shipped `add` / `find` / `select` / `parent` with nodes addressed by id or text
(`src/app/Palette.hpp`). The CLI reuses those verbs and calls `palette::rankMatches`, so
`tt` and the overlay agree on which node a name means. A second vocabulary would drift.

Ambiguity is an error, not a guess: an agent that silently edits the wrong node is worse
than one forced to disambiguate. Ids stay the reliable handle, and `add` prints the new id
so an agent can chain operations.

### 5. The CLI is a well-behaved but *unprivileged* writer

It honours the schema-version guard and refuses a newer store. It deliberately does **not**
call `quarantine()` and does **not** run `migrateJsonToDb()`. Both are recovery/upgrade
decisions with lasting consequences — migration is a one-way door, after which a v1/v2
binary would load stale JSON and diverge (`docs/plans/v3.md` risks). An agent-invoked tool
should never cross that; it reports and exits non-zero so a human runs the app.

### 6. Batch is a courtesy to the user, not just an optimisation

This replaces the earlier "reload ships with the CLI" decision, which slice 8 made moot.

`App::reloadFromStore()` calls `history_.clear()`, and the code says why: *"The undo stack
predates the external edit: undoing into one of its snapshots and saving would erase the
other writer's rows as 'deleted here'."* That is correct, and it means **every `tt` write
against a running app costs the user their entire undo history.** Twenty separate `tt`
calls means twenty reloads and twenty stack clears while they are working.

So `batch` is not merely "fewer writes". It is the difference between an agent that
interrupts once and one that repeatedly destroys the user's ability to undo. Agent-facing
docs should present `batch` as the default way to mutate, with single commands for
interactive use.

### 7. Commit promptly; let the app win same-row conflicts

Both sides run a 3 s busy timeout, so a transaction held open across slow work would make
the *other* side fail. `tt` opens its write transaction only once every mutation is computed,
and commits immediately.

Where both edit the same row, the app wins — it reloads, then saves its own version from
memory. `tt` does not try to detect or defeat this. It is the right default: the human at the
keyboard should beat the background agent, and `CLAUDE.md` already tells agents not to edit a
row the user is actively working on.

### 8. Tests are a real CLI harness, not just unit tests

`tests/cli_tests.cpp` drives command dispatch against a temp store via `XDG_DATA_HOME`,
including the case that matters most: load, let a simulated second writer commit, then save,
and assert the other writer's rows survive. Concurrency claims are worth nothing unasserted.

## Risks / Trade-offs

- **A second writer is a new failure surface for real user data.** → v3's guards cover the
  dangerous cases; the CLI adds no new write primitive, only calls `store::save` with a
  correct baseline.
- **Forgetting `baseline` on one code path silently destroys data.** → Route every mutation
  through one save helper so there is exactly one call site, covered by the
  concurrent-writer test.
- **Every CLI write clears the user's undo stack** (decision 6). → Unavoidable while
  `History` is in-memory snapshots; mitigated by `batch`, and genuinely fixed only by the
  deferred `ops` table.
- **Text addressing may resolve differently as the tree changes.** → Ambiguity is an error,
  and ids are always available. Agents should prefer ids from `tree --json`.
- **JSON stores get a weaker guarantee** — whole-file rewrite, no `deleted_at`, and
  `changedExternally()` is always false so the app will not notice a `tt` write at all. →
  Acceptable: JSON is now export/import and pre-migration files only. The reduced guarantees
  are stated in the spec.
- **CLI and overlay could drift in behaviour.** → Sharing `Forest` mutators and
  `rankMatches` keeps one implementation of the semantics.

## Migration Plan

Additive; nothing to migrate. `tt` ships alongside the existing binary, no file format
changes, and no app code is touched. Rollback is dropping the `tt` target.

`scripts/ttd-scan.sh` stays until `tt` ships, then becomes a thin wrapper over
`tt tree --json`, preserving the `SessionStart` hook's output contract. `CLAUDE.md` step 3
keeps its current "you no longer need to stop the app" guidance and swaps the
`python3 -c "import sqlite3; …"` recipe for `tt`.

## Open Questions

- **Batch input format.** JSON array of operations, or one shell-like command per line? JSON
  is easier for an agent to emit correctly; lines are easier to write by hand.
- **Should `tt` write to a JSON store at all**, or refuse and direct the user to the app so
  migration happens first? Refusing is safer, and on JSON the app cannot even notice the
  write — but it breaks "works with the app closed" for anyone unmigrated.
- **Is `tt undone` the right name** for clearing `done_at`? `reopen` may read better.
- **Should `tt` grow a `restore` for soft-deleted rows?** The data supports it and
  `deletedRows()` already exposes the trash; it may belong to the "restore deleted" view
  that `Store.hpp` anticipates.
- **Should `tt` warn when the app is running and it is not in batch mode**, given each write
  costs an undo stack? Possibly too noisy for a tool an agent calls in a loop.
