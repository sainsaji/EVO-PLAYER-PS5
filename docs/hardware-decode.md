# Hardware decode — the plan

**Goal: get video decoding off the CPU and onto the console's own decoder.**

This document is written to be picked up cold. It carries everything needed to
resume the work without re-deriving anything, and it is deliberately a *plan of
attack* rather than a status page — the raw findings live in
[native-media-research.md](native-media-research.md), which this links to
rather than repeats.

---

> **Read [hardware-decode-review.md](hardware-decode-review.md) alongside this.**
> A reverse-engineering review of this plan argues its ordering is wrong: it
> spends a console deploy per guess when the modules are already mapped and
> decrypted in the payload's own address space. One deploy that dumps them
> turns most of the remaining work into offline static analysis. Ten of its
> top fifteen investigations need no console at all.

---

## Why this is the one that matters

Everything in EVO Player runs on the CPU. There is no hardware video decode,
and there is no GPU either — the sysroot ships OSMesa/llvmpipe, a *software*
rasteriser ([gpu-notes.md](gpu-notes.md)). Every frame is decoded by FFmpeg on
CPU cores and then colour-converted and tiled by hand-optimised C.

That single fact is the ceiling on almost everything still missing:

| Wanted | Blocked by |
|---|---|
| 4K60 | CPU decode throughput |
| HEVC Main10 at speed | CPU decode throughput |
| High-bitrate HEVC without stutter | CPU decode throughput |
| 10-bit preserved end to end | 8-bit output plane, *and* decode cost |
| HDR | 10-bit output + display mode + tone mapping |

Fix decode and most of the list moves at once. Nothing else available — more
converter tuning, more threads — moves any of it, because the decoder is
already the bottleneck.

**Definition of done for this effort:** a decoded video frame produced by a
Sony module, displayed on screen, at a resolution and bitrate the CPU path
cannot sustain. Anything short of that is a step, not the goal.

---

## Start here — the 60-second version

1. The SDK ships **no stubs** for any Sony media module. You cannot
   `-lSceVdecCore`. That is real and does not change.
2. This does **not** mean unreachable. On 12.70 the modules *load at run time*
   and their functions *resolve by NID*. **This is already proven on hardware.**
3. `libSceAvPlayer` — all six entry points resolved to real addresses.
   **Nobody has called any of them yet.** ← this is where the work resumes
4. `libSceVdecCore` — loads, but no function names resolved. It is a *naming*
   problem, not an availability problem.

So: the door is open and nobody has walked through it.

---

## What is already proven — do not re-derive this

From `decoder_test` on firmware 12.70 (`0x12700001`), 2026-08-09. Full log in
[native-media-research.md § Results log](native-media-research.md#results-log).

**All three media modules load:**

| Module | modid | base |
|---|---|---|
| `libSceAvPlayer.sprx` | 0x29 | `0x8008f4000` |
| `libSceVdecCore.sprx` | 0x2b | `0x80094c000` |
| `libSceVideoDecoderArbitration.sprx` | 0x2c | `0x800a1c000` |

**All six `libSceAvPlayer` symbols resolve:**

| Symbol | NID | Address |
|---|---|---|
| `sceAvPlayerInit` | `aS66RI0gGgo` | `0x8008f4d00` |
| `sceAvPlayerAddSource` | `KMcEa+rHsIo` | `0x8008f60e0` |
| `sceAvPlayerGetVideoData` | `o3+RWnHViSg` | `0x8008f7040` |
| `sceAvPlayerGetAudioData` | `Wnp1OVcrZgk` | `0x8008f6e60` |
| `sceAvPlayerIsActive` | `UbQoYawOsfY` | `0x8008f6d30` |
| `sceAvPlayerClose` | `NkJwDzKmIlw` | `0x8008f5e50` |

> Addresses are per-boot and per-firmware. Resolve them at run time; never
> hardcode. They are recorded here only as evidence the resolution works.

**No proprietary files were needed for any of this.**

## The method that works

Three steps, and each one matters:

```c
#include <ps5/nid.h>   /* nid_encode() */

/* 1. Load the module - it is NOT mapped unless you link its stub. */
int res;
int modid = sceKernelLoadStartModule("/system/common/lib/libSceAvPlayer.sprx",
                                     0, NULL, 0, NULL, &res);

/* 2. Get its dynlib handle - a DIFFERENT value from the module id. */
uint32_t dynh;
kernel_dynlib_handle(getpid(), "libSceAvPlayer.sprx", &dynh);

/* 3. Resolve by NID, not by name. */
char nid[12];
nid_encode("sceAvPlayerInit", nid);
intptr_t addr = kernel_dynlib_resolve(getpid(), dynh, nid);
```

`projects/decoder_test/main.c` implements all of this and is the place to add
new probes.

## Three traps, already paid for

- **`sceKernelDlsym` by plain name always fails** (`0x80020003`, ESRCH). Sony
  modules export NIDs — a hash of the name — not names. Use `nid_encode()` +
  `kernel_dynlib_resolve()`.
- **Passive probing lies.** `kernel_dynlib_handle` only finds modules the
  payload actually *depends on*. An unloaded module reports "not mapped" even
  when it is present. An early run reported 0/10 including `libSceVideoOut`,
  which demonstrably works. `decoder_test` therefore links
  `-lSceVideoOut -lSceAudioOut` purely as a control: **if that control does not
  show 3/3, every other result in the run is meaningless.**
- **modid ≠ dynlib handle.** `kernel_dynlib_resolve` wants the latter.

---

## The plan

Two independent routes. They do not block each other — if one dies, the other
is untouched. Run **Route A first**: it is cheaper and its payoff is larger.

### Route A — `libSceAvPlayer` (high level)

One API that does demux + decode + A/V sync. If it works, most of the problem
is solved at once. Work in `projects/avplayer_test/`, which is currently a
34-line placeholder reserved for exactly this.

**A1 — Call `sceAvPlayerInit` and see what it returns.**

The blocker is the argument struct. On PS4 it is `SceAvPlayerInitData`, which
carries a memory-allocator block (allocate / deallocate / allocateTexture /
deallocateTexture), a file-callback block (open / close / readOffset / size),
an event callback, `numOutputVideoFrameBuffers`, `autoStart` and a default
language string.

> **The PS5 layout is NOT confirmed.** Treat the PS4 shape as a starting
> hypothesis, not as fact. Getting it wrong most likely returns an error code;
> it could also fault, so expect to need more than one attempt.

- *Success:* a non-error return and a usable handle.
- *Useful failure:* a **consistent** error code. Look it up — `0x80xxxxxx`
  Sony error codes are informative and often say "bad argument" vs "not
  permitted", which discriminates between "struct is wrong" and "not allowed
  from here".
- *Cheap first move:* pass a zeroed struct and record the error. That alone
  tells you whether it validates arguments before checking permissions.

**A2 — `sceAvPlayerAddSource` with a file on the USB stick.**

Only meaningful once A1 returns cleanly. This is where a sandbox requirement
would show up, because the file callbacks have to actually open something.

**A3 — `sceAvPlayerGetVideoData` in a loop.**

The payoff. If frames come back, find out what format and what memory they
live in.

**A4 — Get a frame to the screen without a CPU copy.**

The open question that decides whether this is *faster* or merely *different*.
A hardware-decoded frame that has to be memcpy'd and colour-converted on the
CPU may not beat what FFmpeg already does. Frames likely arrive in a tiled or
GPU-visible format — the existing tiled VideoOut plane
(`PP_VO_ATTR_TILED_BGRA`, `pp_videoout.c`) may be a closer match than it looks.

### Route B — `libSceVdecCore` (low level)

More control, more work. Currently blocked on **not knowing what the functions
are called**: the module loads and maps, but none of the PS4-era
`sceVideoDecoder*` names resolve. The PS5 API is simply named differently.

**B1 — Recover the real export names.** Two approaches:

- Dump the module's `PT_DYNAMIC` segment for its NID list, then reverse the
  hashes against a wordlist of plausible names. `sce_stubs/genstub.py` already
  walks `PT_DYNAMIC` with pyelftools — read it before writing anything.
- Match this module's NIDs against `aerolib.csv` from
  [zecoxao/sce_symbols](https://github.com/zecoxao/sce_symbols), which the SDK
  already downloads for stub generation.

The second is far cheaper. Try it first.

**B2 — Once names are known**, the same load → handle → NID → resolve method
from Route A applies unchanged.

### Route C — generated stubs (fallback only)

If run-time resolution cannot reach something, the SDK supports generating
linkable stubs from a decrypted `.sprx`
([proprietary.md](proprietary.md)). It needs modules **from your own console**,
and they must never enter this repository — `proprietary/` and `*.sprx` are
gitignored precisely so a mistake is hard to make.

Route A already works without this. Do not reach for it early.

---

## What could kill this

Know these before spending a session on it. None is proven fatal; all are
unproven either way.

| Risk | Why it might bite | How to test it cheaply |
|---|---|---|
| **No user session** | A payload has no user — `sceUserServiceGetInitialUser` returns `0x80940004` (established by `videoout_test`). AvPlayer may require one for file access or output. | Check the error code from A1/A2 for a permissions-shaped failure. |
| **Arbitration gating** | `libSceVideoDecoderArbitration` may refuse when no licensed title is running. | If A3 fails while A1/A2 succeed, suspect this. |
| **Unknown struct layout** | A wrong `SceAvPlayerInitData` may fault rather than return. | Zeroed struct first; expect to iterate. |
| **Output format mismatch** | Decoded frames may need a CPU copy to display, eating the win. | A4. Measure before celebrating. |

**If Route A dies at A1 with a permissions error**, that is a real result — it
points at Route B, and it is worth recording precisely because it saves the
next person the same session.

---

## Console workflow — read before deploying

Hardware work means real deploys, and deploys here are expensive.

- **Never stack launches.** Exit the running app before launching again.
  Repeated `hbldr` launches without exiting kernel-panicked the console during
  development and cost about 50 minutes to recover. Each launch opens VideoOut,
  an audio port and the pad, and the previous instance still holds them.
- **Bound every console-facing call with `timeout`.** `prospero-deploy` is
  `socat` against a socket that only closes when the payload exits, so
  deploying a *resident* payload never returns. Exit code 124 (or curl's 28)
  means "it is up and holding the socket" — that is success, not an error.
- **Confirm the console's address**, do not assume it. `.env` at the repo root
  holds `PS5_HOST`; it has changed more than once.
- **Three services must be running**, all needing the jailbreak re-run after
  every reboot: `elfldr` (9021), `websrv` (8080), `klogsrv` (3232). If websrv
  is down but elfldr is up, `./scripts/install-homebrew.sh --setup` restores it
  without touching the console.
- **Batch your experiments.** One deploy that probes five things beats five
  deploys. Build and syntax-check in the container freely — that costs nothing.

Probes like `decoder_test` are *not* resident and print their results, so they
are the cheap, safe kind of deploy:

```bash
./scripts/build.sh decoder_test
PS5_HOST=<ip> ./scripts/deploy.sh output/elf/decoder_test.elf
```

---

## Running this autonomously

The loop closes without a human in it, which matters because the expensive part
of this work is waiting on someone to fetch a result.

```
  write payload  ->  build (docker)  ->  deploy (elfldr 9021)
        ^                                        |
        |                                        v
   analyse offline  <-  pull from USB  <-  payload writes /mnt/usb0/*.bin
                        (websrv 8080)
```

- **Deploy:** `./scripts/deploy.sh output/elf/<probe>.elf`. A probe that runs
  and exits returns normally. Always wrap in `timeout`.
- **Retrieve:** `curl -s -o local.bin http://$PS5_HOST:8080/fs/mnt/usb0/<name>`
  — the websrv `/fs` endpoint serves files over HTTP. `tools/shot.sh` has used
  this to fetch screenshots for months; it is a proven path, not a new idea.
  It is read-only, so the payload writes and the PC reads. That is all this
  needs.
- **Analyse:** `llvm-objdump-18`, `llvm-readelf-18`, `llvm-nm-18` are all in
  the dev container. A dumped module image is just bytes.

**What still needs a person:**

| | Why |
|---|---|
| A payload that hangs | Nothing software-side recovers a payload holding VideoOut. This is the one that costs an hour. |
| Exiting a resident app before relaunching | The stacking rule. Probes exit on their own; the player does not. |
| Go/no-go before the first call *into* AvPlayer | Everything up to that point is read-only reconnaissance. `sceAvPlayerInit` is the first call that can fault or hang. |
| A USB stick present and writable | The dump has nowhere to go otherwise. |

**Therefore: put a watchdog in every experimental payload.** If it has not
finished within N seconds, log what it knows and exit. An experiment that
fails cleanly costs a deploy; one that hangs costs an afternoon.

---

## Files that matter

| | |
|---|---|
| `projects/decoder_test/main.c` | the working probe — 248 lines, module + symbol reconnaissance, with the control check |
| `projects/avplayer_test/main.c` | placeholder, reserved for Route A. Kept separate "so that a dead end here costs nothing elsewhere" |
| `docs/native-media-research.md` | the raw findings and the results log — **append new results here** |
| `docs/proprietary.md` | Route C, and the rules about what must never be committed |
| `docs/gpu-notes.md` | why there is no GPU either, which is the other half of the ceiling |
| `sce_stubs/genstub.py` (in the SDK) | walks `PT_DYNAMIC`; read before attempting B1 |

## Recording results

Append to the results log in
[native-media-research.md](native-media-research.md#results-log), newest first,
with the firmware version and the method used. A negative result is worth as
much as a positive one here and costs the same console trip to obtain — the
"three things this established" section of the last entry saved more time than
the successes did.
