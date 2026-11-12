# Code graph (graphify)

[graphify](https://github.com/Graphify-Labs/graphify) parses EVO's own source with
tree-sitter and writes a call/include graph to `graphify-out/graph.json`. It runs on
the host, offline, with no model calls. The graph is not committed; each machine
builds its own.

It complements `evo_search` ([tools/rag/](../../tools/rag/README.md)):

| Question | Tool |
|---|---|
| Why is it this way, which doc or issue covers it | `evo_search` |
| Who calls this, what does it call, what breaks if I change it | `graphify` |
| An exact string | Grep |

## Setup (once per machine)

```bash
uv tool install graphifyy          # PyPI name has two y's; installs graphify + graphify-mcp
graphify extract . --code-only     # ~40 s, writes graphify-out/
graphify cluster-only . --no-label # optional: GRAPH_REPORT.md + graph.html, no LLM
graphify hook install              # optional: refresh the graph after every commit/checkout
```

The `/graphify` skill (`.claude/skills/graphify/`) and its Claude Code hooks
(`graphify hook-guard` in `.claude/settings.json`) are committed. The hooks add a
short "the graph already has ..." hint to Grep/Read calls; they are silent when
`graphify-out/` is missing.

## Daily use

```bash
graphify explain "evo_agc_runtime_present"   # callers + callees with file:line
graphify affected "slot_renew_decoder"       # reverse walk: what depends on it
graphify path "Application" "PlaybackController"
graphify query "where is seek handled" --budget 1500
graphify god-nodes --top 10                  # the hubs
graphify update .                            # after code changes, ~15 s
```

Exit codes: 0 answer, 3 ambiguous name (candidates listed), 4 no match.

## What is in the graph

`.graphifyignore` (plus `.gitignore`) limits it to EVO's own code: about 620 files,
13.6k nodes, 29k edges. Kept out:

- `third_party/`, `build/`, `output/`, local server data and media samples
- vendored libraries: QuickJS, cJSON, `stb_*.h`
- generated files: `shaders/agc/*_pipe.h` blobs, `evo_ytjs_blob.c`,
  `evo_rmlui_bundle_data.cpp`, `SDL_ps5tilemap.inc`
- `main.c.legacy`

About 30 headers parse with errors (PS5 SDK macros tree-sitter cannot expand), so a
few symbols are missing. Edges marked `INFERRED` are resolved by name, not by the
compiler; check them in the source before relying on one.

Docs, images and the skill's semantic pass are skipped on purpose (`--code-only`):
that pass spends model tokens, and `evo_search` already indexes the docs.
