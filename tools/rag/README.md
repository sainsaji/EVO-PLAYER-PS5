# EVO search (RAG)

Local search over EVO's code, docs, GitHub issues and Claude memory notes.
Claude Code uses it through the `evo_search` tool; you can use it from a shell too.

```bash
pip install fastembed          # once; without it search is keyword-only
python tools/rag/evo_rag.py index                          # build / update
python tools/rag/evo_rag.py search "why not sceVideoOutOpen" -k 6
python tools/rag/evo_rag.py search "seek" --source code --path projects/evoplayer/media/
python tools/rag/evo_rag.py status
python tools/rag/evo_rag.py eval                           # hit@5 on tools/rag/eval_queries.json
```

## How it works

- `evo_rag.py` chunks every file (Markdown by heading, code by blank line / closing
  brace, ~1500 chars), every issue and comment, and each memory note.
- Each chunk is in an SQLite FTS5 keyword index (BM25, camelCase and snake_case
  names also split into words) and, with `fastembed`, a 384-d `bge-small-en-v1.5`
  vector. A query runs both and fuses the ranks (reciprocal rank fusion). A literal
  identifier in the query gets a small extra boost.
- The index is `output/rag/index.sqlite` (git-ignored, safe to delete). The first
  build embeds a few thousand chunks on the CPU, which takes minutes; after that only
  changed files are redone.
- `evo_rag_mcp.py` is the MCP server (`.mcp.json`). Each search first checks file
  modification times, so edits show up without any hook. Issues refresh every 6 hours;
  `python tools/rag/evo_rag.py index` forces it.

## What is left out

`third_party/`, generated shader pipe tables, the embedded asset bundle,
`main.c.legacy` (CLAUDE.md: do not learn from it), files over 300 KB, binaries.

## Changing things

- Another embedding model: `EVO_RAG_MODEL=<fastembed model name>`; the index re-embeds.
- Memory notes are found from the project path; override with `EVO_RAG_MEMORY=<dir>`.
- Add cases to `eval_queries.json` when a search misses something it should find.
