#!/usr/bin/env python3
"""tools/stage_hunks.py FILE REGEX [--invert]

Stage only the -U0 hunks of FILE (index -> worktree) whose changed lines match
REGEX (or do not, with --invert). For splitting one file between two commits
by topic (the commit-evo skill), without an interactive `git add -p`.
Builds the new blob itself, so zero-context hunks are fine where `git apply
--unidiff-zero` refuses them. Check the result with `git diff --cached FILE`.

  python3 tools/stage_hunks.py core/src/x.cpp 'makeSection\\("[A-Z][a-z]' --invert
"""
import re
import subprocess
import sys

path, rx = sys.argv[1], re.compile(sys.argv[2])
invert = "--invert" in sys.argv


def git(*args, inp=None):
    return subprocess.run(["git", *args], input=inp, capture_output=True, check=True).stdout


diff = git("diff", "-U0", "--no-color", "--", path).decode("utf-8")
hunks = []
cur = None
for ln in diff.splitlines(keepends=True):
    m = re.match(r"@@ -(\d+)(?:,(\d+))? \+\d+(?:,\d+)? @@", ln)
    if m:
        start, n = int(m.group(1)), int(m.group(2) if m.group(2) is not None else 1)
        cur = {"start": start, "n": n, "add": [], "body": ""}
        hunks.append(cur)
    elif cur is not None and ln[:1] in "+-" and not ln.startswith(("+++", "---")):
        cur["body"] += ln[1:]
        if ln[0] == "+":
            cur["add"].append(ln[1:])
    elif cur is not None and ln.startswith("\\"):
        pass

old = git("show", f":{path}").decode("utf-8").splitlines(keepends=True)
crlf = old and old[0].endswith("\r\n")
keep = [h for h in hunks if bool(rx.search(h["body"])) != invert]
# Apply bottom-up on old-file line numbers so earlier offsets stay valid.
for h in sorted(keep, key=lambda h: h["start"], reverse=True):
    add = [a.replace("\n", "\r\n") if crlf and not a.endswith("\r\n") else a for a in h["add"]]
    if h["n"] == 0:          # pure insertion after line `start`
        old[h["start"]:h["start"]] = add
    else:
        old[h["start"] - 1:h["start"] - 1 + h["n"]] = add
sha = git("hash-object", "-w", "--stdin", inp="".join(old).encode("utf-8")).decode().strip()
mode = git("ls-files", "-s", "--", path).decode().split()[0]
git("update-index", "--cacheinfo", f"{mode},{sha},{path}")
print(f"{path}: staged {len(keep)} of {len(hunks)} hunks")
