# Nuvio on EVO: setup and first run

How to build, install and configure the native Nuvio provider, and what to
check on the first console run. Design: [nuvio-native-provider.md](nuvio-native-provider.md).

> **Firmware.** EVO is developed and verified on 12.70. This branch moves the
> SDK to v0.43, the first release with the 13.60 kernel offset table, but
> nothing here has run on 13.60 yet. Do step 0 before anything else.

## 0. Check that stock EVO runs on your firmware

Before building anything, install the latest **release** `PPSA99039.ffpfsc`
(README "Install") and play a local file from USB. If that fails on your
firmware, the problem is EVO on that firmware (hardware decoder, AGC, the
jailbreak daemon), not Nuvio, and it has to be solved first. Keep
`/mnt/usb0/evo.log` from that run either way.

## 1. Build

Everything toolchain-related runs in the pinned Docker container
([../build/building.md](../build/building.md)). Install Docker Desktop with
the WSL 2 engine, then from the repo root:

```bash
docker compose build              # new image tag: llvm18-sdk-v0.43
tests/run_tests.sh                # host tests (inside the container, or WSL)
docker compose run --rm ps5-dev bash -lc './scripts/package-app.sh --ffpfsc'
```

The image is `output/app/PPSA99039.ffpfsc`. `docker compose run --rm ps5-dev
bash -lc './scripts/setup-sdk.sh'` should report both the 12.70 and 13.60
offset tables.

## 2. Install

1. **Close EVO first** with Settings → System & Diagnostics → QUIT EVO.
   Deploying over a running instance, or stacking a second launch, has
   kernel-panicked consoles.
2. Put your console's address in `.env` (`PS5_HOST=192.168.x.y`), then:
   ```bash
   docker compose run --rm ps5-dev bash -lc './scripts/deploy-app.sh --ffpfsc'
   ```
   Or copy the `.ffpfsc` to `/data/homebrew/` yourself over FTP on port 2121.
3. ShadowMountPlus remounts it; launch EVO Player from the Games row.

## 3. Configure

All files are plain text in `/data/evoplayer/providers/nuvio-native/`, which
EVO creates on first launch. Edit them over FTP with EVO closed.

**Addons** (`addons.txt`): one manifest URL per line, in the order you want
them. `stremio://` links work too. Without the file you get Cinemeta only,
which has catalogs but no streams.

```
https://v3-cinemeta.strem.io/manifest.json
https://torrentio.strem.fun/<your config>/manifest.json
```

A single addon can also be added on the console: open Nuvio, press OPTIONS
and type the URL. Square on Nuvio in the provider chooser does the same.
Typing `reset` goes back to Cinemeta only.

**Debrid**: in the provider chooser (the rail's provider slot), highlight
Torbox or Real-Debrid and press X, then type the API key. It is saved to
`/data/evoplayer/torbox.conf` or `realdebrid.conf`. Torrent streams show
"torrent: set up Torbox or Real-Debrid" until one is enabled. Only already
cached torrents play.

**Nuvio account** (`account.conf`, optional): pulls your addon list and
syncs watch progress with the account.

```
server=https://your-nuvio-server.example
email=you@example.com
password=...
profile=1
```

`server` must be a Nuvio server that publishes `/.well-known/nuvio` (a
self-hosted one). For any other Supabase backend, use `backend_url=` and
`publishable_key=` instead of `server=`. The password is replaced by a
refresh token after the first sign-in. If sign-in fails, `evo.log` has a
`nuvio account:` line saying why.

**Skin** (`bundle.txt`, optional): on your PC, not in the container, run
`./tools/provider-server.sh`. Put the printed LAN URL for
`nuvio-native/manifest.json` in `bundle.txt`. Without it, Nuvio uses EVO's
plain built-in list skin.

## 4. Use

| Where | Button | Does |
|---|---|---|
| anywhere | X | open / play |
| anywhere | O | back one level |
| anywhere | Square | search every searchable catalog |
| anywhere | OPTIONS | keyboard: add an addon URL |
| streams | X | play that stream; torrents go through your debrid service |

The streams list is the stream picker: best first, one row per stream, with
addon, quality, size and whether it goes through debrid. Stopping playback
saves your position. The title then appears under Continue Watching and
resumes where you stopped.

## 5. First-run checklist

Pull the log after each run with `tools/evo-remote.sh log`, or FTP
`/mnt/usb0/evo.log`. The provider lines start with `provider:`.

| # | Do | Expect in `evo.log` |
|---|---|---|
| 1 | launch | `registry up: 7/7 registered`, `nuvio: init N addons` |
| 2 | open Nuvio | `nuvio: root -> N rows` |
| 3 | open a catalog | poster rows, no `catalog … failed` |
| 4 | open a film | `asking N addons for streams`, one `-> K streams` line per addon |
| 5 | play a direct (http) stream | `nuvio: resolve x:… -> direct (<addon>)`, then playback |
| 6 | play a torrent stream (debrid set) | `torbox: resolve ok (<file>)` or `realdebrid: …` |
| 7 | stop halfway, reopen Nuvio | a Continue Watching row, and resume at that point |
| 8 | account set | `nuvio account: signed in`, `N addons from the account`, `pulled N progress entries` |

If a stream is listed but will not play, note its subtitle ("may not play
(needs headers)" is a known gap) and the `evo.log` lines around the attempt.
