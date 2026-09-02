# libSceAvPlayer — ABI reference and spike plan (Route A)

**Status: Phase 0 done; Phase 2 gate ran on hardware 2026-09-03 — BLOCKED at
module load.** This is the native-decode plan's **Route A** (`sceAvPlayer` —
hardware demux + decode + A/V sync in one API). Header:
[sce/sce_avplayer.h](../../projects/evoplayer/media/include/sce/sce_avplayer.h).
Gate probe:
[`evo_avplayer_probe.c`](../../projects/evoplayer/src/evo_avplayer_probe.c)
(`--avplayer-probe`).

> **Hardware result #1 (2026-09-03):**
> `EVO avplayer: libSceAvPlayer.sprx load FAILED - Route A blocked`.
> `sceKernelLoadStartModule` of a system PRX is refused for a fake-signed app
> module that did not declare it NEEDED — same wall `libSceAgc.sprx` hits (#27).
>
> **Fix built (2026-09-03):** PRX import stubs —
> `tools/native-app/stubs/prx/*.syms` → a tiny ELF `.so` per module (SONAME
> `libSceX.sprx`, one empty `FUNC` per symbol). `package-app.sh` links them
> `--as-needed` (so a module is only NEEDED if EVO references one of its
> symbols) and passes them to `native_app_builder link --stub`, which computes
> the Sony NID from the plain name. Verified: the converted `eboot.elf` has
> `libSceAvPlayer.prx` NEEDED and `sceAvPlayerInit → aS66RI0gGgo#I#J` etc.
> (NIDs match `prospero-nid` and the hw-verified list); `integrity: valid`.
> The probe now `extern`-declares and calls `sceAvPlayer*` directly — no
> module load, no dlsym. **Awaiting hardware run #2** to confirm the loader
> auto-loads `libSceAvPlayer.sprx` for a fake-signed module (the theory, from
> ProsperoLight/SharpProspero precedent — they ship NEEDED system PRXs).
>
> Stubs also staged (not yet NEEDED — nothing references them): `libSceAgc` +
> `libSceAgcDriver` (#27), `libSceVideodec2` (Route B).

What *is* verified (prior recon, [native-media-research.md](../native-media-research.md#results-log),
fw 12.70, elfldr payload): `libSceAvPlayer.sprx` maps into EVO's process and
the six core symbols resolve by NID. Nothing past that — no `Init`, no
`AddSource`, no decoded frame — has been observed.

---

## 1. Source of truth

Struct layouts and field offsets are transcribed from **SvenGDK/SharpProspero**
(`src/SharpProspero/Interop/Media/AvPlayer.cs`), a clean-room C# SDK for
on-device app modules. Every field there is annotated with its byte offset;
the C header keeps those offsets in comments and `_Static_assert`s the total
sizes. The reference *implementation* of the callbacks is
`src/SharpProspero/Media/MediaPlayer.cs` — the port below follows it closely.

This differs from the **PS4** `SceAvPlayer` ABI in two ways that bite:

| | PS4 | PS5 (this header) |
|---|---|---|
| `FileReplacement` | object pointer **last**, has both `read` + `readOffset`, plus a `memSize` field | object pointer **first**, `readOffset` only, 40 bytes total |
| video frame | `GetVideoData` → `AvPlayerFrameInfo` (no pitch) | `GetVideoDataEx` → `AvPlayerFrameInfoEx` (pitch + 4 crop insets) |

An earlier `avplayer_test` draft pasted a scrambled NID table and the PS4 file
struct — both are why calling code must **compute** NIDs (`nid_encode`) and use
the layouts here.

---

## 2. Call sequence (what the spike runs)

```
sceKernelLoadStartModule("/system/common/lib/libSceAvPlayer.sprx")
kernel_dynlib_handle(pid, "libSceAvPlayer.sprx", &h)
resolve each symbol: nid_encode(name) -> kernel_dynlib_resolve(pid, h, nid)

SceAvPlayerInitData init = {0};                 // sizeof == 120
  init.memory_replacement = { alloc, free, alloc_texture, free_texture }
  init.file_replacement   = { open, close, read_offset, size }   // or all-zero
  init.event_replacement  = { event_cb }                         // or zero, poll
  init.debug_level = SCE_AVPLAYER_DEBUG_ALL
  init.num_output_video_framebuffers = 8
  init.auto_start = 0
player = sceAvPlayerInit(&init)                 // NULL => failed

sceAvPlayerAddSource(player, "/data/x.mp4")     // 0 => ok; extension must be
                                               // .mp4/.m4v/.mov/.webm

// the source is read on the player's own thread; StreamCount is 0 until it finishes
for (~10s) { n = sceAvPlayerStreamCount(player); if (n > 0) break; sleep(); }
for each stream i:
    sceAvPlayerGetStreamInfo(player, i, &info)  // type: 1 video / 2 audio / 3 text
    sceAvPlayerEnableStream(player, i)          // first of each kind; NONE enabled => Start refuses

sceAvPlayerStart(player)

loop:
    if (sceAvPlayerGetVideoDataEx(player, &ex) && ex.data) { ... }   // NV12 + pitch + crop
    if (sceAvPlayerGetAudioData (player, &fi) && fi.data) { ... }    // 16-bit interleaved
    // frame pixels are valid only until the next Get* call — consume synchronously

sceAvPlayerStop(player); sceAvPlayerClose(player)
```

`sceAvPlayerGetVideoData` (basic, no pitch) also exists and is one of the six
hw-verified NIDs — the spike resolves both but drives `...Ex`.

---

## 3. The frame the decoder hands back (`AvPlayerFrameInfoEx`)

From `MediaPlayer.cs`'s `VideoFrame` remarks — the part a naive blit gets wrong:

- the buffer is **wider and taller** than the picture.
- `crop_left` / `crop_right` are measured **from the pitch, not the width** —
  the row padding that rounds the pitch up is counted inside `crop_right`.
- `visible_width  = pitch  - crop_left - crop_right`
- `visible_height = height - crop_top  - crop_bottom`
- luma plane starts at `data`; chroma (interleaved UV) starts at
  `data + (int64)pitch * height` — **buffer** height, not visible height — and
  both planes are then indexed from the picture's corner
  (`crop_top` row, `crop_left` column).
- limited-range BT.601. `MediaPlayer.cs` uses
  `R = (298c + 409e + 128) >> 8` etc. with `c = Y-16, d = U-128, e = V-128`.

For Phase 4 this maps to `pp_frame` as NV12 with the crop insets applied — the
same shape `sceVideodec2` produces (see
[videodec2-abi.md](videodec2-abi.md) §3), so the converter seam downstream does
not care which route fed it.

---

## 4. Memory typing — the port, and the diff against the old try

`MediaPlayer.cs` supplies two allocator pairs. The player has none of its own;
all four are mandatory.

### general (`allocate` / `deallocate`)

Plain aligned heap. `posix_memalign` with the alignment rounded **up to a power
of two** (the player passes a raw byte count). Called from the player's
threads — must not touch anything those threads can't reach, must never let an
exception escape, return `NULL` to fail.

### texture (`allocate_texture` / `deallocate_texture`) — GPU-visible

`MediaPlayer.cs`, verbatim:

```
align = max(alignment, 0x4000) rounded up to pow2
bytes = round_up(size, align)
sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, align,
                              12 /* MemoryTypeCachedShared */, &offset)
sceKernelMapDirectMemory(&addr, bytes,
                         0x33 /* ProtCpuReadWrite | ProtGpuAll */,
                         0, offset, align)
```

The deallocator **must `munmap` *and* `sceKernelReleaseDirectMemory`** — release
alone leaves the address range reserved and a player that recycles frame
buffers runs the VA space out. Track `(addr → offset, size)` in a small fixed
table the player threads can read without locking the general heap.

**Diff against [hardware-decode.md](../hardware-decode.md):** the earlier EVO
`sceVideodec2` attempt used `WC_GARLIC` (memory type **3**) for its GPU
regions. SharpProspero's decode path uses **`MemoryTypeCachedShared` (12)** for
the CPU-and-GPU regions and `MemoryTypeCached` (11) for GPU-only, with
`MapNoCoalesce`, and `VideoDecoder.CreateAvc` runs on **compute pipe 0 /
queue 0**. Whether the wrong memory type contributed to errno 5200 is
unproven, but Route B's Phase 2 re-run (`sceVideodec2` with SharpProspero's
*exact* typing) is the controlled test.

### only the app module can answer this

Native hardware decode does **not** work from an ELF payload — elfldr *and*
hbldr are both borrowed-process sandboxes and both hit the errno-5200 wall
(this is the whole reason Phase 1 repackaged EVO as `PPSA99039`). So the Route A
gate runs **inside the app module**, as a boot-time probe behind
`-DEVO_AVPLAYER_PROBE`, exactly like `evo_agc_probe.c`:

- `projects/evoplayer/src/evo_avplayer_probe.c` — module load + NID resolve
  (inline SHA1, no kernel R/W), the four callbacks (general heap; texture via
  `sceKernelAllocateMainDirectMemory` type 12→3 fallback, prot 0x33; log+serve
  file callbacks), `Init → AddSource → EnableStream → Start → GetVideoDataEx`,
  frame characterisation, `/download0/evoplayer/avpx_frame0.*` dump, watchdog
  thread that `_exit()`s EVO on a hang. Output is **notification popups** (no
  stdout in the app sandbox).

`projects/avplayer_test/` (the payload build) is kept only as a compile-checked
reference for the callback port — it cannot reach hardware decode.

---

## 5. Running the gate (app module)

```bash
docker compose run --rm ps5-dev bash -lc '
  ./scripts/package-app.sh --avplayer-probe --agc-probe --ffpfsc
  ./scripts/deploy-app.sh --ffpfsc'
```

Then on the console: a small **H.264 `.mp4`** at one of `/data/probe.mp4`
(FTP), `/mnt/usb0/probe.mp4` (USB stick), or `/download0/evoplayer/probe.mp4`;
launch EVO from the **Games row**. The probe runs before the UI. Read the
`EVO avplayer:` notification popups off the TV.

Outputs:
- **notification popups** — module/NID result, Init/AddSource/streams/Start,
  first-frame characterisation, and a one-line verdict.
- **`/download0/evoplayer/avpx_frame0.nv12`** + `avpx_frame0.txt` — first
  frame's planes + metadata (`ffplay -f rawvideo -pix_fmt nv12 -video_size
  <pitch>x<height> avpx_frame0.nv12`). Falls back to `/data` or `/mnt/usb0`.

### what each verdict means

| `EVO avplayer:` notification | meaning | → |
|---|---|---|
| `libSceAvPlayer.sprx load FAILED` | module not loadable even in-app | Route A dead; Route B only |
| `core NIDs unresolved` | loaded but symbols missing | check the printed pointers; NID salt/alphabet |
| `NO test file` | probe ran, no media at any candidate path | drop `/data/probe.mp4`, relaunch |
| `sceAvPlayerInit -> NULL` | allocators rejected or context refused Init | check `InitData` size line (want 120) |
| `AddSource(...) -> 0x…` non-zero | demux refused the file (`opened=` / `reads=` show if I/O happened) | extension must be `.mp4/.mov/.m4v/.webm` |
| `pipeline ran, N frames` (N=0) | **the errno-5200-equivalent** — Init+demux OK, no decoded frame | Route A also context-limited; Route B re-run + reassess §8 |
| `ROUTE A WORKS — N video frame(s) hw-decoded` + `cpu-read=YES` | **gate passed** | → Phase 4: `evo_vdec_native.c` on `sceAvPlayer` |

---

## 6. What Phase 4 still has to answer (Route A)

- Frame-buffer lifetime: `GetVideoDataEx` pixels are valid only to the next
  call. `pp_playback` already converts synchronously on push, so the converter
  consumes it in the same tick — confirm no copy is needed.
- No per-AU `send`: `sceAvPlayer` owns the demuxer. The `evo_vdec` seam is
  "one AU in, one frame out"; Route A instead needs `evo_vdec_native` to *also*
  own the source (it bypasses `evo_demux`). That is a wider seam than Route B —
  weigh it in the Phase 2 write-up.
- Audio: Route A decodes audio too. Either feed it into `evo_audio_out` or tell
  `sceAvPlayer` video-only via `EnableStream`.
- Seek: `sceAvPlayerJumpToTime` vs. the seam's `evo_vdec_flush`.
- Threading / priority: `base_priority` 637–764; the player spawns its own.

---

## 7. Reference

- Header: [sce/sce_avplayer.h](../../projects/evoplayer/media/include/sce/sce_avplayer.h)
- Gate probe (app module): [projects/evoplayer/src/evo_avplayer_probe.c](../../projects/evoplayer/src/evo_avplayer_probe.c)
- Callback-port reference (payload, cannot decode): [projects/avplayer_test/main.c](../../projects/avplayer_test/main.c)
- Clean-room ABI: `third_party/SharpProspero/src/SharpProspero/Interop/Media/AvPlayer.cs`
- Reference impl: `third_party/SharpProspero/src/SharpProspero/Media/MediaPlayer.cs`
- Prior recon: [native-media-research.md](../native-media-research.md)
- Route B counterpart: [videodec2-abi.md](videodec2-abi.md)
