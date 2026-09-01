#!/bin/bash
# SessionStart hook (read-only): surface pending `ttd>` dev tasks so ANY Claude instance
# opened in this folder picks them up per CLAUDE.md — no need to be told. Wired in
# .claude/settings.json. Prints nothing when there's nothing pending.
#
# This is now a THIN WRAPPER over the `tt` CLI: `tt` owns store access (it resolves
# tasks.db-else-tasks.json the same way src/main.cpp does, honouring XDG_DATA_HOME, and a
# `tree` read never writes the store), so there is no hand-written SQL here any more. All
# this script does is walk the JSON `tt tree` prints. A task is off the canvas when any
# task on its parent chain — itself included — is done (`done_at != 0`), so we simply do
# not descend into a done subtree. A broken hook must never break session start: every
# failure path exits 0.

# The store the app would use — only its basename is needed, for the header line; `tt`
# resolves the actual path itself. Nothing pending if there is no store at all.
data="${XDG_DATA_HOME:-$HOME/.local/share}/tasktree"
if   [ -f "$data/tasks.db" ];   then store="tasks.db"
elif [ -f "$data/tasks.json" ]; then store="tasks.json"
else exit 0
fi

# Find the tt binary: a dev or prod build in this repo, else one on PATH. Not built yet
# -> print nothing (build it — `cmake --build build` — to enable the scan).
root="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
tt=""
for c in "$root/build/tt" "$root/build-prod/tt"; do
    [ -x "$c" ] && tt="$c" && break
done
[ -n "$tt" ] || tt="$(command -v tt 2>/dev/null)"
[ -n "$tt" ] || exit 0
command -v python3 >/dev/null 2>&1 || exit 0

# `tt tree` is a read (never writes / leaves a sidecar). The JSON is passed to python in an
# env var, NOT on stdin: `python3 -` would read its own SCRIPT from stdin, so the here-doc
# and a piped tree would collide.
tree_json="$("$tt" tree --json 2>/dev/null)"
[ -n "$tree_json" ] || exit 0

TT_TREE_JSON="$tree_json" python3 - "$store" <<'PY'
import json, os, sys

store = sys.argv[1]
try:
    forest = json.loads(os.environ["TT_TREE_JSON"])   # a list of root nodes, children nested
except Exception:
    sys.exit(0)                        # tt printed nothing / errored -> nothing pending

pending = []
def walk(node):
    if node.get("done_at"):            # done: this node and its whole subtree are off-canvas
        return
    text = node.get("text") or ""
    if text.lstrip()[:4].lower() == "ttd>":
        pending.append((node.get("id"), text))
    for child in node.get("children", []):
        walk(child)

try:
    for r in forest:
        walk(r)
    pending.sort(key=lambda p: p[0])
    if pending:
        print("Pending TaskTree dev tasks (store: %s) (per CLAUDE.md, pick these up "
              "proactively — do the clearly-scoped ones, ask on the ambiguous ones, "
              "file to 'ttd ✓ done'):" % store)
        for tid, text in pending:
            print('  [%s] %s' % (tid, text))
except Exception:
    sys.exit(0)   # a broken hook must never break session start
PY
