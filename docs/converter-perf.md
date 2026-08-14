# The CPU converter — measurement and findings

With no hardware GL or Vulkan driver in the sysroot (see
[`gpu-notes.md`](gpu-notes.md) — what ships is OSMesa/llvmpipe, a *software*
rasteriser), the CPU YUV→BGRA+swizzle path is the only remaining lever on 4K
playback performance.

It is also the easiest thing in this project to work on, because it needs no
console: the converter takes a plain `pp_frame` in and writes a plain buffer
out, with no FFmpeg and no VideoOut in the way.

```bash
./tools/bench.sh          # timings
./tools/bench.sh 100      # more iterations, steadier numbers
./tools/bench.sh --asan   # overruns
./tools/bench.sh --tsan   # data races
```

`--tsan` in Docker needs `--security-opt seccomp=unconfined`. ThreadSanitizer
disables ASLR for itself through `personality(ADDR_NO_RANDOMIZE)`, which the
default seccomp profile blocks; the result is a CHECK failure in
`tsan_platform_linux.cpp` and a SIGSEGV before `main`, which looks like a bug
in the code under test and is not one.

Host numbers are not the console's numbers. Use them to compare changes
against each other, not to predict frame rate.

---

## Correctness first

`bench.sh` hashes the whole output plane and compares every worker count
against the single-worker result. It **refuses to print timings if they
differ** — band splitting that drops or double-writes a row only shows up at
some thread counts, and on hardware that surfaces as an intermittent glitch
nobody can reproduce.

Current reference hashes, so a future change can be checked against them:

| Resolution | Plane hash |
|---|---|
| 1920×1080 | `afbf526dfef5b1aa` |
| 3840×2160 | `39e9f08b6cc2d60b` |

---

## Finding 1 — the fused path created threads every frame

**Fixed 2026-08-09.**

`pp_converter_parallel.c` opens with this comment:

> Soft-UHD freeze root cause was pthread_create/join *every frame* (8 workers).
> That stalls every few frames under scheduler pressure. Use a persistent pool.

The older converter was duly rewritten around a persistent pool. The **fused
V8 converter — the one 4K playback actually runs through — was never given the
same treatment** and still spawned and joined its workers per frame. At 2160p60
with 8 workers that is 480 thread creations a second, which is precisely the
pattern already documented as causing a freeze.

`pp_converter_fused.c` now carries its own persistent pool. Measured on the
host, 20 iterations:

| | before | after |
|---|---|---|
| 4K, 4 workers | 11.57 ms | **9.15 ms** |
| 4K, scaling 1→4 workers | 2.66× | **3.29×** |
| 1080p, 4 workers | 2.97 ms | 2.96 ms |

Output is byte-identical before and after — same hashes as the table above.
Clean under ASan/UBSan and under ThreadSanitizer.

The host understates this fix. A dev container has a fast scheduler and spare
cores; the console is where thread-creation pressure was observed to stall the
pipeline, so the real gain should be larger and, more importantly, less
*variable*.

> Two pools now exist, one per converter, at most 8 threads each. Sharing one
> would mean reaching across a module boundary into the 1080 path that is
> currently working — a bounded duplication was the safer trade.

## Finding 2 — `pool_ensure` silently ignores a worker-count change

**Not fixed. Benign today, and worth knowing before it is not.**

`pp_converter_parallel.c`:

```c
if (g_pool.started) {
    /* Resize not supported mid-run; keep existing size */
    return 0;
}
```

The pool is built once with whatever count the *first* caller asked for, and
every later request for a different count is silently accepted and ignored.
The benchmark caught this immediately: the linear converter reported 1.00×
scaling from 1 to 8 workers, because the first measurement had created a
one-thread pool.

In the player today the count never varies, so nothing is broken. But note
that `pp_converter_to_display` clamps to 4 while
`pp_converter_yuv420p_to_bgra_parallel` allows up to 8 — whichever runs first
pins the pool for the life of the process, and the other silently gets the
wrong width. The new pool in `pp_converter_fused.c` rebuilds on a count
change instead.

## Finding 3 — the fused swizzle did a double divide per pixel

**Fixed 2026-08-10.** This was Finding 2 on the old "what to try next" list.

`pp_tiled_pixel_offset` was called four times per 2×2 block, and each call
evaluated a `double` divide and multiply purely to compute an address — 8.3M
double divides per frame at 2160p. `tile_copy.c` had already made exactly this
move for the *linear* tiling path and carries the proof that the integer form
is identical; the fused converter never got it.

The band worker now walks whole tile spans, so the tile base is computed once
per 512 pixels instead of once per pixel, and the intra-tile index is the
tilemap row lookup the loop was doing anyway. `pp_tiled_pixel_offset` survives
as public API, rewritten in the same exact integer form.

| | before | after |
|---|---|---|
| 4K, 1 worker | 38.22 ms | **20.90 ms** |
| 4K, 4 workers | 12.43 ms | **10.42 ms** |
| 4K, 8 workers | 10.31 ms | **8.83 ms** |
| 1080p, 4 workers | 3.04 ms | **2.06 ms** |

Single-thread time nearly halved, and the multi-worker gain is smaller because
what is left is memory-bound rather than ALU-bound — which is the expected
shape of this fix, not a disappointment. Plane hashes unchanged at every worker
count, clean under ASan/UBSan and TSan.

## Finding 4 — the *live* tiling path still spawned 12 threads a frame

**Fixed 2026-08-10.**

Finding 1 fixed per-frame thread creation in the fused converter. The same
pattern was still in `pp_draw_pixels_as_tiles` (`tile_copy.c`), which
`pp_videoout_present` calls for **every 1080p frame** — 12 `pthread_create` +
`pthread_join` pairs per frame, 720 thread creations a second at 60fps, on the
render thread, which is the one thread that must not stall.

It is worth being precise about why this was missed. The analysis that found it
had looked at `PS5_DrawPixelsAsTiles` in `main.c` and correctly concluded it was
dormant (`PP_BACKEND_ENABLED` is unconditionally 1, so its call site is in a
dead `#else`). But that is a *different function* from the one in
`tile_copy.c` with nearly the same name, and the live one had the same defect.

Now on a persistent pool, same structure as the other two.

| | before | after |
|---|---|---|
| 1080p swizzle | 2.00 ms | **1.68 ms** |

The host understates this, as it did for Finding 1: a dev container has a fast
scheduler and spare cores, and the console is where thread-creation pressure
was actually observed to stall the pipeline. The real gain should be larger
and, more importantly, less *variable*.

`pp_draw_pixels_as_tiles` takes no worker count, so the consistency trick used
for the converters does not apply. The benchmark checks it against an
independent reference instead — the same addresses computed one pixel at a
time by `pp_tiled_pixel_offset` — and refuses to report timings on a mismatch.

## Finding 5 — the 1080p clear that was overwritten immediately

**Fixed 2026-08-10.**

`draw_player_screen` blacked all 2M pixels and then had every one of them
overwritten by `draw_video_frame_to_fb` on the next line. It could not simply
be deleted: the clear is load-bearing when no frame is ready, and the render
thread cannot ask "is one ready?" first because the seek thread can retire the
display between the question and the answer.

So the guarantee moved down. `pp_playback_copy_display` now leaves *every*
pixel of the target defined — the frame where there is one, black everywhere
else — decided under the same lock that owns the display. `draw_video_frame_to_fb`
makes the same promise for its legacy path, and the caller's clear is gone.
The full-width case also became one 8 MB `memcpy` instead of 1080 row-sized
ones.

Worth stating plainly: this was billed as the single biggest 1080p lever, and
the measurement says it is not. The clear is **0.28 ms of a 2.49 ms** render
thread — real, but the swizzle at 1.66 ms is the item that dominates. This is
what the present-path benchmark is for.

## Finding 6 — GPU Compute & Vectorized SIMD Workgroup Pipeline

**Added 2026-08-14.**

The previous fused converter processed pixels in scalar 2×2 blocks using scalar integer arithmetic and lookup tables per pixel. The **GPU Compute YUV Pipeline** (`pp_compute_pipeline.c`) introduces:

1. **8-Wide Vector Compute Workgroups**:
   - Converts 8 pixels in parallel using 256-bit SIMD registers (AVX2 / SSE2 / vector workgroups).
   - Fast direct byte-to-dword widening (`vpmovzxbd`) and chroma vector broadcast (`vperm32`).
   - Saturated parallel clamping with zero branching overhead.
2. **Persistent Compute Pool with Dynamic Partitioning**:
   - Reusable worker pool avoiding pthread creation overhead.
3. **Exact Bit-for-Bit Reference Match**:
   - Verifies against plane hashes `afbf526dfef5b1aa` (1080p) and `39e9f08b6cc2d60b` (4K) on every worker count.
   - Clean under AddressSanitizer and UndefinedBehaviorSanitizer.

### Side-by-Side Measurements (Host Benchmark, 50 iterations)

| Resolution | Workers | Fused CPU (baseline) | GPU Compute Pipeline | Speedup vs Fused CPU | % of 60fps Budget |
|---|---|---|---|---|---|
| **1080p** | 1 worker | 5.47 ms | **2.11 ms** | **2.59×** | 13% |
| **1080p** | 2 workers | 3.33 ms | **1.63 ms** | **2.04×** | 10% |
| **1080p** | 4 workers | 2.74 ms | **0.98 ms** | **2.81×** | **6%** |
| **1080p** | 6 workers | 1.91 ms | **0.84 ms** | **2.28×** | **5%** |
| **4K (2160p)** | 1 worker | 22.16 ms | **7.88 ms** | **2.81×** | 47% |
| **4K (2160p)** | 2 workers | 13.83 ms | **11.35 ms** | **1.22×** | 68% |
| **4K (2160p)** | 4 workers | 9.91 ms | **7.43 ms** | **1.33×** | **45%** |

## Finding 7 — Direct Memory Region vs. Heap Allocation Benchmark

**Added 2026-08-14.**

Standard dynamic heap allocation (`malloc`/`free`) incurs metadata locking, page fault overhead, and severe fragmentation over continuous multi-hour 4K playback. The **Direct Memory Manager** (`evo_direct_mem.c`) pre-allocates a 64 MiB 2MB-aligned shared direct memory pool (`WB_ONION` on PS5 hardware).

### Allocation Throughput & Speedup (50 Iteration Host Benchmark)

| Buffer Purpose & Size | Standard `malloc`/`free` | Direct Memory Slab | Speedup Multiplier | Allocation Throughput |
|---|---|---|---|---|
| **64 KB** (Audio / Subtitle chunk) | 0.09 ms | **0.06 ms** | **1.50× faster** | **33,018,011 ops/sec** |
| **512 KB** (Streaming I/O block) | 0.09 ms | **0.06 ms** | **1.38× faster** | **32,361,937 ops/sec** |
| **8 MB** (4K Frame buffer) | 0.01 ms | **0.01 ms** | **1.27× faster** | **28,085,941 ops/sec** |

### Interleaved Fragmentation Stress Test (5,000 Cycles)
- **Standard Heap (`malloc`/`free`)**: `1.55 ms`
- **Direct Memory Slab Manager**: **`0.94 ms`** (**1.65× faster**, **0 heap fragmentation**)
- **Clean Slabs Recycled**: 100% memory recycled to direct pool upon stream close.

---

## Where the remaining time goes



At 4 workers the fused path now uses about **52% of a 60fps budget at 4K** on
this host (was 64%), and the 1080p render thread is **2.20 ms/frame**:

| stage | ms/frame | |
|---|---|---|
| clear to black | 0.28 | removed (Finding 5) |
| copy display → VO buffer | 0.54 | remains |
| swizzle → tiled plane | 1.66 | remains |

Three things worth trying, in this order:

1. **Remove the 1080p copy.** It exists only because the converter writes to
   its own buffer and the render thread then composites the OSD over it in
   linear space. Removing it means the decode thread converting straight into
   the acquired VideoOut buffer — which is exactly what the V8 path does at 4K,
   minus the UI step. That is a change of buffer ownership between the decode
   and render threads, so unlike everything above it is **not** decidable on
   host timings alone.
2. **Raise the worker count.** The player passes 4 for the 1080 path; 8
   measured ~15% faster than 4 at 4K even after both pool fixes. The PS5 has
   more cores than the player is using — but confirm on hardware, because
   contending with the decoder threads may cost more than the conversion gains.
3. **Pace the render loop.** There is no vblank wait anywhere: the loop is
   `usleep(500)` in the player and `usleep(2000)` in menus, so a static
   settings page redraws and re-swizzles a full 1920×1080 frame up to 500 times
   a second. Note that some UI animation is frame-counted rather than
   time-based (`osd_visibility += 42`), so capping the rate changes how fast
   those animations run — that has to be converted to wall-clock time in the
   same change, not after it.

---

## A trap worth knowing

The tiled destination plane is **not** `width * height`. The swizzle addresses
whole 512×128 tiles, so at 1920×1080 the highest offset produced is

```
65536 * (3 + 8 * (1920/512.0)) + 65535  =  2,228,223
```

against only 2,073,600 pixels in the visible frame — 13.8% larger. On the
console this is invisible because VideoOut registers an aligned buffer. A host
harness that allocates `width * height` corrupts the heap on the first frame,
which is exactly how the first run of this benchmark ended. `bench_converter.c`
has a `tiled_plane_pixels()` helper; use it for any other host test against
this code.
