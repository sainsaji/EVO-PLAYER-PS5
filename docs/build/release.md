# Releases, CI and updates

How a version gets from `main` to a console. The user-facing notes live in
[CHANGELOG.md](../../CHANGELOG.md); this page is the mechanics.

## What ships

A release carries four files, all built by CI from the pinned toolchain:

| File | For |
|---|---|
| `EVOPlayer-<tag>-PPSA99039.ffpfsc` | the standard image, for any console. The only build tested on a base PS5 |
| `EVOPlayer-<tag>-PPSA99039-pro.ffpfsc` | the same source with `attribute3` bit 22 set ([Pro memory mode](../hardware/memory-budget.md#pro-memory-mode-and-the-caps-143-decided-2026-10-22)); tested on a PS5 Pro only |
| `EVOPlayer-<tag>-PPSA99039.zip` | the folder-title archive, for loaders that want loose files |
| `SHA256SUMS.txt` | checksums for the above |

Install options for users are in the root [README.md](../../README.md): copy the
image to `/data/homebrew/` for ShadowMountPlus, or install and update from
ProsperoStore (which offers an update only when `contentVersion` in
`param.json` went up).

## Cutting a release

Everything that carries the version string, as changed by the v0.12.5 commit
(`875f766`):

1. `projects/evoplayer/VERSION`: `0.12.6`. The workflow fails if it does not match
   the tag.
2. `projects/evoplayer/sce_sys/param.json` `contentVersion`: `01.012.060` for
   0.12.6 (`01.`, the minor in three digits, then the patch times ten: 0.12.0 is
   `01.012.000`, 0.12.5 is `01.012.050`). **Must be higher than the previous tag's**
   or the workflow fails; a build that forgets this is invisible to ProsperoStore.
3. The `EVO_PLAYER_VERSION` define, still repeated per file:
   `core/src/Application.cpp`, `core/src/screens/SettingsScreen.cpp`,
   `core/src/services/MediaMetadataService.cpp`, `addons/src/evo_net.c`,
   `addons/src/evo_http_avio.c`, `addons/src/evo_updater.c`, and the
   `version` comment in `ui_rml/include/evo_rmlui_bridge.h`. The updater compares
   against this define, so a missed one makes EVO offer its own version as an
   update.
4. `projects/evoplayer/include/evo_changelog.h` (the in-app Changelog screen;
   write the strings in capitals like the others - the old twin at
   `projects/evoplayer/evo_changelog.h` was never included and is gone), plus the
   fixture version in `tools/hui_preview/hui_preview.cpp`.
5. `CHANGELOG.md`: a `## 0.12.6` heading. The workflow lifts exactly that section
   into the GitHub release notes, so the heading must match the version.
6. Root `README.md` "What's new".
7. Build, run the host tests, verify on the console, then commit
   ([../../.claude/skills/commit-evo/SKILL.md](../../.claude/skills/commit-evo/SKILL.md)).
8. `git tag v0.12.6 && git push origin v0.12.6`.

## The workflows

| Workflow | Trigger | Does |
|---|---|---|
| `build.yml` | push to `main`, `develop`, `experiment/**`; PRs; manual | builds the dev image, runs `setup-sdk.sh`, `build-evoplayer.sh` and the compile-database build, validates the ELFs, and runs a `package-app` job (`package-app.sh --ffpfsc` on every PR, archived as an artifact). Skips doc-only changes |
| `release.yml` | tag `v*`; manual (a dry run) | checks VERSION and `contentVersion`; builds FFmpeg 7.1.1 inside the container (the image ships 7.0.1); packages the standard and Pro images and the zip; verifies them; extracts the changelog section; publishes the GitHub release on a tag only |
| `add-to-project.yml`, `issue-triage.yml`, `pr-labeler.yml`, `greetings.yml`, `stale.yml` | issue and PR events | project board, auto-labels, housekeeping |

The release verify step checks the pre-sign ELF is a PS5 binary (not the host
loader), that the signed `eboot.bin` reports a valid container
(`ps5-native-tool self --inspect`), `param.json` `titleId`, `libc.prx`, and that
`assets/` is not empty. A manual `workflow_dispatch` runs all of it and stops short
of publishing, which is how to check the pipeline without a release.

**CI has no console.** Nothing it builds is hardware-tested; the release notes
say so. The hardware check is yours, before the tag
([validation.md](validation.md#hardware-checks)).

## In-app update

Settings → System & Diagnostics → Software Update (`addons/src/evo_updater.c`,
#120):

1. **Check** the latest release from the GitHub API and compare its tag with
   `EVO_PLAYER_VERSION`.
2. **Download** (only after the user says so) the matching asset: the plain
   `-PPSA99039.ffpfsc`, or the `-pro.ffpfsc` on a console in Pro memory mode.
   The image streams through FFmpeg's HTTP client to a staging file beside the
   installed image, never into memory.
3. **Verify** the SHA-256 against the `digest` GitHub publishes for the asset. No
   digest, or a mismatch, fails the update and leaves the installed image alone.
4. **Apply** with one rename on the same filesystem, on a second confirmation. The
   old image stays mounted under its open inode, so the running EVO is undisturbed;
   EVO then closes and the user relaunches from the Games row.

Nothing is replaced before the hash matches, and every step needs a yes.
