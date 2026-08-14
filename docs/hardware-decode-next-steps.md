# Hardware decode — next steps, as phases

Where the work resumes, and in what order.

Phases 0–3 are **done**: the module images are dumped, the ABI is read offline,
the export map is measured, and the first call into the decoder returned `0`.
See [hardware-decode-findings.md](hardware-decode-findings.md) for what that
established — **read it before starting anything below**, because every phase
depends on something in it.

**Phase 4 ran on 2026-08-10 and succeeded**, in three runs. The compute queue
allocates — the risk register's most likely hard blocker is gone — and the
memory ceiling that looked like a second blocker turned out to be an artefact
of the launch slot, not a limit of the console.

**Phase 5 ran on 2026-08-11 and succeeded**, in two deploys.
`sceVideodec2CreateDecoder` returns `0`, for H.264 **and for HEVC**. **No
entitlement gate appeared anywhere** — the risk the phase existed to test. Its offline half, which cost no deploys at all,
corrected two config fields that had been recorded backwards since Phase 2, and
that correction is what let the same run establish HEVC is accepted and VP9 is
not.

**Phase 6 ran on 2026-08-11, in seven deploys, and has not produced a picture
yet.** `projects/decodeframe_test/`. No hang in any run, clean teardown every
time, console healthy throughout.

What is settled:

- **18/18 `Decode` and `GetPictureInfo` validation controls exact.** The
  four-argument ABI, every struct size and offset, and the error mapping are
  measured, not just read.
- **7/7 `mapMemorySize` predictions exact.** The frame-layout formula holds
  across AVC 720p/1080p/4K and HEVC Main and Main10. Most of Phase 7's first two
  rows are now **[E]**.
- **`sceVideodec2MapDirectMemory` is fully characterised and working** — field
  order, the 16 KiB size granularity, `physAddr` as an address in its own space,
  the 16-block limit, and the fact that *everything the hardware touches* must
  be registered: the output frames, the access unit, and the work memory and
  frame pool that `CreateDecoder` was already given. Full contract in
  [hardware-decode-findings.md](hardware-decode-findings.md) §7.

Four successive gates were found and cleared, each one diagnosed from the
module's own diagnostic lines — no sweeps, no guessing:

| gate | cause | fix |
|---|---|---|
| GpDec state 0 | nothing mapped | call `MapDirectMemory` |
| pin ioctl, errno 5031 | `size` not page-aligned | round up to 16 KiB |
| overlap, status `0x33` | every block given `physAddr` 0 | use the real physical offset |
| containment, `0xDA6` | AU, work and pool unregistered | register all of them |

**Where it stands now:** `Decode` gets through every software gate and reaches
the hardware submit, which fails with driver errno **5200**. That is a
different kind of refusal from the four above — the driver declining the job
rather than the module reporting a missing prerequisite.

**The submit is now named.** `libSceVdecCore +0x2b870` issues

    ioctl(fd, _IOW(0x83, 23, 24), { job, dword, arg, dword })

choosing one of three adjacent commands from a mode field at `[obj+0x2c0]`. The
run's `0x254` says the mode is 7, the command is 23, and the driver refused it
with errno 5200.

**A conclusion recorded here previously was wrong and is corrected in
[hardware-decode-findings.md](hardware-decode-findings.md) §7.** It claimed the
refusing code was not in the dump, from a byte search finding no
`mov ecx,0x254` in VdecCore. The search was right; the line number is loaded via
`r14d` at a shared logging tail, so the immediate form never occurs. A byte
search for an immediate only disproves the immediate form.

`projects/codecdump_test/` also settled the question the wrong conclusion had
raised: **an H.264 `CreateDecoder` loads no module at all** — the same 11
modules before and after — and it dumped five modules that had never been
dumped (`libSceSysmodule`, `libSceVdecwrap`, `libSceVdecShevc`,
`libSceGnmDriver`, `libSceAjm`) into `proprietary/dump/codec/`. None contains
the submit.

**Both of those were done on 2026-08-11, and both are now closed.**

**2026-08-13 — the front moved without a deploy.** A static pass over the
VdecCore listing in `PS5-Research`, plus a re-read of transcripts already
captured, produced three results and reordered what to do next. **Phase 6b**
below is the current front; it did not exist before this session.

| finding | what it changes |
|---|---|
| **The submit path is not invariant.** Two writers inside VdecCore select ioctl command **24** under a condition on *bitstream* state, not configuration | run 14's "no cheap way to reach commands 22 or 24" is withdrawn. Testing it is now the **first** thing to do, and it costs one probe |
| **The driver discriminates on the job, not the caller** — the same submit line returns errno **5103** elsewhere in run 8, while its caller succeeds | the authid/caps elevation drops from first place to third. A permission check refuses uniformly |
| **The `0x83` ioctl inventory is complete and the driver handshake succeeds** — a version check that must read `0x07000000`, and an init call, neither ever failing | "we skipped a setup call" is eliminated as a cause |

**The method is the part worth carrying**, because it is the third time it has
paid: everything above came from evidence that was already on disk. Two of the
three were found by asking the whole listing a question — *who writes this
field*, *which ioctl commands does this module issue* — rather than by reading
the one function under suspicion.

1. ~~**Read what the job contains.**~~ **Done — and the premise that it could
   not be read was wrong.** The submit's object is the GpDec device object, not
   the VdecCore object at `decoder+0x78`; it was found by searching our own
   work memory for `mode == 7` at `+0x2c0` together with a plausible fd at
   `+0x1d68` (exactly one match). **The command buffer is well formed**: width
   1920, height 1088, mode 7, real physical addresses into the frame pool. The
   codec layer does build a command, so "the codec module was never loaded" is
   dead — `libSceVdecSavc`/`Savc2` were loaded for the first time and changed
   nothing. Mode index 5 → mode 7 → command 23 is **invariant** across every
   resource class and depth tried. Findings §7.
2. ~~**Arbitration.**~~ **Done, and ruled out.** The hang was an unresolved
   lazy import — `+0x350` is a PLT stub — fixed by loading
   `libSceVideoArbitration.sprx` first. With arbitration fully up
   (`Initialize`/`Enable`/`AcceptEvent` all returning 0) the decode is refused
   with the identical errno 5200. Findings §7.

**Also closed:** Annex-B is the right framing (AVCC is rejected earlier, in
software); the four config fields that differed from the working reference
client are now aligned; and `0x1450` is confirmed a genuine ioctl `errno`, read
off the submit's own `__error()` path.

**What is next: read the kernel.** Everything on our side of the ioctl is now
measured and correct, so the remaining question lives in the driver — and the
driver is readable. `KERNEL_ADDRESS_TEXT_BASE` is resolved by the SDK for this
firmware and `kernel_copyout` is arbitrary kernel read, so **no firmware
decryption is needed**. Findings §13 sets out the two steps: a read-only pass
that reports the payload's authid/caps and disassembles whatever kernel
function produces `0x1450`, and — only with explicit agreement, because it
writes kernel state — wrapping `Decode` in the authid/caps elevation
`prospero_media_standalone/core/pt.c` already performs.

**And one thing that must not be repeated.** `sceVideoOutOpen` from a payload
**kernel-panicked the console**. Video out is not available here at all: the
real logged-in user is refused with `0x80290001`. See findings §9.1 and
standing rules 18–20. This also constrains Phase 8, which can no longer assume
this payload can own a video-out handle.

**Also worth fixing:** the modid sweep shows `0x42`–`0x47` and `0x49` for eight
modules — **`0x48` is unaccounted for**, the same shape as Phase 0's missing
`0x34`. The probe drops modids whose name comes back blank, which may be hiding
it.

**The single most useful thing to carry forward: run probes with
`install-homebrew.sh --run`, not `deploy.sh`.** `deploy.sh` injects into
`SceSpZeroConf`, a background service with `dmem#0`, and caps at 41 MiB. The
app slot allocated every size tested, up to the full 322 MiB 4K working set.
Phase 4's first two runs measured the wrong process.

| | Phase | Where | Deploys | Risk | Status |
|---|---|---|---|---|---|
| 0 | Dump the mapped module images | console | 3 | low | **done** |
| 1 | Offline static analysis | PC | 0 | none | **done** |
| 2 | Passive runtime recon + export map | console | 4 | low | **done** |
| 3 | First call — `QueryDecoderMemoryInfo` | console | 5 | low | **done** |
| 4 | Memory + compute queue | console | 3 | medium | **done** |
| ~~4b~~ | ~~Raise the 41 MiB memory ceiling~~ | console | 0 | — | **done** — it was the launch slot |
| ~~5~~ | ~~Create a decoder~~ | console | 2 | — | **done** — `rc=0` for H.264 **and HEVC** |
| **6** | **Decode one frame** | console | 16 so far | medium | **every software gate cleared; the DRIVER refuses with errno 5200.** Arbitration, the codec module, the framing and the config are all ruled out. Next: read the kernel (§13) |
| 7 | Characterise the frame | console | 1–2 | low | frame-size formula **confirmed 7/7** on hardware |
| 8 | Get a frame on screen | console | 2–4 | **high** | |
| 9 | Paired benchmark against FFmpeg | console | 2+ | low | |
| 10 | Integrate into EVO Player | both | many | medium | |

> The review's numbering had frame characterisation at 4 and benchmarking at 5.
> Those are 7 and 9 here — the gap is the three phases of real work between
> "the query answers" and "we have a frame", which the review folded into its
> Phase 3.

---

## Standing rules

These are not general advice; each one was paid for during Phases 0–3.

1. **Read the prologue before calling anything new.** Dump it, disassemble it,
   work out the arguments and the validation, *then* write the call. This has
   worked every time. Guessing has not.
2. **Never brute-force what you can read.** A blind search burned 1,335 calls
   and stalled because it invented a progress metric that was wrong.
3. **Every probe needs a control.** A known-good call alongside the new one, or
   a null result is uninterpretable. Controls caught two broken APIs.
4. **Flush the log after every line, and write it to `/mnt/usb0` too.** Every
   hang so far was diagnosed purely from where the log stopped — and a Phase 4
   run that *succeeded* was nearly recorded as a failure because only the file
   on the stick had the answer.
5. **Bound the deploy with `timeout` on the PC.** In-payload watchdogs do not
   fire here. **`EXIT=124` does not mean the payload hung** — it means the
   socket did not close. A completed run produced it too. Read the log file
   before concluding anything.
6. **Batch experiments.** One deploy that answers five questions beats five
   deploys.
7. **Never stack launches**, and do not start Phase 8 without reading the
   stacking rule in [hardware-decode.md](hardware-decode.md).
8. **Record negative results.** They cost the same console trip as a success.
9. **Order a batched probe cheapest-first.** Phase 4's first deploy hung and
   still returned four answers, because the query, the memory ladder and the
   validation controls all ran before the call that blocked. A hang at the end
   of a probe costs a `timeout`; a hang at the start costs the whole deploy.
10. **Read the layer below, not just the one you are calling.** Rule 1 applies
    transitively. Phase 4 read every `libSceVideodec2` prologue and none of
    `libSceVdecCore`'s — and reading VdecCore afterwards is what turned the
    hang into a one-line fix.
11. **Load every module in the call chain.** An unresolved lazy import blocks
    silently and forever: no fault, no error code, no log line. This is what
    the Phase 4 hang was.
12. **Know which process your probe is running in.** `deploy.sh` lands in
    `SceSpZeroConf` — headless, `dmem#0`, capped at 41 MiB.
    `install-homebrew.sh --run` lands in the PS Now app slot, where the player
    lives and where 322 MiB allocates. Resource measurements do not transfer
    between them.
13. **Read the validator to the end, not to the first accepted call.** Phase 5's
    offline half found two config fields recorded backwards — the mistake was
    invisible for three phases because the wrong reading still produced working
    calls. A reading that explains the successes you have is not the same as a
    reading that is right.
14. **An entry point is not a behaviour.** `CreateHevcDecoder` and
    `QueryHevcDecoderMemoryInfo` are literal aliases of their plain
    counterparts — and so are `GetPictureInfo`/`GetAvcPictureInfo` and
    `GetHevcPictureInfo`/`GetVp9PictureInfo`. Four of eighteen exports. Check
    whether the function you resolved is a thunk before designing an experiment
    around its name.
15. **Check the GOT slot before you call the function.** Rule 11 says load every
    module in the call chain; this says how to find out which, without a deploy.
    An unbound lazy import still points into its own PLT push sequence, and that
    is visible in the dumped s2. `sceVideodec2MapMemory`'s one import is unbound
    while every VdecCore neighbour is resolved — so calling it would have hung
    exactly like Phase 4, and the Phase 6 plan opened by proposing to call it.
    Reading one table cost nothing and saved a deploy.
16. **The module's own diagnostic lines are worth more than its return code.**
    Phase 6's `Decode` returned `0x811D0111`, which the error table calls
    "unexpected VdecCore code" — a generic bucket that says nothing. The three
    lines printed alongside it said everything:

        [VDECCORE@B0A10C22:00000000]
        [SCEVDECCORE@A01D07A8:00000002]
        [SCEVIDEODEC2@A01A07A7:80C00001]

    `[TAG@<module>​<line>:<value>]`, innermost first. Grepping the disassembly
    for `mov ecx,0x<line>` lands on the exact rejection site, and the value is
    the variable it rejected. That turned "the decoder said no" into "the GpDec
    state field is 0 and `sceVdecCoreMapMemoryBlock` is what sets it to 1"
    without a second deploy. **Always capture them**; they come back on the same
    stdout as the probe's own log.
17. **Reading a layer to the end still only explains that layer.** Rule 13 said
    read the validator to the end; Phase 6 did, for `libSceVideodec2`, and
    concluded from a complete and correct reading of it that the map calls were
    not part of decoding. They are — the state they set lives two layers down,
    in GpDec. A correct reading of the wrong layer is still the wrong answer.

18. **Anything that reaches a kernel driver needs sign-off first, described as
    a panic risk.** Raw `ioctl` on a device fd, video-out or GPU context
    acquisition, kernel writes. `sceVideoOutOpen` from a payload
    **kernel-panicked the console** on 2026-08-11 and cost about fifty minutes
    of recovery — see findings §9.1. The same build carried an ioctl probe on
    the decoder fd justified as "low risk by construction, drivers validate
    their inputs"; that reasoning had nothing behind it. **`slotcheck` cannot
    protect against this** — it detects a busy app slot, not a driver you are
    about to wedge. Read-only observation of driver state has produced every
    real finding in Phase 6 and carries none of the risk.

19. **A practice borrowed from a reference implementation is only valid if what
    it assumes about its process is also true of ours.** Moonlight-ps4 opens
    video out before creating a decoder, and it is right to. It is a *title*
    that owns its process. This payload is injected into a **borrowed** app
    slot that already owns the display pipeline, and doing the same thing there
    panicked the console. Copy the finding; re-derive the precondition.

20. **Validate handles by range, not by sign.** The system-user
    `sceVideoOutOpen` retry returned `1309671680` = `0x4E100000` — positive, so
    a `< 0` check accepted it, and it is not a handle. The *first* call had
    already returned the honest answer (`0x80290001`), and a fallback overrode
    it. **A fallback that turns a clean refusal into garbage is worse than no
    fallback.**

21. **Apply rule 15 to every module, not just the one you remember.** The
    arbitration hang cost two deploys and a wrong hypothesis about user
    sessions, and it was visible offline the whole time: `+0x350` is a PLT stub
    whose GOT slot was unbound. Rule 15 existed and was not applied. When a
    call blocks silently, **check its GOT slot before hypothesising about
    anything else** — that is now three occurrences out of three silent blocks.

22. **Load providers before consumers.** Binding is eager at load time on this
    loader, so a module loaded *after* its consumer cannot satisfy it.
    `libSceVideoArbitration.sprx` had to move ahead of
    `libSceVideoDecoderArbitration.sprx` in the load list before the slot bound.

---

## Phase 4 — Memory and the compute queue — **done, 2026-08-10**

**Goal: obtain the memory the decoder asked for, and a GPU compute queue.**
Three runs of `projects/computequeue_test/`. Goal met, both halves.

### What it established

| | |
|---|---|
| Compute memory requirement | **4,805,120 bytes (4.58 MiB)**, fixed — queried without a decoder config, so it does not scale with resolution |
| Structures | `ComputeMemoryInfo` 0x18, `ComputeQueueInfo` 0x10, both read offline and confirmed **7/7** by deliberate-error controls |
| Compute queue | **allocated.** `AllocateComputeQueue(pipe 0, queue 0)` → `rc=0`, handle `0x2002b0500`; `ReleaseComputeQueue` → `rc=0` |
| The requirement nobody had | **`libSceGnmDriver.sprx` must be loaded.** Without it the call hangs. The queue is a Gnm compute queue |
| Direct memory under `deploy.sh` | **caps between 41 and 64 MiB** — `0x80020023` EAGAIN above it, on both memory types |
| Direct memory in the **app slot** | **every size tested allocated**, to 322 MiB. Same binary, same ladder, different host process |

Full detail in [hardware-decode-findings.md](hardware-decode-findings.md) §7.

The outcome table above anticipated a permissions-shaped refusal as the likely
result. It got a hang instead, which is worse to read — and the thing that
turned it into a one-line fix was **not** a hardware sweep but disassembling
the chain down through VdecCore until it named the import it was blocked on.
The pipe and queue ids, which a brute-force sweep would have spent many deploys
on, were correct from the first attempt.

### Four corrections to the plan above, paid for by these runs

1. **`QueryComputeMemoryInfo` takes one argument**, not the `(cfg, memInfo)`
   pair the decoder query uses.
2. **`MapMemory` and `MapDirectMemory` belong to Phase 5, not 4.** Both check
   for a decoder-handle magic at `+0x68` and return `0x811D0103` without one.
   They cannot be called before `CreateDecoder` exists, so "record what
   `MapMemory` accepts" was not a Phase 4 step at all.
3. **`sceKernelAllocateDirectMemory` is the wrong allocator** under `deploy.sh`
   — `SceSpZeroConf` is spawned with `dmem#0`.
   `sceKernelAllocateMainDirectMemory` is the one that works, as
   `projects/common/include/evo_ps5.h` already recorded.
4. **"Deploys" was the wrong unit.** The phase budgeted 2–3 `deploy.sh` runs
   and never asked which process they land in. Two of its three runs measured
   `SceSpZeroConf` and reported a memory limit the player will never hit.

And the memory-type guess was backwards: the module's own validator wants
`WB_ONION` for the compute queue, not `WC_GARLIC`. Garlic is for frame buffers.

### How the hang was fixed

The first attempt hung. Rather than sweep pipes and queues on hardware, the
call chain was disassembled down through `libSceVdecCore` until it named the
import it was blocked on — a five-argument
`(pipeId, queueId, ringBase, ringSizeInDW, readPtrAddr)`, which is
`sceGnmMapComputeQueue`. Its GOT slot was unresolved in the dump, and the probe
had never loaded `libSceGnmDriver.sprx`.

Adding that one module made the same call return `0`.

**The lesson worth carrying:** an unresolved lazy import blocks silently and
indefinitely — it does not fault and it does not return an error. Load every
module in the call chain, not just the one whose name is on the function.

**And the near-miss worth carrying:** the successful deploy *also* exited 124,
with its socket output cut off at exactly the same line as the hang. Only the
log on `/mnt/usb0` showed `rc=0`. Read the file before concluding anything.

---

## Phase 4b — the memory ceiling — **done, and it was never a ceiling**

It looked like the blocker that could end the project: 1080p decode needs
89 MiB, and the probe could not get 64.

It cost **zero extra work** to settle. The same ELF, relaunched with
`install-homebrew.sh --run` instead of `deploy.sh`, allocated every size on the
ladder — 64, 90, 109, 160 and 322 MiB, on both `WB_ONION` and `WC_GARLIC`. The
4K dpb-16 working set fits. The true ceiling in that slot is above 322 MiB and
was not found.

The explanation was already written down in [building.md](building.md) and had
simply not been connected to this work: `deploy.sh` injects payloads into
**`SceSpZeroConf`**, a background network service spawned with `dmem#0`.
`install-homebrew.sh --run` goes through `hbldr_launch` into the **PS Now app
slot**, a real application process. That page says it in terms of the display
plane — "payloads are headless" — and the same boundary governs memory.

**No new eboot.bin, no new application, nothing to author.** EVO Player already
runs in that slot; the research probes were the only thing that did not.

### What this changes

- **Run every future probe with `install-homebrew.sh --run`.** A probe under
  `deploy.sh` measures `SceSpZeroConf`, which is not where the player lives, so
  its resource answers do not transfer.
- **Phase 5 can go straight to 1080p.** The advice to start at 720p was written
  under the wrong measurement and is withdrawn.
- **The definition of done is back in reach.** 4K dpb-16 is affordable, so the
  effort can target resolutions the CPU path genuinely cannot sustain.
- **The app slot is resident.** Probes exit on their own, but the `/hbldr` pipe
  does not EOF, and the stacking rule applies to launches in a way it never did
  to `deploy.sh`. Use `tools/launch.sh` or the bounded `--timeout`.

---

## Phase 5 — Create a decoder — **done, 2026-08-11**

**Goal: `sceVideodec2CreateDecoder` returns a decoder handle. Met, in two
deploys** — H.264 on both usable resource classes, and HEVC Main on
`0x12384` — with a clean `DeleteDecoder` every time.
`projects/createdecoder_test/`.

### What it established

| | |
|---|---|
| `CreateDecoder` H.264 1080p dpb16, class `0xb6c8` | **`rc=0`** |
| the same on class `0x12384` | **`rc=0`** |
| **`CreateDecoder` HEVC Main 1080p dpb16, class `0x12384`** | **`rc=0`** |
| `DeleteDecoder` | **`rc=0`** all three times |
| **entitlement gate** | **did not appear anywhere** — not at query, not at create, not for HEVC |
| **HEVC, asked properly for the first time** | **accepted, and it creates.** Needs class `0x12384`; `0xb6c8` refuses it |
| **VP9** | **refused** at every configuration, `0x811D0200`. `libSceVdecSvp9.sprx` does not exist on this firmware |
| `memInfo+0x28` | **zero** for every accepted configuration — the third buffer is never needed |
| the decoder handle | **is the caller's own `memInfo+0x10` buffer**, and the VdecCore object sits at exactly `+0x40000`, as read |
| validation controls | **9/9** exact |
| field-correction controls | **5/5** exact — a bogus codec gives `0204`, a bogus resource class `0203` |

HEVC Main at 1080p wants **less** memory than H.264 — 65.9 MiB of buffers
against 85.9 MiB. Full figures in
[hardware-decode-findings.md](hardware-decode-findings.md) §7.

Raw logs: `research-logs/console/evo_createdecoder_log-run1-avc.txt` and
`-run2-hevc.txt`.

### The hang that did not happen

The second deploy was the risky one: `sceVdecCoreCreateDecoder` reaches
`sceSysmoduleLoadModuleInternal` on the HEVC path, and a module load is the one
operation in this work that has hung rather than failed. It returned `0`.
`libSceSysmodule.sprx` was in the probe's module list from the start, put there
by standing rule 11 while reading VdecCore rather than after a hang — the first
time in this effort that rule has been applied *before* paying for it.

### How it was run

    ./scripts/build.sh createdecoder_test
    ./scripts/install-homebrew.sh --run --timeout 120 output/elf/createdecoder_test.elf
    # and, for the HEVC create, a second run:
    ./scripts/install-homebrew.sh --run --timeout 120 --args "eboot.elf hevc" \
        output/elf/createdecoder_test.elf
    # then read /mnt/usb0/evo_createdecoder_log.txt off the stick - not the socket

Three things carry over from Phase 4:

- **Launch with `install-homebrew.sh --run`.** Under `deploy.sh` the 1080p
  working set will not allocate and the phase will fail for a reason that has
  nothing to do with the decoder.
- **Load `libSceGnmDriver.sprx`.** Not optional, and not obvious from
  `CreateDecoder`'s own name or prologue.
- **1080p is affordable**; so is 4K at dpb 4.

And one new one, the same shape as the Phase 4 hang: **load
`libSceSysmodule.sprx` too.** `sceVdecCoreCreateDecoder` calls
`sceSysmoduleLoadModuleInternal` on the HEVC path, and an unresolved lazy
import there blocks silently and forever rather than failing.

### What the disassembly changed

**`cfg+0x08` and `cfg+0x0c` were recorded the wrong way round.** `+0x0c` is the
codec — `1` H.264, `0xee049` HEVC, `0x245bfd` VP9 — and `+0x08` is a resource
class. The proof is the profile and level validation that follows each one:
H.264 wants `profile_idc` 66/77/100 and `level_idc` 10..111, HEVC wants profile
1/2 and `general_level_idc` (level × 30), VP9 wants profile 0/2 and level × 10.
Full detail in [hardware-decode-findings.md](hardware-decode-findings.md) §7.

**So every configuration Phase 3 accepted was H.264, and HEVC has never been
validly queried.** The `0xb6c8 v=ee049 → 0x811D0205` lines in the Phase 3 log
are the probe handing HEVC a profile of 100 and a level of 51 — H.264 numbers —
against a code that means *unsupported profile or level*. The discriminator
this section used to carry, "succeeds for `0xb6c8` and fails for `0x12384` →
the HEVC entitlement gate is real", **cannot work**: neither of those values is
a codec.

Three more things worth having before the run:

- **`CreateDecoder` takes three arguments** — config, memory-info, and the
  output handle. `CreateHevcDecoder` is a byte-for-byte **alias** of it; so is
  `QueryHevcDecoderMemoryInfo` of the plain query. Calling the Hevc-named entry
  points changes nothing.
- **The decoder handle is the caller's own buffer.** `memInfo+0x10` is where
  the module builds its object, and the same pointer comes back out.
- **Half of `SceVideodec2DecoderMemoryInfo` is input.** The query fills the
  sizes and explicitly zeroes `+0x10`, `+0x20`, `+0x30` and `+0x44`; those are
  the caller's to fill. The module's own checker says it wants **`WB_ONION`**
  for `+0x10` and **`WC_GARLIC`** for `+0x20`.

### What the probe does, cheapest first

1. **Control** — the Phase 3 query re-measured. If the 1080p frame pool is no
   longer 86,507,776 nothing below it means anything.
2. **The full 0x48-byte memory-info hexdump.** Phase 3 logged three of the four
   sizes; `+0x28` has never been measured, and `CreateDecoder` range-checks it.
3. **The codec matrix** — H.264, HEVC and VP9, each with its own profile and
   level, across both usable resource classes, plus deliberate-error controls
   that must return `0x811D0204` for a bogus codec and `0x811D0203` for a bogus
   resource class. Pure queries, so this is as safe as Phase 3 and it settles
   the HEVC question in the same deploy as everything else.
4. Compute queue, exactly as Phase 4 proved it.
5. **Nine `CreateDecoder` validation controls** — all of them return before the
   module allocates anything.
6. **The real call**, H.264 1080p DPB 16 on resource class `0xb6c8`, then the
   same on `0x12384`, each paired with `DeleteDecoder` and each allocating and
   releasing its own buffers.
7. On success, a hexdump of the decoder object and its two magic cookies.

**The HEVC create is opt-in** — `--args "eboot.elf hevc"` — and deliberately
runs last, because it is the one call here that reaches a module load, and a
module load is the one thing in this work that has hung rather than failed.
That is the second deploy in the budget, not the first.

**Deliberately not tested: `CreateDecoder` with a NULL compute queue.** It
would settle whether the queue is mandatory, but VdecCore does not NULL-check
it, so the failure mode is a fault rather than an error code — and a faulted
payload in the app slot costs far more than the answer is worth.

### Discriminators, prepared in advance

| Signature | Reading |
|---|---|
| Fails identically with correct *and* deliberately-wrong config | permission, not configuration |
| A `0x811Dxxxx` code that changes as fields change | still configuration — keep reading the validator |
| `0x811D0102` on an obviously non-NULL pointer | the module's argument validation is enabled, and it runs through `sceKernelVirtualQuery`, which is broken in a payload. Not a permissions problem |
| `0x811D0109` / `0x811D010A` | the memory-type check is enabled after all, and wants onion for work / garlic for frames |
| H.264 creates and **HEVC is refused at query time** | **the entitlement gate is real** — and this is the first run in a position to say so. AVC-only hardware decode is still a large win; do not treat it as failure |
| H.264 creates and HEVC is refused at *create* time only | the gate is on the decoder, not the configuration. Compare the two error codes before concluding anything |
| Succeeds, then decode fails | arbitration. Try `sceVideoDecoderArbitrationInitialize` / `Enable` first, which is what AvPlayer does |

---

## Phase 6 — Decode one frame — **run; no picture; blocked at the driver**

> **This heading said "built, not yet run" and was stale.** Phase 6 has since run
> **sixteen** times. It cleared four successive software gates and now reaches
> the hardware submit, where the driver refuses the job with ioctl errno **5200**.
> The prose below is the *offline* half and is still accurate — it is what the
> disassembly said before any of it was executed, and every prediction in it
> held. What happened on the console is in
> [hardware-decode-findings.md](hardware-decode-findings.md) §7, and what to do
> about it is **Phase 6b** below.

**Goal: feed one access unit and have the decoder report a picture.**

`projects/decodeframe_test/` is written, builds clean, and carries its own
bitstream. Everything below is the offline half, which cost no deploys.

The offline read changed the plan in three ways, and the corrections are worth
more than the plan they replace.

### 1. The frame buffer is an argument to `Decode`, not something you map

The plan opened with "a measured `memInfo+0x38` (`mapMemorySize`) that has no
buffer of its own — which is very likely what `MapMemory` and `MapDirectMemory`
are for". It is not. **`sceVideodec2Decode` takes four arguments**, and the
third one carries the output buffer:

```c
int sceVideodec2Decode(void *decoder, const SceVideodec2InputData *au,
                       SceVideodec2FrameBuffer *fb, SceVideodec2OutputInfo *out);
```

Nothing on the decode path consults a "mapped" flag, and neither map call is
reachable from it. Full ABI in
[hardware-decode-findings.md](hardware-decode-findings.md) §7.

### 2. `MapMemory` would very likely have hung — and that was visible offline

**`sceVideodec2MapMemory`'s single import is unbound in the dump.** Its GOT slot
(`libSceVideodec2` s2 `+0x60`) still points back into its own PLT push sequence,
while every neighbouring slot that lands in `libSceVdecCore` is resolved. That
is the exact shape of the Phase 4 hang: an unresolved lazy import blocks
silently and indefinitely rather than failing — and unlike Phase 4, we cannot
even name the module to load, because the slot never bound.

The probe does not call it. **A phase that was going to open by calling
`MapMemory` instead opens by not calling it, and the reason is one line of a
GOT dump.** New standing rule 15.

### 3. `mapMemorySize` is exactly one output frame, and Phase 7's first question is mostly answered

Phase 5 measured `memInfo+0x38` for eight configurations without knowing what it
was. It is one decoded frame plus its metadata:

    mapMemorySize = align(width, 256) * align(height, N) * bytes * 3/2 + 5 * 1024

with `N` = 16 for H.264 and 1 for HEVC, and `bytes` = 2 for Main10. It
reproduces **all eight** of Phase 5's figures to the byte:

| configuration | predicted | Phase 5 measured |
|---|---|---|
| AVC 720p | 1280 × 720 × 3/2 + 5120 = **1,387,520** | 1,387,520 |
| AVC 1080p | 2048 × 1088 × 3/2 + 5120 = **3,347,456** | 3,347,456 |
| AVC 4K | 3840 × 2160 × 3/2 + 5120 = **12,446,720** | 12,446,720 |
| HEVC Main 1080p | 2048 × 1080 × 3/2 + 5120 = **3,322,880** | 3,322,880 |
| HEVC Main10 1080p | × 2 = **6,640,640** | 6,640,640 |
| HEVC Main 4K | **12,446,720** | 12,446,720 |
| HEVC Main10 4K | **24,888,320** | 24,888,320 |

A `× 3/2` with half-height chroma is **NV12**, and `× 3` is **P010**. The stride
is the width rounded up to **256**, and the height padding differs by codec —
H.264 pads to a macroblock, HEVC does not pad at all. That is Phase 7's "pixel
format" and "stride vs width" rows, for free, from arithmetic that already had
eight independent confirmations.

**It is a prediction, not a measurement**, and the probe treats it as one: it
re-checks all seven live configurations against the module and prints
`MATCH` / `*** DIFFERS ***` per row before it decodes anything.

The trailing 5,120 bytes are five 1 KiB blocks, and `GetPictureInfo` says what
they are: it reads picture metadata from
`frameBuffer + frameBufferSize - pictureCount * 1024`, walking backwards 1 KiB
per picture. **The frame buffer carries its own metadata in its tail.**

### The one question the disassembly could not answer

**Bitstream framing.** `Videodec2` passes `auData`/`auSize` straight through to
VdecCore, which hands them to the hardware. Nothing on the path scans for start
codes, so nothing on the path says whether the hardware wants **Annex-B**
(`00 00 01` prefixes) or **AVCC** (4-byte big-endian NAL lengths). Following it
further means following the `uvd_dec` ioctl into a driver that is not in the
dump.

So it is tested rather than guessed: **both framings in one deploy**, Annex-B
first, each on its own decoder with its own buffers. The AVCC attempt only runs
if Annex-B produced nothing.

### The input needs no USB stick

The plan said to put an elementary stream on the stick. The console's `/fs` is
read-only, so that is a hand-copy on every run of a workflow that is already
expensive per deploy. Instead `tools/gen-test-stream.sh` encodes eight access
units of `testsrc2` at 1080p High L4.0 with ffmpeg, and `test_stream.S` links
the result into the payload's `.rodata` with `.incbin`. The stream is committed
— it is 284 KB of synthetic video we generated — so the probe is one command
from a clean checkout.

Access units are split on **NAL type 9** (access unit delimiter), which the
encoder is told to emit, so the split is exact rather than heuristic. AU 0 is
`AUD + SPS + PPS + SEI + IDR`.

### What the probe does, cheapest first

1. **Control** — Phase 5's query re-measured. If the 1080p frame pool is no
   longer 86,507,776, nothing below it means anything.
2. **Control** — the alias map. Four of the eighteen exports are two pairs of
   byte-identical thunks into one body (rule 14, again — see §6 of the
   findings). Resolving all four records it rather than asserting it.
3. **Control** — `mapMemorySize` predicted vs measured, seven configurations.
4. Compute queue, exactly as Phase 4 proved it.
5. `CreateDecoder`, exactly as Phase 5 proved it, at **pipeline depth 1** — the
   shortest path from one access unit to one picture. The Phase 5 control above
   still runs at depth 4, so the comparison with Phase 5's figures is intact.
6. **Eleven `Decode` validation controls** and **eight `GetPictureInfo`
   controls**, all of which return before `sceVdecCoreSetDecodeInput` and none
   of which touches the hardware.
7. **The real decode.** Four access units, then `Flush` to drain.
8. On a picture: the raw `OutputInfo`, the raw 1 KiB metadata block, the raw
   0x78-byte `AvcPictureInfo`, and samples of the frame at the offsets the
   arithmetic predicts.

**`Flush` goes last and is one-way.** It latches `decoder+0x50`, after which
every `Decode` returns `0x811D0100` until `Reset`.

### How to run it

    ./scripts/build.sh decodeframe_test
    ./scripts/install-homebrew.sh --run --timeout 120 output/elf/decodeframe_test.elf
    # then read /mnt/usb0/evo_decodeframe_log.txt off the stick - not the socket

Arguments, via `--args "eboot.elf <flags>"`: `one` feeds a single AU, `all`
feeds all eight, `dump` writes a whole frame to
`/mnt/usb0/evo_decodeframe_frame0.bin` for Phase 7, `no-avcc` suppresses the
second framing attempt.

### The risk, stated plainly

`sceVdecCoreSyncDecode` **blocks on the hardware**, and this is the first call
in the whole effort that gives the video hardware work to do. If it never
completes, the payload hangs — and in-payload watchdogs do not fire on 12.70.
The guard is `timeout` on the PC plus a log flushed after every line. Every
control, the whole memory-arithmetic check, the compute queue and the decoder
creation are in the log before the first byte reaches the hardware, so a hang
still returns three controls' worth of evidence and its location is itself a
finding.

### Discriminators, prepared in advance

| Signature | Reading |
|---|---|
| `mapMemorySize` rows all `MATCH` | the frame layout is NV12/P010 at a 256-aligned stride, and Phase 7 starts from a known layout instead of measuring one |
| any row `DIFFERS` | the arithmetic is wrong. Do not carry it into Phase 7 — that is exactly how a wrong reading survives three phases (rule 13) |
| a control returns something other than its predicted code | the struct layout read is wrong. Fix that before reading anything into the decode result |
| `rc=0`, `isValid=1` | **the goal is met.** Read the metadata dump; it is where the real dimensions are |
| `rc=0`, `isValid=0` on every AU **and** every `Flush` | the decoder accepted the data and produced nothing. Framing or compute queue — not entitlement |
| `0x811D0300` / `0x811D0304` | the decoder latched an error state. The layer below rejected the bitstream, which points at framing |
| Annex-B yields nothing and AVCC yields a picture | the hardware wants length-prefixed NALs. Record it; it changes what Phase 10's demuxer has to emit |
| the log stops inside an AU | `sceVdecCoreSyncDecode` blocked. Read `tools/klog.sh` and consider whether the compute queue is actually being serviced |

**Success is not "a picture appeared".** Success is the decoder reporting a
decoded picture with plausible dimensions. Whether the pixels are correct is
Phase 7's problem.

---

## Phase 6b — Get past errno 5200 — **the current front**

**Goal: find out what the driver is objecting to, or make it stop objecting.**

Added 2026-08-13, after a session that produced three findings and used no
console time at all. Everything came from the disassembly already sitting in
`PS5-Research` and from transcripts already captured — which is the ordering
[hardware-decode-review.md](hardware-decode-review.md) argued for from the start,
arriving one phase later than it should have.

The three findings, in the order they change what to do
([findings](hardware-decode-findings.md) §7 has the evidence for each):

- **The submit path is not invariant.** Two writers inside VdecCore set the mode
  selector `[obj+0x1d08]` to `1` — ioctl command **24** rather than 23 — guarded
  by an equality between two counters in the codec context. Run 14 concluded the
  path was fixed; it swept *configuration*, and configuration is not what writes
  this field. **Bitstream state is.**
- **The driver discriminates on the job, not the caller.** The same submit line
  returns errno **5103** elsewhere in run 8, while its caller returns success.
- **Nothing was skipped.** VdecCore issues exactly four group-`0x83` commands;
  the two nobody had read are a version handshake, and it has never failed.

> **Run 2026-08-14. 6b.1 is answered — negatively — and 6b.2 is withdrawn as
> unsafe.** The selector never moved off 5 across 34 readings; the kernel scan
> 6b.2 proposed panicked the console and `kdump` has been deleted. The current
> front is now **6b.4** at the bottom of this section. Details in
> [native-media-research.md](native-media-research.md#2026-08-14--6b1-run-the-submit-selector-does-not-move-and-a-kernel-scan-panicked-the-console).

### 6b.1 — The free experiment, and it goes first — **DONE, and the answer is no**

**No kernel access, no elevation, one probe.**

> **Result, 2026-08-14 [E].** Eight access units into one decoder, a Reset
> attempted between each, `[obj+0x1d08]` read after every Decode and every
> Reset: **5 every time**, mode 7, ioctl command 23, across both
> configurations. `obj+0x1448` never changed. This is the middle row of the
> table below — the writers at `+0x13e58` / `+0x158d8` are not on the object
> the submit uses, or their guard is unreachable from the public API.
>
> **Bounded by one fact:** `sceVideodec2Reset` returned `0x811D0111` every
> time and never cleared the error latch, so the decoder never returned to a
> clean state between AUs. Each Decode still returned `0x811D0111` rather than
> the software-latch codes `0x811D0300/0304`, so the attempts were reaching
> past the API's gates — but the codec sequence handlers plausibly never
> advance their counters while every submit is refused. What is established is
> *the selector does not move while the driver is refusing the job*.
>
> **A false positive was produced first and is worth reading**, because the
> mistake is a general one: to let a moved selector be found, the search was
> widened from `mode == 7` to `mode == 7 || mode == 0x10`, and `0x10` is
> sixteen — a value the work arena is full of. The predicate's power was that
> **7 is rare**, and widening it spent that power. It is bought back with two
> free corroborating checks, both now in `find_device_object`: the jump table
> must agree with itself (index 5 ↔ mode 7, index 1 ↔ mode 0x10), and the fd
> at `+0x1d68` must be one the process really has open as a character device.
> With both in, exactly one candidate survives.

Feed a stream that changes the equality guard — more than one access unit, or one
whose reference counts differ — and read `[obj+0x1d08]` back off the live decoder
by the object-search method run 13 already used (`mode == 7` at `+0x2c0` plus a
plausible fd at `+0x1d68`, two constraints `0x1ab8` apart; exactly one candidate
matched).

| result | reading |
|---|---|
| `[obj+0x1d08]` reads **1**, submit takes command 24 | **command 24 is reachable from the public API.** Errno 5200 may stop being the question entirely. Go straight back to Phase 6 |
| it stays **5** across every stream tried | the two writers operate on a different object, or the guard is unreachable from here. The `[H]` in §7 is killed and 6b.2 is the route |
| the submit still refuses, but with a *third* errno | the driver is definitely validating job content. Record the code — a third anchor makes 6b.2 easier, not harder |

The cost is one non-resident probe. It should be run before anything that writes
kernel state.

### 6b.2 — ~~Read the driver, do not guess at it~~ — **WITHDRAWN: it panics the console**

> **Do not do this. 2026-08-14.** `projects/kdump` implemented exactly the scan
> below. It printed the process identity, printed `probing in 1 MiB steps, up to
> 128 MiB ...`, and the console panicked. The user reports it **always** panics.
> The project has been deleted from the repo and from `scripts/build.sh`.
>
> **The premise was wrong.** This step was written on findings §2's note that
> `kernel_copyout` is "working and unable to fault the caller". That is true of
> **a single small read at a known-good address**. It says nothing about walking
> megabytes of address space, where unmapped or guarded pages take the kernel
> down — and the sweep below is the whole of the difference between the two.
>
> The errno question does not get answered by reading the running kernel from a
> payload. It needs a kernel image obtained another way and disassembled
> offline. Until someone has one, **6b.4** is the route.
>
> What this step did produce before dying is worth keeping — the app slot's
> identity, which is the one thing §13 wanted measured:
>
>     authid : 0x4800000000000027
>     caps   : ffffffffff1cff40ffffffffffffffff
>
> psdevwiki records the decoder modules under `4900000000000002`. This is not
> that.

<details>
<summary>The withdrawn plan, kept only so nobody re-derives it</summary>

`kernel_copyout` is established as working and unable to fault the caller
(findings §2 — **and that is the sentence that was over-read**). Scan kernel
`.text` from `KERNEL_ADDRESS_TEXT_BASE` for **three** immediates, not one:

| immediate | decimal | what is already known about it |
|---|---|---|
| `0x13A7` | 5031 | **understood** — `size` not page-aligned, fixed in run 3 |
| `0x13EF` | 5103 | the `Flush`-path refusal |
| `0x1450` | 5200 | the decode refusal |

**Start from 5031.** It is the one whose meaning is already established, so
finding it proves the scan is reading the right code and the right driver before
either of the other two is interpreted. A scan that cannot locate 5031 is a scan
whose 5200 result should not be believed.

Dump the containing functions and disassemble offline with `tools/re/`. Report
`kernel_get_ucred_authid` and `kernel_get_ucred_caps` in the same probe — free,
and it is the baseline 6b.3 would need anyway.

**A method note, paid for once already** (findings §7): a byte search for
`mov ecx, <imm>` disproves only the immediate form. VdecCore loads its line
numbers into `r14d` and moves them to `ecx` at a shared logging tail, so the
immediate never appears next to the register. Search for the *value*, then look
at what the function does with it.

</details>

### 6b.3 — Elevation

**Writes kernel state. Needs explicit sign-off** — findings §9.1 is why.

Wrap `sceVideodec2Decode` in the authid/caps elevation `core/pt.c` already
performs, and retry. This was the leading candidate before 2026-08-13, was
demoted to third when the driver turned out to discriminate on the job, and is
now **second** only because 6b.2 died — not because the evidence for it
improved. The argument against it still stands: a driver that returns 5103 for
one job and 5200 for another in the same process is not refusing the process.

The identity it would be elevating *from* is now measured: authid
`0x4800000000000027`, caps `ffffffffff1cff40ffffffffffffffff`, in the app slot.

### 6b.4 — Make `Reset` work, because everything else is downstream of it — **READ, 2026-08-14**

**No kernel access. The cheapest thing left, and it is now the front.**

> **Done offline, no console. It is outcome 2 below: `Reset` passes every one of
> its own gates and is refused one layer down.** The whole chain is readable in
> `PS5-Research/derived/`, and the payoff is that **`Reset` is precisely the
> function that clears the latches `Decode` gates on, and it never reaches its
> clear.** Full evidence in
> [hardware-decode-findings.md](hardware-decode-findings.md) §7. In short:
>
> ```
> sceVideodec2Reset            +0x30f0   passes NULL / magic / lock gates
>   -> sceVdecCoreResetDecoder +0x1a40   arg2 = 0, which this function
>                                        explicitly permits (it means mode 0)
>      -> GpDec reset          +0xc550   FAILS on its very first call
>         logs B0A1 line 0x164C, returns 1
>      logs A01D line 0x649, returns 0x80C00001
>   state clear SKIPPED, returns 0x811D0111
> ```
>
> **The skipped clear is the thing.** On its success path `Reset` executes
> `vmovups XMMWORD PTR [rbx+0x44],xmm0` — sixteen bytes from `decoder+0x44`,
> which covers `+0x48`, `+0x4c` and `+0x50`: the two error latches `Decode`
> gates on (`0x811D0300` / `0x811D0304`) **and** the one-way flush latch
> (`0x811D0100`). It also zeroes `+0x70`, `+0x8`..`+0x27`, `+0x54`, and
> increments `+0x40`. So the API does have a clean way back to a decodable
> state, and the only reason this project has never seen it is that GpDec
> refuses the reset before it happens.
>
> **One question left, and the probe is already built to answer it.** GpDec's
> first check is a `memcmp` against a 16-byte magic (findings §7 has the proof),
> so its three failure lines are three different statements about the object at
> `[vdeccore+0x140]`: `0x1645` no object at all, `0x164C` magic mismatch,
> `0x1653` the lock is held. The VdecCore object is in memory this payload
> allocated, so that pointer can just be **read** —
> `diagnose_gpdec_object()` in `projects/decodeframe_test/main.c` walks it on
> the first Reset failure and tells the three apart without the console
> diagnostics. **Built and compiled on 2026-08-14; it has not run yet.**
>
> Note the refusal happens *before* any ioctl — the failing call is `memcmp` —
> so this does **not** show the driver refusing `Reset`.
>
> **When the console is back, one launch answers it.** Redirect the whole of
> stdout, which is where the `B0A1`/`A01D` line numbers go and which the last
> run lost to a `tail -40`:
>
> ```bash
> PS5_HOST=<ip> ./scripts/install-homebrew.sh --run --timeout 45 \
>     output/elf/decodeframe_test.elf > deploy-stdout.txt 2>&1
> curl -s http://<ip>:8080/fs/mnt/usb0/evo_decodeframe_log.txt -o run.txt
> ```
>
> The payload keeps running after the launcher detaches, so fetch the log a
> minute later, not immediately — fetching too early on 2026-08-14 produced a
> truncated log that looked exactly like a hang at `AllocateComputeQueue`.

`sceVideodec2Reset` returns `0x811D0111` and does not clear the error latch.
That single fact bounds 6b.1's negative result, blocks any multi-AU experiment,
and means every run this project has ever made has shown the hardware exactly
one access unit in one decoder state.

Read `sceVideodec2Reset` (`+0x30f0`) the way `Decode` was read — offline, off
the dump already in `PS5-Research` — and find out whether it fails on its own
gates or bottoms out in VdecCore like the submit does. Two outcomes, both
useful:

- **It fails on its own gates.** Then there is a state the API expects between
  Reset and the next Decode that this probe is not producing, and the fix is
  free.
- **It reaches VdecCore and is refused there.** Then the driver is refusing
  more than the submit, the refusal is not specific to decode jobs, and errno
  5200 is one symptom of something broader.

Either way it is answered by reading, which is the method that has produced
every real finding in this effort and has never cost a console.

### Go / no-go

- **6b.1 is done. It cost one probe and no risk, exactly as predicted.**
- **6b.2 is withdrawn — it panics the console.** Do not run it, and do not
  rebuild `kdump`.
- **6b.4 needs no console at all.** Do it next.
- **6b.3 needs a person**, and it should wait for 6b.4 — there is no point
  spending a kernel write while `Reset` is still unexplained.

---

## Phase 7 — Characterise the frame

**Goal: know exactly what we have before trying to display it.**

Cheap once a frame exists, and skipping it produces corruption that looks like
a decoder bug. **Phase 6's offline half already answers the first two rows, and
the Phase 6 probe checks both against the module before it decodes**: the
`mapMemorySize` arithmetic says NV12 / P010 at a stride of `align(width, 256)`,
with H.264 padding height to 16 and HEVC not padding at all. Treat those as
predictions with eight confirmations, not as measurements — they are still
arithmetic until a real frame agrees with them.

Measure and record:

| | Why it matters |
|---|---|
| Pixel format | **[I]** NV12, or P010 for 10-bit — the `× 3/2` and `× 3` in the `mapMemorySize` formula. Do **not** assume YUV420P |
| Tiled or linear, and which tiling | The existing VideoOut plane is tiled (`PP_VO_ATTR_TILED_BGRA`) and the repo already has working tile-address logic. It may or may not match |
| **Stride vs width** | **[I]** `align(width, 256)` — 2048 for 1080p, not 1920. Hardware decoders pad, and assuming `stride == width` is the classic first-frame corruption bug |
| The metadata tail | **[E]** 1 KiB picture-metadata blocks live at the *end* of the frame buffer, which is where `GetPictureInfo` reads them from. Do not treat the whole buffer as pixels. **The count is not fixed: five normally, FOUR when `optimizeProgressiveVideo` is set** — measured 7/7 on 2026-08-11 |
| Plane count and pointers | One interleaved chroma plane (NV12) or two changes the converter entirely |
| **CPU readability** | Can the payload read the buffer at all without faulting? Given every module's text is execute-only, assume nothing |
| **Cacheability** | If it is write-combined or uncached, CPU reads are catastrophically slow — and a naive `memcpy` benchmark will blame the decoder for the read |
| Alignment, physical vs virtual | Needed for any zero-copy attempt |
| Lifetime and ownership | How long is the frame valid, must it be returned? Getting this wrong gives intermittent corruption |

**Write one frame to the USB stick as a raw file and inspect it on the PC.**
That settles format, stride and plane layout offline, without a display, and
without risking the console.

---

## Phase 8 — Get a frame on screen

**Goal: a Sony-decoded frame visible on the TV.**

**This is the highest-risk phase** — the first that holds VideoOut, and the one
failure mode that has historically cost an hour. Read the stacking rule first.
Do not combine this with any other experiment.

The question this answers is the one that decides the whole effort:

- **If the decoder's output tiling is something VideoOut can scan out** — the
  win is large, and most of the goal is met.
- **If every frame needs a CPU read from uncached memory plus a colour
  conversion** — hardware decode may be *slower* than the FFmpeg path already
  in the player. That outcome is entirely possible and must be measured rather
  than assumed.

Measure **copy cost separately from decode cost**. They are different problems
with different fixes.

### Go / no-go

**Check in before this phase.** Everything up to here either exits cleanly or
hangs harmlessly under a `timeout`.

---

## Phase 9 — Paired benchmark against FFmpeg

**Goal: prove it is actually faster.**

`tools/bench.sh` already hashes output and refuses to report timings if the
pixels changed. Apply the same discipline.

- **The same clip, both paths, same console, same session.** Absolute numbers
  on a console with unknown background load prove very little; a paired
  comparison proves everything.
- Choose a clip **the CPU path already struggles with** — that is the case the
  whole effort exists for.
- Record: init latency, **time to first frame**, steady-state fps at a stated
  resolution and bitrate, per-frame CPU time in our own process, **copy cost
  separately**, and presentation latency.
- Emit machine-readable lines, one per frame or per second, so runs diff
  against each other. `output/logs/` and `tools/klog.sh` already support this.

**A hardware path that is not decisively faster on a clip the CPU path
struggles with is not worth the integration risk.** That is a legitimate place
to stop, and recording it would be a real result.

---

## Phase 10 — Integrate into EVO Player

Only after Phase 9 says it is worth it.

The architecture constraint from
[native-media-research.md](native-media-research.md) stands and is not
negotiable: **the player must keep working with hardware decode absent.**

- Decoder access goes behind an interface with the **FFmpeg software path as
  the always-available fallback**.
- The hardware implementation is selected **at run time after probing**, never
  at build time.
- Probing must be cheap and must fail safe — on any doubt, use software.
- Bump `projects/evoplayer/VERSION` on every deploy, per the existing workflow.

---

## Route A — `libSceAvPlayer`, still untouched

Still worth keeping alive as a **parallel track**, not a fallback, because it
fails independently of Route B.

Phase 4 lowered its urgency — Route B got its own compute queue and does not
need AvPlayer's sequence to do it — but raised the value of one thing it
offers: AvPlayer knows how much memory it asks for and where from, and the
memory ceiling is now the binding constraint.

It is one API that does demux, decode and A/V sync together. Phase 1 made it
considerably more approachable than the original plan assumed:

- `sceAvPlayerInit` takes **one argument**, no hidden return-struct pointer.
- **A zeroed struct is an explicitly supported input** — the code loads canned
  defaults when the priority field is zero.
- **NULL returns 0 rather than faulting.**
- 27 entry points are resolved, including `AddSourceEx`, `GetVideoDataEx`,
  `SetAvSyncMode`, `JumpToTime`, `Pause`/`Resume` and `GetStreamInfo`.

**The highest-value experiment here is still the one the review identified:**
instrument the allocator and file callbacks *before* the first call, so they
log their arguments and return something benign. A failed `sceAvPlayerInit`
with instrumented callbacks yields the struct layout, the allocation pattern
and the file-access model in one deploy. Without them it yields an error code.
Same deploy cost, an order of magnitude more information.

Note that AvPlayer calls `sceVideoDecoderArbitrationInitialize` / `Enable` /
`AcceptEvent`, which Route B does not have to. If Route B stalls on arbitration
or the compute queue, **AvPlayer's own sequence is the worked example** for how
Sony expects those to be driven.

Work in `projects/avplayer_test/`, which is still the 34-line placeholder.

---

## What could still kill this

Updated from the original plan. Struck-through rows are now settled.

| Risk | Status |
|---|---|
| ~~No user session~~ | **Not a factor for the decoder.** AvPlayer imports nothing from `libSceUserService`; the Phase 3 query succeeded without a user |
| ~~Unknown struct layout~~ | **Read, not guessed.** The config struct is size-prefixed and every field is documented |
| ~~Wrong API names~~ | **Solved.** `sceVideodec2*` / `sceVdecCore*` |
| ~~Compute queue refused~~ | **Settled — a payload gets one.** [E] `rc=0` and a live handle, 2026-08-10. It needs `libSceGnmDriver` loaded; that is all it needed |
| ~~HEVC entitlement~~ | **Settled, and it is not a blocker.** [E] With the codec field read correctly, HEVC is accepted — Main and Main10, 1080p and 4K — and **`CreateDecoder` returns 0 for it**. No gate at query time or create time |
| **VP9 is unavailable** | [E] Refused at every configuration, `0x811D0200`, and `libSceVdecSvp9.sprx` does not exist on 12.70. Not a risk so much as a scope limit: H.264 and HEVC only |
| ~~Memory budget~~ | **Settled — it was the launch slot.** [E] `deploy.sh` caps at 41 MiB; the app slot allocated 322 MiB, the full 4K working set. Run probes with `install-homebrew.sh --run` |
| **Output needs a CPU copy** | **The one that decides whether this is worth doing.** Phases 7–9 |
| ~~**Arbitration**~~ | **RULED OUT, 2026-08-11.** [E] The hang was an unresolved lazy import (`+0x350` is a PLT stub); loading `libSceVideoArbitration.sprx` first fixes it. With arbitration fully up, the decode is refused with the identical errno 5200 |
| **Process authority** | **Downgraded 2026-08-13, and no longer the first thing to test.** [H] The same submit ioctl returns errno **5103** elsewhere in run 8 while its caller succeeds, against 5200 for the decode job — same process, same fd, same command, two answers. A permission check refuses uniformly, so the driver is inspecting *what is asked* rather than *who is asking*. Not killed: a driver can do both. Phase 6b.3, after 6b.1 and 6b.2. Findings §7, §13 |
| ~~**The wrong submit command**~~ | **RULED OUT, 2026-08-14.** [E] The probe was built and run: 8 access units, 2 configurations, 34 readings of `[obj+0x1d08]` off the live GpDec object, **all of them 5** — mode 7, ioctl command 23. The writers at VdecCore `+0x13e58` / `+0x158d8` are not on the object the submit uses, or their guard is unreachable from the public API. Bounded: `Reset` never cleared the error latch, so the decoder never left the refused state. Findings §7 |
| **`Reset` does not work** | **New, 2026-08-14, and now the cheapest open risk.** [E] `sceVideodec2Reset` returns `0x811D0111` on every call and never clears the latch at `decoder+0x48/0x4c`. Every run this project has made has therefore shown the hardware one access unit in one state. Answerable offline by reading `+0x30f0` — Phase 6b.4 |
| ~~**The kernel holds the answer**~~ | **NOT REACHABLE THIS WAY, 2026-08-14.** [E] Scanning kernel `.text` with `kernel_copyout` panics the console, every time. `projects/kdump` did it and has been deleted. Findings §2's "cannot fault the caller" is true of one small read at a known-good address and does not generalise to a sweep. Phase 6b.2 is withdrawn |
| ~~**A skipped setup ioctl**~~ | **RULED OUT, 2026-08-13.** [E] VdecCore issues exactly four group-`0x83` commands, and the two nobody had read are a version handshake — ioctl 11 must return `0x07000000`, then ioctl 18. Neither has failed in any run: their four diagnostic lines appear in no transcript. Nothing was skipped |
| **Video out** | **A hard constraint, discovered expensively.** [E] A payload cannot open video out — `0x80290001` for the real user — and trying it **kernel-panicked the console**. Phase 8 cannot assume this payload owns a video-out handle. Findings §9.1 |

---

## Definition of done

Unchanged, and worth restating because it is easy to drift from:

> **A decoded video frame produced by a Sony module, displayed on screen, at a
> resolution and bitrate the CPU path cannot sustain.**

Anything short of that is a step, not the goal. Phase 3 was a large step.

Phase 4 removed the blocker everyone expected — the GPU compute queue — and
then briefly appeared to replace it with a worse one, a 41 MiB memory ceiling
that would have capped hardware decode at 720p and made the last clause
unreachable. That ceiling was an artefact of running the probe in the wrong
process. In the slot the player actually runs in, the full 4K working set
allocates.

So the definition of done stands, unqualified, and nothing measured so far
argues against it.
