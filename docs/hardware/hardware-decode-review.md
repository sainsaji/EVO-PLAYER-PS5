# Hardware decode — critical review of the research plan

A reverse-engineering review of [hardware-decode.md](hardware-decode.md). That
document is a sound *project* plan. This one argues it is the wrong *research*
plan, and says what to do instead.

It does not replace it. Read that first.

---

## Conventions used throughout

Reverse engineering goes wrong when a plausible guess gets quoted back later as
a fact. Every claim below is tagged:

| Tag | Means |
|---|---|
| **[E]** | **Evidence.** Observed on this console, on 12.70, and recorded. |
| **[I]** | **Inference.** Follows from evidence plus how ELF/SysV/PS4 demonstrably work. Strong, but not observed here. |
| **[H]** | **Hypothesis.** Plausible, untested, and could be wrong. Costs a session if trusted. |

Nothing below is **[E]** unless it is already in the results log.

---

## The headline problem: the cost model is inverted

The current plan's first real step is *call `sceAvPlayerInit` and see what
happens*, with a hypothesised PS4 struct layout. That is expensive
guess-and-check: every iteration is a console deploy, and a wrong struct most
likely returns an opaque error or faults, which teaches you almost nothing
about **why**.

The plan has a much stronger position available and does not use it.

> **[E]** The payload runs arbitrary code in a process where
> `libSceAvPlayer.sprx`, `libSceVdecCore.sprx` and
> `libSceVideoDecoderArbitration.sprx` are loaded and mapped, and it can
> already read their base addresses (`kernel_dynlib_mapbase_addr`, used by
> `decoder_test` today).
>
> **[I]** A module mapped into a process is *decrypted*. Whatever the on-disk
> SELF/SPRX encryption is, the executable image in memory is plain.
>
> **[I]** Therefore a single payload can dump those images to USB, and every
> question about ABI, imports, layout and structure becomes an **offline**
> question answerable with `llvm-objdump` and a hex editor, with **zero**
> further deploys.

**This reframes the whole effort.** The correct first experiment is not "call
Init". It is "get the bytes off the console". One deploy buys effectively
unlimited static analysis; the current plan spends one deploy per guess.

Everything else in this review follows from that.

### The revised phase order

| Phase | Where | Deploys |
|---|---|---|
| **0** Dump the mapped module images | console | **1** |
| **1** Offline static analysis — ABI, imports, layout, strings | PC | 0 |
| **2** Passive runtime recon (no media calls) | console | 1, batched |
| **3** First calls, with a failure taxonomy prepared in advance | console | 1–2 |
| **4** Frame characterisation | console | 1 |
| **5** Benchmarking | console | 1+ |

The existing plan is roughly phases 3→4→5 with 0/1/2 missing. Adding them is
the single largest improvement available, and it front-loads the cheap work.

---

## 1. ABI reconnaissance — yes, and it is nearly free

**Should the ABI be determined before calling `sceAvPlayerInit`? Yes**, and
after Phase 0 it costs no deploys at all.

**[E]** PS5 is x86-64. **[I]** Sony modules use the SysV AMD64 ABI: integer and
pointer arguments in `RDI, RSI, RDX, RCX, R8, R9`; floats in `XMM0–7`; return
in `RAX`; a struct returned by value larger than 16 bytes uses a hidden
pointer in `RDI` (sret), shifting every other argument right by one.

What a prologue disassembly gives you, in order of reliability:

- **Parameter count.** Which argument registers are *read before being
  written*. A function that never touches `RDX` takes at most two arguments.
  This is the highest-confidence signal available and it is free.
- **Hidden `sret`.** If the first thing the function does is store `RDI`
  somewhere and treat `RSI` as the "real" first argument, it is returning a
  large struct by value. Getting this wrong shifts every argument by one, and
  it is a classic reason a call "should work" and does not.
- **Struct layout, directly.** A function taking a pointer to a struct
  dereferences it at fixed offsets. `mov rax, [rdi+0x18]` followed by an
  indirect `call rax` says: **offset 0x18 holds a function pointer** — an
  allocator or a callback. A run of such offsets *is* the struct layout, read
  straight off the instructions rather than guessed from PS4 headers.
- **Size and alignment expectations.** A `memset`/`rep stos` of a constant
  length against `RDI` reveals the struct size the callee believes in.
  Comparing that constant with the PS4 `SceAvPlayerInitData` size is a direct
  test of the plan's central hypothesis, without a single call.
- **Early validation branches.** Functions that check a version or magic field
  usually do it in the first few instructions: `cmp dword [rdi], 0x...` /
  `jne fail`. That constant is a version tag you would otherwise brute-force.

> **[H]** — and worth stating plainly — the PS4 `SceAvPlayerInitData` layout
> may simply be wrong for PS5. The current plan already flags this as a
> hypothesis, which is good. Prologue analysis *replaces* the hypothesis with a
> reading. That is the difference between iterating blind and knowing.

**Cheapest single artefact:** disassemble the first ~40 instructions of all six
resolved AvPlayer entry points. That is minutes of work and probably settles
argument count, sret, and much of the init struct.

---

## 2. Import graph analysis — the biggest omission in the current plan

The plan resolves **exports** from `libSceAvPlayer`. It never looks at what
`libSceAvPlayer` **imports**. That is backwards: imports describe the
architecture, and they describe it without calling anything.

**[I]** Every Sony module's `PT_DYNAMIC` carries its needed-module list and the
NIDs it imports from each. This is the module's own declaration of its
dependencies — not a guess.

What the import list would settle, by module:

| If AvPlayer imports from… | It tells you | Confidence once observed |
|---|---|---|
| `libSceVdecCore` / `Shevc` / `Svp9` | AvPlayer is a *client* of the low-level decoder, and Route A and Route B are the same road at different altitudes | **[E]** once seen |
| `libSceVideoDecoderArbitration` | Arbitration sits on the decode path and will gate you | **[E]** once seen |
| `libSceAjm` | Audio decode is offloaded to the AJM job manager, not done in AvPlayer | **[E]** once seen |
| `libSceVideoOut` | AvPlayer talks to display directly — implies it may own or expect a video-out handle | **[E]** once seen |
| `libSceAudioOut` | Same, for audio output ownership |  |
| `libSceUserService` | **Confirms the user-session risk is real**, and is the single most valuable negative-risk signal available | **[E]** once seen |
| `libSceFios2` | File I/O goes through Sony's async filesystem, and the PS4-style file callbacks may be optional or unused | **[E]** once seen |
| `libkernel` | Which allocator, thread and mmap primitives it needs — directly informs what the allocator callbacks must return | |
| `libSceGnmDriver` | Decoded frames live in GPU memory and you will need GPU submission to touch them | **[E]** once seen |

**The `libSceUserService` case deserves emphasis.** The current plan lists "no
user session" as a risk to be discovered by a failing call in Phase 3. If
AvPlayer imports `sceUserServiceGetInitialUser`, you know before deploying that
the risk is real and roughly where it will bite. If it imports nothing from
`libSceUserService`, a whole category of worry evaporates. Either answer is
worth more than a deploy and costs none.

**Also worth doing: the reverse direction.** Dump imports for
`libSceVdecCore` and `libSceVideoDecoderArbitration` too. If VdecCore imports
heavily from Arbitration, arbitration is mandatory rather than advisory
**[I]** — which changes Route B's difficulty considerably.

---

## 3. Static investigations, ranked by expected value

All of these are Phase 1: offline, zero deploys, after the Phase 0 dump.

| Rank | Investigation | Yields | Effort |
|---|---|---|---|
| 1 | **Import list per media module** (`PT_DYNAMIC` needed + imported NIDs) | The architecture, and the user-session/arbitration/GPU risks, without calling anything | Low |
| 2 | **Export NID list for `libSceVdecCore`** | The input to the Route B name-recovery problem. Without it Route B cannot start | Low |
| 3 | **Prologue disassembly of the six AvPlayer entry points** | Argument count, sret, struct offsets, size constants, version checks | Low |
| 4 | **`aerolib.csv` lookup of every exported NID** | Free names for anything already catalogued. **Do this before any hash brute force** | Very low |
| 5 | **String references** | Codec names, error strings, path prefixes, internal component names. Strings near a function are often its best documentation | Very low |
| 6 | **Relocation tables** | Indirect call targets and vtable construction; distinguishes data pointers from code | Medium |
| 7 | **RTTI / vtables** | **[I]** Sony media modules are substantially C++. RTTI gives real class names (`SceAvPlayer::...`), and a vtable's size and ordering exposes an interface far faster than disassembly | Medium |
| 8 | **`.init_array` / constructors** | What the module sets up at load, and what global state must exist before a call succeeds | Medium |
| 9 | **Import *ordering*** | Weak signal, occasionally shows link-time grouping | Low value |
| 10 | **Firmware diffing** | Only valuable with a second firmware to compare against; high effort, low near-term value | High |

**Two of these deserve promoting above where the current plan puts them.**

**`aerolib.csv` first, always (rank 4).** The plan mentions it as one of two
options for Route B alongside "reverse the hash against a wordlist". Those are
not equal. A CSV lookup is seconds; hash reversal is a search. Look up first.

**NID hashing is a dictionary attack you can run offline (rank 2 + 4).**
**[E]** The SDK ships `nid_encode()`. **[I]** Therefore you can hash *candidate
names* and compare against the module's export list — you never need to invert
the hash. Generate candidates from PS4 names, PS5 naming conventions, strings
found in the module itself, and observed prefixes, hash them all, intersect.
This is embarrassingly cheap and entirely offline, and it is the realistic
route to Route B's blocker.

---

## 4. Runtime reconnaissance — what to add to `decoder_test`

Phase 2. All of this is *passive* — no media API calls — so it is safe, and it
batches into one deploy.

Worth adding, roughly in value order:

- **Full module list of the process** before and after loading each media
  module. **[I]** Loading AvPlayer will pull in its dependencies; the delta
  *is* the dependency list, confirmed at run time rather than read from
  `PT_DYNAMIC`. This cross-checks Phase 1 for free.
- **Map base and size for every loaded module** — needed for the Phase 0 dump
  anyway, and reveals load order.
- **Thread enumeration** before/after load. **[H]** Some modules spawn workers
  in constructors. If AvPlayer creates threads at *load* rather than at
  `Init`, that changes the threading model materially.
- **Open file descriptors / devices** before and after. **[H]** A decoder
  device node appearing (`/dev/vdec*` or similar) would be strong evidence the
  hardware path is reachable at all — and its *absence* after a successful load
  is equally informative.
- **`sceUserServiceGetInitialUser`** result, recorded explicitly. **[E]** It
  returns `0x80940004` in a payload — already established by `videoout_test` —
  but re-record it in the same run so the log is self-contained.
- **Memory map snapshot** (regions, protections) before/after load, to see
  whether the module maps anything unusual — GPU-visible or shared regions.

**One discipline point:** the existing control check (`-lSceVideoOut
-lSceAudioOut` must show 3/3) is excellent practice and should be extended —
every new probe wants a known-good control alongside it, or a null result is
uninterpretable. This is already the sharpest thing in the current plan.

---

## 5. Frame characterisation — measure before displaying

The plan's A4 asks the right question but underestimates it. If
`GetVideoData` returns, these must be measured **before** attempting display,
and all are cheap once you have a frame:

- **Pixel format** — **[H]** most likely NV12 for a hardware decoder, possibly
  P010 for 10-bit content. Do not assume YUV420P.
- **Tiled or linear**, and if tiled, *which* tiling. **[E]** The existing
  VideoOut plane is tiled (`PP_VO_ATTR_TILED_BGRA`) and the repo already
  contains working tile-address logic; a decoder output tiling may or may not
  match it.
- **Stride vs width** — hardware decoders pad aggressively. Assuming
  `stride == width` is a classic first-frame corruption bug.
- **Plane count and plane pointers** — one interleaved chroma plane (NV12) or
  two (YUV420P) changes the converter entirely.
- **CPU visibility** — can the payload read the pointer at all without
  faulting? **[H]** Not guaranteed: the buffer may be GPU-only.
- **Cacheability** — **[I]** if it is write-combined or uncached GPU memory,
  CPU reads are catastrophically slow, and a naive `memcpy` benchmark will
  mislead you into thinking decode is slow when the *read* is.
- **Alignment and physical vs virtual** — needed for any zero-copy attempt.
- **Lifetime / ownership** — how long is the frame valid, and must it be
  returned? Getting this wrong produces intermittent corruption that looks
  like a decode bug.

**The zero-copy question is the one that decides the whole effort.** **[I]** If
frames arrive in GPU memory in a tiling VideoOut can scan out, the win is
enormous. If every frame needs a CPU read from uncached memory plus a colour
conversion, hardware decode may be *slower* than FFmpeg — and that outcome is
entirely possible and must be measured, not assumed.

---

## 6. Timing methodology

The repo already has the right instincts here — `tools/bench.sh` hashes output
and refuses to report timings if the pixels changed. Apply the same discipline.

Measure and log, per run:

| Metric | Why |
|---|---|
| `Init` latency | One-off, but a multi-second init changes UX design |
| **Time to first frame** | The number users feel; also catches lazy hardware init |
| Steady-state decode throughput (fps at resolution/bitrate) | The actual goal |
| Per-frame CPU time in *your* process | Distinguishes "hardware decoded" from "software decoded inside their library" |
| **Copy cost separately from decode cost** | Non-negotiable — see cacheability above |
| Presentation latency (decode → on screen) | Whether A/V sync is achievable |
| Memory bandwidth / bytes touched per frame | The repo's existing bottleneck framing |

**The critical control:** run the *identical clip* through the existing FFmpeg
path and the new path, on the same console, in the same session. Absolute
numbers on a console with unknown background load prove little; a paired
comparison proves everything. **[I]** A hardware path that is not decisively
faster on a clip the CPU path already struggles with is not worth the
integration risk.

**Log format matters.** Emit machine-readable lines (one record per frame or
per second) rather than prose, so a run can be diffed against a previous run.
The existing `output/logs/` convention and `tools/klog.sh` already support
this.

---

## 7. Thread model

**How to determine whether AvPlayer creates threads:** enumerate threads before
`Init`, after `Init`, and after `AddSource`. The deltas answer it directly, and
this piggybacks on the Phase 2 probe with no extra deploy.

**Callback threading is the real hazard.** **[I]** On PS4, AvPlayer's allocator
and file callbacks are invoked from *its* internal threads, not the caller's.
That means:

- The allocator callbacks must be thread-safe. A naive `malloc` wrapper is
  probably fine; anything touching player state is not.
- The event callback may arrive on an arbitrary thread, so anything it touches
  in EVO Player's own state needs the same care.

**Deadlock detection, early and cheaply:**

- Give every callback a **timeout-guarded** log line — record entry and exit
  with a timestamp and thread id. A callback that enters and never exits is
  visible immediately in the log rather than as a hang.
- Never call back into AvPlayer from inside a callback on the first attempt.
  **[H]** Re-entrancy rules are unknown and re-entering is a plausible
  deadlock.
- Set a watchdog: if no frame arrives within N seconds, log the state of every
  thread and exit cleanly rather than hanging the console. **A payload that
  hangs holding VideoOut is exactly the state that costs an hour to recover
  from** — this is not a theoretical concern in this project.

---

## 8. Failure taxonomy — prepare it *before* Phase 3

The plan says "look up the error code", which is right but insufficient.
Prepare the discriminator table in advance so a single deploy classifies the
failure instead of merely recording it.

| Class | Expected signature **[H]** | Cheapest discriminator |
|---|---|---|
| Incorrect structure | `0x8...` invalid-argument family, *consistent* across attempts | Vary **one** field at a time; a changing code means it is parsing your struct |
| Permission / privilege | Fails identically with a zeroed struct **and** a plausible struct | Zeroed-struct control — if both fail the same way, it is not the struct |
| Missing user session | Fails at the point file or output is touched, not at `Init` | Correlate with the recorded `sceUserServiceGetInitialUser` result |
| Licensing / arbitration | `Init` succeeds, decode fails | Whether `libSceVideoDecoderArbitration` appears in the import graph |
| Unavailable hardware | Fails after arbitration, or returns "no resource" | Query resource info first if such an export exists |
| Bad callback | Fault *inside* the module, or a hang | Callbacks that only log and return a fixed value |
| Allocation failure | Fails after your allocator is called | Log every allocator invocation with size and alignment |
| Unsupported environment | Fails immediately, no callbacks invoked at all | **Zero callback invocations is the signal** |

**The single highest-value experiment design point in this review:**

> **Instrument the callbacks before the first call.** Make every allocator and
> file callback log "called, with these arguments" and return something benign.
> Then a failing `sceAvPlayerInit` still tells you: *did it call my allocator?*
> *with what size and alignment?* *did it try to open a file?*
>
> A failed call with instrumented callbacks yields the struct layout, the
> allocation pattern and the file access model in one deploy. A failed call
> without them yields an error code. **Same deploy cost, order-of-magnitude
> difference in information.**

This is the point where the current plan leaves the most value on the table.

---

## 9. Route expansion — yes, and one new route outranks Route B

The three existing routes are sound but incomplete. Proposed additions:

**Route D — Import-graph architecture reconstruction.** *(Promote above Route
B.)* Purely static, zero deploys after Phase 0, and it informs every other
route. Should arguably be Route A′ — done *before* the first call, not after.

**Route E — Reverse `libSceVideoDecoderArbitration`.** **[I]** Its name says it
mediates access to a scarce hardware resource. If it gates decoding, it is not
optional and discovering that in Phase 3 wastes a deploy. Its export list and
imports are cheap to read in Phase 1.

**Route F — Investigate `libSceAjm`.** The current plan is video-only, but
AvPlayer does audio too. **[H]** AJM ("audio job manager") is the likely audio
decode offload path. Relevant both because AvPlayer probably needs it, and
because **offloading audio decode is a real win in its own right** — E-AC3 and
TrueHD are not free on CPU. This may be a smaller, independently valuable
target than video.

**Route G — GPU video surfaces.** **[E]** The sysroot has no hardware GL/Vulkan
(OSMesa/llvmpipe only), but **[I]** that is about *rendering*; a decoder
writing to a GPU-visible surface that VideoOut scans out does not require a GL
driver. Worth separating these two "no GPU" claims, because conflating them may
be hiding an available path.

**Route H — Analyse a Sony application that uses AvPlayer.** **[H]** If a
system app on the console links AvPlayer, its import list and call sequence are
a worked example of the correct calling convention. Legally and practically
this stays within "read what is already on your own console", the same standing
as the modules themselves. Potentially the highest-value route of all — a
*known-correct* usage example — but with the highest uncertainty about whether
a suitable binary is accessible.

**Not recommended:** firmware diffing, until there is a specific question a
second firmware would answer.

---

## 10 & 13. Media architecture reconstruction

**Everything in this section is [I] or [H].** No module internals have been
examined; this is reconstruction from module names, PS4 precedent and general
media-stack architecture. It is a hypothesis to *test in Phase 1*, and the
import graph will confirm or destroy most of it in an afternoon.

```
   file on USB
        │
        ▼
  ┌─────────────────┐   [H] file I/O may go via libSceFios2 rather than
  │  libSceAvPlayer │        the PS4-style file callbacks
  │  orchestration: │
  │  demux, sync,   │
  │  presentation   │
  └────────┬────────┘
           │
     ┌─────┴───────────────────────────┐
     ▼                                 ▼
 VIDEO                              AUDIO
     │                                 │
     ▼                                 ▼
┌────────────────────┐        ┌──────────────────┐
│ libSceVdecwrap     │ [H]    │ libSceAjm        │ [H] audio decode
│  ↓ dispatch by     │        │  AAC/AC3/ATRAC   │     offload
│ libSceVdecCore     │        └────────┬─────────┘
│ libSceVdecShevc    │                 │
│ libSceVdecSvp9     │                 ▼
└─────────┬──────────┘        ┌──────────────────┐
          │                   │ libSceAudioOut   │ [E] known reachable
          ▼                   └──────────────────┘
┌─────────────────────────────┐
│ libSceVideoDecoderArbitration│ [H] gates decoder access
└─────────┬───────────────────┘
          ▼
   decoded frame buffers
   [H] GPU memory, NV12/P010, tiled
          │
          ▼
┌──────────────────┐   [H] colour conversion / scaling may happen here,
│ libSceGnmDriver  │        in the decoder, or not at all
└─────────┬────────┘
          ▼
┌──────────────────┐
│ libSceVideoOut   │ [E] known reachable, currently used by EVO Player
└─────────┬────────┘
          ▼
       display
```

### Confidence per relationship

| Relationship | Confidence | Basis |
|---|---|---|
| `libSceVideoOut` reachable and usable | **[E]** high | EVO Player uses it today |
| `libSceAudioOut` reachable and usable | **[E]** high | EVO Player uses it today |
| AvPlayer / VdecCore / Arbitration load and map | **[E]** high | Results log, 12.70 |
| AvPlayer's six entry points resolvable | **[E]** high | Results log |
| AvPlayer orchestrates demux + decode + sync | **[I]** high | PS4 precedent, name |
| Arbitration gates decoder access | **[H]** medium | Name only |
| `Vdecwrap` dispatches to codec-specific modules | **[H]** medium | Naming pattern |
| AJM handles audio decode | **[H]** medium | PS4 precedent |
| Frames land in GPU memory | **[H]** medium | Typical HW decoder design |
| Frames are NV12/P010 and tiled | **[H]** medium | Typical HW decoder design |
| Fios2 used for file I/O | **[H]** low | Not yet examined |
| Colour conversion happens in the GPU/decoder | **[H]** low | Genuinely unknown |

### Modules likely essential and missing from the current investigation

| Module | Why it probably matters | Confidence |
|---|---|---|
| **`libSceAjm`** | Audio decode offload. AvPlayer does audio too, and this is independently valuable for E-AC3/TrueHD | **[H]** |
| **`libSceFios2`** | Sony's async file I/O. May be how AvPlayer reads files, making the PS4 callback model irrelevant | **[H]** |
| **`libSceGnmDriver`** | If frames are GPU-resident, this is how you touch them | **[H]** |
| **`libSceSysmodule`** | `sceSysmoduleLoadModuleInternal` is the *supported* load path; `decoder_test`'s own header comment mentions it as an alternative to `sceKernelLoadStartModule`. Worth comparing — it may initialise state that the raw load does not | **[I]** |
| `libSceNpDrm` | Only if arbitration turns out to be licence-driven | **[H]** low |

**`libSceSysmodule` is the most actionable of these.** **[H]** If AvPlayer
expects to be brought up via `sceSysmoduleLoadModuleInternal`, loading it with
`sceKernelLoadStartModule` may leave internal state uninitialised — and would
present exactly as "`Init` fails for no discernible reason". That is a
one-line experiment worth running early, and it is not in the current plan.

---

## 11. Ranked priorities

Scored on information gained, effort, risk, and likelihood of actually
unlocking hardware decode.

| # | Investigation | Info | Effort | Risk | Unlocks | Deploys |
|---|---|---|---|---|---|---|
| 1 | **Dump mapped module images to USB** | Very high | Low | Low | Enables all | **1** |
| 2 | **Import graph of all media modules** | Very high | Low | None | High | 0 |
| 3 | **`aerolib.csv` lookup of all exported NIDs** | High | Very low | None | High (Route B) | 0 |
| 4 | **Prologue disassembly of AvPlayer entries** | High | Low | None | High | 0 |
| 5 | **NID dictionary attack for VdecCore names** | High | Low | None | High (Route B) | 0 |
| 6 | **Instrumented callbacks + `Init` attempt** | High | Medium | Medium | Decisive | 1 |
| 7 | Passive runtime recon (threads, fds, modules) | Medium | Low | Low | Medium | 1 (batch) |
| 8 | `sceSysmoduleLoadModuleInternal` vs raw load | Medium | Very low | Low | Medium | 1 (batch) |
| 9 | Strings + RTTI/vtable analysis | Medium | Medium | None | Medium | 0 |
| 10 | Frame characterisation | High | Medium | Medium | Decisive | 1 |
| 11 | Paired benchmark vs FFmpeg | High | Medium | Low | Validates | 1 |
| 12 | Arbitration module analysis | Medium | Medium | None | Medium | 0 |
| 13 | AJM investigation | Medium | Medium | Low | Separate win | 0–1 |
| 14 | Sony app usage example | Very high | High | Medium | Very high | 0–1 |
| 15 | Firmware diffing | Low | High | None | Low | 0 |

**Ten of the top fifteen need zero deploys.** That is the review's central
point: the current plan's first action is #6, and #1–#5 all make #6 more likely
to succeed while costing one console trip between them.

---

## 12. Cheap wins

Highest information per unit of console risk, all Phase 0/1:

1. **The memory dump itself** — one deploy, unlocks everything static.
2. **Import lists** — the architecture, for free.
3. **`aerolib.csv` intersection** — free names for already-catalogued NIDs.
4. **Prologue disassembly** — argument counts and struct offsets in minutes.
5. **NID dictionary attack** — `nid_encode()` already exists; hash candidates
   and intersect. Never invert a hash you can forward-compute.
6. **Strings** — codec lists, error text, component names.
7. **Module load-order delta** — dependencies confirmed at run time, piggybacked
   on an existing probe.
8. **Recording negative results properly** — the last entry's "three things this
   established" saved more time than its successes. This is a *process* cheap
   win and the project already does it well.

---

## What I would change about the existing plan, concretely

1. **Insert Phase 0 and Phase 1 before A1.** Dump, then analyse offline. This
   is the whole review in one line.
2. **Instrument callbacks before the first `Init` call.** Turns a failed deploy
   from "an error code" into "the struct layout and allocation pattern".
3. **Promote import-graph analysis to its own route, ahead of Route B.** It
   informs every other route and costs nothing.
4. **Reorder Route B's two options** — `aerolib.csv` lookup first, hash search
   second; and frame the hash work as forward-hashing candidates, not inversion.
5. **Add AJM and Fios2** to the investigation. The plan is video-only; AvPlayer
   is not.
6. **Add the `sceSysmodule` load-path comparison** — cheap, and a plausible
   silent cause of `Init` failure.
7. **Separate "no GPU driver" from "no GPU-visible surfaces"** — the first is
   evidenced, the second is assumed, and conflating them may be hiding a path.
8. **Make the benchmark a paired A/B against FFmpeg on the same clip**, and
   measure copy cost separately from decode cost.
9. **Add a watchdog to every experimental payload.** A hang holding VideoOut is
   the expensive failure mode in this project, and it is preventable.

## What the existing plan already gets right

Worth stating, because these should not be lost in a revision:

- **The control check** (`-lSceVideoOut -lSceAudioOut` must show 3/3, else the
  run is meaningless). This is better experimental hygiene than most RE work.
- **Two independent routes**, so a dead end costs nothing elsewhere — and
  `avplayer_test` kept as a separate project for exactly that reason.
- **Console safety rules** stated up front, with the panic cost made explicit.
- **Recording negative results** as first-class findings.
- **Flagging the PS4 struct layout as a hypothesis** rather than asserting it.
- **A definition of done** that requires beating the CPU path, not merely
  producing a frame.
