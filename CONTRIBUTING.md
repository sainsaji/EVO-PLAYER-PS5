# Contributing to EVO Player

Thanks for helping! EVO is maintained by one person in spare time, and most testing means sitting in front of a
PS5. The rules below are here so a change can be understood, tested and released quickly. They are not meant to be
gatekeeping; they make the review easier for everyone.

- [Ways to help](#ways-to-help)
- [Report a bug](#report-a-bug)
- [One pull request, one change](#one-pull-request-one-change)
- [Files that stay out of pull requests](#files-that-stay-out-of-pull-requests)
- [Before you open a pull request](#before-you-open-a-pull-request)
- [Describe the problem and what you tested](#describe-the-problem-and-what-you-tested)
- [Commits](#commits)
- [Using AI tools](#using-ai-tools)

To build EVO, run its tests or put it on a console, see [Building from Source](README.md#building-from-source) and
[docs/build/building.md](docs/build/building.md).

## Ways to help

- **Report a bug.** A file that will not play, a stream that stutters, a screen that misbehaves. See
  [Report a bug](#report-a-bug).
- **Test a file or a source.** If a codec, a container, a drive or a provider works or fails, a
  [playback report](../../issues/new?template=playback-problem.yml) with EVO's report file attached helps even when you
  cannot say why.
- **Fix a bug or add a feature.** Look for an open issue. For a large change, open an issue first (see below).
- **Improve the UI kit.** EVO's screens are drawn on [ps5-homebrew-ui](third_party/ps5-homebrew-ui). A fix to the
  kit itself goes there first (see [Files that stay out of pull requests](#files-that-stay-out-of-pull-requests)).

## Report a bug

1. Search the [open issues](../../issues). If yours is there, add what is new to it instead of opening another.
2. Get EVO's report. It holds the version, your console model and firmware, the settings and the recent log, with
   passwords, tokens and debrid keys masked. The network-shares file (it has passwords) is never in it.
   - **EVO is still running:** Settings → System & Diagnostics → **Report a Problem**. Scan the QR code to download
     it on your phone, or press Cross to save `evo_report.txt` to the USB drive (or to `/data/evoplayer` with no
     drive).
   - **EVO crashed or froze:** start EVO again, then Settings → System & Diagnostics → **Report the last run**. It
     saves `evo_report_last_run.txt` the same way. Report a Problem would only show the new, healthy run.
3. [Open an issue](../../issues/new/choose): **Playback problem** for anything about playing a file or a stream,
   **Bug report** for the rest. Attach the report file and fill in the form.

For a playback problem, say where the file comes from (USB, NTFS drive, network share, Emby, Jellyfin, Plex,
Stremio addon, IPTV) and what Media Info shows for it (container, video codec, resolution, HDR, audio codec and
channels). Whether the same file plays in another player helps too.

## One pull request, one change

A pull request fixes one bug or adds one feature. If you find yourself writing "also", "additionally" or "while I
was there" in the description, split it.

- A pull request that adds a feature does not also fix unrelated bugs, even small ones. Open a second pull request
  for the fix. Small fixes are easy to review on their own and can ship right away.
- A pull request that fixes a bug does not also refactor, rename or reformat code it did not need to touch.
- If two changes are independent, they are two pull requests. If one needs the other, open the one it needs first
  and mention it in the second.
- A change to the UI kit is its own pull request on the kit, before the EVO one.

A good rule of thumb: if a reviewer could reasonably want to merge half of the pull request and reject the other
half, it should have been two. Google's [Small CLs](https://google.github.io/eng-practices/review/developer/small-cls.html)
guide explains why.

**Open an issue first** for large changes: a new feature, a new provider, anything that touches decode, the GPU
present path, audio output, credentials or the settings file. Describe the problem and the approach. This keeps you
from spending a weekend on something that conflicts with work in progress or with where the project is going.

Pull requests target `main`. Rebase on `main` rather than merging it in.

## Files that stay out of pull requests

These change only at release time, or are generated. Including them conflicts with every other open pull request or
breaks consoles that update:

- **The version**: `projects/evoplayer/VERSION`, `contentVersion` in `projects/evoplayer/sce_sys/param.json`, and
  the `EVO_PLAYER_VERSION` defines. ProsperoStore offers an update only when `contentVersion` goes up, so a version
  in a pull request is a version nobody can release over. [docs/build/release.md](docs/build/release.md) lists every
  place.
- **Release notes**: `CHANGELOG.md`, the in-app changelog (`projects/evoplayer/include/evo_changelog.h`) and the
  "What's New" part of the README.
- **Generated files**: `projects/evoplayer/ui_rml/src/evo_rmlui_bundle_data.cpp` is rebuilt by every package build.
  Leave your copy out.
- **Submodule pins**: `third_party/ps5-homebrew-ui` may only point at a commit already on the kit's `main`. The pull
  request on the kit comes first; once it is merged, an EVO pull request may move the pin.
- **Releases and tags.**

**The settings file** is line-based and old files must keep loading. Add a new setting as a new line at the end, never
reorder or reuse a line, and extend the round-trip and legacy tests in `tests/test_settings.c`.

A merged pull request ships in the next release.

## Before you open a pull request

Everything runs in the pinned Docker toolchain. **Never call `make` directly**: the Makefile is missing FFmpeg's
dependencies. Use the scripts.

```sh
docker compose run --rm ps5-dev ./scripts/build-evoplayer.sh       # compile check
docker compose run --rm ps5-dev ./tests/run_tests.sh               # host tests
docker compose run --rm ps5-dev ./scripts/package-app.sh --ffpfsc  # the .ffpfsc you test on the console
./tools/hui_preview.sh                                             # every screen rendered on your PC
```

New behaviour comes with tests where it can be tested on the host; changed behaviour comes with changed tests. For a
UI change, attach the `hui_preview.sh` render or a console screenshot.

Before you put a build on your console, read [docs/console/hardware-cycle.md](docs/console/hardware-cycle.md). Never
deploy over a running EVO and never start it twice: both have crashed consoles.

Match the style of the code around your change. Reusable UI fixes belong in ps5-homebrew-ui; EVO's own behaviour
belongs here.

## Describe the problem and what you tested

Fill in the [pull request template](.github/PULL_REQUEST_TEMPLATE.md). The description answers, in this order:

1. **What was wrong**, from a viewer's point of view. "HDR10 films look washed out" is a problem. "Refactor the
   colour matrix" is not.
2. **Why it happened**, briefly, if you know.
3. **What changed** in behaviour. Not a list of functions edited.
4. **How you tested it**, on a console: model (PS5 or PS5 Pro), firmware, the file or stream (container, codecs,
   where it comes from), what you did and what happened. Attach the report file or the log for playback changes.
   Host tests and a PS5 build are necessary but are not console testing. If you could not test something on a
   console, say so.

Describe only what is in the pull request. Link the issue it addresses; if there is none, the first paragraph is the
issue.

## Commits

- One line that says what changed for the viewer, with a type and area: `fix(subtitles): ...`,
  `feat(stremio): ...`. Types: `fix`, `feat`, `perf`, `docs`, `test`, `refactor`, `ci`, `chore`.
- A commit that resolves an issue says `Fixes #N` in its body; partial work says `Refs #N`.
- Each commit builds and passes the tests on its own. Fixup and "address review" commits are fine during review;
  they are squashed before merging.

## Using AI tools

**You are welcome to use AI coding tools.** Most of EVO was written with them. Two things to keep in mind:

1. **Say so.** The pull request template asks which tool and model you used and roughly how much of the change it
   wrote. This is not a judgement; it tells the maintainer where to look harder.
2. **You are the author.** You must be able to explain why each change is there, and you must have run the result
   on a PS5.

AI tools are especially prone to the problems this page is about. They bundle unrelated features into one change,
bump versions and pins, fix nearby things nobody asked about, and write long, confident descriptions of changes
that were never verified, or that are not in the diff at all. Before you open the pull request, check that it does
one thing and that every sentence in the description is true.

Coding agents read [CLAUDE.md](CLAUDE.md), which holds the console rules that are expensive to relearn on real
hardware.

## Review

Expect a few questions, and possibly a request to split the pull request. CI must pass (Build project suite and
Package app module). Reviews can take a while, because testing means sitting in front of a console. A pull request
that follows this page gets through much faster, because it can be understood and tested in one sitting. Thanks for
your patience, and for contributing!

## License

EVO Player is GPL-3.0 ([LICENSE](LICENSE)). Submodules and vendored sources keep their own licences; see
[docs/proprietary.md](docs/proprietary.md) before you copy code from another project.
