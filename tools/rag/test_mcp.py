#!/usr/bin/env python3
"""Drive evo_rag_mcp.py over stdio the way Claude Code does, and check the replies.

  python tools/rag/test_mcp.py
"""
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
proc = subprocess.Popen([sys.executable, str(HERE / "evo_rag_mcp.py")],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                        text=True, encoding="utf-8", cwd=HERE.parents[1])


def rpc(i, method, params=None):
    proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": i, "method": method,
                                 "params": params or {}}) + "\n")
    proc.stdin.flush()
    return json.loads(proc.stdout.readline())


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        proc.kill()
        sys.exit(1)


r = rpc(1, "initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                          "clientInfo": {"name": "test", "version": "0"}})
check(r["result"]["serverInfo"]["name"] == "evo-rag", "initialize")
proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
proc.stdin.flush()
r = rpc(2, "tools/list")
check({t["name"] for t in r["result"]["tools"]} == {"evo_search", "evo_rag_status"}, "tools/list")
r = rpc(3, "tools/call", {"name": "evo_search",
                          "arguments": {"query": "why must we never call sceVideoOutOpen", "k": 3}})
text = r["result"]["content"][0]["text"]
check(not r["result"].get("isError") and "1. " in text, "evo_search returns hits")
print(text[:900])
r = rpc(4, "tools/call", {"name": "evo_search",
                          "arguments": {"query": "slot_renew_decoder", "source": "code", "k": 2}})
check("evo_vdec_native.c" in r["result"]["content"][0]["text"], "exact name finds the code")
r = rpc(5, "tools/call", {"name": "nope", "arguments": {}})
check(r["result"].get("isError") is True, "unknown tool is an error, not a crash")
r = rpc(6, "nosuchmethod")
check("error" in r, "unknown method answers with an error")
proc.stdin.close()
proc.wait(timeout=10)
print("all good")
