## Why

TaskTree's data is reachable only through the overlay's keyboard and mouse, so an agent
cannot read or change the tree. v3 has since built every piece a second writer needs —
row-level incremental writes, WAL, baseline-diffed saves, soft delete, and now
`store::Session` polling `PRAGMA data_version` so the app notices another writer and
reloads within a second — but **nothing uses any of it**. `SqliteStore.cpp` names "a CLI,
an agent" as the motivating case in its own comments. This change supplies that client.

The ttd loop is the concrete first consumer. `CLAUDE.md` step 3 no longer requires
stopping the app, but it still asks an agent to hand-write SQL through
`python3 -c "import sqlite3; …"` while personally preserving *"ids, order, parents,
status, done state […] `ord` and `parent` too, and `meta.next_id` must not go backwards."*
That is a corruption opportunity on every edit, and it is what `tt` removes.

## What Changes

- **New `tt` executable.** Links `tt_core` + `tt_io` only: no GLFW, GL, X11, or Cocoa, so
  it builds anywhere the toolchain does and is testable from `ctest`.
- **Read commands**: `tree`, `show`, `find`, `deleted`. `tree` is the agent's primary read
  and exposes `created_at` / `done_at`, persisted today but reachable only by walking the
  forest in C++.
- **Write commands**: `add` (prints the new id), `edit`, `done`, `undone`, `status`,
  `parent`, `rm` (soft delete).
- **`batch`**: many operations in a single `load → mutate → save` cycle. Beyond avoiding N
  writes, this matters because **each external commit costs the user their undo stack** —
  `App::reloadFromStore` calls `history_.clear()`, since undoing into a pre-external
  snapshot would erase the other writer's rows. One batch is one reload, not twenty.
- **`--json` on every command**, stable field names, documented exit codes: the primary
  consumer is a program.
- **Node addressing by id or text query**, reusing `palette::rankMatches` so `tt` resolves
  a name to the same node the in-app palette would. Ambiguity is an error, not a guess.
- **Cooperating with the live app**: one transaction per invocation, committed promptly so
  neither side exhausts the other's 3 s busy timeout. The app already defers its reload
  during a drag or an in-bar edit and wins a same-row conflict; `tt` does not fight it.
- **Store safety rules**: refuse a store whose schema is newer than this build, never
  `quarantine()` (the app owns recovery), and never run the JSON→SQLite migration — that
  first run is a one-way door and belongs to the app alone.
- **`scripts/ttd-scan.sh` becomes redundant** — `tt tree --json` covers it. Replaced by a
  thin wrapper so the `SessionStart` hook keeps working.

Not breaking: no existing behaviour, file format, or command changes. `tt` is purely
additive; no app code needs to change.

## Capabilities

### New Capabilities
- `task-cli`: the `tt` executable — command surface, node addressing, JSON output
  contract, exit codes, batch semantics, cooperation with a live app, and the store-safety
  rules it must obey.

### Modified Capabilities
<!-- None. openspec/specs/ is empty — OpenSpec was initialized on this branch, so there
     are no established specs to delta.

     NOTE: an earlier draft of this change also proposed a `store-reload` capability, for
     the app noticing another writer. That shipped upstream first, in v3 commit 2fe2f5f
     ("the app notices another writer — one held connection, data_version polled"):
     store::Session::changedExternally(), App::pollStore() on a 1 s interval, and
     App::reloadFromStore(). Its spec was removed from this change rather than left as a
     promise to re-implement something that already exists. -->

## Impact

- **New code**: `src/cli/` (argument parsing, command dispatch, JSON output),
  `tests/cli_tests.cpp`.
- **New build target**: `tt` in `CMakeLists.txt`, linking `tt_core` + `tt_io`. Both
  presets, plus a new `ctest` entry.
- **No app code changes.** `App`, `main.cpp`, and the event loop are untouched — the
  reload half of this problem is already done.
- **Reused unchanged**: `store::load` / `store::save(…, &baseline)`, `store::deletedRows`,
  `store::dbSchemaVersion`, `Forest`'s mutators, `palette::rankMatches`. No new
  third-party dependency — SQLite arrived with v3.
- **Docs**: `README.md` (CLI section), `docs/OVERVIEW.md` (the new layer), and `CLAUDE.md`
  step 3 — replacing the hand-written `python3 -c "import sqlite3; …"` recipe and its list
  of invariants with `tt` commands.
- **Risk**: a second writer is a new failure surface for real data. Mitigated by v3's
  existing guards, by the CLI never migrating or quarantining, and by `batch` keeping the
  common agent workflow to one transaction and one reload.
