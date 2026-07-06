# AGENTS.md — Iterative Development Loop for TaskTree

This file instructs any AI agent (or human) picking up TaskTree how to move the
project forward **one reviewable version at a time**. The core idea: never leave the
repo without a plan for what comes next. As soon as a version is built and reviewed,
the next version's plan already exists, ready to execute.

## The loop

```
        ┌──────────────────────────────────────────────┐
        │  1. EXECUTE the current plan (docs/plans/vN.md)│
        │  2. VERIFY it (build + tests + manual checks)  │
        │  3. RECORD what shipped (docs/plans/vN.md ✓)   │
        │  4. DRAFT the next plan (docs/plans/vN+1.md)    │
        │  5. STOP for human review of vN+1               │
        └───────────────┬──────────────────────────────┘
                        │ on approval
                        └────────────► back to step 1 for vN+1
```

At any moment the repo contains: the code for the latest **completed** version, and a
**drafted, not-yet-executed** plan for the next one. The human's only gate is step 5.

## Conventions

- **Version plans** live in `docs/plans/vN.md` (this project's POC is version 1; its
  plan is the approved root plan). Each plan has: Goal, Scope (in/out), Files touched,
  Verification, and a "Shipped" checklist filled in at step 3.
- **The backlog** is `docs/FUTURE.md`. The next plan is assembled by pulling the
  highest-value items from it plus anything raised in the last review.
- **The build must stay green.** `cmake --build build && ctest --test-dir build` must
  pass before a version is considered shipped. The pure layers (`tt_core`, `tt_io`)
  have dependency-free tests — extend them with every model/layout change.
- **Keep the seams.** New capabilities go behind the existing interfaces
  (`IPlatform`, `IClassifier`) and the pure modules (`TidyLayout`, `Forest`). Don't
  couple rendering into the model or layout — that is what keeps the alt-stack
  branches (`plan/qt6-fallback`, `plan/imgui-alt`) viable.

## Step-by-step for the next agent

1. **Read** the approved root plan, this file, `docs/FUTURE.md`, and the most recent
   `docs/plans/vN.md`. Confirm the build and tests pass on a clean checkout.
2. **Execute** the current uncompleted plan if one exists. Implement, keeping commits
   focused. Prefer extending pure/testable code over adding logic into GL/X11 paths.
3. **Verify** end-to-end (see the root plan's Verification section: run the app, press
   the hotkeys, add/drag/persist) and run `ctest`. Record results honestly — if a
   step was skipped or a test fails, say so.
4. **Record** the outcome in `docs/plans/vN.md` under "Shipped": what landed, what was
   cut and why, any new risks.
5. **Draft** `docs/plans/vN+1.md`:
   - Pick a coherent slice (2–5 related items) from `docs/FUTURE.md` + review notes.
   - Prefer slices that de-risk something (e.g. animation before richer LLM UI) or
     unlock several future items at once.
   - Specify files to touch, the approach, and concrete verification steps.
   - Note anything that would push toward the **Qt6 fallback** (e.g. text-editing/IME
     needs) or that is cheaper on the **ImGui** branch — cross-reference those plans.
6. **Stop** and present `vN+1.md` for human review. Do not start executing it until it
   is approved.

## Picking the next slice — heuristics

- **Value × confidence.** Ship the thing users feel most, that you're most sure how to
  build. For a task-organizer overlay, motion/feedback (auto-align animation, drag
  polish) and node editing usually beat back-end cleverness.
- **De-risk early.** Anything touching the compositor (Wayland, multi-monitor) or the
  LLM UX should get a small spike before a full slice.
- **One theme per version.** A version is easier to review if it has a single story
  ("v2: it moves" / "v3: you can edit and collapse" / "v4: links + browser").
- **Never silently drop scope.** If a slice turns out too big, cut it explicitly in
  the plan and move the remainder to `FUTURE.md`.

## Suggested first few versions (starting point, not binding)

- **v2 — "It moves":** auto-align tween animation on relayout + drag ghost/snap-back
  polish + insertion caret. Pure win on the "no need to remember positions" goal.
- **v3 — "Edit & shape":** in-place node text editing, collapse/expand subtrees,
  delete-subtree, undo/redo.
- **v4 — "Links & reach":** URL highlighting + `xdg-open` redirect; true superellipse.
- **v5 — "Smart intake":** enable + refine the local LLM classifier UX (candidate
  suggestions the user confirms) with embeddings-based prefiltering.
- **later — "Everywhere":** Wayland `PlatformWayland`, multi-monitor, tray fallback.
