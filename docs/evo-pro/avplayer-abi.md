# libSceAvPlayer — ABI reference and spike plan (Route A)

**Status: transcription, not yet hardware-run.** This is Phase 0 for the
native-decode plan's **Route A** (`sceAvPlayer` — hardware demux + decode +
A/V sync in one API). Header:
[sce/sce_avplayer.h](../../projects/evoplayer/media/include/sce/sce_avplayer.h).
The spike that turns this into a hardware result is
[`projects/avplayer_test/`](../../projects/avplayer_test/main.c) — Phase 2.

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

### the payload-context caveat (why the spike may still fail cleanly)

`sceKernelGetDirectMemorySize()` returns **0** in an elfldr payload — there is
no direct-memory budget, so `sceKernelAllocateDirectMemory` cannot be used as
`MediaPlayer.cs` does. `projects/avplayer_test` therefore routes the texture
allocator through `sceKernelAllocateMainDirectMemory` (the main pool, as
`videoout_test` does). If the decoder rejects that memory, **that is the
finding**: Route A needs the registered-app-module context, same conclusion as
the Phase 1 gate reached for Route B. The clean follow-up is to fold the probe
into the app module behind a `-D` flag, next to `evo_agc_probe.c`.

---

## 5. Running the spike

```bash
docker compose run --rm ps5-dev bash -lc '
  cd projects/avplayer_test && make'
# put a small .mp4 (H.264, .mp4/.mov extension) at /data/bunny.mp4 on the console
./tools/launch.sh output/elf/avplayer_test.elf --timeout 45      # payload; watchdog _exit()s at 40s
# read the transcript from the /hbldr pipe + the notification popups
```

Outputs:
- **stdout transcript** (over the `/hbldr` pipe) — every resolve, every
  callback, the full `AvPlayerFrameInfoEx` dump, the characterisation.
- **`/data/avplayer_probe/frame0.nv12`** + `frame0.txt` — first frame's planes
  and metadata, for off-console inspection (`ffplay -f rawvideo -pix_fmt nv12
  -video_size <pitch>x<height> frame0.nv12`).
- **notification popup** — one-line verdict.

### what each result means

| transcript ends at | meaning | → |
|---|---|---|
| `FAILED (symbol resolve)` | module not mappable in this context | Route A dead from a payload; try in-app |
| `Init returned NULL` | allocators rejected, or context refused | inspect the `[tex]`/`[file]` logs just above |
| `AddSource 0x…` non-zero | demux refused the file | check extension + path; try `file://` |
| `StreamCount -> 0` (stays) | source-read thread never finished | file unreadable via the callbacks, or hang (watchdog fires) |
| `Start -> 0x…` non-zero | no stream enabled, or start refused | |
| decode loop, `video frames: 0` | **the interesting failure** — pipeline ran, no frame out | this is the errno-5200-equivalent for Route A; compare `isActive` polls |
| `OK — N video frame(s) decoded` + `cpu-readable: yes` | **Route A works** | → Phase 4: `evo_vdec_native.c` on `sceAvPlayer` |

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
- Spike: [projects/avplayer_test/main.c](../../projects/avplayer_test/main.c)
- Clean-room ABI: `third_party/SharpProspero/src/SharpProspero/Interop/Media/AvPlayer.cs`
- Reference impl: `third_party/SharpProspero/src/SharpProspero/Media/MediaPlayer.cs`
- Prior recon: [native-media-research.md](../native-media-research.md)
- Route B counterpart: [videodec2-abi.md](videodec2-abi.md)
