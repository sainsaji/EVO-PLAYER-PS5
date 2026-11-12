# Per-video HDR/SDR and refresh-rate switching (research, FW 13.60)

**Status: not solved** (#244). EVO cannot yet switch the TV between SDR and HDR per
video, or match 23.976 / 24 Hz, the way the Blu-ray player and the official
YouTube app do. This page records every test made on 2026-11-07/08 so the next
person starts where this stopped. Older 12.70 work on frame rates is in
[refresh-rate-modes.md](refresh-rate-modes.md).

Test setup: PS5 Pro, FW 13.60, system setting "HDR: On When Supported", LG
OLED (TV picture mode readout used as the ground truth), EVO 0.13.5 app module
`PPSA99039` from ShadowMount+.

## How HDR works in EVO today

- `param.json` `attribute` bit `0x20000000` marks the app HDR-capable. With it,
  the PS5 puts the HDMI output in HDR for the **whole session**, menus
  included. Without it, EVO runs SDR and the PS5 refuses the HDR10 buffer
  attribute (`0x80290003`). Bits `0x02000000` and `0x40000000` change nothing.
- Inside the session EVO retypes its scanout buffers (HDR10
  `Bgr10A2Bt2100Pq` / SDR `Bgra8`, `SubmitChangeBufferAttribute2`, rc=0). The
  HDMI signal stays HDR either way: `sceVideoOutGetOutputStatus` keeps
  `dynamic_range=2`, and the TV stays HDR.
- So the honest choice today is HDR held for the session (current builds) or an
  SDR-only build without the flag.

## Who decides: the shell's AvControl

klog at every launch (record it with `tools/klog.sh` before launching):

```
[AvControl] -- app[0](appid=0x2019 attr=0x8360197)(MEDIA RUNNING)(HDR:o HFR:x VR:x VRR:x 8K:x) ...
[AvControl]  video: port:HDMI 3840_2160P_5994 RGB444 limited RGB2100_PQ 36bpp 16:9 game ...
```

The HDMI mode (range, refresh, HFR) is a per-app policy that AvControl takes
from the app's registered attributes when the app starts. A running app does
not choose it with the buffer attribute.

## What the Blu-ray player does

`/system_ex/app/NPXS40140/UHDBdPlayerCore.elf` (decrypted with ftpsrv `SELF`),
switch code at `0x18c200..0x18c400`:

1. read the output status (unnamed `y4l-VBLsNio`), then wait up to 30 s for a
   previous change (`d7eh3psk1J8`, `q9HFykGcuWU`)
2. `sceVideoOutInitializeOutputOptions`, `sceVideoOutSetContentType(0)`, flag
   setter `yoK0AjkkBkI(0xb)`
3. `sceVideoOutSysSetHdrMetadata` (HDR content only)
4. `sceVideoOutConfigureOutputEx(handle, mode, options, NULL, 0)`, mode e.g.
   `0xc00000802` (HDR 4K 23.976)

It imports no AvSetting function and never calls the VideoOut capability
setter `Jl91AhxKc0w`, so its right to use the option flags comes from the
process, not from a call. Mode values: [refresh-rate-modes.md](refresh-rate-modes.md#mode-values).

## Tests and results

| test | result |
|------|--------|
| Media category: `applicationCategoryType` 65536, `contentBadgeType` 2, no `gameIntent`, `attribute3` 0x100000 (ProsperoTV's), registry edited to match | launches as a media app (`sandboxType=0x2`, `MEDIA RUNNING`). Without removing `gameIntent`: CE-105760-9 |
| Media category + `--pro-memory` (`attribute3` becomes 0x500000, registry still 0x100000) | CE-105760-9. Image and registry must match |
| `ConfigureOutputEx` as a game | `0x80290016` |
| `ConfigureOutputEx` as a media app, current modes (`0x1`, SDR 4K 59.94 `0xb0000080a`) | rc=0; screen black ~2 s, TV changes picture mode (Filmmaker -> HDR personal), stays HDR, `dynamic_range` stays 2 |
| same, 23.976 (`0x2`, `0xb00000802`) | IsOutputSupported `0x8029001e`, Ex `0x80290016` |
| options block with only the content type | accepted |
| options flags `0xb` (Blu-ray's) | `0x80290001` |
| capability setter `Jl91AhxKc0w(handle, 0, 0x80)` | `0x8029001c` (one run where dlsym worked) |
| finding its address: `sceKernelDlsym` by name or NID from the app module | `0x80020003` |
| finding its address by reading EVO's own import stub | EVO crashes (execute-only code); console fine |
| eboot with YouTube's authority `0x4400001084c20672`, program type 4 | launch refused, CE-100008-9 |
| skip EVO's jailbreak request (Lapy/etaHEN file drop) | the media sandbox is already open (`/data` visible), same results as above. The jailbreak would set `sceAuthId 0x4801000000000013` and all caps (SharpProspero docs), so caps were never the missing part |
| `attribute3` 0x200 (the official YouTube app's value) in `param.json` and the registry, reboot | `sceVideoOutOpen` fails `0x80290001`: no picture at all. Reverted |
| ProsperoTube (EVO 0.11.0 + patch scripts, media category) | same method as EVO (buffer retype, 60/120 Hz only). Owner test on the same setup: TV stays HDR for SDR videos |
| other YouTube launchers: PPSA99350 (ytps5-sponsorblock), PPSA98260 | SDR-only, or HDR held for the session. Neither switches |

Note: AvControl logs `ERROR unknown vrr parameter(0x100000)` for `attribute3`
0x100000, so that value is probably not right either; it only happens to
launch.

## Registered params for reference

| title | category | `attribute` | `attribute3` |
|-------|----------|-------------|--------------|
| YouTube PPSA01650 / 01651 | 65536, badge 2 | `0x62000000` | `0x200` |
| Disc Player NPXS40140 | `0x10100` | `0x62800038` | `0x4` |
| EVO 0.13.5 (game) | 0, badge 1 | `0x62000000` | `0x80040` (+`0x400000` with `--pro-memory`) |

## How to repeat a test

- The registry rows live in `/system_data/priv/mms/app.db` (`tbl_contentinfo`:
  `categoryType`, `contentBadgeType`, `pprAttribute3`, `AppInfoJson`),
  `/system_data/priv/mms/appinfo.db` (`tbl_appinfo` key/val) and
  `/user/appmeta/PPSA99039/param.json`. Back them up first, edit copies with
  sqlite, upload over FTP, then reboot. The image's `param.json` must match.
- Close EVO with `evo-remote.sh quit` then `close` before every deploy. A build
  that cannot open VideoOut cannot park; close it from the PS button only after
  the log shows `agc shutdown: released`.
- `package-app.sh` validates the game category; the media-category builds
  used local experiment hooks that are not merged.

## Open leads

- Which attribute or registry field makes AvControl let an app change range
  and rate at run time (YouTube, Disc Player). `attribute3` alone is not it, or
  0x200 needs YouTube's real signature.
- The process check behind `0x80290001` for the `0xb` flags (VideoOut
  options validator `0x163e0`, worker `0x178d0` on 12.70).
- Disc Player's `attribute` bits `0x00800038` and category `0x10100`.
