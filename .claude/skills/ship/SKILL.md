---
name: ship
description: Build, deploy, launch, drive or screenshot EVO Player on the PS5, or render its UI on the host. Use whenever asked to run / launch / deploy / install / screenshot / hardware-test the app, verify a change on real hardware, or do a console session. ONE hardware path - the PPSA99039 app module (.ffpfsc) - plus the host UI renderer. Has the exact command loop, so do not re-derive it.
---

# Shipping EVO Player

Everything re-execs through the Docker container (`ps5-dev`). **Never call `make`.**
Never stack launches, never deploy over a running EVO. Reference: `docs/build/tooling.md`.
Run the console loop yourself (CLAUDE.md); do not hand testing to the user.

## 1. Pick the route

| Change is about | Route |
|---|---|
| UI layout / look / text rendering | Host renderer `./tools/hui_preview.sh` (kit screens -> `output/uiview/hui_*.png`) or `./tools/uiview.sh --all` (RmlUi). No console. |
| Decode, GPU, audio, `/data`, input, anything needing hardware | App module loop below |
| Compile check only | `docker compose run --rm ps5-dev ./scripts/build-evoplayer.sh` |

The host has no compiler. Anything that compiles C/C++ (even a one-off test) runs in the container:
`MSYS_NO_PATHCONV=1 docker compose run --rm ps5-dev bash -c 'cd output/x && clang-18 ...'`.

## 2. The hardware loop (copy this)

```bash
# build (5-10 min): run in the BACKGROUND, you are notified on exit
docker compose run --rm ps5-dev ./scripts/package-app.sh --ffpfsc --usb-remote > output/pkg.log 2>&1
grep -n "error:" output/pkg.log; tail -3 output/pkg.log

# cycle (one step at a time, stop at the first anomaly, never auto-retry)
timeout 100 ./tools/evo-remote.sh quit     # expect "ok parked"
timeout 100 ./tools/evo-remote.sh close    # expect "ok closed"
docker compose run --rm ps5-dev ./scripts/deploy-app.sh --ffpfsc   # sha256-verified
timeout 200 ./tools/evo-remote.sh launch   # waits for ShadowMount idle
```

- Status line: `evo-remote.sh status` (`build=... fatal=0 parked=0`). Log: `evo-remote.sh log | grep -i hui`.
- A tool call blocked by the user = they want to drive that part; wait for their word.
- Generated `projects/evoplayer/ui_rml/src/evo_rmlui_bundle_data.cpp` is tracked and regenerates on package (huge diff, expected).
- Adding a source file: list it in `projects/evoplayer/Makefile` (media/*.c and ui_kit/*.cpp are listed by hand, not globbed).
- Assets under `projects/evoplayer/assets/{rml,fonts,icons,wallpaper,hui}` are embedded automatically at package time.

## 2b. Issue to done (checklist)

1. State "agy: yes/no, because ..." in the first reply on the issue (CLAUDE.md).
2. Mechanical parts go to agy (`tools/agy-run.sh`); review its report and `git diff`, re-run its checks.
3. Build: compile check, then `package-app.sh --ffpfsc --usb-remote` when the change needs the console.
4. Host tests: `docker compose run --rm ps5-dev ./tests/run_tests.sh`. A new setting also extends the settings round-trip test.
5. Console check: `evo-remote.sh verify --check <name>` (add a script in `tools/checks/` if the feature has none), or the manual loop in section 2.
6. One commit, only after 3-5 pass. Bump `contentVersion` on a release.
7. Close the issue with the evidence: the PASS line, the commit hash, and what was not verified.

## 3. Drive and look (no user needed)

```bash
# several keys in ONE container start (each start costs seconds)
docker compose run --rm ps5-dev bash -c 'for k in down down right cross; do ./tools/evo-remote.sh key $k >/dev/null 2>&1; sleep 2; done; ./tools/evo-remote.sh ui --line 2>&1 | tail -1'
```
- `ui --line` = focused element + screen as text; use it to navigate. Keys: up down left right cross circle square triangle l1 r1 l3 ...
- **Jump to any screen by name:** `evo-remote.sh go <home|browse|internal|favorites|recent|providers|settings[-audio|-playback|...]|about|changelog|themes|devtools|<provider id>|<ScreenId>>`; works from any screen, no key walk. A provider (about 10 s to load its shelves) can also be opened with `evo-remote.sh provider <id>`. Fill the search keyboard with `key square` then `type --submit <text>` instead of walking the grid.
- **Change a setting without the D-pad walk (#201, `--usb-remote` builds):** `evo-remote.sh set <name> <value>` writes it, applies it live and prints it back (`set audio-delay +100`); `set --list` shows every name and its values; `focus <row>` opens the section and puts the cursor on the row.
- **Whole check in one command:** `evo-remote.sh verify [--check <name>]` = soft close, close, deploy, launch, run `tools/checks/<name>.sh`, quit, close, then PASS/FAIL with the evidence line. No `--check` runs every script in `tools/checks/`. Use `--no-build` to reuse the last `.ffpfsc`.
- Home -> PROVIDERS by keys (only if you need the chooser itself): `down down right right right cross`. `r1`/`l1` flip pages.
- **Screenshot** (`key l3` writes `/mnt/usb0/evo_shot_NNN.bmp`, 3840x2160, numbers wrap at 32). `shot.sh grab` often fails (needs the web server); use FTP:
  `tools/grab_shot.sh <name>` (presses l3, finds the shot EVO just wrote - `/mnt/usb0`, or `/data/evoplayer` with no stick - from its log line or the FTP listing, writes `output/screenshots/<name>.png` at 1920x1080; host or container), then Read the PNG. Only for pixel questions; use `ui` for focus/text.

## 4. Test data on the console

- FTP is `ftp://192.168.0.12:2121` (PS5_HOST in `.env`). Config files: `/data/evoplayer/*.conf`. Fonts: `/data/evoplayer/fonts/` (CJK Noto OTFs are there). Read-only web `/fs`; delete over FTP.
- IPTV test playlist: serve any `.m3u` from the HOST (not the container) with `python -m http.server <port> --bind <host LAN ip>`; write `playlist=http://<ip>:<port>/x.m3u` to `/data/evoplayer/iptv.conf` by `curl -T`. **Port 8099 is ignored by the provider on purpose**; use another. Restart EVO (quit/close/launch) to reload the conf. Back up the original conf first (`curl ftp://.../iptv.conf`) and restore it when done.
- Provider setup is in `/data/evoplayer/*.conf`; Xtream/Jellyfin/Nuvio are already configured on the dev console.

## 5. Rules that bite (details in CLAUDE.md)

- Quit -> close -> deploy -> launch. `close` only after `parked=1`. Never `--force` past the deploy interlock.
- A static `evo.log` proves nothing (idle heartbeat ~6 min).
- Bash heredocs with backslashes or feeding python are blocked by a hook: use the Write tool for scripts, or the Edit tool; run with `python -X utf8 file.py`. `sed -i` with `\n` replacements in Git Bash is fragile: use Edit.
- Kit (`third_party/ps5-homebrew-ui`) is a git submodule: its changes are separate commits; EVO's `ui_kit/` and `tools/hui_preview/` are in the main repo.
- No commits until the hardware check passes; one clean commit (memory: no-commits-until-verified).
- Reply to the user short: result first, then next step.

## 6. Font / text pipeline (what exists, #128)

- Kit text: baked Inter/Montserrat/DejaVu atlases (ASCII) -> `noto-fallback.huifont` (Latin ext, Greek, Cyrillic, Hebrew, Arabic, symbols; built by `third_party/ps5-homebrew-ui/tools/font-baker/bake_fallback.cpp`) -> `ComplexText` (`ui_kit/src/evo_hui_complex.cpp`: HarfBuzz + stb SDF, Thai/Indic embedded, CJK from `/data/evoplayer/fonts`).
- Arabic joining and bidi order live in `third_party/ps5-homebrew-ui/src/gfx/font.cpp` (`visual_codepoints`).
- Subtitle sidecars: `media/src/evo_charset.c` (UTF-16 + legacy code pages; tables from `tools/gen_charset_tables.py`). Native language names: `media/src/evo_langname.c`.
- Host preview has no HarfBuzz: Thai/Indic/CJK only show on the console.

## 7. If text comes out as OTHER strings

After a build, strings that show as OpenSSL names / OIDs, missing recents: check `readelf -lW output/app/.build/eboot.elf` - the R LOAD file offset must be a multiple of 64 KB (package-app.sh now dies if not; see tools/native-app/ps5-pie.ld). Do not blame the fonts or the image first.
