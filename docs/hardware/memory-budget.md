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
- **`malloc` could not reach any of it** — until #94. Flexible and direct are
  separate, so 11 GB of headroom did nothing for a component that allocates
  with `malloc`, which is every software decoder in the tree. The malloc shim
  now spills to direct memory once flexible memory refuses; see
  [Correction: 4K software decode does run flexible memory dry](#correction-4k-software-decode-does-run-flexible-memory-dry-94).

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
- **The "route software decode through direct memory" project was cancelled
  here, and then turned out to be needed** (#94, below): 4K 10-bit software
  decode does run flexible memory dry. It landed as a fallback in the malloc
  shim rather than a custom allocator in `evo_vdec_ffmpeg.c`.
- **Hardware 4K decode was never in question** and still isn't.

## Correction: 4K software decode does run flexible memory dry (#94)

**Measured on hardware 2026-09-25/26**, 4K 10-bit AV1 (Netflix Chimera, raw
`.obu` and `.mkv`) on dav1d with `/mnt/usb0/evo_sw_4k` set.

"There is no shortage" above was measured at decode-open. Two seconds into
4K 10-bit AV1, the same pool is **empty**:

```
alloc [decode-open] live=55MB  ... map_fail=0 flex_avail=188MB direct=0    direct_live=0MB
alloc [play]        live=488MB ... map_fail=0 flex_avail=0MB   direct=1460 direct_live=246MB
```

Before the fix the shim counted `map_fail=60` in one such run. The first
refused allocation broke FFmpeg's `av1_frame_merge` (and, in a container,
the AV1 parser) for the rest of the file — which is what 2ddd018 misread as
an FFmpeg bug.

`tools/native-app/stubs/malloc_shim.c` backs the heap with **direct memory**
(`sceKernelAllocateDirectMemory` + `sceKernelMapDirectMemory`, 64 KiB
granularity, a table so `free()` can release by physical offset). #94 first
added it as a last resort after flexible memory and anon `mmap`; dav1d then
held a steady ~245–280 MB of direct memory through full-length 4K playback
with `map_fail=0`, all of it released at stop. `evo.log`'s `alloc [...]` lines carry
`direct= direct_live= direct_peak=` at decode-open, every 2 s of playback,
and at stop.

**The memory type matters.** The fallback first shipped with type **3** —
what `evo_direct_mem.c`'s pool uses — and dav1d's pictures, once they spilled
into it, decoded and staged at ~2 fps: reads from it are uncached. It uses
type **11** (general-purpose cached, SharpProspero `KernelMemory.cs`; 12 is
cached-shared-with-GPU), mapped CPU read/write only, and runs at full speed.

As a last resort it still left flexible memory at **0 MB**, and whatever
needs flexible memory outside the shim (system libraries, thread stacks) got
nothing: EVO twice died silently a few seconds after a far seek, with
`flex_avail=0`, `map_fail=0` and no crash-handler line. So blocks of **1 MB
and up now come from direct memory first** (commit `115d9ff`); smaller ones
still use flexible memory, then anon `mmap`, then direct. The large-block
header records which kind it is, so `free()` never scans the table for a
flexible block.

Hardware, 2026-09-26, the same 4K AV1 `.mkv` and Chimera `.obu` with 11 seeks
including a far one: `flex_avail` **200–235 MB** throughout, `map_fail=0`,
`direct_peak` 478 MB, no crash.

## How much direct memory EVO can actually take

**Measured 2026-09-26** with the boot probe (`/mnt/usb0/evo_dm_probe`, see
[tooling.md](../build/tooling.md)), after the GPU runtime and resident
hardware decoders already held their share:

```
dm probe: +1  total=256MB  alloc_us=3 map_us=6 touch_us=561 verify=ok largest_free=10678MB
...
dm probe: +32 total=8192MB alloc_us=3 map_us=4 touch_us=607 verify=ok largest_free=2742MB
dm probe: held 8192MB in 32 chunks, all released
```

- **At least 8 GB**, type 11, every 16 KiB page written and read back. The
  probe stopped at its ceiling, not at a refusal - 2.7 GB was still free.
- **Cheap:** ~5 µs to allocate and map 256 MB; 73 ms for the whole 8 GB.
- **Clean:** `direct_largest_free` was back to 10934 MB afterwards.

A 4K software decode needs about 0.5 GB of it. Memory is no longer a reason
to refuse anything the CPU can decode in time.

## Open, and not explained by capacity

`evo_direct_mem.h` records that raising EVO's pool to 192 MiB "wedged the first
4K V8 present" (#6, backed out → #55). There were gigabytes free then too, so
**capacity is not the explanation** — something else about a larger WB_ONION
reservation is wrong, and it has to be found rather than assumed away.

**Probable answer (#94):** that pool is type 3, which `evo_direct_mem.c`
labels WB_ONION but which behaves as uncached for CPU reads — the same type
made dav1d crawl at ~2 fps when the malloc shim first used it. Routing video
buffers into a larger type-3 pool would look exactly like a wedge. Not yet
re-tested with type 11/12. Do not
take "11 GB is free" to mean "we can take a gigabyte of it" until that is
understood.

## Related

- `media/src/evo_direct_mem.c` — the reporter, and EVO's pool
- `core/src/services/PlaybackController.cpp` — the 1080p guard and its switch
- [codec-support.md](../codec-support.md) — which codecs land on which path
- [evo-pro/status.md](../evo-pro/status.md) — the 2026-09-02 figures that were right
