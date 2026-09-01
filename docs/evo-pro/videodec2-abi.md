# libSceVideodec2 — verified ABI and call sequence

**Status: hardware-verified 2026-09-01.** Every call below returned `0` on a
PS5 (12.70) from a fake-signed *game-category app module*, decoding a 1080p
H.264 IDR to a valid NV12 frame. Header: [sce/sce_videodec2.h](../../projects/evoplayer/media/include/sce/sce_videodec2.h).
Context and caveats: [native-decode-plan.md](native-decode-plan.md).

This is Phase 0 of the native-decode plan — a transcription, not new code.
Every struct and value here is lifted from `blackbearreloaded/ProsperoLight`
(`src/moonlight_stream.cpp`), which decodes on the VCN block in production, and
was re-confirmed by the `PROSPEROLIGHT_VDEC_SELF_TEST` build (an offline probe
that runs this sequence and feeds one bundled AU through `sceVideodec2Decode`).

---

## 1. The one thing that mattered

The prior EVO effort ([hardware-decode.md](../hardware-decode.md)) got
`sceVideodec2CreateDecoder` to succeed and then hit **errno 5200** at the
decode ioctl, from every payload context it tried, with and without credential
elevation.

The ProsperoLight self-test hit **none of that**. Same API, same call order,
same struct layouts. The only material difference is the **process**: a
registered app module (its own `TITLE_ID`, `param.json`, sandbox, user
session), launched from `/data/homebrew/PPSA99002/` by ShadowMountPlus — not an
ELF injected into a host process by elfldr, and not the borrowed PS-Now slot
hbldr uses.

Result line from the on-device notification:

```
VDEC self-test: HARDWARE DECODE OK
sysmod207=00000000 computeQ=00000000 memQuery=00000000 cpuMap=00000000 alloc=00000000
CREATE=00000000 reset=00000000 DECODE=00000000 flush=00000000
out valid=1 error=0 pics=1 1920x1088 pitch=2048 codec=1
```

No arbitration calls (`sceVideodec2ControlArbitration` / credential shims) were
made. They are not in ProsperoLight and were not needed.

---

## 2. Exact bring-up sequence

All of this runs once per playback session, off the render thread. Sizes are
16K-aligned (`(v + 0x3FFF) & ~0x3FFF`) before every allocation. Every SCE
struct is zeroed, then `.size = sizeof(struct)` set, before use.

```
1.  sceSysmoduleLoadModule(207)                        // SCE_SYSMODULE_VIDEODEC2

2.  limit = sceKernelGetDirectMemorySize()

3.  ComputeMemoryInfo cm = { .size = sizeof cm };
    sceVideodec2QueryComputeMemoryInfo(&cm)
    cm_bytes = align16k(cm.cpu_gpu_size)
    alloc_direct(cm_bytes, PROT 0x33) -> cm.cpu_gpu ;  cm.cpu_gpu_size = cm_bytes
    ComputeConfigInfo cc = { .size=sizeof cc, .pipe_id=0, .queue_id=0 };
    sceVideodec2AllocateComputeQueue(&cc, &cm, &compute_queue)

4.  DecoderConfigInfo config = {
        .size = sizeof config,
        .resource_type = 1,                 // compute path
        .codec_type   = 1,                  // AVC   (HEVC = 974921)
        .profile      = 100,                // AVC High  (HEVC Main=1 / Main10=2)
        .max_level    = 51,                 // AVC 5.1 for 1080p60
        .max_width    = 1920,
        .max_height   = 1088,               // 1080 padded to MB
        .max_dpb_frames = 4,
        .pipeline_depth = 1,
        .compute_queue  = (uint64_t)compute_queue,
        .cpu_affinity   = 0x3F,             // cores 0-5
        .cpu_priority   = 700,
        .optimize_progressive = 1,
    };

5.  DecoderMemoryInfo mem = { .size = sizeof mem };
    sceVideodec2QueryDecoderMemoryInfo(&config, &mem)
    // mem.cpu_size / gpu_size / cpu_gpu_size / max_frame_size now filled

6.  cpu_map = align16k(mem.cpu_size)
    sceKernelAvailableFlexibleMemorySize(&avail)             // advisory
    sceKernelMapNamedFlexibleMemory(&mem.cpu, cpu_map, 0x03, 0, "VdecCpu")

7.  gpu     = align16k(mem.gpu_size)      ; alloc_direct(gpu,     0x32) -> mem.gpu
    cpu_gpu = align16k(mem.cpu_gpu_size)  ; alloc_direct(cpu_gpu, 0x33) -> mem.cpu_gpu   (if non-zero)
    frame_sz = align16k(mem.max_frame_size)
    au_pool    = alloc_direct(0x800000 * 3, 0x32)     // 8 MiB * PIPELINE_BUFFER_COUNT
    frame_pool = alloc_direct(frame_sz * 3, 0x32)
    mem.gpu_size = gpu ;  mem.cpu_gpu_size = cpu_gpu (if non-zero)

8.  sceVideodec2CreateDecoder(&config, &mem, &decoder)
9.  sceVideodec2Reset(decoder)
```

`alloc_direct(size, prot)` =
`sceKernelAllocateDirectMemory(0, limit, size, 0x4000, 12, &start)` then
`sceKernelMapDirectMemory(&ptr, size, prot, 0, start, 0x4000)`.

| prot | meaning | used for |
|---|---|---|
| `0x33` | CPU r/w + GPU all | compute `cpu_gpu`, decoder `cpu_gpu` |
| `0x32` | CPU w + GPU all | decoder `gpu`, AU pool, frame pool |
| `0x03` (flexible) | CPU r/w | decoder `cpu` workspace |

---

## 3. Per-frame decode

`slot = au_index % 3`. AU and frame pools are ring-indexed by `slot`.

```
memcpy(au_pool + slot*0x800000, annexb_au, au_len);   // SPS+PPS+slice NALs, one frame

InputData  in = { .size=sizeof in, .au = au_pool + slot*0x800000,
                  .au_size = au_len, .pts = pts, .dts = UINT64_MAX, .attached = 0 };
FrameBuffer fb = { .size=sizeof fb, .buffer = frame_pool + slot*frame_sz,
                   .buffer_size = frame_sz };
OutputInfo  out = { .size=sizeof out };

rc = sceVideodec2Decode(decoder, &in, &fb, &out);

if (rc == 0 && !out.valid) {                 // buffered — drain it
    memset(&out, 0, sizeof out); out.size = sizeof out;
    rc = sceVideodec2Flush(decoder, &fb, &out);
}
// success: rc == 0 && out.valid && !out.error && fb.accepted
```

On success `out.buffer` points **inside** the frame-pool slot and holds:

* **NV12** (8-bit) normally, **P010** (`out.pitch_bytes == out.pitch*2`) for HEVC Main10.
* `out.width` x `out.height` luma (e.g. `1920 x 1088`), luma stride
  `out.pitch` **samples** / `out.pitch_bytes` bytes (e.g. `2048` / `2048`).
* chroma plane follows luma at the same stride, half height.
* `out.codec` echoes `codec_type`; `out.picture_count == 1`.

The buffer is valid until the next `Decode`/`Flush` reuses that slot. EVO's
converter consumes synchronously on push today, so a straight hand-off works;
if that changes, copy into `evo_direct_mem` here.

---

## 4. Teardown (reverse order, all rc ignored)

```
sceVideodec2DeleteDecoder(decoder)
release frame_pool, au_pool, mem.cpu_gpu, mem.gpu   (munmap + ReleaseDirectMemory)
sceKernelReleaseFlexibleMemory(mem.cpu, cpu_map) + munmap
sceVideodec2ReleaseComputeQueue(compute_queue)
release cm.cpu_gpu
sceSysmoduleUnloadModule(207)
```

---

## 5. Codec / profile / level values

| codec | `codec_type` | `profile` | `max_level` (1080/1440/2160) |
|---|---|---|---|
| H.264 | `1` | `66` BP / `77` MP / `100` HP | `51` / `52` / `52` |
| HEVC | `974921` (`0xEE049`) | `1` Main / `2` Main10 | `123` / `150` / `153` |
| VP9 | `2382845` | — | — (ProsperoLight declares it, never exercised) |

`max_width`/`max_height` per resolution: `1920x1088`, `2560x1440`, `3840x2176`.

For EVO the values come from the demuxer's `AVCodecParameters`
(`codec_id`, `profile`, `level`, `width`, `height`) — map `AV_CODEC_ID_H264` ->
`1`, `AV_CODEC_ID_HEVC` -> `974921`, round `width`/`height` up to the next 16.

---

## 6. What Phase 4 (`evo_vdec_native.c`) still has to answer

Confirmed working: the whole sequence above, H.264 High 1080p, one IDR.

Not yet tested on hardware, in rough risk order:

1. **A continuous stream** — P/B frames, DPB reordering, `out.valid==0` then a
   later frame. ProsperoLight does this against a live Moonlight host; EVO
   would feed it demuxed file AUs. Reordering: `sceVideodec2Decode` appears to
   output in decode order with `pts` passed through — EVO's existing PTS/DTS
   reorder queue in the play loop still applies.
2. **Seek** — `sceVideodec2Reset(decoder)` then feed a fresh IDR. Cheap to try.
3. **HEVC Main / Main10** — `codec_type 974921`, `profile 1/2`, P010 output
   into `pp_frame` (`PP_FRAME_P010`, converter change — plan §3).
4. **Non-1080p** — `max_width/height` per clip; the decoder is sized at
   `Create` time, so a resolution change mid-file means recreate.
5. **`evo_direct_mem` integration** — route the pool allocations through the
   slab manager so multi-hour playback doesn't fragment direct memory.
6. **VideoOut interaction** — EVO already owns its VideoOut handle
   (`pp_videoout.c`). The decoder uses the **compute-queue** path and never
   opens video out, so the payload-only `sceVideoOutOpen` panic rule is not in
   play here. Still: bring the decoder up *after* VideoOut, tear down *before*.

---

## 7. Reference build

The instrumented ProsperoLight lives at
`<scratchpad>/ProsperoLight` this session. To rebuild the probe:

```
docker run --rm -v <path>/ProsperoLight:/pl -w /pl pl:build \
  make deploy PS5_HOST=<console> DEPLOY_FORMAT=folder VDEC_SELF_TEST=1
```

The self-test is `moonlight_vdec_self_test()` in `src/moonlight_stream.cpp`
(guarded by `PROSPEROLIGHT_VDEC_SELF_TEST`) plus `RunVdecSelfTest()` in
`src/main.cpp`. It writes nothing the console web `/fs` route can read (app
sandbox) — the result comes back as an on-screen notification and a screen
tint (green = decoded frame, amber = created but decode failed, red = create
failed).
