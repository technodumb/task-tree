## ADDED Requirements

### Requirement: Store resolution without side effects
The `tt` executable SHALL resolve the store with the same rule the app uses — `tasks.db`
when it exists, otherwise `tasks.json` — honouring `XDG_DATA_HOME`. It SHALL NOT create a
store, and it SHALL NOT run the JSON→SQLite migration: that first-run migration is a
one-way door and belongs to the app alone.

#### Scenario: SQLite store present
- **WHEN** `tasks.db` exists in the data directory
- **THEN** `tt` operates on `tasks.db` and does not read `tasks.json`

#### Scenario: Only a pre-migration JSON store present
- **WHEN** `tasks.json` exists and `tasks.db` does not
- **THEN** `tt` operates on `tasks.json`
- **AND** no `tasks.db` is created and no migration is performed

#### Scenario: No store at all
- **WHEN** neither `tasks.db` nor `tasks.json` exists
- **THEN** `tt` exits non-zero with a message naming the expected path
- **AND** creates no file

#### Scenario: Store path overridden for testing
- **WHEN** `--store <path>` is given, or `XDG_DATA_HOME` is set
- **THEN** `tt` uses that location instead of the default

### Requirement: Refuse a store this build cannot safely write
`tt` SHALL compare a SQLite store's schema version against
`store::supportedDbSchemaVersion()` before any write, and refuse to touch a newer one.
It SHALL NOT call `store::quarantine()` under any circumstance — moving a damaged store
aside is the app's recovery path, and a CLI doing it silently would surprise the user.

#### Scenario: Store written by a newer build
- **WHEN** the store's schema version exceeds this build's supported version
- **THEN** `tt` exits non-zero naming both versions
- **AND** no byte of the store is written, including journal-mode pragmas

#### Scenario: Store exists but cannot be read
- **WHEN** the store file exists and fails to load
- **THEN** `tt` exits non-zero advising that the app be run to recover it
- **AND** the store is neither written nor renamed

### Requirement: Node addressing by id or text
Commands taking a node SHALL accept either a numeric task id or a text query. A text query
SHALL be resolved with `palette::rankMatches`, so the CLI picks the same node the in-app
palette would. An ambiguous or unmatched query SHALL be an error rather than a guess.

#### Scenario: Addressing by id
- **WHEN** a numeric id for a live task is given
- **THEN** that task is used

#### Scenario: Addressing by unique text match
- **WHEN** a query matches exactly one live task
- **THEN** that task is used

#### Scenario: Ambiguous text match
- **WHEN** a query matches more than one live task
- **THEN** `tt` exits non-zero and lists the candidate ids and texts
- **AND** the store is not modified

#### Scenario: Unknown id or no match
- **WHEN** the id does not exist, names a deleted task, or the query matches nothing
- **THEN** `tt` exits non-zero and the store is not modified

### Requirement: Read the tree
`tt tree` SHALL print the forest, and SHALL accept an optional node to print a subtree.
Output SHALL include each task's id, text, parent, sibling order, `status`, `collapsed`,
`created_at`, and `done_at`. Done tasks SHALL be included and marked, because in v3 a done
task keeps its parent and sibling slot rather than moving.

#### Scenario: Whole forest as JSON
- **WHEN** `tt tree --json` runs
- **THEN** stdout is a single JSON document containing every live task with those fields
- **AND** the exit code is 0

#### Scenario: Subtree only
- **WHEN** `tt tree <id|query>` runs
- **THEN** only that task and its descendants are printed

#### Scenario: Human-readable default
- **WHEN** `tt tree` runs without `--json`
- **THEN** an indented outline is printed with ids, one task per line

#### Scenario: Empty store
- **WHEN** the store loads but holds no tasks
- **THEN** an empty tree is reported with exit code 0, not an error

### Requirement: Inspect a node and search
`tt show` SHALL print one task with its children. `tt find` SHALL print matches for a
query in `rankMatches` order. `tt deleted` SHALL list retired rows via
`store::deletedRows()`, most recently deleted first.

#### Scenario: Show a node
- **WHEN** `tt show <id|query>` runs
- **THEN** that task's fields and its immediate children are printed

#### Scenario: Ranked search
- **WHEN** `tt find <query>` runs
- **THEN** matching tasks are printed best-first in `rankMatches` order
- **AND** exit code 0 even when nothing matches

#### Scenario: Reading the trash
- **WHEN** `tt deleted --json` runs against a SQLite store
- **THEN** each retired row's id, parent, text, and `deleted_at` are printed

#### Scenario: Trash on a JSON store
- **WHEN** `tt deleted` runs against a JSON store, which keeps no retired rows
- **THEN** an empty list is reported with exit code 0

### Requirement: Mutate the tree
`tt` SHALL support `add`, `edit`, `done`, `undone`, `status`, `parent`, and `rm`. Every
write SHALL pass the forest as loaded as the `baseline` argument to `store::save`, so
absences are interpreted as this process's deletions and rows it never saw are left alone.
`rm` SHALL be a soft delete, leaving the row with `deleted_at` stamped.

#### Scenario: Adding a task reports its id
- **WHEN** `tt add "text"` succeeds
- **THEN** the new task's id is printed on stdout
- **AND** the task exists in the store on the next load

#### Scenario: Adding under a parent
- **WHEN** `tt add "text" --parent <id|query>` succeeds
- **THEN** the new task is the last child of that parent

#### Scenario: A concurrent writer's rows survive
- **WHEN** another writer adds a task after `tt` loaded and before `tt` saved
- **THEN** that task is still present after `tt`'s save, because it is in neither the
  baseline nor `tt`'s forest

#### Scenario: Completing a task keeps its place
- **WHEN** `tt done <id>` runs
- **THEN** `done_at` is set and the task keeps its parent and sibling position

#### Scenario: Reversing completion
- **WHEN** `tt undone <id>` runs on a done task
- **THEN** `done_at` returns to 0 and the task is live on the canvas again

#### Scenario: Setting status
- **WHEN** `tt status <id> <normal|in-progress|priority>` runs
- **THEN** `status` becomes 0, 1, or 2 respectively

#### Scenario: Reparenting
- **WHEN** `tt parent <child> <parent>` runs
- **THEN** the child and its subtree become the last child of the new parent

#### Scenario: Reparenting refused when it would cycle
- **WHEN** the target parent is the child itself or one of its descendants
- **THEN** `tt` exits non-zero and the store is not modified

#### Scenario: Soft delete
- **WHEN** `tt rm <id>` runs
- **THEN** the task and its descendants stop loading into the forest
- **AND** their rows survive with `deleted_at` set, and appear in `tt deleted`

### Requirement: Writes cooperate with a running app
The app holds a `store::Session` and polls `PRAGMA data_version` every second, reloading
when another writer commits. `tt` SHALL be a good citizen of that arrangement: one
transaction per invocation, committed promptly so neither side exhausts the other's 3 s
busy timeout, and no attempt to defeat the app's conflict behaviour.

#### Scenario: A write appears in the live overlay
- **WHEN** `tt add "text"` commits while the app is running
- **THEN** the app's next poll observes the change and the task appears within ~1 second,
  with no restart

#### Scenario: Contended write waits rather than failing
- **WHEN** the app holds the write lock as `tt` tries to commit
- **THEN** `tt` waits within its busy timeout and succeeds, rather than erroring immediately

#### Scenario: Lock genuinely unavailable
- **WHEN** the write lock cannot be taken before the busy timeout expires
- **THEN** `tt` exits non-zero saying the store was busy, and writes nothing partial

#### Scenario: Batch limits the cost imposed on the user
- **WHEN** an agent applies many operations
- **THEN** using `batch` produces one commit, so the app reloads once
- **AND** the user's undo stack — which `App::reloadFromStore` clears on every external
  change — is cleared once rather than once per operation

### Requirement: Batch operations apply atomically
`tt batch` SHALL read a list of operations from a file or stdin and apply them in one
`load → mutate → save` cycle, so N operations cause one write. If any operation is
invalid, nothing SHALL be written.

#### Scenario: Many operations, one write
- **WHEN** a batch of twenty operations is applied
- **THEN** the store is written exactly once
- **AND** every operation's effect is present

#### Scenario: Ids usable within the batch
- **WHEN** a batch adds a task and a later operation in the same batch references it
- **THEN** that reference resolves to the newly created task

#### Scenario: One bad operation aborts the whole batch
- **WHEN** any operation in the batch is invalid
- **THEN** `tt` exits non-zero identifying the failing operation by position
- **AND** the store is byte-for-byte unchanged

#### Scenario: Reporting created ids
- **WHEN** a batch containing adds succeeds with `--json`
- **THEN** the new ids are reported in operation order

### Requirement: Machine-readable output and exit codes
Every command SHALL accept `--json` and emit a single JSON document on stdout with stable
field names. Diagnostics SHALL go to stderr so stdout stays parseable. Exit codes SHALL
distinguish the failures a caller must tell apart.

#### Scenario: Errors keep stdout parseable
- **WHEN** a command fails with `--json`
- **THEN** the error is on stderr and stdout carries either nothing or a JSON error object
- **AND** never a mix of prose and JSON

#### Scenario: Distinguishable exit codes
- **WHEN** a command fails
- **THEN** the exit code distinguishes usage error, node-not-found, ambiguous match,
  store-unreadable, and store-too-new
- **AND** each code is documented in `--help`

#### Scenario: Field names are a contract
- **WHEN** a task is emitted as JSON
- **THEN** its keys are the documented set, including `created_at` and `done_at`, and are
  not renamed without a spec change
