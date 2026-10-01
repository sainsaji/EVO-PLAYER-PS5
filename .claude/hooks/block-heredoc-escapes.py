#!/usr/bin/env python3
"""PreToolUse hook for Bash.

Blocks a heredoc whose body carries backslash escapes (\\t \\n \\r \\\\), and any
heredoc fed straight into python. Through Git Bash and the tool layer those
escapes get turned into real tabs/newlines (or the whole command dies with
"unexpected EOF"), which silently corrupts the code being patched. Plain
heredocs with no backslashes (git commit messages, etc.) pass untouched.

Exit 2 = block; stderr goes back to Claude.
"""
import json
import re
import sys

MSG = (
    "Blocked: this Bash command has a heredoc with backslash escapes (or feeds a "
    "heredoc into python). Git Bash + the tool layer mangle those: \\t \\n \\r become "
    "real characters and quotes can kill the command with 'unexpected EOF'.\n"
    "Do this instead: create the script with the Write tool (a .py file in the "
    "scratchpad directory), then run it with `python3 -X utf8 <path>`. For small "
    "edits use the Edit tool directly. If a script must emit a backslash, build it "
    "with chr(92)."
)

HEREDOC = re.compile(r"<<-?\s*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\1")


def main() -> int:
    try:
        data = json.load(sys.stdin)
    except Exception:
        return 0
    cmd = (data.get("tool_input") or {}).get("command") or ""
    m = HEREDOC.search(cmd)
    if not m:
        return 0

    head = cmd[: m.start()]
    last_line = head.rsplit("\n", 1)[-1]
    into_python = re.search(r"\bpython3?\b", last_line) is not None

    nl = cmd.find("\n", m.end())
    body = cmd[nl + 1 :] if nl != -1 else ""
    has_escape = re.search(r"\\[tnr\\]", body) is not None

    if into_python or has_escape:
        sys.stderr.write(MSG + "\n")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
