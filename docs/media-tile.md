# Installing EVO Player as a home-screen Media app

**Status: built, not yet installed on hardware.** `scripts/build-media-tile.sh`
produces both payloads. Nothing has been registered on a console yet — §5
records what was done, §6 the risks that still apply on first install.

Today EVO Player is launched the way [install-homebrew.sh](../scripts/install-homebrew.sh)
does it: upload the ELF to `/data/homebrew/EVOPlayer/`, then hit
`http://<ps5>:8080/hbldr` on ps5-payload-websrv, or click the entry on websrv's
index page from a browser. The console never shows the player anywhere; you
need a second device to start it.

The question is whether EVO Player can instead appear on the PS5 home screen
under **Media**, like ProsperoPlayer does, and be started with the controller.

**Yes — and the mechanism is already sitting in this repository, unbuilt and
unwired.** `projects/evoplayer/prospero_media_standalone/` is a complete
Media-tile launcher inherited from the ProsperoPlayer fork. It compiles against
the pinned SDK today (verified below). What it is missing is branding, an icon,
its own title ID, and any integration with the repo's scripts.

---

## 1. The route that cannot work: `package-pkg.sh --format app`

Worth restating because it is the obvious first thing to try.
[packaging.md](packaging.md) already records the refusal, and it still holds:

```
$ llvm-readelf -h output/elf/EVOPlayer.elf
  Type: DYN (Shared object file)
```

`make_fself.py` accepts only a **static `ET_EXEC`** ELF. Every payload this repo
builds — EVO Player included — is position-independent, so the fake-SELF route
rejects it by design. An app's `eboot.bin` was never meant to be the homebrew;
it is a launcher stub that starts the real payload.

The Media-tile approach below is the same insight taken further: the registered
title's `eboot.bin` is not EVO Player at all, and is not even a stub we build —
it is a **copy of a Sony system binary**, used purely as a process to hijack.

## 2. How ProsperoPlayer actually does it

Source: `projects/evoplayer/prospero_media_standalone/`, mirrored in
`projects/evoplayer/docs/PROSPERO_MEDIA_LAUNCHER.md`. The loader core
(`core/hbldr.c`, `core/elfldr.c`, `core/pt.c`) is John Törnblom's
ps5-payload-websrv BigApp path, GPL-3.0-or-later; the orchestration around it is
ProsperoPlayer's.

One resident payload does five things:

| # | Step | Code | Why |
|---|---|---|---|
| 1 | Write the player to `/data/homebrew/EVOPlayer/eboot.elf` | `pp_install_runtime` | The player ELF is `.incbin`-embedded in the launcher; nothing to copy by hand |
| 2 | Create the system host `/system_ex/app/PRSP10001/` | `hbldr_prepare_host` | `nmount(MNT_UPDATE)` remounts `/system_ex` read-write, then writes `param.json` |
| 3 | Host `eboot.bin` = byte copy of **NPXS40106** (the system media app) | `fakeapp_create_if_missing` | A genuinely signed binary the shell will launch; we replace its process image later |
| 4 | Raise the process authid to `0x4801000000000013`, register `/user/app/PRSP10001` with `applicationCategoryType: 65536` | `pp_with_appinst_authid` → `pp_register_media_tile` | AppInstUtil refuses the install at normal privilege; 65536 is what puts the tile in the **Media** row |
| 5 | Serve `127.0.0.1:9055`, stay resident | `pp_serve` | The tile's `deeplinkUri` is `http://127.0.0.1:9055/launch` |

Pressing the tile then does this:

```
shell → GET http://127.0.0.1:9055/launch
      → launcher replies 204 immediately, on the request thread
      → worker thread: sceSystemServiceLaunchApp("PRSP10001")   (bigapp_launch)
      → ptrace the new NPXS40106-derived process, raise privileges,
        replace its image with EVOPlayer.elf                    (bigapp_replace)
```

Two details in there are hard-won and worth not rediscovering:

- **Answer 204 before launching.** The shell treats a slow deeplink as failure
  and flashes "not supported". `pp_launch_player_async` exists solely for this.
- **No `contentId` in either `param.json`.** A fake content id makes the shell
  license-check the title and show a padlock. Both the tile param and the host
  param omit it deliberately (`core/hbldr.c:62`).

Removal is its own problem: Sony usually hides Options → Delete for Media BigApp
hosts, so the package ships `/uninstall` on the loopback service *and* a
standalone `prospero_uninstall_tile.c` one-shot for when the launcher is not
running. It stops the running BigApp, calls `sceAppInstUtilAppUnInstall` under
the raised authid, and wipes `/user/app` and `/system_ex/app` by hand.

## 3. What this does and does not buy you

It does **not** escape the jailbreak. The tile is inert on its own — it is a
deeplink to a loopback port, and if the launcher payload is not resident, the
shell opens nothing. So the boot sequence becomes:

```
jailbreak  →  inject EVOPlayer_MediaLauncher.elf  →  Home → Media → EVO Player
```

versus today's:

```
jailbreak  →  websrv up  →  browser on another device  →  /hbldr
```

The win is real but specific: **once per boot** you inject one payload (or put
it on the jailbreak's autoload list, which most 12.70 exploit hosts support),
and after that the console is self-contained — no phone, no laptop, no browser.
It also gets the player a display plane through the same BigApp path websrv
uses, so playback behaves exactly as it does today.

| | jailbreak needed | launch from console | extra step per boot |
|---|---|---|---|
| websrv homebrew (today) | yes | no — needs a browser | start websrv |
| **Media tile launcher** | yes | **yes** | inject launcher (autoloadable) |
| fake-SELF app (`--format app`) | yes | yes | blocked: needs a static EXEC stub |
| signed fPKG | no | yes | impossible without Sony's `prospero-pub-cmd` |

## 4. Verified against this repo's toolchain

Both ELFs build clean, unmodified, against the pinned image
(`evo-player/ps5-dev:llvm18-sdk-v0.42`), with the current player embedded:

```
prospero-clang -Os -Wall -I. -Icore -I$SDK/target/include \
  -o launcher.elf prospero_media_launcher.c core/hbldr.c core/elfldr.c core/pt.c \
  -lSceIpmi -lSceUserService -lSceAppInstUtil -lSceSystemService -lSceNet -lpthread

launcher.elf    34,317,784 bytes   (ET_DYN — fine, elfldr wants PIE)
uninstall.elf      111,104 bytes
```

Every SCE symbol the launcher needs is present in the SDK v0.42 stubs —
`sceAppInstUtilAppUnInstall`, `sceSystemServiceLaunchApp`,
`sceSystemServiceGetAppIdOfRunningBigApp`, `sceUserServiceGetForegroundUser`,
`sceUserServiceTerminate`, `sceKernelGetAppState`,
`sceKernelSendNotificationRequest`. Note that
[sdk-audit.md](sdk-audit.md) lists `libSceUserService` as having "no
`Terminate`" — that is stale for v0.42; the stub exports it.

The launcher ELF is player-size plus ~400 KB, because the player is embedded
whole. That is a 34 MB payload over elfldr on 9021 each boot.

## 5. What was done

The identity work is complete. EVO Player registers its **own** title and
coexists with a ProsperoPlayer install rather than replacing it — upstream's
`PRSP10001` is never touched by anything here.

| | ProsperoPlayer | EVO Player |
|---|---|---|
| Title ID | `PRSP10001` | **`EVOP10001`** |
| Loopback port | 9055 | **9056** |
| Runtime dir | `/data/homebrew/EVOPlayer` | **`/data/evoplayer/app`** |
| Tile name | ProsperoPlayer 1.0 | **EVO Player** |

Specifically:

- **Title id and port changed** in all three places that carry them —
  `prospero_media_launcher.c`, `core/hbldr.c` (the system host param), and
  `prospero_uninstall_tile.c`.
- **The runtime dir moved off `/data/homebrew`.** The tile keeps its own copy
  of the player. Sharing `/data/homebrew/EVOPlayer` with
  `install-homebrew.sh` would mean the tile silently overwriting a freshly
  installed dev build, with no way to tell which binary just launched.
- **`PP_LEGACY_TITLES` is empty** and `PP_TITLES` in the uninstaller holds only
  `EVOP10001`. Both previously listed Prospero's experiment ids, which would
  have made our installer uninstall somebody else's titles.
- **The uninstaller no longer wipes `/data/homebrew/EVOPlayer`** — removing a
  tile must not break the websrv development loop.
- **A real icon.** `tools/gen_app_icon.py` renders a 512×512 `icon0.png` from
  vector shapes using the same signed-distance machinery as `gen_icons.py`,
  in the UI accent. Upstream's branded artwork is gone, and the icon can be
  restyled by editing the generator rather than by finding the artist.
- **The Makefile matches its own sources** — it depended on
  `assets/ProsperoPlayer.elf` while the code `.incbin`s `assets/EVOPlayer.elf`,
  so `make` could never have worked. A missing player now produces an
  explanation instead of an assembler error.
- **`scripts/build-media-tile.sh`** stages the player, builds both payloads
  (uninstaller **first**), copies them to `output/elf/`, and greps the linked
  binary to prove `EVOP10001` reached it and `PRSP10001` did not.
- **`PP_VERSION` is `0.2.0`**, matching `projects/evoplayer/VERSION`. It is
  still a separate constant — a real fix would generate it.

```bash
./scripts/build-media-tile.sh              # build both payloads
./scripts/build-media-tile.sh --install    # register the tile
./scripts/build-media-tile.sh --uninstall  # remove it again
```

Both payloads build clean and were verified by string-extraction from the
linked ELFs: the launcher carries `EVOP10001`, port 9056 and
`/data/evoplayer/app`; neither binary contains `PRSP10001` or `PSMC00002`.

## 6. Risks, honestly

This route writes to places the homebrew route never touches, and the failure
modes are console-level, not app-level:

- **`/system_ex` is remounted read-write** to plant the host. Upstream's own
  README warns that older experimental installers "can corrupt `app.db` or leave
  lock / disc-like entries", and the current launcher carries cleanup code for
  two of its own earlier mis-registered ids (`PRSP00001`, `PSMC00002`). A
  half-finished registration is a ghost tile you then have to hunt.
- **Uninstall must be built and tested at the same time as install**, not after.
  If Options → Delete is hidden and the launcher is not resident, the one-shot
  `uninstall.elf` is the only way back — it needs to exist and be known-good
  before the first install is attempted, not written in response to a stuck tile.
- **The BigApp process is killed and replaced** on every launch
  (`sceSystemServiceKillApp` in `pp_stop_running_bigapp`). Testing this means
  repeatedly killing a foreground app — see the relaunch-safety note; do not
  stack launches.
- **Every install attempt is a hardware round trip.** There is no way to test
  app.db registration on the desktop.

The identity work in §5 removes the collision risk, but **none of the console
risk above is retired by it** — that only happens on first install.

The order that remains: install once, confirm the tile appears and launches,
then immediately prove `--uninstall` removes it completely
(`/user/app/EVOP10001` and `/system_ex/app/EVOP10001` both gone over FTP)
before relying on it. `EVOPlayer_UninstallTile.elf` is built first by the
script, and unconditionally, precisely so it exists before it is needed.

## 7. Recommendation

Keep `--format homebrew` as the release and development path — it is
zero-risk, it is what the ecosystem expects, and it is where the fast
edit-build-launch loop lives.

Treat the Media tile as an **opt-in extra**, exactly as upstream does. It is now
built and branded; what remains is a careful first hardware test with the
uninstaller in hand.

## References

- `projects/evoplayer/prospero_media_standalone/` — the launcher, uninstaller, and loader core
- `projects/evoplayer/docs/PROSPERO_MEDIA_LAUNCHER.md` — upstream's own write-up of the technique
- [packaging.md](packaging.md) — the three distribution routes and why fake-SELF rejects our ELF
- [sdk-audit.md](sdk-audit.md) — SCE stub inventory
- `scripts/install-homebrew.sh` — the current launch path, and why hbldr rather than elfldr
