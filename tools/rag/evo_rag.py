#!/usr/bin/env python3
"""evo_rag.py - local search over EVO's code, docs, GitHub issues and memory notes.

One SQLite file (output/rag/index.sqlite) holds every chunk twice: in an FTS5
keyword index (BM25) and, when `fastembed` is installed, as a 384-d embedding.
A query runs both and fuses the rankings (reciprocal rank fusion), so an exact
name like `slot_renew_decoder` and a question like "why does a seek lose frames"
both land. Without fastembed it still works, keyword-only.

  python tools/rag/evo_rag.py index            # build / update (changed files only)
  python tools/rag/evo_rag.py search "why not call sceVideoOutOpen" -k 6
  python tools/rag/evo_rag.py status
  python tools/rag/evo_rag.py eval             # does it find the right file?

Sources (the `source` filter):
  code     .c .h .cpp .hpp .py .sh .rml .rcss ... from `git ls-files` (+ untracked)
  docs     *.md in the repo
  issues   every GitHub issue, open and closed, with comments (needs `gh`)
  memory   the Claude Code memory notes for this project

Skipped on purpose: third_party/, generated pipe tables and the embedded asset
bundle, main.c.legacy (CLAUDE.md says not to learn from it), anything over 300 KB.
"""
import argparse
import hashlib
import json
import os
import re
import sqlite3
import subprocess
import sys
import threading
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
INDEX_DIR = ROOT / "output" / "rag"
DB_PATH = INDEX_DIR / "index.sqlite"
MODEL_NAME = os.environ.get("EVO_RAG_MODEL", "BAAI/bge-small-en-v1.5")
QUERY_PREFIX = ("Represent this sentence for searching relevant passages: "
                if "bge" in MODEL_NAME.lower() else "")

CODE_EXT = {".c", ".h", ".cpp", ".hpp", ".cc", ".py", ".sh", ".rml", ".rcss",
            ".yml", ".yaml", ".cmake", ".mk", ".s", ".glsl"}
CODE_NAMES = {"Makefile", "Dockerfile", "CMakeLists.txt"}
SKIP_FRAGMENTS = ("third_party/", "/third_party/", ".legacy", "bundle_data",
                  "_pipe.h", ".pipe", ".inc", "/output/", "node_modules/",
                  "uiview/obj/")
MAX_FILE_BYTES = 300_000
TARGET_CHARS = 1500
HARD_CHARS = 2400
CHUNKER_VERSION = 2          # bump when chunking changes; forces a re-chunk
MD_TARGET_CHARS = 700
MD_MAX_CHARS = 1800
ISSUE_STALE_S = 6 * 3600

STOP = set("the a an of to in is are was be how what why where when do does did "
           "i we it for on with and or not this that from by as at can should "
           "would could our my me you your there their then than into out up".split())


# ---------------------------------------------------------------- text helpers

def expand_identifiers(text):
    """Append the words inside camelCase / snake_case names, so a search for
    'playback controller' finds PlaybackController."""
    extra = set()
    for m in re.findall(r"[A-Za-z_][A-Za-z0-9_]{2,}", text):
        parts = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", m).replace("_", " ").split()
        if len(parts) > 1:
            extra.update(p.lower() for p in parts if len(p) > 1)
    return text + "\n" + " ".join(sorted(extra))


def query_terms(q):
    toks = set()
    for m in re.findall(r"[A-Za-z0-9_]+", q):
        low = m.lower()
        for part in re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", m).replace("_", " ").split():
            p = part.lower()
            if len(p) > 1 and p not in STOP:
                toks.add(p)
        if "_" not in m and len(low) > 2 and low not in STOP:
            toks.add(low)
    return sorted(toks)


def exact_idents(q):
    """Tokens that look like a code name (snake_case, camelCase, file.ext)."""
    out = []
    for m in re.findall(r"[A-Za-z_][A-Za-z0-9_.]{3,}", q):
        if "_" in m or re.search(r"[a-z][A-Z]", m) or re.search(r"\.\w{1,4}$", m):
            out.append(m)
    return out


def split_blocks(text, target=MD_TARGET_CHARS, hard=MD_MAX_CHARS):
    """Split prose into (piece, first_line_offset) of about `target` chars.

    A cut falls on a blank line or just before a top-level list item, so each
    rule or bullet stays whole and small enough for its meaning not to be
    diluted. A paragraph longer than `hard` (the memory notes are single huge
    lines) is cut at a sentence end."""
    pieces, cur, size, cur_start = [], [], 0, 0

    def emit():
        raw = "\n".join(cur)
        s = raw.strip("\n")
        if s.strip():
            pieces.append((s, cur_start + len(raw) - len(raw.lstrip("\n"))))
        cur.clear()

    for n, line in enumerate(text.split("\n")):
        item = re.match(r"^(?:[-*+] |\d+[.)] )", line) is not None
        if cur and size >= target and (item or not line.strip()):
            emit()
            size = 0
        if not cur:
            cur_start = n
        cur.append(line)
        size += len(line) + 1
        while size >= hard:
            joined = "\n".join(cur)
            cut = max(joined.rfind(". ", 0, hard), joined.rfind("\n", 0, hard))
            cut = hard if cut < hard // 2 else cut + 1
            head, tail = joined[:cut], joined[cut:]
            if head.strip():
                pieces.append((head.strip("\n"), cur_start))
            cur[:] = [tail.lstrip(" ")] if tail.strip() else []
            cur_start = n
            size = len(tail)
            if not tail.strip():
                size = 0
    emit()
    return pieces


# -------------------------------------------------------------------- chunkers

def chunk_markdown(text):
    lines = text.splitlines()
    out, stack, cur, start, fence = [], [], [], 1, False

    def flush(end):
        body = "\n".join(cur).strip("\n")
        if len(body.strip()) >= 40:
            title = " > ".join(t for _, t in stack)
            for piece, off in split_blocks(body):
                out.append(dict(kind="docs", start=start + off,
                                end=start + off + piece.count("\n"),
                                title=title, text=piece))
        cur.clear()

    for i, line in enumerate(lines, 1):
        if line.lstrip().startswith("```"):
            fence = not fence
        m = None if fence else re.match(r"^(#{1,4})\s+(.*\S)", line)
        if m:
            flush(i - 1)
            level = len(m.group(1))
            while stack and stack[-1][0] >= level:
                stack.pop()
            stack.append((level, m.group(2).strip()))
            start = i
        cur.append(line)
    flush(len(lines))
    return out


def chunk_code(text):
    lines = text.splitlines()
    out, cur, size, start = [], [], 0, 1

    def emit(end):
        body = "\n".join(cur)
        if len(body.strip()) >= 40:
            out.append(dict(kind="code", start=start, end=end, title="", text=body))

    for i, line in enumerate(lines, 1):
        cur.append(line)
        size += len(line) + 1
        boundary = (not line.strip()) or line.startswith("}")
        if (size >= TARGET_CHARS and boundary) or size >= HARD_CHARS:
            emit(i)
            keep = cur[-3:] if size >= HARD_CHARS else []
            cur[:] = keep
            size = sum(len(x) + 1 for x in keep)
            start = i - len(keep) + 1
    if cur:
        emit(len(lines))
    return out


def chunk_issue(issue):
    n = issue["number"]
    labels = ", ".join(l["name"] for l in issue.get("labels", []))
    head = f"Issue #{n} [{issue.get('state', '').lower()}] {issue['title']}"
    if labels:
        head += f"\nLabels: {labels}"
    parts = [(head + "\n\n" + (issue.get("body") or "")).strip()]
    for c in issue.get("comments", []):
        who = (c.get("author") or {}).get("login", "?")
        parts.append(f"Issue #{n} comment by {who}:\n{(c.get('body') or '').strip()}")
    out = []
    for p in parts:
        for piece, _ in split_blocks(p):
            if piece.strip():
                out.append(dict(kind="issues", start=n, end=n,
                                title=f"#{n} {issue['title']}", text=piece))
    return out


# ----------------------------------------------------------------------- files

def memory_dir():
    env = os.environ.get("EVO_RAG_MEMORY")
    if env:
        return Path(env)
    base = Path.home() / ".claude" / "projects"
    if not base.is_dir():
        return None
    slug = re.sub(r"[^A-Za-z0-9]", "-", str(ROOT)).lower()
    for p in base.iterdir():
        if p.name.lower() == slug and (p / "memory").is_dir():
            return p / "memory"
    return None


def wanted(rel):
    low = rel.lower()
    if any(f in low for f in SKIP_FRAGMENTS):
        return False
    name = rel.rsplit("/", 1)[-1]
    ext = os.path.splitext(name)[1].lower()
    return ext == ".md" or ext in CODE_EXT or name in CODE_NAMES


def list_repo_files():
    try:
        raw = subprocess.run(["git", "ls-files", "-co", "--exclude-standard", "-z"],
                             cwd=ROOT, capture_output=True, timeout=30).stdout
    except Exception:
        return []
    out = []
    for b in raw.split(b"\0"):
        if not b:
            continue
        rel = b.decode("utf-8", "replace")
        if wanted(rel):
            out.append(rel)
    return out


def read_text(path):
    try:
        if path.stat().st_size > MAX_FILE_BYTES:
            return None
        data = path.read_bytes()
    except OSError:
        return None
    if b"\0" in data[:4096]:
        return None
    return data.decode("utf-8", "replace")


# ----------------------------------------------------------------------- index

class Embedder:
    def __init__(self):
        self.model = None
        self.err = None

    def load(self):
        if self.model or self.err:
            return self.model
        try:
            from fastembed import TextEmbedding
            self.model = TextEmbedding(MODEL_NAME, cache_dir=str(INDEX_DIR / "models"))
        except Exception as e:                       # not installed, offline, ...
            self.err = f"{type(e).__name__}: {e}"
        return self.model

    def embed(self, texts):
        vecs = np.asarray(list(self.model.embed(texts, batch_size=32)), dtype=np.float32)
        norms = np.linalg.norm(vecs, axis=1, keepdims=True)
        return vecs / np.maximum(norms, 1e-9)


class Index:
    def __init__(self):
        INDEX_DIR.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(str(DB_PATH), check_same_thread=False)
        self.lock = threading.RLock()
        self.embedder = Embedder()
        self._mat = None                            # cached dense matrix
        self._mat_version = -1
        self._version = 0
        self._last_sync = 0.0
        with self.lock:
            self.db.executescript("""
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS files(path TEXT PRIMARY KEY, source TEXT, sig TEXT);
                CREATE TABLE IF NOT EXISTS chunks(
                    id INTEGER PRIMARY KEY, path TEXT, source TEXT, kind TEXT,
                    line_start INT, line_end INT, title TEXT, text TEXT, sha TEXT, vec BLOB);
                CREATE INDEX IF NOT EXISTS chunks_path ON chunks(path);
                CREATE INDEX IF NOT EXISTS chunks_vec ON chunks(vec IS NULL);
                CREATE TABLE IF NOT EXISTS vec_cache(sha TEXT PRIMARY KEY, vec BLOB);
                CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT);
                CREATE VIRTUAL TABLE IF NOT EXISTS fts USING fts5(terms, tokenize='unicode61');
            """)
            row = self.db.execute("SELECT v FROM meta WHERE k='model'").fetchone()
            if row and row[0] != MODEL_NAME:        # a different model: old vectors are useless
                self.db.execute("UPDATE chunks SET vec=NULL")
                self.db.execute("DELETE FROM vec_cache")
            self.db.execute("INSERT OR REPLACE INTO meta VALUES('model', ?)", (MODEL_NAME,))
            row = self.db.execute("SELECT v FROM meta WHERE k='chunker'").fetchone()
            if (row[0] if row else "1") != str(CHUNKER_VERSION):
                # The way text is cut changed: re-chunk everything. Chunks whose
                # text is identical still reuse their stored vectors (vec_cache).
                self.db.execute("UPDATE files SET sig=''")
                self.db.execute("DELETE FROM meta WHERE k='issues_synced'")
            self.db.execute("INSERT OR REPLACE INTO meta VALUES('chunker', ?)",
                            (str(CHUNKER_VERSION),))
            self.db.commit()

    # ---- writing

    @staticmethod
    def _embed_text(path, ch):
        return f"{path}\n{ch['title']}\n{ch['text']}"[:2000]

    def _replace_file(self, path, source, sig, chunks):
        with self.lock:
            ids = [r[0] for r in self.db.execute("SELECT id FROM chunks WHERE path=?", (path,))]
            self.db.executemany("DELETE FROM fts WHERE rowid=?", [(i,) for i in ids])
            self.db.execute("DELETE FROM chunks WHERE path=?", (path,))
            for ch in chunks:
                sha = hashlib.sha1(self._embed_text(path, ch).encode("utf-8", "replace")).hexdigest()
                hit = self.db.execute("SELECT vec FROM vec_cache WHERE sha=?", (sha,)).fetchone()
                cur = self.db.execute(
                    "INSERT INTO chunks(path,source,kind,line_start,line_end,title,text,sha,vec) "
                    "VALUES(?,?,?,?,?,?,?,?,?)",
                    (path, source, ch["kind"], ch["start"], ch["end"], ch["title"],
                     ch["text"], sha, hit[0] if hit else None))
                terms = expand_identifiers(f"{path}\n{ch['title']}\n{ch['text']}")
                self.db.execute("INSERT INTO fts(rowid, terms) VALUES(?,?)", (cur.lastrowid, terms))
            self.db.execute("INSERT OR REPLACE INTO files VALUES(?,?,?)", (path, source, sig))
            self.db.commit()
            self._version += 1

    def _drop_file(self, path):
        with self.lock:
            ids = [r[0] for r in self.db.execute("SELECT id FROM chunks WHERE path=?", (path,))]
            self.db.executemany("DELETE FROM fts WHERE rowid=?", [(i,) for i in ids])
            self.db.execute("DELETE FROM chunks WHERE path=?", (path,))
            self.db.execute("DELETE FROM files WHERE path=?", (path,))
            self.db.commit()
            self._version += 1

    def sync_files(self, force=False, min_interval=3.0):
        """Bring files / memory notes up to date. Returns (changed, removed)."""
        now = time.time()
        if not force and now - self._last_sync < min_interval:
            return 0, 0
        self._last_sync = now
        present = {}
        for rel in list_repo_files():
            try:
                st = (ROOT / rel).stat()
            except OSError:
                continue
            src = "docs" if rel.endswith(".md") else "code"
            present[rel] = (src, ROOT / rel, f"{st.st_size}:{st.st_mtime_ns}")
        mem = memory_dir()
        if mem:
            for p in mem.glob("*.md"):
                if p.name == "MEMORY.md":
                    continue
                st = p.stat()
                present[f"memory/{p.name}"] = ("memory", p, f"{st.st_size}:{st.st_mtime_ns}")
        with self.lock:
            known = dict((r[0], r[1]) for r in self.db.execute(
                "SELECT path, sig FROM files WHERE source IN ('code','docs','memory')"))
        removed = [p for p in known if p not in present]
        changed = [p for p, v in present.items() if known.get(p) != v[2]]
        for p in removed:
            self._drop_file(p)
        for p in changed:
            src, abspath, sig = present[p]
            text = read_text(abspath)
            if text is None:
                self._drop_file(p)
                continue
            chunks = chunk_markdown(text) if abspath.suffix.lower() == ".md" else chunk_code(text)
            if src == "memory":
                for c in chunks:
                    c["kind"] = "memory"
            self._replace_file(p, src, sig, chunks)
        return len(changed), len(removed)

    def sync_issues(self, force=False):
        with self.lock:
            row = self.db.execute("SELECT v FROM meta WHERE k='issues_synced'").fetchone()
        if not force and row and time.time() - float(row[0]) < ISSUE_STALE_S:
            return 0
        try:
            raw = subprocess.run(
                ["gh", "issue", "list", "--state", "all", "--limit", "1000", "--json",
                 "number,title,body,state,labels,comments,updatedAt"],
                cwd=ROOT, capture_output=True, timeout=90)
            issues = json.loads(raw.stdout.decode("utf-8", "replace"))
        except Exception as e:
            print(f"[rag] issues not synced: {e}", file=sys.stderr)
            return 0
        seen, changed = set(), 0
        for it in issues:
            path = f"issue#{it['number']}"
            seen.add(path)
            sig = f"{it.get('updatedAt')}:{len(it.get('body') or '')}:{len(it.get('comments', []))}"
            with self.lock:
                old = self.db.execute("SELECT sig FROM files WHERE path=?", (path,)).fetchone()
            if old and old[0] == sig:
                continue
            self._replace_file(path, "issues", sig, chunk_issue(it))
            changed += 1
        with self.lock:
            gone = [r[0] for r in self.db.execute("SELECT path FROM files WHERE source='issues'")
                    if r[0] not in seen]
        for p in gone:
            self._drop_file(p)
        with self.lock:
            self.db.execute("INSERT OR REPLACE INTO meta VALUES('issues_synced', ?)",
                            (str(time.time()),))
            self.db.commit()
        return changed

    def embed_pending(self, batch=64):
        if not self.embedder.load():
            return 0
        done = 0
        while True:
            with self.lock:
                rows = self.db.execute(
                    "SELECT id, path, title, text, sha FROM chunks WHERE vec IS NULL LIMIT ?",
                    (batch,)).fetchall()
            if not rows:
                break
            vecs = self.embedder.embed(
                [self._embed_text(r[1], dict(title=r[2], text=r[3])) for r in rows])
            with self.lock:
                for r, v in zip(rows, vecs):
                    blob = v.tobytes()
                    self.db.execute("UPDATE chunks SET vec=? WHERE id=?", (blob, r[0]))
                    self.db.execute("INSERT OR REPLACE INTO vec_cache VALUES(?,?)", (r[4], blob))
                self.db.commit()
                self._version += 1
            done += len(rows)
        return done

    # ---- reading

    def _matrix(self):
        with self.lock:
            if self._mat is not None and self._mat_version == self._version:
                return self._mat
            rows = self.db.execute(
                "SELECT id, source, path, vec FROM chunks WHERE vec IS NOT NULL").fetchall()
            ids = np.array([r[0] for r in rows], dtype=np.int64)
            mat = (np.vstack([np.frombuffer(r[3], dtype=np.float32) for r in rows])
                   if rows else np.zeros((0, 1), dtype=np.float32))
            self._mat = (ids, [r[1] for r in rows], [r[2] for r in rows], mat)
            self._mat_version = self._version
            return self._mat

    def search(self, query, k=8, source="all", path_prefix="", mode="hybrid"):
        terms = query_terms(query)
        sources = None if source in ("", "all", None) else set(source.split(","))

        def ok(src, path):
            return (sources is None or src in sources) and path.startswith(path_prefix)

        bm25 = []
        if terms and mode in ("hybrid", "bm25"):
            match = " OR ".join(f'"{t}"' for t in terms)
            with self.lock:
                cand = self.db.execute(
                    "SELECT rowid FROM fts WHERE fts MATCH ? ORDER BY bm25(fts) LIMIT 400",
                    (match,)).fetchall()
                meta = {}
                if cand:
                    qs = ",".join("?" * len(cand))
                    for r in self.db.execute(
                            f"SELECT id, source, path FROM chunks WHERE id IN ({qs})",
                            [c[0] for c in cand]):
                        meta[r[0]] = r
            bm25 = [c[0] for c in cand if c[0] in meta and ok(meta[c[0]][1], meta[c[0]][2])][:60]

        dense = []
        if mode in ("hybrid", "dense") and self.embedder.load():
            ids, srcs, paths, mat = self._matrix()
            if len(ids):
                q = self.embedder.embed([QUERY_PREFIX + query])[0]
                sims = mat @ q
                order = np.argsort(-sims)
                for i in order:
                    if ok(srcs[i], paths[i]):
                        dense.append(int(ids[i]))
                        if len(dense) >= 60:
                            break

        score = {}
        for ranking in (bm25, dense):
            for rank, cid in enumerate(ranking):
                score[cid] = score.get(cid, 0.0) + 1.0 / (60 + rank)
        if not score:
            return []
        # Reciprocal rank fusion rewards chunks both rankings agree on, which
        # pushes out a clear rank-1 hit from only one of them (an exact rule in
        # CLAUDE.md that the embedding model cannot see, or a paraphrase the
        # keywords miss). Each method's top two always make the cut.
        for ranking in (bm25, dense):
            for cid in ranking[:2]:
                score[cid] += 1.0
        with self.lock:
            qs = ",".join("?" * len(score))
            rows = {r[0]: r for r in self.db.execute(
                f"SELECT id, path, source, line_start, line_end, title, text "
                f"FROM chunks WHERE id IN ({qs})",
                list(score))}
        idents = exact_idents(query)
        if idents:
            for cid, r in rows.items():
                hits = sum(1 for t in idents if t in r[6] or t in r[1])
                score[cid] += 0.004 * min(hits, 3)
        hits, per_path = [], {}
        for cid in sorted(score, key=lambda c: -score[c]):
            r = rows.get(cid)
            if not r:
                continue
            if per_path.get(r[1], 0) >= 2:                  # two chunks per file is enough
                continue
            per_path[r[1]] = per_path.get(r[1], 0) + 1
            hits.append(dict(id=cid, path=r[1], source=r[2], start=r[3], end=r[4],
                             title=r[5], text=r[6], score=score[cid]))
            if len(hits) >= k:
                break
        return hits

    def status(self):
        with self.lock:
            by = dict(self.db.execute("SELECT source, COUNT(*) FROM chunks GROUP BY source"))
            total = sum(by.values())
            embedded = self.db.execute(
                "SELECT COUNT(*) FROM chunks WHERE vec IS NOT NULL").fetchone()[0]
            files = self.db.execute("SELECT COUNT(*) FROM files").fetchone()[0]
            iss = self.db.execute("SELECT v FROM meta WHERE k='issues_synced'").fetchone()
        return dict(files=files, chunks=total, by_source=by, embedded=embedded,
                    model=MODEL_NAME, dense_error=self.embedder.err,
                    issues_synced_min_ago=(round((time.time() - float(iss[0])) / 60)
                                           if iss else None))


def format_hits(hits, snippet_lines=14):
    if not hits:
        return "no results"
    out = []
    for n, h in enumerate(hits, 1):
        if h["source"] == "issues":
            where = f"issue #{h['start']}"
        elif h["source"] == "memory":
            where = f"{h['path']}"
        else:
            where = f"{h['path']}:{h['start']}-{h['end']}"
        head = f"{n}. {where}  [{h['source']}]"
        if h["title"] and h["source"] != "code":
            head += f"  {h['title']}"
        lines = h["text"].splitlines()
        body = "\n".join("   " + l[:160] for l in lines[:snippet_lines])
        if len(lines) > snippet_lines:
            body += f"\n   ... (+{len(lines) - snippet_lines} lines)"
        out.append(head + "\n" + body)
    return "\n\n".join(out)


# ------------------------------------------------------------------------ CLI

def run_eval(idx, k=5):
    path = Path(__file__).with_name("eval_queries.json")
    cases = json.loads(path.read_text(encoding="utf-8"))
    modes = ["bm25", "dense", "hybrid"] if idx.embedder.load() else ["bm25"]
    for mode in modes:
        hit = 0
        misses = []
        for c in cases:
            res = idx.search(c["q"], k=k, mode=mode)
            paths = [h["path"] for h in res]
            if any(any(e in p for p in paths) for e in c["expect"]):
                hit += 1
            else:
                misses.append(c["q"])
        print(f"{mode:7s} hit@{k}: {hit}/{len(cases)}")
        for m in misses:
            print(f"    miss: {m}")


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("index")
    p.add_argument("--no-issues", action="store_true")
    p.add_argument("--no-embed", action="store_true")
    p.add_argument("--rebuild", action="store_true", help="delete the index first")
    p = sub.add_parser("search")
    p.add_argument("query")
    p.add_argument("-k", type=int, default=8)
    p.add_argument("--source", default="all")
    p.add_argument("--path", default="")
    p.add_argument("--mode", default="hybrid", choices=["hybrid", "bm25", "dense"])
    sub.add_parser("status")
    p = sub.add_parser("eval")
    p.add_argument("-k", type=int, default=5)
    a = ap.parse_args()

    if a.cmd == "index" and a.rebuild and DB_PATH.exists():
        for f in INDEX_DIR.glob("index.sqlite*"):
            f.unlink()
    idx = Index()
    if a.cmd == "index":
        t = time.time()
        ch, rm = idx.sync_files(force=True)
        print(f"files: {ch} changed, {rm} removed  ({time.time() - t:.1f}s)")
        if not a.no_issues:
            print(f"issues: {idx.sync_issues(force=True)} changed")
        if not a.no_embed:
            t = time.time()
            n = idx.embed_pending()
            print(f"embedded {n} chunks ({time.time() - t:.1f}s)"
                  if idx.embedder.model else f"no embeddings: {idx.embedder.err}")
        print(json.dumps(idx.status(), indent=1))
    elif a.cmd == "search":
        idx.sync_files(force=True)
        print(format_hits(idx.search(a.query, a.k, a.source, a.path, a.mode)))
    elif a.cmd == "status":
        print(json.dumps(idx.status(), indent=1))
    elif a.cmd == "eval":
        idx.sync_files(force=True)
        run_eval(idx, a.k)


if __name__ == "__main__":
    main()
