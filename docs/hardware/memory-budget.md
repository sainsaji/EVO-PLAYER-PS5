# The title's memory budget — measured

**Measured on hardware 2026-09-25**, app module `PPSA99039`, build
`fd90f0f9-dirty_0925-0900`, firmware 12.70. Every figure here comes from a
kernel query, not from an inference. If you are about to say "EVO is
memory-limited", say which pool, and check it against this page first.

---

## The numbers

```
mem budget [boot] direct_total=12288MB direct_largest_free=11000MB
                  flex_total=448MB flex_free=268MB evo_pool=0/64MB peak=0MB hw=1
```

| Pool | Size | Free | What uses it | EVO's share |
|---|---|---|---|---|
| **Direct memory** | **12288 MB** | 11000 MB largest contiguous run | GPU / sceAgc / VideoOut, anything explicitly allocated + mapped | a **64 MiB** pool, peak 17 MB — **0.5%** |
| **Flexible memory** | **448 MB** | 268 MB at boot, 144–192 MB during playback | `malloc`, and therefore FFmpeg, dav1d, RmlUi, everything ordinary | ~50 MB live, 93–102 MB peak |
| **Hardware decoder** | internal to `sceVideodec2` | — | native AVC / HEVC / VP9 | 4K real time, no constraint found |

Two things worth stating plainly, because both contradict what this repo
believed for months:

- **Direct memory is not scarce.** 12 GB, of which EVO reserves 64 MiB. The
  pool size was chosen because it was proven not to wedge the GPU, never
  because anything asked the kernel what was available.
- **`malloc` cannot reach any of it.** Flexible and direct are separate. Code
  has to allocate and map direct memory explicitly, so 11 GB of headroom does
  nothing for a component that allocates with `malloc` — which is every
  software decoder in the tree.

## How to re-measure

`evo_mem_budget_log(const char *when)` — `media/src/evo_direct_mem.c`. It asks
the kernel directly (`sceKernelGetDirectMemorySize`,
`sceKernelAvailableDirectMemorySize`, `sceKernelConfiguredFlexibleMemorySize`,
`sceKernelAvailableFlexibleMemorySize`) and writes one line to
`/mnt/usb0/evo.log`. Called at four points:

| tag | where |
|---|---|
| `boot` | `Application::initialize`, right after the pool is created |
| `decode-open` | every successful decoder open, beside the allocator's own line |
| `sw-refuse` | the 1080p software-decode refusal |
| `native-declined` | native decoder declining, falling back to FFmpeg |

`-1` in a field means the call failed rather than returned zero.
`direct_largest_free` is the largest **contiguous run**, not total free — a
fragmented pool with plenty free and no contiguous stretch cannot back a 4K
plane, and that distinction is the whole question for video.

## What was believed before, and why it was wrong

A "~180 MB flexible ceiling" was quoted across the docs and in code comments.
It came from reading the allocator's own telemetry at decoder open —
`alloc live=54MB … flex_avail=124MB` — and adding the two.

That sum is not a total. `live` is what the malloc shim tracks; `flex_avail` is
what the kernel says is left; plenty of flexible memory is committed by neither.
On one run the sums read **227, 241, 224 and 197 MB** — a 44 MB spread. A
number that moves by 44 MB was never measuring a ceiling.

`docs/evo-pro/status.md` had recorded `flex_avail=281M` and a "hard ~450 MB"
budget back on 2026-09-02, which was right. The ~180 MB figure appeared later
and was believed anyway.

## What it changes

- **The 1080p software-decode cap was wrong.** `PlaybackController.cpp`
  refused software decode above 1080p because AV1 4K's 135 MB peak was thought
  not to fit in ~125 MB. Actual free flexible memory at that moment is
  144–192 MB. Lifted behind `/mnt/usb0/evo_sw_4k`, 4K 10-bit AV1 decoded
  **faster than real time** (`late_drop=0`, `early_sleeps` climbing,
  `flex_free=192MB`). The guard was refusing something that works.
- **The "route software decode through direct memory" project is cancelled.**
  It existed to solve a flexible-memory shortage. There is no shortage.
  `evo_vdec_ffmpeg.c` sets no custom allocator and does not need one.
- **Hardware 4K decode was never in question** and still isn't.

## Open, and not explained by capacity

`evo_direct_mem.h` records that raising EVO's pool to 192 MiB "wedged the first
4K V8 present" (#6, backed out → #55). There were gigabytes free then too, so
**capacity is not the explanation** — something else about a larger WB_ONION
reservation is wrong, and it has to be found rather than assumed away. Do not
take "11 GB is free" to mean "we can take a gigabyte of it" until that is
understood.

## Related

- `media/src/evo_direct_mem.c` — the reporter, and EVO's pool
- `core/src/services/PlaybackController.cpp` — the 1080p guard and its switch
- [codec-support.md](../codec-support.md) — which codecs land on which path
- [evo-pro/status.md](../evo-pro/status.md) — the 2026-09-02 figures that were right
