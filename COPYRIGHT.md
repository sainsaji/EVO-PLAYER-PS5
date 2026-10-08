# Licensing

**This repository is GPL-3.0-or-later.** That is not a preference — it is
required, and it overrides the Apache-2.0 licence the repository was
originally created with.

## Why

`projects/evoplayer/` is a modified version of
[ProsperoPlayer](https://github.com/KINGDKAK/ProsperoPlayer), which is
GPL-3.0-or-later. GPL-3.0 is a copyleft licence: a derivative work must be
distributed under the same terms. Apache-2.0 is compatible *into* GPLv3
(one-way), but the combined work must still be GPLv3 — it cannot be
redistributed under Apache-2.0.

The root `LICENSE` was therefore replaced with GPL-3.0 to match.

## What that means in practice

* Source must be made available to anyone you distribute binaries to.
* Copyright notices and attribution must be preserved — see
  `projects/evoplayer/NOTICE`, which records the fork point and the changes.
* Modifications must be marked as such.
* You cannot add restrictions beyond the GPL.

## Credit is appreciated

If EVO Player helps your project (you fork it, borrow code, or build on its
research), a mention such as *"Based on EVO Player by sainsaji —
github.com/sainsaji/EVO-PLAYER-PS5"* in your README or credits would be
great, and so would crediting the people EVO itself builds on (see the
README's Credits).

This is a friendly request, not an extra licence term: the GPL is the only
condition. Keeping the existing copyright notices is already required by
the GPL itself.

## Components

| Part | Origin | Licence |
|---|---|---|
| `projects/evoplayer/` | fork of ProsperoPlayer @ 282b449 | GPL-3.0-or-later |
| Development environment (`Dockerfile`, `scripts/`, `docs/`, `tools/`) | original to this project | GPL-3.0-or-later, for consistency |
| `projects/*_test/`, `projects/common/` | original to this project | GPL-3.0-or-later |
| PS5 Payload SDK | ps5-payload-dev/sdk | GPL-3.0-or-later (not vendored) |
| FFmpeg | ffmpeg.org | LGPL-2.1+/GPL-2+ (not vendored) |

The development scaffolding is original work and could carry a permissive
licence on its own. It is GPL-3.0 here simply because splitting licences
within one repository creates more confusion than it removes.

No Sony code, binaries, keys or SDK material is included in or required by
this repository.
