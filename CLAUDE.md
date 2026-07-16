# CLAUDE.md — repo guide

## ttd dev workflow — run this at the START of every session

The user develops TaskTree *through* their running TaskTree app: they plan in the task
tree and mark dev work with a `ttd>` prefix. A read-only `SessionStart` hook
(`scripts/ttd-scan.sh`, wired in `.claude/settings.json`) prints the pending `ttd>`
tasks into your context at startup — so this works from **any** Claude instance in this
folder. When you start a session here, **proactively** pick up that work — don't wait to
be asked:

1. Read `~/.local/share/tasktree/tasks.json`. Collect **canvas** (non-DONE) tasks whose
   text starts with `ttd>` (case-insensitive, optional space). These are dev tasks for you.
2. If there are any clearly-scoped ones, **start implementing them now**. For each:
   implement → build (`cmake --build build -j`) → `ctest` → commit (one focused commit
   per task; **never push**).
3. File each completed task into the DONE section under a done-root titled **`ttd ✓ done`**
   (create it if missing). Because the running app owns `tasks.json`, edit it safely:
   `~/.init-scripts/tasktree.sh stop` → re-read the CURRENT file → apply the edit →
   `~/.init-scripts/tasktree.sh start`. Preserve every other task exactly (ids, order,
   parents, status, roots/doneRoots).
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

## Layout of the code
`src/model` (Task/Forest + JSON store), `src/layout` (pure tidy-tree engine),
`src/render` (NanoVG), `src/ui` (text input, drag), `src/platform` (X11 window + hotkeys
behind `IPlatform`), `src/llm` (pluggable classifier), `src/app` (state machine, config).
Roadmap in `docs/FUTURE.md`; iterative-plan protocol in `docs/AGENTS.md`.
