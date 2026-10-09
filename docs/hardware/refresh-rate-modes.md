# Output refresh-rate modes (frame rate matching)

The goal is to switch the TV to the video's own rate (23.976 / 24 Hz for film,
50 Hz for PAL), the way the PS5's Blu-ray player does with its 24p output. On
FW 12.70 a game-slot app like EVO **cannot do this**. The console refuses
everything except the default rate and 119.88 Hz. So the setting is
**Experimental**: Settings → Experimental → `MATCH VIDEO FRAME RATE`. It tries
the exact rate once per session, and if that is refused it falls back:

| video | asks for | what you get on 12.70 |
|-------|----------|-----------------------|
| 23.976 fps | 23.976 Hz | 119.88 Hz (5:5, no judder) if the display has 120 Hz, else 59.94 |
| 24 fps | 24 Hz | same as above |
| 25 / 50 fps | 50 Hz | default 59.94 Hz |
| 29.97 / 30 / 59.94 / 60 | default | default |

119.88 Hz already shows every film frame for the same time, so film pans are
smooth. Only 25/50 fps content gains nothing.

Code:
- `evo_agc_runtime_set_output_rate()` in `media/src/evo_agc_runtime.c`
- `match_video_rate()` and the refresh block in `core/src/Application.cpp`

## Mode values

`sceVideoOutConfigureOutput(handle, mode, NULL, NULL, 0)`, the same call as the
120 Hz switch. Each mode was decoded by running libSceVideoOut's own parser
(`0x188e0`) under unicorn:

| mode | refresh token | rate |
|------|---------------|------|
| `0x1` | (system default) | normally 59.94 Hz |
| `0x2` | 1 | 23.976 Hz |
| `0x3` | 4 | 24 Hz |
| `0x5` | 6 | 29.97 Hz |
| `0x9` | 2 | 50 Hz |
| `0xa` | 3 | 59.94 Hz |
| `0xf` | 13 | 119.88 Hz |

The Blu-ray player (`/system_ex/app/NPXS40140/UHDBdPlayerCore.elf`, function
`0x18b820`) uses full modes with the same low byte:
- `0xb` = SDR, `0xc` = HDR
- `0x400` = 2K, `0x800` = 4K

For example, `0xc00000802` = HDR 4K 23.976. Its fallback `0xb0000040a` is the
"SDR-2K-59.94Hz" in its log string.

## Why EVO is refused (hardware, 2026-10-09)

Each step was tested on the console, a PS5 Pro on 12.70, with a 1440p monitor
that does offer 23.98/24/50 Hz. The display is not the limit.

1. **Plain `sceVideoOutConfigureOutput`**: modes 2/3/9 and the full modes →
   `0x8029001e`. The gate at `0x18200` lets a non-Ex call reach only the
   default and `0xf`.
2. **`sceVideoOutConfigureOutputEx`** (NID `2CxY28ewv8g`, the Blu-ray player's
   call), no options → `0x80290016`. Every userland check passes. This is the
   video driver's `0x802a0005`, translated through the table at `0x28450`.
3. **Ex + the Blu-ray player's options block** → `0x80290001`. The options are
   64 bytes:
   - byte 2 = content type 0 (setter `KHBNAkJhT64`)
   - qword +8 = `0xb` (setter `yoK0AjkkBkI`)

   They become driver flags `0x10 | 0x40000 | 0x200000`. The worker `0x178d0`
   only allows those when the process's VideoOut capability mask (`+0x6d0`) has
   bit `0x80`.
4. **Setting bit `0x80`.** The unnamed export `Jl91AhxKc0w(handle, 0 = set,
   bits)` (`0x65a0`) accepts `0x40`/`0x80` only if `get_authinfo`
   (syscall 587, libkernel `igMefp4SAv0`) reports ucred caps[0] bit 62, a
   system-app capability. Called by address, it returned 0, but step 3 still
   failed with `0x80290001`. That was where the work stopped.

Notes for whoever picks this up:
- libSceVideoOut's code is **execute-only**. Reading it (for example with a
  byte check) segfaults: klog showed `xotext:"libSceVideoOut.sprx"+0x65a4`.
  Its base is `&sceVideoOutOpen - 0x109b0`, and dlsym by name works.
- dlsym by raw NID fails in the app module (`0x80020003`).
- klog keeps no backlog. Record it (port 3232) *before* reproducing a crash.
  `tools/klog.sh` needs `nc` in the container image.
- The decrypted system files used here were pulled with ftpsrv's `SELF` mode
  into `output/re/`.

## Open

- Why step 3 still fails after the setter returned 0. Either the setter took a
  path that does not set `0x80`, or another check in the options validator
  (`0x163e0`) or the flag mask applies.
- Full modes would also change resolution and HDR. EVO only ever asks for the
  small ones, so the HDR retype path (#114) is unchanged.
