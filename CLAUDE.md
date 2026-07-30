# CLAUDE.md — repo guide

## Default branch: `v2`

Active development happens on **`v2`**, checked out in this folder (the main repo
checkout, `Out/TaskTree`). At session start, check you're on `v2` and switch to it if
not — don't start work on `main` or `v1`. `v1` is the maintenance line; `main` only
tracks whatever the current line has published. Commit ttd work to `v2`; **never push**.

## ttd dev workflow — run this at the START of every session

The user develops TaskTree *through* their running TaskTree app: they plan in the task
tree and mark dev work with a `ttd>` prefix. A read-only `SessionStart` hook
(`scripts/ttd-scan.sh`, wired in `.claude/settings.json`) prints the pending `ttd>`
tasks into your context at startup — so this works from **any** Claude instance in this
folder. When you start a session here, **proactively** pick up that work — don't wait to
be asked:

1. Read the store in `~/.local/share/tasktree/`: **`tasks.db` if it exists, otherwise
   `tasks.json`** — the DB wins once created, which is the rule `src/main.cpp` and
   `scripts/ttd-scan.sh` both use. Collect **canvas** (non-DONE) tasks whose text starts
   with `ttd>` (case-insensitive, optional space). These are dev tasks for you.
   In the DB there are no `roots`/`doneRoots`/`children` lists: a task is top-level when
   `parent = 0`, a DONE root when `parent = 0 AND done = 1`, and the DONE section is that
   row plus its descendants. `ord` is the position among siblings.
2. If there are any clearly-scoped ones, **start implementing them now**. For each:
   implement → build (`cmake --build build -j`) → `ctest` → commit (one focused commit
   per task; **never push**).
3. File each completed task into the DONE section under a done-root titled **`ttd ✓ done`**
   (create it if missing). Edit safely: `~/.init-scripts/tasktree.sh stop` → re-read the
   CURRENT store → apply the edit → `~/.init-scripts/tasktree.sh start`. **Still stop the
   app first**, but for a narrower reason than before: the DB save is now incremental, so
   it no longer wholesale-overwrites and a row you insert while it runs will survive. What
   it does not yet do is *notice* — the app holds a stale in-memory copy, so your change is
   invisible until restart, and if the app later edits that same task it writes its own
   version over yours. Preserve every other task exactly (ids, order, parents, status, done
   state); for the DB that means `ord` and `parent` too, and `meta.next_id` must not go
   backwards. There is no `sqlite3` CLI on this machine — use `python3 -c "import sqlite3; …"`.
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
  individual edit. Restart is safe: tasks persist in `tasks.json` across it.

## Layout of the code
`src/model` (Task/Forest + JSON store), `src/layout` (pure tidy-tree engine),
`src/render` (NanoVG), `src/ui` (text input, drag), `src/platform` (X11 window + hotkeys
behind `IPlatform`), `src/llm` (pluggable classifier), `src/app` (state machine, config).
Roadmap in `docs/FUTURE.md`; iterative-plan protocol in `docs/AGENTS.md`.
