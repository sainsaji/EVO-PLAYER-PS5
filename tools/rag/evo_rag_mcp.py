#!/usr/bin/env python3
"""evo_rag_mcp.py - MCP (stdio) server exposing evo_rag to Claude Code.

Tools:
  evo_search       search code, docs, GitHub issues and memory notes
  evo_rag_status   what is indexed, and whether embeddings are on

Registered in .mcp.json. No third-party MCP package: the protocol is newline-
delimited JSON-RPC 2.0 on stdin/stdout, and only initialize, ping, tools/list and
tools/call are needed. Everything diagnostic goes to stderr (stdout is the wire).

The index is kept fresh without hooks: every search first checks file mtimes
(at most once every 3 s) and re-chunks what changed. A background thread does the
first sync, the embedding backlog and the GitHub issue refresh (every 6 h), so a
cold start answers keyword-only straight away and gets sharper as vectors land.
"""
import json
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import evo_rag  # noqa: E402

PROTOCOL_DEFAULT = "2025-06-18"

INSTRUCTIONS = (
    "Search EVO Player's own code, docs, GitHub issues and project memory notes. "
    "Use evo_search BEFORE grepping broadly or re-deriving a hardware rule: ask in plain "
    "words ('why must we never deploy over a running EVO') or by name ('slot_renew_decoder'). "
    "Results give path:lines - Read that range for the full text.")

TOOLS = [
    {
        "name": "evo_search",
        "description": (
            "Hybrid keyword + semantic search over EVO Player: source code, docs/*.md, "
            "every GitHub issue (open and closed, with comments) and the project memory "
            "notes. Returns the best chunks as path:lines with a snippet. Good for 'where "
            "is X handled', 'why was Y done this way', 'what did we learn about Z on the "
            "console', and for finding the issue or doc that covers a feature."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string",
                          "description": "A question or identifiers. Plain words and exact names both work."},
                "k": {"type": "integer", "description": "Results to return (default 8, max 20)."},
                "source": {"type": "string",
                           "description": "all (default), or comma list of: code, docs, issues, memory."},
                "path": {"type": "string",
                         "description": "Only paths starting with this, e.g. 'projects/evoplayer/media/'."},
            },
            "required": ["query"],
        },
    },
    {
        "name": "evo_rag_status",
        "description": "How many files/chunks are indexed per source, whether embeddings are on, "
                       "and when issues were last synced.",
        "inputSchema": {"type": "object", "properties": {}},
    },
]

_idx = None
_bg = threading.Event()          # set while the background sync is running


def log(msg):
    print(f"[evo-rag] {msg}", file=sys.stderr, flush=True)


def index():
    global _idx
    if _idx is None:
        _idx = evo_rag.Index()
    return _idx


def background_sync():
    _bg.set()
    try:
        idx = index()
        idx.sync_files(force=True)
        idx.sync_issues()
        n = idx.embed_pending()
        log(f"background sync done, {n} chunks embedded; {idx.status()}")
    except Exception as e:                              # never kill the server
        log(f"background sync failed: {e!r}")
    finally:
        _bg.clear()


def call_tool(name, args):
    idx = index()
    if name == "evo_search":
        if not _bg.is_set():
            idx.sync_files()                            # cheap, throttled
        k = max(1, min(int(args.get("k") or 8), 20))
        hits = idx.search(args.get("query", ""), k=k, source=args.get("source") or "all",
                          path_prefix=args.get("path") or "")
        text = evo_rag.format_hits(hits)
        st = idx.status()
        notes = []
        if _bg.is_set():
            notes.append("index is still being built; results may be incomplete")
        if st["embedded"] < st["chunks"]:
            notes.append(f"{st['chunks'] - st['embedded']} chunks not embedded yet (keyword-only for those)")
        if st["dense_error"]:
            notes.append(f"semantic search off: {st['dense_error']}")
        return text + ("\n\n(" + "; ".join(notes) + ")" if notes else "")
    if name == "evo_rag_status":
        return json.dumps(idx.status(), indent=1) + ("\nbackground sync running" if _bg.is_set() else "")
    raise ValueError(f"unknown tool {name}")


def handle(msg):
    method = msg.get("method")
    params = msg.get("params") or {}
    if method == "initialize":
        return {"protocolVersion": params.get("protocolVersion", PROTOCOL_DEFAULT),
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "evo-rag", "version": "1.0"},
                "instructions": INSTRUCTIONS}
    if method == "ping":
        return {}
    if method == "tools/list":
        return {"tools": TOOLS}
    if method == "tools/call":
        try:
            text = call_tool(params.get("name"), params.get("arguments") or {})
            return {"content": [{"type": "text", "text": text}]}
        except Exception as e:
            return {"content": [{"type": "text", "text": f"error: {e}"}], "isError": True}
    raise KeyError(method)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stdin.reconfigure(encoding="utf-8")
    threading.Thread(target=background_sync, daemon=True).start()
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        if "id" not in msg:                             # a notification: no reply
            continue
        try:
            reply = {"jsonrpc": "2.0", "id": msg["id"], "result": handle(msg)}
        except KeyError:
            reply = {"jsonrpc": "2.0", "id": msg["id"],
                     "error": {"code": -32601, "message": f"method not found: {msg.get('method')}"}}
        except Exception as e:
            reply = {"jsonrpc": "2.0", "id": msg["id"],
                     "error": {"code": -32603, "message": str(e)}}
        sys.stdout.write(json.dumps(reply) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
