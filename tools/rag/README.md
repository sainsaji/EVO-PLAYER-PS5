# EVO search (RAG)

Local search over EVO's code, docs, GitHub issues and Claude memory notes, plus the
reference repos in `third_party/` and their commit messages. Claude Code uses it through
the `evo_search` tool; you can use it from a shell too.

## Which source for which question

| You want to know | Use |
|---|---|
| why / where / how in EVO | plain search (sources `code`, `docs`, `issues`, `memory`) |
| has a sibling project already fixed this? | `--source commits` |
| how does another PS5 app call this API? | `--source thirdparty` |
| one exact identifier or string | Grep, or a plain search with that name |
| EVO's own history | `git log` / `git blame` |

`commits` and `thirdparty` are left out of a plain search on purpose, so reference code
never buries EVO's own results. Naming a path under `third_party/` (or `commits/`) in
`--path` also turns them on. Hits are hints to port by hand, not code to paste: check the
licence and test on the console. `CLAUDE.md` tells Claude to check these two sources before
designing a fix for a decode, audio, A/V sync, GPU or provider bug.

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
  brace, ~1500 chars), every issue and comment, each memory note, the files of every
  `third_party/` repo, and one chunk per commit message.
- Each chunk is in an SQLite FTS5 keyword index (BM25, camelCase and snake_case
  names also split into words) and, with `fastembed`, a 384-d `bge-small-en-v1.5`
  vector. A query runs both and fuses the ranks (reciprocal rank fusion). A literal
  identifier in the query gets a small extra boost. On the **CPU**, third-party code is
  keyword-only (about 115k chunks would take hours); third-party docs and commit messages
  are embedded. On the **GPU** (below) everything is embedded.
- The index is `output/rag/index.sqlite` (git-ignored, safe to delete). The first
  build embeds a few thousand chunks on the CPU, which takes minutes; after that only
  changed files are redone.
- `evo_rag_mcp.py` is the MCP server (`.mcp.json`). Each search first checks file
  modification times, so edits show up without any hook. Issues refresh every 6 hours;
  `python tools/rag/evo_rag.py index` forces it.

## Embedding on the GPU

Embedding is the slow part: about 2 chunks/s on the CPU, about 235/s on an RTX 3050 Laptop
(a full 130k-chunk index in under 8 minutes). Searching needs no GPU (one query at a time),
so only `index` uses it. Set it up once, in its own venv so the normal Python stays CPU-only:

```bash
python -m venv output/rag/gpu-venv
output/rag/gpu-venv/Scripts/python.exe -m pip install onnxruntime-gpu==1.24.4 fastembed-gpu==0.8.1 \
    nvidia-cudnn-cu12 nvidia-cublas-cu12 nvidia-cuda-runtime-cu12 nvidia-cufft-cu12 \
    nvidia-curand-cu12 nvidia-cuda-nvrtc-cu12 numpy
output/rag/gpu-venv/Scripts/python.exe tools/rag/evo_rag.py index      # prints "embedded N chunks on cuda"
```

- Keep `onnxruntime-gpu` at **1.24.x**: 1.31 is built for CUDA 13 and wants `cudart64_13.dll`,
  which these CUDA 12 wheels do not have (the GPU then silently falls back to the CPU).
- It keeps the same model, so GPU and CPU vectors are interchangeable (cosine 1.0000).
- `EVO_RAG_DEVICE=cpu` forces the CPU, `EVO_RAG_BATCH=n` sets the batch size. If the GPU
  cannot start, the indexer prints why and uses the CPU.
- Do not run two `index` jobs at once (they share one SQLite file). Stop a background job
  before starting another.

## Reference repos (`thirdparty`)

Every directory under `third_party/` (the homebrew clones, ProsperoTV, ProsperoLight,
SharpProspero, kodi-ps5, ps5-homebrew-ui, the PS5 research repos, ...) is indexed as source
`thirdparty`, except the skip list in `evo_rag.py` (ffmpeg, FastCopy, daemons). Drop a new
clone into `third_party/` and the next search picks it up (add it to `.gitignore` so git
leaves it alone). A plain search leaves them out, so reference code never buries EVO's own
results. Ask for them:

```bash
python tools/rag/evo_rag.py search "lip sync audio clock flip time" --source thirdparty
python tools/rag/evo_rag.py search "decoder recovery" --path third_party/ProsperoLight/
```

Their commit messages are a second source, `commits` (every ref of each repo, so a fix
that was fetched but not checked out still shows, tagged `[not in checkout]`). It answers
"did a sibling project already fix this?":

```bash
python tools/rag/evo_rag.py search "aac passthrough lip sync" --source commits
```

Limit both with `EVO_RAG_THIRDPARTY=ProsperoTV,kodi-ps5` (empty turns it off). The
index follows each repo's checked-out files: `git fetch` alone changes nothing until you
update the checkout.

## What is left out

The rest of `third_party/` (ffmpeg, FastCopy, the Half-Life port), generated shader pipe
tables, the embedded asset bundle,
files over 300 KB, binaries.

## Changing things

- Another embedding model: `EVO_RAG_MODEL=<fastembed model name>`; the index re-embeds.
- Memory notes are found from the project path; override with `EVO_RAG_MEMORY=<dir>`.
- Add cases to `eval_queries.json` when a search misses something it should find.
