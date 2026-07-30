#!/bin/bash
# SessionStart hook (read-only): surface pending `ttd>` dev tasks so ANY Claude
# instance opened in this folder picks them up per CLAUDE.md — no need to be told.
# Wired in .claude/settings.json. Prints nothing when there's nothing pending.
#
# Store-agnostic: reads tasks.db when it exists, else tasks.json — the same rule
# src/main.cpp uses, so the hook follows the app rather than guessing. Read-only in
# the strong sense: it never writes the store and never leaves a file behind (see
# from_db for how; SQLite would otherwise create -shm/-wal sidecars just to read).
d="$HOME/.local/share/tasktree"
[ -f "$d/tasks.db" ] || [ -f "$d/tasks.json" ] || exit 0
command -v python3 >/dev/null 2>&1 || exit 0
python3 - "$d/tasks.db" "$d/tasks.json" <<'PY'
import json, os, shutil, sqlite3, sys, tempfile

db, js = sys.argv[1], sys.argv[2]


def descendants(children, roots):
    """Every id at or under `roots` — i.e. the whole DONE section."""
    seen, stack = set(), list(roots)
    while stack:
        i = stack.pop()
        if i in seen:
            continue
        seen.add(i)
        stack.extend(children.get(i, []))
    return seen


def query(uri):
    con = sqlite3.connect(uri, uri=True, timeout=2)
    try:
        return con.execute("SELECT id,parent,text,done FROM tasks").fetchall()
    finally:
        con.close()


def from_db(path):
    # A -wal file means something has the store open (or closed it uncleanly), so read
    # with real locking: mode=ro, whose -shm the live app already created. With no -wal
    # nothing is mid-write, and immutable=1 reads without creating any sidecar at all —
    # a plain mode=ro would drop a -shm + -wal next to an idle store.
    try:
        rows = query(("file:%s?mode=ro" if os.path.exists(path + "-wal")
                      else "file:%s?immutable=1") % path)
    except sqlite3.Error:
        # A WAL database whose -shm is missing cannot be opened read-only. Read a copy
        # (read-write, so SQLite may replay the WAL) rather than touch the live store.
        tmp = tempfile.mkdtemp()
        try:
            for suffix in ("", "-wal", "-shm"):
                if os.path.exists(path + suffix):
                    shutil.copy2(path + suffix, os.path.join(tmp, "c.db" + suffix))
            rows = query("file:%s" % os.path.join(tmp, "c.db"))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    # roots/doneRoots/children are derived in the DB: parent 0 + done = a DONE root.
    tasks, children, done_roots = [], {}, []
    for tid, parent, text, done in rows:
        tasks.append({"id": tid, "text": text or ""})
        if parent:
            children.setdefault(parent, []).append(tid)
        elif done:
            done_roots.append(tid)
    return tasks, descendants(children, done_roots)


def from_json(path):
    with open(path) as fh:
        d = json.load(fh)
    tasks = d.get("tasks", [])
    children = {t["id"]: list(t.get("children", [])) for t in tasks}
    return tasks, descendants(children, d.get("doneRoots", []))


try:
    store = db if os.path.exists(db) else js
    tasks, done = (from_db(db) if store == db else from_json(js))
    pending = [t for t in sorted(tasks, key=lambda t: t["id"])
               if t["id"] not in done and t.get("text", "").lstrip()[:4].lower() == "ttd>"]
    if pending:
        print("Pending TaskTree dev tasks (store: %s) (per CLAUDE.md, pick these up "
              "proactively — do the clearly-scoped ones, ask on the ambiguous ones, "
              "file to 'ttd ✓ done'):" % os.path.basename(store))
        for t in pending:
            print('  [%s] %s' % (t["id"], t["text"]))
except Exception:
    sys.exit(0)   # a broken hook must never break session start
PY
