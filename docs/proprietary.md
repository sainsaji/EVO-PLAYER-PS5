# Proprietary files

**Nothing in `proprietary/` is ever committed.** It is excluded by
`.gitignore`, and so are `*.sprx`, `*.self`, `*.pkg`, `*.pup`, `*.sfo`, `*.key`
and `*.pem` wherever they appear in the tree.

Do not commit, and do not paste into issues:

- Sony `.sprx` / `.self` system modules, decrypted or otherwise
- Any part of the official Prospero SDK, including `prospero-pub-cmd`
- Console dumps, kernel dumps, `pfsSKKey` material
- Private keys, certificates, credentials, PSN account data

This repository builds entirely from open sources. Nothing in the normal build
([build/building.md](build/building.md)) needs a proprietary file. Proprietary material is
only ever needed for *optional* research paths, and each of those fails loudly rather than
silently substituting something wrong.

## Layout

```
proprietary/
├── README.md          committed - explains the layout
├── sprx/              your decrypted Sony modules      (ignored)
├── tools/             licensed Sony tooling            (ignored)
└── keys/              never share these                (ignored)
```

## Calling a Sony module

EVO does not need a Sony module on disk to call one. System PRXs the SDK has no link stub
for are imported **by name**: a plain list of exported symbols in
`tools/native-app/stubs/prx/*.syms` becomes a stub library, and the converter derives the
NID from the name and emits a real PS5 import, so the loader maps the module from the
console's own firmware at start ([architecture/app-module.md](architecture/app-module.md#system-prx-imports)).
Only modules under `/system/common/lib` can be imported this way.

The SDK's own route, generating a stub `.so` from a decrypted `.sprx` with
`sce_stubs/genstub.py` and `aerolib.csv`, still exists. If you ever use it, the module
must come from **your own console**, live in `proprietary/sprx/`, and never be committed.
The generated `.c` holds only symbol names, but the `.sprx` it came from stays out of the
repository regardless. Symbols whose NID is not in `aerolib.csv` come out as raw NID strings,
which is a naming gap, not a failure.

## Licensed Sony tooling (fPKG signing)

True signed `.pkg` creation needs `prospero-pub-cmd` from the official Prospero
SDK. If you hold a legitimate licensed copy:

```
proprietary/tools/prospero-pub-cmd
```

`scripts/package-pkg.sh` (an unused leftover) checks for it and **refuses with a clear
message** when it is absent. EVO ships as a `.ffpfsc` image ([build/packaging.md](build/packaging.md)),
which does not need it at all.

## Fail loudly, never substitute

An absent optional proprietary dependency must produce a clear error, not a silent
fallback. `proprietary/tools/prospero-pub-cmd` missing makes `package-pkg.sh --format pkg`
exit with an explanation; `proprietary/` missing entirely changes nothing about the build.

## Third-party open-source code in the build

Not proprietary, but recorded here so the licences are in one place.

| Component | Where | Licence | Notes |
|---|---|---|---|
| AMD FidelityFX Super Resolution 1.0 (EASU + RCAS) | ported to GLSL in `tools/gen_upscale_pipes.py` → `projects/evoplayer/shaders/agc/upscale_{easu,rcas}.pipe` | MIT, © Advanced Micro Devices | #103 Sharp upscaler. A port of `ffx_fsr1.h`'s float path, not a copy of the header |
| ps5-homebrew-dev-protocol launch/close controllers | `tools/ps5-controllers/` (vendored unchanged, with `COPYING`) | GPL-3.0-or-later, © BlackBearReloaded; adapted from shsrv/websrv © John Törnblom | Dev tooling only: standalone ELF payloads sent to elfldr by `evo-remote.sh`. Never linked into EVO or shipped in the `.ffpfsc` |
| Anime4K `Upscale_CNN_x2` S and M | `third_party/anime4k/` (vendored unchanged, with its `LICENSE`) → `upscale_a4k_*.pipe` | MIT, © bloc97 | #103 AI upscaler. The network weights are the literal constants in those mpv hooks |
| Noto Sans Thai; Noto Sans CJK SC/JP/KR (subset OTF) | Thai in `assets/fonts/NotoSansThai-*.ttf` (embedded); CJK is not embedded (4-8 MB a script): `/data/evoplayer/fonts/NotoSans{SC,JP,KR}-Regular.otf`, loaded when present | SIL OFL 1.1, © The Noto Project Authors | Subtitle fallback faces in RmlUi (`evo_rmlui_app.cpp`) for scripts the baked UI fonts lack |
| Noto Sans (Latin, Greek, Cyrillic), Arabic, Hebrew, Symbols, Symbols 2; Noto Sans Devanagari, Bengali, Tamil, Telugu, Malayalam | Baked into `assets/hui/fonts/noto-fallback.huifont` (source TTFs in `third_party/ps5-homebrew-ui/third_party/fonts/`, licence `Noto-LICENSE.txt` beside the atlas); the Indic faces are embedded whole in `assets/fonts/NotoSans{Devanagari,Bengali,Tamil,Telugu,Malayalam}-Regular.ttf` | SIL OFL 1.1, © The Noto Project Authors | #128 fallback font and run-time shaping for the kit screens (`ui_kit/src/evo_hui_complex.cpp`) |
| Provider logos: Jellyfin, Emby, Stremio | `assets/icons/brand_{jellyfin,emby,stremio}.png`, rendered from Simple Icons 13.21.0 SVGs in each brand's colour | Path data CC0-1.0 (Simple Icons); the logos are the trademarks of their owners | Shown only to name the service in the provider chooser and header. `brand_plex.png` is a plain chevron drawn for EVO in Plex's colour (Simple Icons only has the wordmark) |
| Nuvio logo mark | `assets/icons/brand_nuvio.png`, cut from `assets/nuvio/assets/brand/app_logo_mark.png` (the bundled NuvioTVSmart build) | As the bundled Nuvio app; the mark is Nuvio's | Same use as above |
| Mozilla CA root certificates | `projects/evoplayer/assets/certs/cacert.pem` (curl.se's PEM extract of Mozilla's NSS store, unchanged, checked against its published SHA-256) | MPL-2.0, © Mozilla | TLS certificate verification in `evo_net.c` (0.14.1). Refresh from https://curl.se/ca/cacert.pem |
| ps5-homebrew-ui | `third_party/ps5-homebrew-ui/` (cloned unchanged, own `LICENSE`); compiled into the app via `ui_kit/`; shader ported to `ui_sdf.pipe` | GPL-3.0-or-later, © BlackBearReloaded | EVO's own screens ([ui/hui-kit.md](ui/hui-kit.md)). Bundled from it: the Inter, Montserrat and DejaVu Sans Mono baked fonts (`assets/hui/fonts`, each with its licence file) and the "glass" sound effects (`assets/hui/sfx`, the author's own, project licence). Third-party notices: `third_party/ps5-homebrew-ui/THIRD_PARTY_NOTICES.md` |
| libsmb2 | Linked from PS5 Payload SDK sysroot (`/opt/ps5-payload-sdk/target/user/homebrew/lib/libsmb2.a`) | LGPL-2.1-or-later, © Ronnie Sahlberg | SMB client library used for Samba and Windows network share browsing and AVIO streaming (`evo_smb_io.c`, `SmbClient.cpp`) |

Live-action network (#123), tried 2026-10-08 and not shipped. FSRCNNX x2 8-0-4-1
(igv/FSRCNN-TensorFlow 1.1) has LGPL-3.0-or-later weights, which fit EVO's
GPL-3.0; the training data is undocumented. It translated to 13 conv passes and
ran on the PS5 Pro at about 3.2 ms per 1080p to 4K frame, but rendered
tile-sized black holes. Suspected cause: EVO's between-pass barrier
(RELEASE_MEM, no wait) lets the next pass read before the last one finishes. A
fix needs a new GPU sync call (AcquireMem), which is a panic risk. Also
checked: ArtCNN (MIT, anime-aimed), CuNNy (LGPL-3.0, visual novels), RAVU
(LGPL-3.0 at repo level, per-file headers unchecked).

## Sample photos in the Settings previews

`projects/evoplayer/assets/icons/preview_up_*.png` and `preview_hdr_*.png` are
derived from NASA images (public domain; NASA does not endorse EVO Player):
the Lagoon Nebula (Hubble, `GSFC_20171208_Archive_e001955`) for the upscaling
comparison and an ISS photograph of sunglint over the ocean (`iss031e035310`)
for the SDR / HDR comparison. The softer upscaling versions and the SDR / HDR
looks are simulated from the one photo; they illustrate the effect, they are not
captures of EVO's output. `preview_controller.png` is original artwork rendered
from `tools/preview-art/controller.svg` (`tools/preview-art/render.sh`).

## If you commit something by accident

A `git rm` is not enough — the blob stays in history.

```bash
# Stop immediately; do not push.
git rm --cached path/to/file
# Then purge it from history (rewrites commits):
git filter-repo --path path/to/file --invert-paths
```

If it was already pushed to a shared remote, treat the material as disclosed:
rotate any keys and assume the file is public.
