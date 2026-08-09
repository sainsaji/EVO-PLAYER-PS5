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

---

## Where the remaining time goes

At 4 workers the fused path uses about **64% of a 60fps budget at 4K** on this
host, and scaling flattens after 4 workers (3.29× at 4, 3.72× at 8). Two
things worth trying, in this order:

1. **Raise the worker count.** The player passes 4 (`workers typically 4
   (match V3/V6B)`). 8 measured ~11% faster than 4 at 4K even after the pool
   fix. The PS5 has more cores than the player is using — but confirm on
   hardware, because contending with the decoder threads may cost more than
   the conversion gains.
2. **The swizzle, not the colour math.** `pp_tiled_pixel_offset` is called per
   pixel and still evaluates a `double` divide and multiply per call. The
   comment in `tile_copy.c` describes exactly this being optimised out of the
   *linear* tiling path; the fused path calls the unoptimised helper four
   times per 2×2 block. Hoisting the tile base out of the inner loop is the
   obvious next experiment, and the hash check makes it safe to attempt.

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
