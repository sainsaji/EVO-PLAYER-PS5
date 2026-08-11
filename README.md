# EVO Player

**A media player for jailbroken PS5 on firmware 12.70.**

Plays your video off a USB stick, with surround sound, subtitles, resume, and
an interface built for a controller and a television rather than a mouse.

![EVO Player launch screen](docs/images/launch.png)

---

## Install

Grab the latest **[release](https://github.com/sainsaji/EVO-PLAYER-PS5/releases/latest)**
and take either route:

**From the home screen** — download `EVOPlayer-*-homebrew.zip`, extract it to a
USB stick as `/homebrew/EVOPlayer/`, and launch it from the websrv launcher at
`http://<your-ps5>:8080`. Build the home-screen tile with
`./scripts/build-media-tile.sh --install` and it appears in the console's own
Media row, no browser needed.

**You will need**

- A PS5 on firmware **12.70**, jailbroken — the jailbreak must be re-run after
  every reboot
- `ps5-payload-elfldr` and `ps5-payload-websrv` running on the console
- A USB stick, formatted exFAT or FAT32, with your media on it

Put video in any folder on the stick. EVO Player reads `/mnt/usb0`.

---

## What it does

### Browse your library

Folders first, then A–Z. The inspector shows codec, resolution, size and
length before you commit to opening anything, and files you have started show
how far in you got.

![Browsing a folder](docs/images/browse.png)

Hold a direction to scroll, shoulder buttons to page, triggers to jump A–Z.
TRIANGLE favourites a file; SQUARE opens full details.

### Pick up where you left off

The launch screen leads with whatever you were last watching, and the shelf
below it holds the rest of your recent files with real cover art pulled from
the video itself.

![Recent files](docs/images/recent.png)

### Subtitles, chosen by what is in them

Tracks are ranked by how many cues they actually contain, not by what the
metadata claims — so the real subtitle track is at the top and a signs-only
track is labelled as one instead of being picked by accident.

![Subtitle track picker](docs/images/picker.png)

Press DOWN during playback. Language names come from the track's language code,
50 of them mapped. L2 and R3 nudge subtitle timing if a file is out of sync.

### Read text files

Open a `.txt`, `.log`, `.md`, `.nfo`, `.srt` or `.json` and it opens in a
reader rather than doing nothing.

![Text reader](docs/images/reader.png)

D-pad scrolls, shoulders page, triggers jump a tenth of the file, TRIANGLE
changes text size and keeps your place. Curly quotes, em dashes and accented
letters are folded to what the font can draw, so text from the web reads
properly.

### Know what you are playing

![Media info](docs/images/mediainfo.png)

### Make it yours

![Settings](docs/images/settings.png)

Four built-in themes, plus any `.theme` file you drop in `/mnt/usb0/evo_themes`
— see [docs/theming.md](docs/theming.md). Navigation sounds and a lightbar
tinted to the theme, both of which can be turned off.

---

## Controls

| | Browsing | Playing |
|---|---|---|
| **CROSS** | open | pause / resume |
| **CIRCLE** | back | stop, after confirming |
| **TRIANGLE** | favourite | view mode: fit / fill / stretch |
| **SQUARE** | file details | — |
| **D-pad / stick** | move, hold to scroll | — |
| **LEFT** | open the side rail | — |
| **DOWN** | — | choose a subtitle track |
| **L1 / R1** | page | skip a chapter, or ±60s |
| **L2 / R2** | jump A–Z | subtitle timing |
| **L3** | — | save a screenshot to USB |

CIRCLE asks before it stops playback — it is the same button that means "back"
everywhere else, and it used to throw away your place in the file on one press.

---

## Performance

Playback does less work per frame than it used to. Every frame is converted
from the decoder's format and rearranged into the layout the console's display
hardware wants, and both steps got faster:

| per frame | before | after |
|---|---|---|
| 4K conversion | 10.3 ms | **8.8 ms** |
| 1080p conversion | 3.0 ms | **2.1 ms** |
| 1080p drawing | 2.9 ms | **2.2 ms** |

The change that probably matters most is not in that table. The step that
rearranges each frame used to create and destroy twelve threads **every frame**
— 720 a second at 60fps — which is the pattern behind the stalls that show up
as an occasional hitch rather than as a lower frame rate. It reuses a pool now.
The player also stopped painting two million pixels black immediately before
covering every one of them with video.

> These are measurements from a development PC, taken with `./tools/bench.sh`,
> which hashes the output and refuses to report a timing if the picture
> changed. They compare the changes against each other; they are not a
> prediction of frame rate on your console, and the improvements have not yet
> been measured on hardware.

## Good to know

- **Resume** is offered when you reopen a file you did not finish.
- **Screenshots** go to the USB stick. The websrv `/fs` endpoint is read-only,
  so delete them from the console's own web terminal or by pulling the drive.
- **4K** takes its fastest path on 8-bit SDR up to 30fps. Larger or 10-bit 4K
  still plays, on a slower path.
- **10-bit files play**, converted down to 8-bit — the console's display plane
  here is 8-bit, so the extra precision cannot be carried through. Fine on most
  material; a smooth gradient like a sky or a fade can show banding.
- **HDR is not supported.** PQ and HLG content plays but is shown as SDR
  without tone mapping, so it will look washed out or dark.
- Media stays on your stick. Nothing is copied to the console, and nothing
  leaves your network.

---

## Building it yourself

Everything builds in a pinned Docker container — no toolchain to install:

```bash
git clone https://github.com/sainsaji/EVO-PLAYER-PS5
cd EVO-PLAYER-PS5
echo "PS5_HOST=192.168.0.10" > .env      # your console's IP

docker compose build                     # one-time, ~10 min
docker compose run --rm ps5-dev ./scripts/build-evoplayer.sh
```

The full setup — Windows host, the SDK, FFmpeg, deployment, and why each
version is pinned where it is — is in **[docs/building.md](docs/building.md)**.

The interface can be rendered on your PC without a console at all, which is how
most of it was built:

```bash
./tools/uiview.sh --all          # every screen, to output/uiview/
```

Every screenshot on this page is one of those renders.

### Documentation

| | |
|---|---|
| [docs/building.md](docs/building.md) | full build and deploy guide |
| [docs/architecture.md](docs/architecture.md) | how the code is laid out |
| [docs/ui-handoff.md](docs/ui-handoff.md) | the UI layer, and working on it |
| [docs/converter-perf.md](docs/converter-perf.md) | the video path, measured |
| [docs/hardware-decode.md](docs/hardware-decode.md) | the plan for getting decode off the CPU |
| [docs/hardware-decode-review.md](docs/hardware-decode-review.md) | a critical review of that plan |
| [docs/hardware-decode-findings.md](docs/hardware-decode-findings.md) | **what is known** about the PS5 media stack — module map, import graph, recovered API and ABI |
| [docs/hardware-decode-next-steps.md](docs/hardware-decode-next-steps.md) | **what to do next**, as phases 4–10, with the go/no-go points |
| [docs/theming.md](docs/theming.md) | writing a `.theme` file |
| [docs/tooling.md](docs/tooling.md) | the scripts, and what each is for |
| [CHANGELOG.md](CHANGELOG.md) | what changed, per release |

---

## Credits and licence

Forked from [ProsperoPlayer](https://github.com/KINGDKAK/ProsperoPlayer) by
KINGDKAK, and licensed **GPL-3.0-or-later** — see [COPYRIGHT.md](COPYRIGHT.md).

- [ps5-payload-dev](https://github.com/ps5-payload-dev) (John Törnblom) — SDK,
  elfldr, websrv, pacbrew-repo
- [KINGDKAK](https://github.com/KINGDKAK) — ProsperoPlayer
- [zecoxao/sce_symbols](https://github.com/zecoxao/sce_symbols) — NID database

EVO Player is independent homebrew, **not affiliated with Sony or
PlayStation**. Use at your own risk, with media you own. No Sony code, binaries
or keys are included in or required by this repository.
