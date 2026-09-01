## 1. Build target and skeleton

- [x] 1.1 Add a `tt` executable to `CMakeLists.txt` linking only `tt_core` + `tt_io` (no GLFW/GL/X11/Cocoa), built by both the `dev` and `prod` presets
- [x] 1.2 Create `src/cli/main.cpp` with `argc`/`argv` parsing, a command table, `--help`, `--version`, and global `--json` / `--store <path>` flags
- [x] 1.3 Define the exit-code enum (0 ok, usage error, node-not-found, ambiguous-match, store-unreadable, store-too-new, store-busy) in one header and document it in `--help`
- [x] 1.4 Verify `tt --help` builds and runs, and that the binary links no windowing library (`otool -L` / `ldd`)

## 2. Store resolution and safety gate

- [x] 2.1 Implement store resolution: `paths::dbFile()` if it exists, else `paths::tasksFile()`, honouring `XDG_DATA_HOME` and `--store`
- [x] 2.2 Exit non-zero with the expected path when neither store exists; create nothing
- [x] 2.3 Gate every command on `store::dbSchemaVersion()` vs `store::supportedDbSchemaVersion()`; refuse a newer store before any write, naming both versions
- [x] 2.4 On a load failure where the file exists, exit non-zero advising the app be run — never call `store::quarantine()`
- [x] 2.5 Never call `store::migrateJsonToDb()`; test that a JSON-only store is left un-migrated with no `tasks.db` created

## 3. Node addressing

- [x] 3.1 Implement `resolveNode(spec)`: numeric ids via `Forest::exists`, text via `palette::rankMatches`
- [x] 3.2 Treat multiple matches as an error listing candidate ids and texts; leave the store untouched
- [x] 3.3 Treat unknown ids, deleted tasks, and zero matches as errors; leave the store untouched
- [x] 3.4 Unit-test id, unique-match, ambiguous, and no-match paths

## 4. Read commands

- [x] 4.1 Define the JSON task shape once (`id`, `text`, `parent`, `ord`, `status`, `collapsed`, `created_at`, `done_at`, `children`) and serialise through a single function
- [x] 4.2 `tt tree [<id|query>] [--json]` — whole forest or a subtree, done tasks included and marked, empty store is exit 0
- [x] 4.3 Human-readable indented outline for `tree` without `--json`, one task per line with ids
- [x] 4.4 `tt show <id|query>` — one task plus its immediate children
- [x] 4.5 `tt find <query> [--json]` — matches in `rankMatches` order, exit 0 when nothing matches
- [x] 4.6 `tt deleted [--json]` via `store::deletedRows()`, newest first; empty list on a JSON store

## 5. Write commands

- [x] 5.1 Write one save helper that is the **only** call site of `store::save`, always passing the loaded forest as `baseline` (design decision 2)
- [x] 5.2 Compute every mutation before opening the write transaction, and commit promptly, so neither side exhausts the other's 3 s busy timeout (design decision 7)
- [x] 5.3 `tt add "text" [--parent <id|query>]` — appends as last child, prints the new id
- [x] 5.4 `tt edit <id|query> "text"`
- [x] 5.5 `tt done <id|query>` / `tt undone <id|query>` via `Forest::markDone` / `restoreFromDone`, preserving parent and sibling slot
- [x] 5.6 `tt status <id|query> <normal|in-progress|priority>` mapping to 0/1/2
- [x] 5.7 `tt parent <child> <parent>` via `Forest::reparent`, refusing cycles with a non-zero exit and no write
- [x] 5.8 `tt rm <id|query>` — soft delete; assert rows survive with `deleted_at` and appear in `tt deleted`
- [x] 5.9 Exit non-zero with a distinct "store busy" code if the write lock cannot be taken before the busy timeout, writing nothing partial (maps a failed atomic `store::save` to `kBusy`; SQLite's `busy_timeout` already absorbs transient contention)

## 6. Batch mode

- [ ] 6.1 Decide and document the batch input format (open question in design.md); implement reading from a file or `-` for stdin
- [ ] 6.2 Apply all operations against one loaded forest and a single save
- [ ] 6.3 Resolve references to tasks created earlier in the same batch
- [ ] 6.4 Validate the whole batch before writing; on any invalid operation exit non-zero naming its position and write nothing
- [ ] 6.5 Report created ids in operation order under `--json`
- [ ] 6.6 Test that a 20-operation batch produces exactly one store write

## 7. CLI tests

- [x] 7.1 Add `tests/cli_tests.cpp` and register it with CTest; drive commands against a temp store via `XDG_DATA_HOME`
- [x] 7.2 Round-trip coverage for every command: mutate, reload, assert
- [x] 7.3 **Concurrent-writer test**: load, let a second writer commit an added task, then save, and assert that task survives — the claim design decision 2 rests on
- [x] 7.4 Cover the safety gates: newer schema refused, unreadable store not quarantined, JSON store not migrated
- [x] 7.5 Assert stdout stays parseable on failure (diagnostics to stderr) and that exit codes distinguish the failure classes

## 8. Verification against the live app

- [ ] 8.1 `ctest` green for all suites, on macOS and Linux  (green on Linux; macOS not yet run)
- [ ] 8.2 With the app running: `tt add` and confirm the node appears in the overlay within ~1 s (`kStorePollInterval`) with no restart
- [ ] 8.3 Confirm `/tmp/tasktree.log` prints `Store: another writer changed … — reloaded (N tasks)` for that write
- [ ] 8.4 Confirm a 20-operation `batch` triggers exactly one reload, not twenty (design decision 6)
- [ ] 8.5 Confirm a `tt` write during an active drag or in-bar edit is deferred by the app, not lost
- [ ] 8.6 With the app stopped: the full command surface works, then start the app and confirm it loads the changes
- [ ] 8.7 Confirm the prod preset builds `tt` and that a stripped `tt` runs

## 9. Docs and the ttd loop

- [x] 9.1 `README.md`: a CLI section — commands, `--json`, exit codes, and that it works with the app running or stopped
- [x] 9.2 `docs/OVERVIEW.md`: add `src/cli/` to the layer map and note that `tt` is the client the store work was built for
- [x] 9.3 Replace `scripts/ttd-scan.sh` with a wrapper over `tt tree --json`, keeping the `SessionStart` hook's output contract
- [x] 9.4 `CLAUDE.md` step 3: replace the `python3 -c "import sqlite3; …"` recipe and its list of hand-preserved invariants with `tt` commands, keeping the existing "you no longer need to stop the app" guidance and the two caveats (reload clears undo; avoid rows the user is actively editing)
- [ ] 9.5 Document `batch` as the default way for an agent to mutate, explaining that each external commit clears the user's undo stack
