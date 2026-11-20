# Dolby Vision output to the TV (research, FW 13.60)

**Status: not possible today.** EVO cannot make the PS5 send a Dolby Vision
signal to the TV. This page explains why, with what was measured on
2026-11-20, so the next attempt starts here.

What EVO does instead: it reads the Dolby Vision RPU itself (Profile 5/8
reshaping, per-shot peak mapping) and sends HDR10. That keeps the main benefit
of DV, the per-shot metadata. See "Dolby Vision, per shot" in
[video-decode.md](video-decode.md).

Test setup: PS5 Pro FW 13.60 (Relapse), LG G5 OLED (2025), NVIDIA Shield TV
Pro 2019 as the reference source, all on the same LAN.

## Short answer

A DV signal needs three things:

1. a decoder for the DV stream
2. Dolby's display management, which builds the picture for the TV
3. the HDMI signalling: a Dolby VSIF (vendor-specific InfoFrame) and the right
   pixel format

EVO already covers 1 well enough (base layer + RPU). The PS5 has no copy of 2.
It is licensed Dolby code and is not on the console. For 3, there is no known
way to make the PS5 send a Dolby VSIF. A DV flag on EVO's own HDR10 picture
would be a forged label, and on the G5 it would not look better than HDR10.

## What the jailbreak can change (FW 13.60)

| | |
|---|---|
| Userland code, other processes' memory and page protection | yes (payload SDK `kernel_proc_copyin/out`, `kernel_mprotect`) |
| Kernel data read/write | yes (`kernel_copyin/copyout`) |
| Kernel code | no. It is hypervisor-protected, and sweeping it panics (CLAUDE.md) |
| System library code | execute-only (`libSceVideoOut.sprx` faulted on a read, [refresh-rate-modes.md](refresh-rate-modes.md)) |
| PS5-Linux | no, it needs a hypervisor exploit |

So the display driver cannot be rewritten. All that is left is data patches
(kernel or the shell's AvControl), with no map of the registers and no
way to see the result except the TV.

## What the PS5 sends today

From AvControl in klog:

```
video: port:HDMI 3840_2160P_5994 RGB444 limited RGB2100_PQ 36bpp 16:9 game
```

That is 12-bit RGB 4:4:4, PQ, BT.2020. AvControl picks the HDMI mode per app,
from the app's registered attributes. Even per-video HDR/SDR switching is
still open (#244).

None of the decrypted system binaries has a single string about Dolby video:
`UHDBdPlayerCore.elf` (the console's own UHD Blu-ray player), `libSceVideoOut`,
`libSceAvSetting`. The only Dolby on the console is audio
(`ENCODE_DOLBY AC3`).

`libSceVideoOutRawEdid` exists and AvControl logs
`get raw hdmi monitor info`, so the shell can read the TV's EDID. EVO does not
use it.

## What the TV accepts (LG G5 EDID)

Read from the Shield (`adb shell dumpsys SurfaceFlinger`, "Raw edid") and
decoded with `edid-decode`. Dolby Vision Video Data Block, OUI `00-D0-46`:

```
eb 01 46 d0 00 48 02 b7 7f 68 86 85
```

- version 2, DM 4.x
- interface: Standard + Low-Latency + Low-Latency-HDMI
- 10b/12b 4:4:4: 12 bit
- target PQ 0 .. 3485 (2607 cd/m²), backlight min luma 75 cd/m²
- primaries R 0.6875/0.3125, G 0.2461/0.7031, B 0.1484/0.0508

Also: HDR static metadata (PQ, HLG), BT.2020 YCC/RGB, HDMI Forum VSDB
(600 MHz, FRL up to 12 Gbps x 4), ALLM. No HDR10+ block.

"Low-Latency-HDMI" at 12-bit 4:4:4 matters here: it is close to what the PS5
already sends. If I read the spec right (not verified), the source sends a
picture already mapped to the TV, plus the Dolby VSIF. In that case the pixel
format is not the blocker. The VSIF and the Dolby mapping still are.

## What a working source does (Shield, during DV playback)

`adb shell dumpsys SurfaceFlinger` and `logcat`, playing a `dvhe.07.06` MKV:

- HDMI mode `3840x2160 @ 23.976 YUV422/12/Rec.2020/hdr/DV`: standard
  (TV-led) DV, logged as `Dolby Vision Sink Supports Normal Mode`. Every DV mode
  the Shield lists is YUV 4:2:2 12-bit.
- Decoder `OMX.Nvidia.DOVI.decode`. Whether the FEL is applied is unknown.
- The Dolby code runs in NVIDIA's graphics composer HAL
  (`vendor.nvidia.hardware.graphics.composer@2.0-service`): `DoviLutGenCtxt`,
  `DOVI_CONTROL_LIB_VERSION = 4.5.2`, a LUT built on the CPU. It switches
  between video priority (VPM) and graphics priority (GPM, menus on top).
- Android's own `HdrCapabilities` says HDR10 only and HWC says `dv=false`. The
  DV path is NVIDIA's own.

That is licensed ARM64 Dolby code. It shows how the parts fit together, but it
cannot run on the PS5 and must not be copied.

The VSIF bytes themselves were not captured: the Shield's
`/d/tegradc.0` needs root, and the G5 only shows "Dolby Vision" or "HDR",
never the bytes. Capturing them needs root on the Shield or an HDMI 2.1
analyzer.

## Why a forged signal is not worth it

If a way to send the VSIF were found, the best possible result would be "approximate
LLDV": EVO's own tone mapping, aimed at the G5's target range, labelled as DV.
The G5 would then switch off its own HDR10 processing and show EVO's mapping.
That gains nothing over HDR10 from EVO, and a wrong VSIF field or colour space
gives wrong colours with no error anywhere.

## If someone tries again

1. Find where the VSIF is built for HDR10 (the static HDR InfoFrame path from
   `sceVideoOutSysSetHdrMetadata` down to the driver) without reading kernel
   code. Kernel data and the shell's memory are the only places available.
2. Have a way to watch the result: an HDMI analyzer, not the TV.
3. Capture a reference VSIF first (Shield with root, or the analyzer).
4. Never write display registers blind. An unknown MMIO write is a likely
   kernel panic.

To repeat the Shield capture: enable network debugging, accept the key on the
TV, then:

```bash
adb connect <shield-ip>:5555
adb shell dumpsys SurfaceFlinger > sf.txt      # modes, current mode, raw EDID
adb logcat -d | grep -i -E "dovi|dolby"        # during DV playback
```
