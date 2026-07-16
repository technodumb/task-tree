#!/bin/bash
# SessionStart hook (read-only): surface pending `ttd>` dev tasks so ANY Claude
# instance opened in this folder picks them up per CLAUDE.md — no need to be told.
# Wired in .claude/settings.json. Prints nothing when there's nothing pending.
f="$HOME/.local/share/tasktree/tasks.json"
[ -f "$f" ] || exit 0
command -v python3 >/dev/null 2>&1 || exit 0
python3 - "$f" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
nodes = {t["id"]: t for t in d.get("tasks", [])}
done = set()
def mark(i):
    done.add(i)
    for c in nodes.get(i, {}).get("children", []): mark(c)
for r in d.get("doneRoots", []): mark(r)
ttd = [t for t in d.get("tasks", [])
       if t["id"] not in done and t.get("text", "").lstrip()[:4].lower() == "ttd>"]
if ttd:
    print("Pending TaskTree dev tasks (per CLAUDE.md, pick these up proactively — do the "
          "clearly-scoped ones, ask on the ambiguous ones, file to 'ttd ✓ done'):")
    for t in ttd:
        print(f'  [{t["id"]}] {t["text"]}')
PY
