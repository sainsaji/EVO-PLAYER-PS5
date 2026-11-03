---
name: commit-evo
description: Commit finished EVO work cleanly - split by topic, leave out files that were already modified before the session, match the repo's message style, add the attribution trailer, never push. Use only when the user says commit (or ship it) after the build and console check passed.
---

# Commit EVO work

Only on the user's word, and only after the change was built and checked
(CLAUDE.md: no commits until verified). Never push.

## 1. Decide what goes in

```bash
git status --short
git diff --stat
```

- Compare with the `gitStatus` snapshot at the start of the session. Files that were already
  modified or untracked then (for example `assets/providers/iptv/manifest.json`,
  `tools/oc-run.sh` in the 2026-10 sessions) are not yours: leave them out and say so.
- Temporary diagnostics (a `PROV_LOG` added only to find a cause) either come out first or are
  named in the reply. A commit should equal what was tested, so removing code means retesting
  or saying it is unverified.
- `projects/evoplayer/ui_rml/src/evo_rmlui_bundle_data.cpp` is generated and tracked; a huge diff
  there after packaging is expected, include it only if assets changed.

## 2. Split by topic

One commit per user-visible topic, for example a bug fix and a UI feature separately. Add paths
by name (`git add path1 path2`), never `git add -A` or `.`.

A file with hunks for two topics (or the user's own earlier hunks): stage only the matching
hunks with `python3 tools/stage_hunks.py <file> '<regex>' [--invert]` (no interactive
`git add -p` here), then check `git diff --cached <file>`. Each commit should still build: keep a
helper and its first caller in the same commit, or the helper first.

## 3. Message

Follow `git log --oneline -8`: `type(scope): what changed, in plain words`, e.g.
`fix(playback): a pad scrub always commits - the preview wait is capped at 3 s`,
`feat(ui): stream and audio pickers show pills and a details card`. The subject says the
result, not the file. Add the issue number when there is one (`(#214)`).

End the message with the attribution trailer from the session's system reminder (this repo's
sessions use `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`), as a second `-m`:

```bash
git commit -q -m "fix(playback): ..." -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

Do not use a heredoc with backslashes (a hook blocks it). No `--no-verify`, no `--amend`
unless asked.

## 4. After

`git log --oneline -3` and `git status --short`. Reply short: hashes, what each covers, what
was left out, what was NOT verified (for example "layout checked on the host only"), and
what is still open. Bump `contentVersion` in `param.json` only for a release.
