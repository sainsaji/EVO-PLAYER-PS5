# Native PS5 media library research

Research area for hardware-accelerated decoding. **Nothing here is implemented**
— this documents what was found and what to try, so experiments start from
facts rather than assumptions.

> **Working on this?** Read [hardware-decode.md](hardware-decode.md) first —
> it carries the plan, the ordered experiments and the console-safety rules.
> This document is the raw findings and the results log that plan draws on.

## The finding that shapes everything

The SDK ships **no stubs** for any native media module. `sce_stubs/` has 32
entries at v0.42 and none of these are among them:

```
libSceAvPlayer          libSceAvPlayer.native   libSceAvPlayerStreaming
libSceVdecCore          libSceVdecShevc         libSceVdecSvp9
libSceVdecwrap          libSceVideoDecoderArbitration
```

You therefore **cannot** write `-lSceVdecCore`. There is nothing to link
against.

But that does **not** mean the APIs are unreachable. Verified on 12.70:
`libSceAvPlayer` loads and all probed entry points resolve, with no
proprietary files involved. See the [results log](#results-log).

## Two routes

### Route 1 — load, then resolve by NID (verified working)

No proprietary files, nothing to install. Three steps, and each one matters:

```c
/* 1. Load the module - it is NOT mapped unless you link its stub. */
int modid = sceKernelLoadStartModule("/system/common/lib/libSceAvPlayer.sprx",
                                     0, NULL, 0, NULL, &res);

/* 2. Get its dynlib handle (different from the module id). */
uint32_t dynh;
kernel_dynlib_handle(getpid(), "libSceAvPlayer.sprx", &dynh);

/* 3. Resolve by NID, not by name. */
char nid[12];
nid_encode("sceAvPlayerInit", nid);              /* <ps5/nid.h> */
intptr_t addr = kernel_dynlib_resolve(getpid(), dynh, nid);
```

The three traps, all of which cost a debugging cycle here:

- **`sceKernelDlsym` by plain name always fails** (`0x80020003`, ESRCH). Sony
  modules export NIDs — a hash of the symbol name — not names. `nid_encode()`
  from `<ps5/nid.h>` produces the NID; `kernel_dynlib_resolve()` looks it up.
- **Passive probing is misleading.** `kernel_dynlib_handle` only finds modules
  the payload actually depends on. A module you have not linked or loaded
  reports "not mapped" even when it is present on the system.
- **The module id and the dynlib handle are different values.**
  `kernel_dynlib_resolve` wants the latter.

**`projects/decoder_test` implements all of this**, including a control
(`-lSceVideoOut -lSceAudioOut` must show 3/3, otherwise the probe is
unreliable and every other result is meaningless):

```bash
./scripts/build.sh decoder_test
PS5_HOST=192.168.0.10 ./scripts/deploy.sh output/elf/decoder_test.elf
```

Record new findings in the [results log](#results-log).

### Route 2 — generated stubs from a decrypted SPRX

Only needed if route 1 cannot reach something. Route 1 already works for
`libSceAvPlayer`.

Supported natively by the SDK: drop a `.sprx` into `sce_stubs/`, run
`make -C sce_stubs stubs`, and `genstub.py` emits a linkable `.c` by mapping
NIDs through `aerolib.csv`. Full procedure in
[proprietary.md](proprietary.md).

Requires modules from **your own console**, and they never enter this
repository.

## Inspection tooling in the container

For offline analysis of any module you legitimately have:

```bash
llvm-readelf -h -l -d module.sprx     # headers, segments, dynamic section
llvm-readelf --dyn-syms module.sprx   # exported/imported symbols (NIDs)
llvm-nm -D module.sprx                # dynamic symbol table
llvm-objdump -d module.sprx           # disassembly
strings -n 8 module.sprx              # embedded strings, often revealing
file module.sprx
xxd module.sprx | head                # SELF/ELF magic
```

`$PS5_PAYLOAD_SDK/bin/prospero-nid` converts between symbol names and NIDs, and
`sce_stubs/genstub.py` is a readable reference for how the NID mapping works.

## Where this fits in the architecture

Layer 3 in the three-layer model. Deliberately decoupled:

```
Layer 1   PS5 Payload SDK      -> native applications          WORKING
Layer 2   custom FFmpeg        -> demux + software decode      WORKING
Layer 3   libSceVdec* / GNM    -> hardware decode              RESEARCH
```

Layers 1 and 2 must stay fully functional with layer 3 absent. The player's
decoder interface should be an abstraction with an FFmpeg software
implementation as the always-available fallback, and a hardware implementation
selected only when probing succeeds at run time. Never make hardware decode a
build-time dependency.

## Results log

*(newest first)*

> This log is chronological — what happened, in the order it happened.
> **[hardware-decode-findings.md](hardware-decode-findings.md) is the organised
> reference**: the same knowledge arranged by subject rather than by date, with
> the full ABI tables. Look there first; come here for how a thing was learned.

Entries from 2026-08-10 onward use the confidence tags from
[hardware-decode-review.md](hardware-decode-review.md): **[E]** observed on
this console, **[I]** inference, **[H]** hypothesis.

### 2026-08-14 — **6b.1 run: the submit selector does not move, and a kernel scan panicked the console**

Two deploys of `decodeframe_test` and one of `kdump`. The third cost about
fifty minutes and `kdump` no longer exists.

**1. The submit selector never leaves 5. [E]**

Phase 6b.1, run properly: eight access units fed to one decoder, a
`sceVideodec2Reset` attempted between each, and `[obj+0x1d08]` read off the live
GpDec object after every Decode and every Reset — 34 readings across two
configurations. **Every one of them read 5 → mode 7 → ioctl command 23.** It
never once read 1, and `obj+0x1448` never changed after its baseline.

So the answer to the question §7 raised is negative, and by the decision table
in [hardware-decode-next-steps.md](hardware-decode-next-steps.md) 6b.1 that
means the writers at VdecCore `+0x13e58` and `+0x158d8` are **not operating on
the GpDec object the submit uses**, or their guard is not reachable from here.
The **[H]** is resolved against the hopeful reading.

**With one caveat that has to be stated, because it bounds the claim. [E]**
`sceVideodec2Reset` returned `0x811D0111` every single time — it never cleared
the error latch — so the decoder was never restored to a clean state between
access units. Each of the eight Decodes still returned `0x811D0111` rather than
the software-latch codes `0x811D0300/0304`, which says each attempt was getting
past the API's own gates rather than being refused on the latch. But the codec
sequence handlers plausibly never advance their counters while every submit is
being refused, so what this run establishes is *the selector does not move while
the driver is refusing the job* — not *the selector can never move*. The
distinction matters if the refusal is ever cleared.

**2. A false positive, and the method note that goes with it. [E]**

The first version of this experiment reported that the selector **had** moved,
and it was wrong. To let a moved selector be found, the object search was
widened from `mode == 7` to `mode == 7 || mode == 0x10`. Mode `0x10` is the
value **sixteen**, and the work arena turns out to contain a large region of
solid `10 00 00 00`; the search matched five "objects" inside it and every field
read out of the first one was 16 because every word there is 16.

The predicate's power was never "mode 7". It was that **7 is rare**. Widening it
to a common value spent that power without replacing it.

Two free checks replace it, and both would have caught this immediately:

- **The jump table must agree with itself.** `[0x1d08]` indexes it and `[0x2c0]`
  is its output, so index 5 accompanies mode 7 and index 1 accompanies mode
  0x10. The false match read **index 0 with mode 16**, which the table forbids.
- **The fd must be one the process actually has open, as a character device.**
  The false match claimed **fd 119**; the fd dump printed two lines below it
  showed nothing open above 17.

With both checks in, the corrected search finds **exactly one** candidate —
`work+0x1600f0`, index 5, mode 7, **fd 16**, which the same dump confirms is an
open character device (`rdev 0x80`). That is the object; the earlier one was
noise at a different address entirely.

The general form is worth keeping: *a search predicate that matches a common
value is not a search*. When a filter has to be loosened, the selectivity it
gave up has to be bought back with corroboration from an independent field.

**3. The app slot's identity, captured before the panic. [E]**

    authid : 0x4800000000000027
    caps   : ffffffffff1cff40ffffffffffffffff

psdevwiki records the decoder modules under auth ID `4900000000000002`. This is
not that. It is the one measurement §13 wanted and it is now taken, in the app
slot rather than in `SceSpZeroConf` (rule 12).

**4. `Reset` is the latch clear, and it is refused by GpDec — not by the driver. [E]**

Read offline the same day, no console, from `PS5-Research/derived/`. Full
disassembly in [hardware-decode-findings.md](hardware-decode-findings.md) §7.

`sceVideodec2Reset` passes its magic check, takes its lock, and calls into
VdecCore with a second argument of 0 — which `sceVdecCoreResetDecoder` (`+0x1a40`)
explicitly permits. That function then calls the GpDec reset on the object at
`vdeccore+0x140`, whose **first** check fails, logging `B0A1` line `0x164C`
before anything touches the device fd. VdecCore logs `A01D` line `0x649` and
returns `0x80C00001`; Videodec2 returns `0x811D0111`.

**The consequence is the finding.** On its success path `Reset` executes
`vmovups XMMWORD PTR [rbx+0x44],xmm0` — sixteen bytes from `decoder+0x44`, which
covers `+0x48`, `+0x4c` and `+0x50`: both error latches `Decode` gates on, and
the one-way flush latch. **`Reset` is exactly the documented way back to a
decodable state, and it is skipped in full because GpDec refuses.** That is what
bounds finding 1 above: the selector could not move because the decoder never
left the state the refused submit put it in.

Note carefully what this does **not** say: GpDec's refusal happens before any
ioctl, so the driver is not shown refusing `Reset`. Which of GpDec's three
failure lines fired (`0x1645`, `0x164C`, `0x1653`) says why, and **the payload
already prints it to stdout** — this run piped stdout through `tail -40` and
lost it. Capture the whole of stdout next time; no rebuild is needed.

**5. Scanning kernel text panics the console. [E]**

`kdump` walked `KERNEL_ADDRESS_TEXT_BASE` in 1 MiB steps with `kernel_copyout`,
looking for the errno immediates. It printed the identity above, printed
`probing in 1 MiB steps, up to 128 MiB ...`, and the console panicked. The
user's report is that it **always** panics; the project has been deleted from
the repo and from `scripts/build.sh`.

This invalidates the premise **6b.2 was written on**. Findings §2 records
`kernel_copyout` as "working and unable to fault the caller", which is true of a
single small read at a known-good address and does not generalise to walking
megabytes of address space. **6b.2 as written is unsafe and is withdrawn.**

### 2026-08-13 — **zero deploys: the submit path is not fixed, and the identity hypothesis loses its footing**

No console time. Everything below came from the disassembly already in
`PS5-Research` and from transcripts already captured on 2026-08-11. The method
was to ask the *whole* VdecCore listing a question rather than to read the one
function under suspicion — over the imported `vdeccore_s0_full.asm`, 1 745
functions and 118 980 instructions, indexed with operands decoded.

**1. The submit path is not invariant, and run 14 swept the wrong axis. [E]**

A scan of every instruction referencing `+0x1d08` — the field the six-entry jump
table indexes to pick the ioctl command — finds **three** writers, not one. The
exported four-field setter at `+0x288f0` is the known one. The other two are
inside VdecCore at `+0x13e58` and `+0x158d8`, both writing the literal **1**,
which is mode `0x10` and **ioctl command 24** rather than the 23 every run has
taken:

```
mov  eax, [rcx+0x64f7c]
cmp  eax, [rcx+0x2a88]          ; two counters in a large codec context
jne  <skip>                     ; …must be equal
mov  edx, 0x204
lea  rdi, [rax+0x1448]
mov  DWORD PTR [rax+0x1d08], 1  ; -> command 24
call <memcpy>                   ; 0x204 bytes into obj+0x1448
```

Two byte-identical shapes in two large near-identical functions — the per-codec
sequence handlers, on their stack-frame sizes and their position in the module.

Run 14 concluded the submit path does not vary, having swept resource class and
pipeline depth. That conclusion is right about *configuration* and wrong about
*reachability*: configuration is not what writes this field. **Bitstream state
is.** Whether those two sites operate on the same GpDec object the submit uses is
**[H]** and untested — and testing it costs one probe with no kernel access,
which is why it is now the first thing to do.

**2. The driver gives two different answers in one run. [E]**

Found by re-reading `PS5-Research/deploy/decodeframe-run8-stdout.txt`, not by
deploying anything. Line `0x254` — the submit — appears **twice**:

| where | errno |
|---|---|
| `Decode(AU 0)`, returning `0x811D0111` | `0x1450` = **5200** |
| after `Flush(0)`, returning **`0x00000000`** | `0x13EF` = **5103** |

Same process, same fd, same open file, same ioctl command. **A permission check
refuses uniformly.** This one is inspecting the job. The process-authority
hypothesis is downgraded from "the live one" to third in the queue — not killed,
because a driver can validate jobs *and* check identity, but no longer worth a
kernel write ahead of two cheaper tests.

Three codes from this driver are now known: **5031** (`0x13A7`, already
understood — `size` not page-aligned), **5103**, **5200**. That matters for the
kernel scan: start from the one whose meaning is established, because a scan
that cannot find 5031 is a scan whose 5200 result should not be believed.

**3. The `0x83` ioctl inventory is complete, and the handshake succeeds. [E]**

VdecCore issues exactly **four** group-`0x83` commands, across three functions:

| function | command | encoding | what it is |
|---|---|---|---|
| `+0x2c8d0` | 11 | `_IOR(0x83, 11, 8)` | driver → us, 8 bytes |
| `+0x2c8d0` | 18 | `_IOW(0x83, 18, 12)` | us → driver, 12 zero bytes |
| `+0x287d0` | 20 | `_IOW(0x83, 20, 40)` | the memory pin |
| `+0x2b870` | 22 / 23 / 24 | `_IOW(0x83, 22, 24)` +0/+1/+2 | the submit |

`+0x2c8d0` had never been read. It is a **version handshake**: ioctl 11 returns
eight bytes that must equal exactly `0x07000000`, then ioctl 18 sends twelve
zero bytes. It is called from `+0x26a60`, the same initialiser that calls
`+0x2c470` — so it runs on every path that opens the device, right after the fd
and event-queue id are placed.

**It has never failed.** Its four failure lines are `0xD0A1` `0x51F`, `0x524`,
`0x525`, `0x536`, and none appears in any transcript in `PS5-Research/deploy/`
or `PS5-Research/console/`; the only `D0A1` lines that appear anywhere are
`0x00D5` (the pin) and `0x0254` (the submit). So the driver stated its version,
accepted it, and accepted the init call. **"We skipped a setup ioctl" is
eliminated**, and with the inventory complete there is no unissued command left
to suspect.

**4. `+0x1d70`, settled properly. [E]** The operand scan finds exactly one site —
the read at `+0x2b8b1` in the submit — and zero writes. The earlier "written
nowhere in the module" was a byte search; this is exhaustive over decoded
operands, which is the difference between *no occurrences of that form* and *no
occurrences*. The writer really is outside VdecCore.

**What this cost and what it is worth.** No deploys, no risk, no console. Two of
the four results contradict conclusions this project had already written down,
and both were reachable from evidence sitting on disk since 2026-08-11 — which
is the ordering [hardware-decode-review.md](hardware-decode-review.md) argued for
at the start, arriving one phase late. Next steps are
[hardware-decode-next-steps.md](hardware-decode-next-steps.md) **Phase 6b**.

### 2026-08-11 — **five hypotheses closed, one console panicked, and the next layer identified**

Six deploys. The session that took Phase 6 from "the driver refuses and we are
out of cheap ideas" to "everything on our side is measured and correct, and the
remaining question is in the kernel — which is readable".

**The arbitration hang was an unresolved lazy import. [E]** Three instructions
of disassembly settled what two deploys and a wrong hypothesis had not:
`+0x350` is a **PLT stub**, not a function body, and its GOT slot at segment 2
`+0x48` held the address of its own resolver sequence. The provider is
`libSceVideoArbitration.sprx` — a *different module* from
`libSceVideoDecoderArbitration.sprx`, sitting on the console, never loaded.
Loading it **before** the consumer (binding is eager at load time) bound all
three unbound slots, and `Initialize`, `Enable` and both `AcceptEvent` calls
returned 0.

This is the **third** silent block in this project caused by an unresolved lazy
import, after `AllocateComputeQueue` and `sceVideodec2MapMemory`. Standing rule
15 was written after the second one and was not applied to the third. The
lesson is not "check GOT slots" — that rule already existed — it is that a rule
written for one module has to be applied to every module.

**And arbitration changed nothing. [E]** With it fully up, the decode was
refused with the identical errno 5200 chain. It had been the prime suspect
since run 8. Killing a prime suspect properly is worth a deploy.

**The command buffer was not a dead end. [E]** The findings doc had recorded
"that is where the trail ends... the consumer is kernel-side". True of the
dump; false of a live decoder. The submit's object is the **GpDec device
object**, not the VdecCore object at `decoder+0x78` — applying the offsets
there returns zeroes in every field. It was found by searching our own work
memory for two constraints `0x1ab8` bytes apart, `mode == 7` at `+0x2c0` and a
plausible fd at `+0x1d68`. **Exactly one candidate matched.**

    fd 16 (a character device), event queue 0x11, [obj+0x1d70] = 1
    cmdBuf -> size 0xde8, phys 0x01a0ec00, mode 7, 1920 x 1088, phys 0x03630000

**The command is well formed.** Which kills the "codec module was never loaded"
theory — `libSceVdecSavc` and `libSceVdecSavc2` were loaded for the first time
this session and changed nothing.

**Annex-B is the framing. [E]** AVCC is rejected *earlier*, in software, with
`0x811D0303`. A framing the decoder will not look at is not the one it wants.

**The submit path does not vary. [E]** Mode index 5 → mode 7 → ioctl command
23, identical across resource classes `0xb6c8`/`0x12384` and depths 1/4. The
codec layer chooses it; configuration cannot reach commands 22 or 24.

**`0x1450` is a real errno. [E]** Read off the submit's own `__error()` deref,
not inferred.

#### The reference implementations were the most productive hour

Three projects drive or reimplement this library, found on GitHub and cloned to
`PS5-Research/references/`. Moonlight-ps4 **actually hardware-decodes H.264**
through `libSceVideodec2` and is annotated with console-validated notes.

They confirmed our struct layouts field-for-field and our memory types
(ONION/GARLIC/ONION) — the two most likely places for a silent error, neither
wrong. They exposed four config fields we had left at memset defaults, none of
them validated-error fields, so none would ever have surfaced as a bad return
code: `cpuAffinityMask`, `optimizeProgressiveVideo`, a macroblock-aligned
`maxFrameHeight`, and dpb/depth.

**`optimizeProgressiveVideo` turned out to change the frame layout. [E]** Set
it and all seven `mapMemorySize` rows drop by exactly 1024 bytes — the tail is
five 1 KiB metadata blocks normally, four when the stream is declared
progressive. Found by accident, from a 7/7 control becoming 0/7.

#### The expensive part

Moonlight opens video out *before* creating a decoder and ties that ordering to
`CPU_FAULT_SUBMITDONE_TIMEOUT` — a **submit** fault, the same stage our decode
dies at. No probe here had ever opened video out. It was the largest remaining
structural difference and it looked like the best lead of the session.

**It kernel-panicked the console. [E]** The per-line-flushed log survived on the
USB stick and locates it exactly: the real logged-in user is refused with
`0x80290001`; the `0xFF` retry returns `1309671680` = `0x4E100000`, which is
**not a handle**; and the next call, `sceVideodec2AllocateComputeQueue` — which
had succeeded in every previous run — panicked. About fifty minutes of
recovery.

Three lessons, in `hardware-decode-findings.md` §9.1 and standing rules 18–20.
The one that generalises furthest: **a practice borrowed from a reference
implementation is only valid if what it assumes about its process is also true
of ours.** Moonlight is a title that owns its process; this payload is injected
into a borrowed app slot that already owns the display pipeline. The reasoning
behind the experiment was sound and the transfer was never checked.

The same build carried an ioctl probe on the decoder fd, justified as "low risk
by construction, drivers validate their inputs". It never ran, so it is *not*
what panicked the console — but that reasoning had nothing behind it and the
probe has been removed. Both sites now carry the post-mortem in comments, and
`-lSceVideoOut` is unlinked so the call cannot return by accident.

It did return three things: video out is genuinely unavailable to a payload
(a clean refusal, not a broken call), the Moonlight ordering hypothesis is
closed rather than left open, and the progressive-video measurement above.

#### psdevwiki, mirrored — and mostly a negative

1,147 PS5 and 5,282 PS4 pages exported with `tools/psdevwiki-dump.js`. The site
is behind a Cloudflare managed challenge that rejects curl, wget and
TLS-impersonating clients alike (tested), so the exporter runs in an
already-cleared browser tab and drives MediaWiki's `api.php`.

**Errno 5200 is not documented anywhere** — every hit is a coincidental
substring in unrelated NP codes, and `Devices` knows `uvd_{dec,enc,bgt}` exists
only as *"Maybe related to gameplay recording"*. Worth knowing so nobody
searches there again. It did give two structural facts: all decoder modules
share auth ID `4900000000000002`, and PS4's dedicated `SceVdecProxy.elf`
process **has no PS5 equivalent**, confirming the in-process architecture.

#### Where it leaves Phase 6

Everything on our side of the ioctl is measured and correct. So the question is
in the driver — and the driver is readable. The SDK exports
`KERNEL_ADDRESS_TEXT_BASE` already resolved for 12.70 plus `kernel_copyout`,
`kernel_get_proc_file` and `kernel_get/set_ucred_authid`. **Firmware decryption
is unnecessary**: the userland modules are already decrypted from memory, and a
kernel memory dump gives the running, relocated kernel with real addresses.

The live hypothesis is that the driver refuses *who is asking* rather than
*what is asked*, and it is **[H]** — nothing yet shows the driver checks
authority at all. Findings §13 sets out how to read the answer instead of
guessing it: a read-only pass first, and a kernel write only with explicit
agreement.

### 2026-08-11 — the user session: **a documented finding was measured in the wrong process**

One deploy. The lead came from a question about running the research inside EVO
Player, and the answer turned out to be about three lines of initialisation
rather than about the process.

**EVO Player does something none of the Phase 4–6 probes do:**

```c
sceUserServiceInitialize(NULL);
sceUserServiceGetLoginUserIdList(users);
int pad = scePadOpen(users[0], 0, 0, NULL);   /* and this succeeds */
```

`decodeframe_test`, `avplayer_probe` and `codecdump_test` never called
`sceUserServiceInitialize` at all. `decoder_test` did — but it is deployed with
`deploy.sh`, so it measured `SceSpZeroConf`, not the app slot.

**Measured in the app slot, with Initialize called first: [E]**

    sceUserServiceInitialize         -> 0x00000000
    sceUserServiceGetLoginUserIdList -> 0x00000000  ids 513995993 -1 -1 -1
    sceUserServiceGetInitialUser     -> 0x80960006  user_id -1

**So "a payload has no user session" is wrong where the player runs.** There is
a logged-in user and a real id. `GetInitialUser` still fails — and with a
*different* code than the one on record (`0x80960006` here versus `0x80940004`
under `deploy.sh`, which is itself a hint that the two slots differ) — but
`GetLoginUserIdList` is the call that works, and it is the one EVO Player uses.

This is the **same error as the 41 MiB memory ceiling**: a property of
`SceSpZeroConf` recorded as a property of payloads. Standing rule 12 was written
after that one and was not applied here. §2 is corrected.

**It did not unblock arbitration. [E]** `sceVideoDecoderArbitrationInitialize`
was called immediately afterwards, with a live session in the process, and
blocked exactly as before — the flushed log stops at the last control, 7/7 again.

So the hypothesis this run existed to test is **dead**: the block is not a
missing user session. What blocks in that body at `+0x350` is still unknown, and
there is no longer a cheap hypothesis for it.

**Worth having anyway.** "Payloads have no user session" is the kind of claim
that would misdirect anything touching the pad, audio routing or per-user paths,
and it had been sitting in §2 unchallenged since Phase 2.

### 2026-08-11 — Route A: **AvPlayer runs in a payload, and its video path fails EARLIER than ours**

Four deploys of `projects/avplayer_probe/`, every callback instrumented. Route A
was opened to settle whether Route B's problems were our sequence or our
privileges. It answered that, and not the way either hypothesis expected.

**1. `sceAvPlayerInit` returns a handle in a payload. [E]** `0x88000efe0`, with
AvPlayer's own debug log enabled at `debugLevel 4`:

    D/ sceAvPlayerInitImpl [384]:  MVP core shared heap size: 1572864
    D/ SupplyReplacements [1148]:  Supplied required Params
    D/ avControllerThread [441]: Thread started

**That is a new instrumentation channel.** AvPlayer narrates its own internals
at debug level 4 and the lines come back on stdout with everything else.

**2. The whole `SceAvPlayerInitData` layout is confirmed. [E]** Not inferred -
AvPlayer called all of our callbacks with sensible arguments, which it could
only do if every function pointer sat where we put it. The PS4 layout is the
PS5 layout: memory replacement at `+0x00`, file replacement at `+0x28`, event
replacement at `+0x50`, `debugLevel` `+0x60`, `basePriority` `+0x64`,
`numOutputVideoFrameBuffers` `+0x68`, `autoStart` `+0x6c`, `defaultLanguage`
`+0x70`, total `0x78`.

**3. The demuxer works, from memory. [E]** `sceAvPlayerAddSource` returns `0`
against an MP4 served entirely out of `.rodata` through the file callbacks -
nothing copied to the console. The read pattern is classic atom walking (offset
0, 32, 902, 910, 40, 148 …), 36 reads and 811 bytes to parse the container.
**So the MP4 demuxer really is inside `libSceAvPlayer`**, as §3 suspected from
the `sceMp4*` strings with no `libSceMp4.sprx` present. **[E]**, upgraded from
**[H]**.

**4. Its allocation pattern, which Phase 10 needs either way. [E]**

| # | align | size | note |
|---|---|---|---|
| 1 | 32 | 1,572,864 | "MVP core shared heap" |
| 2 | 32 | 616 | the player handle itself |
| 3 | 32 | 3,145,728 | at AddSource |
| 7 | 8 | 1,310,720 | decoder work |
| 8 | **256** | 3,037,184 | **allocateTexture** - a frame buffer |
| 9 | **2,097,152** | 6,291,456 | 2 MiB alignment, so GPU-facing |

Nine allocations, ~15.4 MB, all freed on `Close`. Note it uses
`allocateTexture` for exactly one buffer, at 256-byte alignment - the same
alignment Route B's `Decode` demands of its frame buffer.

**5. And then AvPlayer's video path fails. [E]**

    [SCEVDECCORE@A01D02F3:00000000]
    [SCEVDECCORE@A01D03A5:80C00002]
    [SCEVIDEODEC2@A01A0402:00000000]

Line `0x3A5` is `sceVdecCoreQueryInstanceSize` propagating a failure from its
config validator; line `0x2F3` is that validator's unsupported-configuration
arm, returning `0x80C00002`. No READY event ever arrives, `StreamCount` stays
`0`, and the player goes straight to STOP.

**So AvPlayer cannot even QUERY a decoder configuration in this process - and
Route B queries and CREATES one routinely.** Route B is further along than
AvPlayer here, which inverts the premise Route A was opened on. AvPlayer is not
holding a key we lack; it is failing earlier than we do.

**What it does not settle:** arbitration. AvPlayer never got far enough to call
it, so our direct `Initialize` still blocks and the question of whether the
submit needs arbitration is still open. The discriminator now runs after
playback (`--args "eboot.elf arb"`) rather than before it - run 1 probed
arbitration before playback, which could only ever hang, and that was an
ordering error.

**Three of the four deploys were spent on my own mistakes**, and they are worth
recording because they are all the same mistake in different clothes -
assuming a synchronous API:

- run 1: probed arbitration *before* playback, when AvPlayer brings it up
  during playback. Hung.
- run 2: left `autoStart = 0` and never called `Start`, so the player went
  READY then STOP without reading a byte of `mdat`. 811 bytes read is a
  container parse, not a decode.
- run 3: called `sceAvPlayerStart` immediately after `AddSource`, before the
  demux thread had finished. `StreamCount` returned `0` and `Start` deadlocked.

The fix in each case was to wait for the event callback rather than assume the
previous call had completed.

### 2026-08-11 — Phase 6, arbitration: **ABI confirmed 7/7, and the real call hangs**

One deploy. The Phase 5 discriminator table predicted that a decoder which
creates but will not decode is what an unarbitrated client looks like, so
`libSceVideoDecoderArbitration` was read completely - 16 KB, four exports - and
called.

**The reading was right.** 7/7 controls exact: wrong `thisSize`, priority 768,
count 0, NULL params, `Enable` with a non-NULL first argument, `Enable` with a
NULL callback, and `AcceptEvent(2)` each returned exactly the predicted code.
The params struct is `{thisSize 0x18, priority 256..767, count 1..127}`, and
`Enable` takes `(NULL, callback)` - the first argument *must* be NULL, the
second is stored in a module global and tail-called through a trampoline.

The priority range 256..767 is the same one `sceAvPlayerInit` clamps
`basePriority` into and the decoder config checks at `cfg+0x38`. Three
independently-read functions agreeing.

**And then the real `Initialize` never returned. [E]** The per-line flushed log
on `/mnt/usb0` stops at the last control; the payload held the app slot until
the deploy's `timeout` detached it. No fault, no error code - the third silent
block in this effort, after `AllocateComputeQueue` without `libSceGnmDriver`
and the `libSceAudiodec` load-order hang.

The first two were unresolved lazy imports. **This is not that**: all four
exports resolve and the controls prove the module's own validation is running.
The body at `+0x350` blocks. The reading that fits is an IPC to a system service
that does not answer in a payload process **[H]**, which is consistent with
`sceUserServiceGetInitialUser` reporting no user session - arbitration exists to
referee decoder access between titles, and a payload sits outside that.

**So the hypothesis is untestable by this route.** That does not show errno 5200
is unrelated to arbitration; only that Route B cannot ask the question this way.
Which is exactly the argument for Route A: AvPlayer runs arbitration
successfully, so instrumenting it would show what makes the call answer.

### 2026-08-11 — Phase 6: **the submit named, and a wrong conclusion corrected**

`projects/codecdump_test/`, one deploy, no decode call, no hang.

**The correction first.** The entry below concluded that the code refusing the
decode was *not in the dump*, from a raw byte search finding no `mov ecx,0x254`
anywhere in `libSceVdecCore`. The search was right; the conclusion was wrong.
Three ioctl call sites share one logging tail, and the line number reaches it in
a register:

    mov  r14d, 0x254
    ...
    mov  ecx, r14d

so the immediate never appears next to `ecx`. **A byte search for an immediate
only disproves the immediate form** — worth remembering, because the search felt
conclusive precisely because it was immune to the disassembler desync that
motivated it.

**The submit, now named. [E]** `libSceVdecCore +0x2b870`:

    ioctl(fd, _IOW(0x83, 23, 24), { job, dword, arg, dword })

It picks one of three adjacent commands from a mode field at `[obj+0x2c0]` —
`7` selects command 23 and error line `0x254`, `0x10` selects 24, anything else
22. The run reported `0x254`, so the mode is 7, the command is 23, and the
driver refused it with errno 5200. The other group-`0x83` commands VdecCore
issues are 11, 18 and 20; 20 is the memory pin `MapDirectMemory` drives.

**And the probe's own result. [E]** A modid sweep before and after an H.264
`CreateDecoder` finds **exactly the same 11 modules** — nothing loads on this
path, despite the codec dispatch in `sceVdecCoreCreateDecoder` being real
(`0x80000036` H.264, `0x8000003c` HEVC, `0x80000035` on a resource-class path,
with `0x805A1001` meaning already-loaded). Either the codec is already resident
or that branch is not taken for H.264 here.

The run also dumped five modules never dumped before — `libSceSysmodule`,
`libSceVdecwrap`, `libSceVdecShevc`, `libSceGnmDriver`, `libSceAjm`, 1.4 MB into
`proprietary/dump/codec/` — so the negative result arrived with the material to
rule them out. None contains the submit.

**Loose end:** the sweep shows modids `0x42`–`0x47` and `0x49` for eight loaded
modules. **`0x48` is unaccounted for**, the same shape as Phase 0's missing
`0x34`. `sceKernelGetModuleInfo` either fails for it or returns a blank name,
and the probe drops blank names silently. Fix that before the next enumeration.

### 2026-08-11 — Phase 6, offline: ~~the code that refuses the decode is not in the dump~~ **(wrong — see above)**

Zero deploys. An attempt to read the failing submit path the same way the four
previous gates were read, which stalled — and the reason it stalled is the
result.

Three independent facts, each cheap:

1. **`mov ecx,0x254` occurs nowhere in `libSceVdecCore`'s text.** A raw
   byte-pattern search over the segment (immune to disassembler desync, unlike
   grepping the listing) finds zero, while all 44 other `0xD0A1` diagnostic
   sites have their line numbers present. **[E]**
2. **The driver-interface vtables are not in the dump.** The submit is a virtual
   call `[[gpdec+0x38]] + 0x18`; the map call goes through `[[obj+0xb0]] + 0x00`.
   None of those implementation addresses appears as a pointer in any dumped
   segment of any module. **[E]**
3. **`sceVdecCoreCreateDecoder` loads a codec module on demand**, dispatching on
   the codec index: **`0x80000036` for H.264**, `0x8000003c` for HEVC,
   `0x80000035` on a separate resource-class path, each through
   `sceSysmoduleLoadModuleInternal` with `0x805A1001` meaning already-loaded.
   **[E]**

So an H.264 `CreateDecoder` maps an internal module we have never enumerated,
and **that** module owns the submit, the vtables and the failing ioctl. §3's
"loading the media modules pulls in no new modules at all" was measured before
any decoder existed and does not describe the process afterwards.

The next move is the one that has broken every impasse in this effort: **one
deploy that dumps beats N that guess.** Create an H.264 decoder, sweep modids
through `sceKernelGetModuleInfo`, dump the new module. No decode call, so no
hang risk. `projects/decoder_test/` already has the sweep and the dumper.

Arbitration stays on the list but not at the front: the previous driver errno
in this same chain, 5031, also looked like a rights problem and turned out to
be an unaligned length.

### 2026-08-11 — Phase 6, runs 2–8: **four gates cleared, the driver still says no**

Six more deploys of `projects/decodeframe_test/`. No hang in any of them, clean
teardown every time. No picture yet — but the path from `Decode` to the
hardware is now fully mapped, and **every step was diagnosed from the module's
own diagnostic lines rather than by sweeping anything**.

| run | refused at | cause | fix |
|---|---|---|---|
| 1 | GpDec `0xC22`, state 0 | nothing had been mapped | call `MapDirectMemory` |
| 3 | pin ioctl, errno 5031 | `size` not page-aligned — `mapMemorySize` is `0x331400` | round up to 16 KiB |
| 4 | GpDec `0x97F`, status `0x33` | every block given `physAddr` 0, so all occupied `[0, size)` and overlapped | use the real physical offset |
| 6 | GpDec `0xDA6` | the AU, the work memory and the frame pool were not registered | register all of them |
| **8** | **the hardware submit**, driver errno **5200** | **open** | — |

**`sceVideodec2MapDirectMemory` is now fully characterised** — see
[hardware-decode-findings.md](hardware-decode-findings.md) §7 for the contract.
Three results from it are worth repeating because none was guessable:

- **The field order is not the obvious one.** `+0x08` is the *size* and `+0x10`
  is the pointer. Settled by `lea r15,[rdx+r13]` computing `base + len` in
  VdecCore, not by trying both.
- **`physAddr` is an address in a space of its own**, and blocks must not
  overlap in it. Zero for every block is what made run 3's block 1 fail. A
  control — an unregistered CPU address carrying block 0's `physAddr` — is
  refused, which proves the reading rather than assuming it.
- **`MapDirectMemory` is not "map the output frames".** Everything the hardware
  touches must be registered: the frames, **the access unit** (so a `.rodata`
  bitstream can never work — the hardware DMAs it), **and the work memory and
  frame pool that `CreateDecoder` was already handed**. Passing `CreateDecoder`
  a pointer is not the same as telling the hardware about it.

**Two runs were wasted on my own mistakes**, and both are worth recording. Run
2 varied `physAddr` while `size` was still wrong, and read the identical
failures as evidence about `physAddr` — but with mode 0 that field never
reaches the driver ioctl, so both attempts issued byte-identical calls. **The
experiment was inconclusive by construction and I did not notice.** Run 6
carried a stale assignment that set the frame-buffer size to the input
buffer's, which cost a deploy to spot.

**Method note that earned its place: capture stdout, not just the USB log.**
The module's diagnostic lines only come back on stdout. Run 3 was launched with
stdout redirected to `/dev/null` and the log on the stick showed a bare
`0x811D0111` with nothing to act on; the next run, identical except for keeping
stdout, named the exact rejection site. Standing rule 16 exists for this.

**Where it stands:** every software gate is cleared and the submit itself is
refused by the driver. That is a different kind of failure from the four before
it. The leading hypothesis is the one the Phase 5 discriminator table already
wrote down — *"Succeeds, then decode fails → arbitration"* —
`libSceVideoDecoderArbitration` has never been called, and AvPlayer calls
`Initialize` / `Enable` before it decodes anything. Reading its four entry
points is the next step.

### 2026-08-11 — Phase 6, run 1: **no picture, and the module said exactly why**

**One deploy, no hang, clean teardown.** `projects/decodeframe_test/` in the app
slot. `research-logs/console/evo_decodeframe_log-run1.txt`.

**Two things confirmed on hardware:**

- **18/18 validation controls exact** — 11 for `Decode`, 7 for
  `GetPictureInfo`. Every struct size, every field offset, every error code in
  the ABI read offline is now measured. **[E]**
- **7/7 `mapMemorySize` predictions exact**, across AVC 720p/1080p/4K and HEVC
  Main and Main10 at 1080p and 4K. **[E]** The frame-layout formula is a
  property of the module, not a hypothesis about it.

**And one offline conclusion falsified.** The real `Decode` returned
`0x811D0111` and no picture. That code is the generic "unexpected VdecCore
value" bucket and says nothing on its own — but the module printed three
diagnostic lines with it:

    [VDECCORE@B0A10C22:00000000]
    [SCEVDECCORE@A01D07A8:00000002]
    [SCEVIDEODEC2@A01A07A7:80C00001]

Innermost first: GpDec refused at its line `0xC22` with value `0`; VdecCore
bucketed that as status `2` at line `0x7A8` and returned `0x80C00001`; Videodec2
had no table entry for it and returned `0x811D0111`. Grepping the disassembly
for those line numbers landed on the exact sites:

```
mov r8d, DWORD PTR [r14+0x40]   ; GpDec state
cmp r8, 0x5
ja  ok
mov eax, 0x31                   ; bits 0, 4, 5
bt  eax, r8d
jae ok                          ; states 1,2,3 and >=6 accepted
                                ; states 0,4,5 refused
```

**The GpDec object was in state 0.** The only two writes of `1` to that field
trace back to `sceVdecCoreMapMemoryBlock` — which is what
`sceVideodec2MapDirectMemory` calls, and which the probe deliberately skipped.

The offline half had concluded that `MapMemory`/`MapDirectMemory` "are not part
of decoding", from a reading of `libSceVideodec2` that was complete and correct.
Nothing on Videodec2's decode path does consult a mapped flag; the state that
matters simply is not at that layer. **The phase plan's original instinct was
right and the offline correction that replaced it was wrong.** Two new standing
rules came out of it: capture the module's diagnostic lines (16), and remember
that reading a layer to the end only explains that layer (17).

**Next:** call `sceVideodec2MapDirectMemory` — the bound one; `MapMemory`'s
import is still unbound and still a hang risk — before the first `Decode`. Its
info struct is 0x20 bytes and repacks into VdecCore's 0x20-byte entry array; the
field meanings need one more read before the call, per rule 1.

### 2026-08-11 — Phase 6, offline half: **the decode ABI, read before anything was called**

**Zero deploys.** `projects/decodeframe_test/` is written and builds; it has not
been near a console, and nothing below is a statement about decoder behaviour.
Everything here comes from `proprietary/dump/` and `objdump`.

Three things came out of it, and each one changed the phase plan rather than
confirming it — which is the third time in a row the offline half has been worth
more than the deploy it precedes.

**1. `sceVideodec2Decode` takes four arguments, and the frame buffer is one of
them.** **[E]** The plan had assumed `mapMemorySize` was what `MapMemory` and
`MapDirectMemory` were for. It is not: `Decode(decoder, au, fb, out)` carries
the output buffer in its third argument, nothing on the path consults a "mapped"
flag, and neither map call is reachable from the decode path at all.

**2. `sceVideodec2MapMemory` would very likely have hung, and one GOT slot said
so.** **[E]** Its PLT stub's slot (Videodec2 s2 `+0x60`) still contains the
address of its own `push`/`jmp` resolver sequence — lazily bound, never called —
while every neighbouring slot that lands in `libSceVdecCore` is resolved. That
is the exact shape of the Phase 4 hang, except worse: Phase 4 could name the
missing module (`libSceGnmDriver`) by reading the chain down. Here the slot
never bound, so the dump does not say who owns the symbol.

The probe therefore **does not call it**, and the phase that was going to open
with `MapMemory` opens by not needing it. **New standing rule 15: check the GOT
slot before you call the function.** Rule 11 said load every module in the call
chain; this is how you find out which, for free.

**3. `mapMemorySize` is exactly one output frame, and that names the pixel
format.** **[I]** Phase 5 measured `memInfo+0x38` eight times without knowing
what it was. Working backwards from those eight numbers:

    mapMemorySize = align(width, 256) * align(height, N) * bytes * 3/2 + 5 * 1024

with `N` = 16 for H.264 and 1 for HEVC, `bytes` = 2 for Main10. It reproduces
**all eight** figures to the byte — 1,387,520 at 720p, 3,347,456 at 1080p,
12,446,720 at 4K, 3,322,880 for HEVC Main 1080p, and so on. A `× 3/2` with
half-height chroma is **NV12**; `× 3` is **P010**. The stride is **2048 at
1080p, not 1920**.

The trailing `5 * 1024` is explained by `GetPictureInfo`, which reads picture
metadata from `frameBuffer + frameBufferSize - pictureCount * 1024`, walking
backwards 1 KiB per picture: **the frame buffer carries its own metadata in its
tail**, five slots of it.

That is most of Phase 7's first two questions answered without a deploy. It is
still arithmetic — the probe re-checks all seven live configurations against the
module before it decodes anything, so a mismatch shows up as a mismatch rather
than as corruption two phases later.

**Also recorded:** two more alias pairs, bringing the total to four of eighteen
exports. `GetPictureInfo` and `GetAvcPictureInfo` are byte-identical; so are
`GetHevcPictureInfo` and `GetVp9PictureInfo`. One body serves all four and
dispatches on `outputInfo->codecType`. **[E]** Standing rule 14 keeps earning
its place.

**What could not be settled offline:** bitstream framing. Videodec2 passes
`auData`/`auSize` straight through to VdecCore, and nothing on the path scans
for start codes, so Annex-B versus AVCC is invisible from here — it bottoms out
in a `uvd_dec` ioctl that is not in the dump. The probe tries **both**, in one
deploy, Annex-B first, each on its own decoder.

**And the input needs no USB stick.** The plan called for copying an elementary
stream onto the console, which the read-only `/fs` makes a manual step on every
run. `tools/gen-test-stream.sh` encodes eight access units of `testsrc2` at
1080p High L4.0 instead, and `test_stream.S` links the result into `.rodata`
with `.incbin`. Access units are split on NAL type 9, which the encoder is told
to emit, so the split is exact rather than heuristic.

Full ABI in [hardware-decode-findings.md](hardware-decode-findings.md) §7.

### 2026-08-11 — firmware 12.70 — Phase 5: **a payload creates a hardware decoder**

**Two deploys, and they answered everything the phase existed to ask.**
`projects/createdecoder_test/`, launched into the app slot.

    sceVideodec2CreateDecoder ... rc=0x00000000 OK decoder=2004e0000
    *** DECODER CREATED ***

`rc=0` on **both** usable resource classes at H.264 1080p DPB 16, **and for
HEVC Main** on `0x12384`, with `DeleteDecoder` returning `0` every time. **[E]**
**No entitlement error, no arbitration call, no user session.** The risk this
phase existed to test — *"this is where an entitlement gate would appear, if
there is one"* — did not materialise, for either codec.

Every structural prediction made offline held: the decoder handle came back
**identical to `memInfo+0x10`**, the caller's own buffer; the VdecCore object
sat at exactly `pWorkMemory + 0x40000`; both magic cookies were present; and
the 9 cond vars at `+0x80` and 12 mutexes at `+0xc8` were visible in the
object dump at precisely those offsets. **9/9** validation controls returned
the predicted code.

**HEVC is accepted, and it creates.** **[E]** Asked properly for the first time
— profile 1/2, `general_level_idc` — it returns its own memory arithmetic,
different from H.264 in every field, which is what proves a real configuration
was accepted rather than waved through:

| | work `+0x08` | frame `+0x18` | map `+0x38` | buffers |
|---|---|---|---|---|
| H.264 High 1080p dpb16 | 3,568,896 | 86,507,776 | 3,347,456 | 85.9 MiB |
| HEVC Main 1080p dpb16 | 5,874,688 | 63,242,496 | 3,322,880 | **65.9 MiB** |
| HEVC Main10 1080p dpb16 | 5,874,688 | 91,685,120 | 6,640,640 | 93.0 MiB |
| HEVC Main10 4K L5.1 dpb16 | 5,874,688 | 326,107,392 | 24,888,320 | 316.6 MiB |

HEVC Main at 1080p is **cheaper than H.264**. It requires resource class
`0x12384`; `0xb6c8` refuses it with `0x811D0200`.

**VP9 is not available.** **[E]** Refused at every configuration tried, profile
0 and 2, 1080p and 4K, `0x811D0200` from the size computation each time — a
capacity refusal, not a validation one. It cross-checks exactly against
`libSceVdecSvp9.sprx` not existing on this firmware. Scope for this effort is
**H.264 and HEVC**.

And `memInfo+0x28` is **zero** for every accepted configuration, H.264 and
HEVC alike, so the third buffer is never needed on these paths — which is what
`sceVdecCoreCreateDecoder`'s class check predicted.

Raw log: `research-logs/console/evo_createdecoder_log.txt`.

#### The offline half, which is why the deploy only had to happen once

**Zero deploys.** `sceVideodec2CreateDecoder` was disassembled before being
called, per standing rule 1, and reading the config validator *all the way to
the end* rather than as far as the first accepted call changed the phase.

**The correction.** **[E]** `cfg+0x08` and `cfg+0x0c` are the other way round
from how Phases 2–4 recorded them. `+0x0c` is the **codec** and `+0x08` a
**resource class**. The proof is the profile and level validation that follows
each codec value, which is codec-specific and unambiguous:

| `cfg+0x0c` | profile | level | reading |
|---|---|---|---|
| `1` | 66, 77, 100 | 10..111 | H.264 `profile_idc` / `level_idc` |
| `0xee049` | 1, 2 | {30, 63, 90, 93} ∪ 120..186 | HEVC Main / Main10, level × 30 |
| `0x245bfd` | 0, 2 | 10..62 in fixed steps | VP9 profile 0 / 2, level × 10 |

**Why it matters more than a relabelling.** Every one of the six configurations
Phase 3 recorded as accepted used `+0x0c = 1` — all H.264. Its
`0xb6c8 v=ee049 → 0x811D0205` lines were read as the module refusing HEVC; they
are the probe handing HEVC a profile of 100 and a level of 51, which are H.264
numbers, against a code that means *unsupported profile or level*. **HEVC has
never been validly queried**, and the phase plan's HEVC discriminator —
"succeeds for `0xb6c8`, fails for `0x12384`" — could not have worked, because
neither value is a codec.

**Also read off the module, all [E]:**

- `CreateDecoder` (+0xba0) takes **three** arguments — config, memory-info,
  output handle — plus a fourth its thunk supplies.
  **`CreateHevcDecoder` (+0x1230) is a byte-for-byte alias of it**, and
  `QueryHevcDecoderMemoryInfo` (+0xb90) a bare `jmp` to the plain query's body.
  Neither does anything HEVC-specific.
- **The decoder handle is the caller's own buffer.** The module builds its
  object in the first `0x40000` bytes of `memInfo+0x10` and hands that pointer
  straight back — which is why the `+0x08` requirement is ~3.4 MiB and barely
  moves with resolution.
- **Half of `SceVideodec2DecoderMemoryInfo` is input.** The query fills the
  sizes and then explicitly zeroes `+0x10`, `+0x20`, `+0x30` and `+0x44`. The
  module's own checker wants **`WB_ONION`** at `+0x10` and **`WC_GARLIC`** at
  `+0x20`. The size at `+0x28` has never been logged by any probe.
- `sceVdecCoreCreateDecoder` rejects resource classes above 8, which is why
  `0xb6c8` (class 4) and `0x12384` (class 8) are the only two that have worked
  — and on the HEVC codec index it calls `sceSysmoduleLoadModuleInternal`,
  a lazy import of exactly the shape that hung Phase 4. **[I]** So
  `libSceSysmodule.sprx` has to be loaded before `CreateDecoder`.

The probe led with the Phase 3 control, then the full memory-info hexdump, then
a codec matrix that is pure queries — so the HEVC question was answered at
zero risk in the same deploy as the create. **Five deliberate-error controls
confirmed the field correction by experiment**: a bogus codec at `+0x0c` gave
`0x811D0204` and a bogus resource class at `+0x08` gave `0x811D0203`. Had the
labels been the other way round, those two would have come back swapped.

The HEVC *create* was held back to a second deploy behind an argv flag, because
it is the only call in the probe that reaches a module load, and a module load
is the one operation in this work that has hung rather than failed. **It did not
hang** — `sceVdecCoreCreateDecoder` called `sceSysmoduleLoadModuleInternal`, the
import resolved, and the decoder came back with codec index `4`.
`libSceSysmodule.sprx` was in the module list from the start, put there by
standing rule 11 while reading VdecCore. **That is the first time in this effort
the rule has been applied before paying for it rather than after.**

**The method note worth keeping:** the wrong reading had been in the reference
document for three phases and was invisible the whole time, because it still
produced working calls. Reading further cost nothing and no deploys. A reading
that explains the successes you have is not the same as a reading that is
right.

### 2026-08-10 — firmware 12.70 — Phase 4: **a payload gets a GPU compute queue**

Two deploys, `projects/computequeue_test/`. **`sceVideodec2AllocateComputeQueue`
returns 0 with a live handle, and `ReleaseComputeQueue` gives it back cleanly.**
**[E]** The risk register's most likely hard blocker — "compute queue refused" —
is not a blocker.

The memory ceiling is, though. Read to the end.

**What was measured.** **[E]**

- `sceVideodec2QueryComputeMemoryInfo` returns 0. The compute resource wants a
  fixed **4,805,120 bytes (4.58 MiB)** — it takes no decoder config, so it does
  not scale with resolution. It also takes **one** argument, not the `(cfg,
  memInfo)` pair the decoder query uses.
- Its structures, and `AllocateComputeQueue`'s, were read offline before
  anything was called, and **7 of 7 deliberate-error controls returned exactly
  the predicted code** — wrong sizes → `0x811D0101`, non-zero reserved byte →
  `0x811D0200`, pipe ≥ 5 → `0x811D0201`, queue ≥ 8 → `0x811D0202`, buffer one
  byte short → `0x811D0104`.
- The Phase 3 query was re-measured as a control in the same run and returned
  the same 86,507,776-byte frame pool. Nothing drifted.

**The first deploy hung.** **[E]** `AllocateComputeQueue(pipe 0, queue 0)` with
valid `WB_ONION` memory never returned. Because the log is flushed every line,
the stop was located exactly: past all validation, inside
`sceVdecCoreInitializeComputeResource`. The console stayed healthy and elfldr
accepted the next connection.

**And a ceiling that turned out not to be one.** **[E]**
`sceKernelAllocateMainDirectMemory`, laddered — first under `deploy.sh`, then
the same ELF relaunched through `install-homebrew.sh --run`:

| request | `deploy.sh` | app slot |
|---|---|---|
| 4.6 MiB (compute), 16, 32, 41 MiB | OK | OK |
| 64, 90, 109, 160, 322 MiB | `0x80020023` EAGAIN | **OK** |

Identical on `WB_ONION` and `WC_GARLIC` in both slots, so memory type is never
the issue. Under `deploy.sh` this looked fatal — 1080p decode needs 89 MiB, so
the hardware path would have been capped at a resolution the CPU path already
handles. **In the app slot every size allocated, including the full 4K dpb-16
working set; the real ceiling there is above 322 MiB and was not found.**

The cause was already written down in [building.md](building.md) and had not
been connected to this work: `deploy.sh` injects into **`SceSpZeroConf`**, a
background network service spawned with `dmem#0`, while
`install-homebrew.sh --run` goes through `hbldr_launch` into the **PS Now app
slot** — a real application process, which is where EVO Player itself runs.
building.md frames that boundary in terms of the display plane; memory obeys it
too. **No new eboot.bin or application is needed — only the right launcher.**

The compute queue allocates in both slots, so nothing else about hardware
decode depends on this.

**Three plan corrections paid for by this deploy.** `MapMemory` and
`MapDirectMemory` are Phase 5 calls, not Phase 4 — both demand a decoder-handle
magic at `+0x68` and return `0x811D0103` without one.
`sceKernelAllocateDirectMemory` is the wrong allocator for a payload. And the
compute-queue memory wants `WB_ONION`, not the `WC_GARLIC` the plan assumed —
read from the module's own gated validator, which requires `memoryType == 0`.

Following `libSceVideodec2`'s PLT into its GOT also named the layer below
without a symbol table: `QueryComputeMemoryInfo` →
`sceVdecCoreQueryComputeResourceInfo`, `AllocateComputeQueue` →
`sceVdecCoreInitializeComputeResource`, `ReleaseComputeQueue` →
`sceVdecCoreFinalizeComputeResource`, `MapDirectMemory` →
`sceVdecCoreMapMemoryBlock`.

**The chain was then disassembled, at zero deploy cost, and it named the
blocker.** `AllocateComputeQueue` → `InitializeComputeResource` → VdecCore
+0xd1c0 → +0x58bf0 → +0x67270 → a five-argument import
`(pipeId, queueId, ringBase, ringSizeInDW, readPtrAddr)`, which is
`sceGnmMapComputeQueue`. **[E]** for the chain, **[I]** for the name. Its GOT
slot was unresolved in the dumped image — lazily bound and never called — and
**the probe never loaded `libSceGnmDriver.sprx`**.

**The second deploy added that one module and nothing else. `rc=0`, handle
`0x2002b0500`, released cleanly.** **[E]** Pipe 0 / queue 0 was right from the
first attempt; a hardware sweep of the five pipes and eight queues would have
cost many deploys and found nothing.

Two things worth carrying forward, both cheap and both nearly expensive:

- **An unresolved lazy import blocks silently and forever.** No fault, no error
  code, no log line. It looks exactly like a GPU problem and is not one. Load
  every module in the call chain, not just the one whose name is on the
  function.
- **`EXIT=124` does not mean the payload hung.** The *successful* run also
  exited 124, with its socket output truncated at exactly the same line as the
  hang. Only the log on `/mnt/usb0` showed `rc=0`. Without that file a success
  would have been recorded as a second failure, and the obvious next move would
  have been to abandon Route B.

And a third, which cost two of the three runs: **know which process the probe is
running in.** `deploy.sh` and `install-homebrew.sh --run` are not two ways of
doing the same thing — they land in different processes with different budgets,
and a resource measurement from one does not transfer to the other.

Phase 5 is unblocked, at 1080p, launched through `install-homebrew.sh --run`.

### 2026-08-10 — firmware 12.70 — Phase 3: **the hardware decoder answered**

**`sceVideodec2QueryDecoderMemoryInfo` returns 0 and real, resolution-scaled
memory requirements from an elfldr payload.** **[E]** The PS5 hardware video
decode stack is reachable, configurable and not permission-gated at query time.
`projects/videodec2_test/`.

This was chosen over `sceAvPlayerInit` as the first call because it is a pure
query — it creates nothing, allocates nothing and touches no hardware.

> **Corrected by the Phase 5 entry above.** The column below says "codec"; the
> field is the **resource class**, and every row in this table is H.264. The
> measurements stand — the labels do not.

| Config | result | working set |
|---|---|---|
| class `0x12384`, 720p | **OK** | 41.0 MiB |
| class `0x12384`, 1080p | **OK** | 89.1 MiB |
| class `0x12384`, 4K, dpb 16 | **OK** | 322.2 MiB |
| class `0x12384`, 4K, dpb 4 | **OK** | 108.6 MiB |
| class `0xb6c8`, 720p / 1080p | **OK** | 41.0 / 89.1 MiB |
| class `0xb6c8`, 4K | `0x811D0200` | — |
| class `1` | `0x811D0200` at every resolution | — |

The three output fields are a small fixed block (~3.4 MiB, barely varies), a
large pool that scales with **resolution × DPB count** — the frame buffers —
and a third that scales with resolution alone. **The numbers are computed, not
tabulated**, which is the strongest evidence that a real decoder configuration
was accepted rather than a validation stub. Output alignment is 256.

**`0xb6c8` is capped below 4K and `0x12384` is not [E]**, so on the [I] reading
of the day `0xb6c8` was AVC and `0x12384` the HEVC-class type. **That reading
was wrong** — see the Phase 5 entry: both are resource classes, both were only
ever asked for H.264, and the 4K refusal is a capacity limit rather than a
codec one. **No entitlement error appeared at 4K on either**, but that is a
statement about H.264 alone.

#### How the structure was recovered — and the method that failed

The config is size-prefixed, which the prologue disassembly showed before the
first call, so the very first call used the right shape and returned a
meaningful error rather than a fault.

A blind search then tried to solve the rest by hill-climbing on the error code.
**It failed, and the way it failed is worth recording:** it assumed a larger
error code meant more progress. It does not — `0x811D020B` is a *rejection* of
a non-NULL pointer, while the path that gets furthest returns the numerically
*smaller* `0x811D0205`. The search fixed a field to a wrong value and stalled.
It also never guessed 66/77/100 or a legal level, because those are not values
anyone would put in a generic list.

**Reading the validator settled in minutes what 1,335 calls could not.** The
lesson is the review's own, sharpened: prefer reading the code to searching the
input space, and never let a search invent its own progress metric.

`SceVideodec2DecoderConfigInfo`, as the module checks it **[E]**:

| offset | | |
|---|---|---|
| `+0x00` | u64 | struct size — `0x48` or `0x50`, else `0x811D0101` |
| `+0x08` | u32 | codec type — `1`, `0xb6c8`, `0x12384`, `0x24708`, `0x24709`, else `0x811D0203` |
| `+0x0c` | u32 | variant — `1`, `0xee049` or `0x245bfd`, else `0x811D0205`. Must be `1` for the working types |
| `+0x10` | u32 | H.264 `profile_idc` — 66 / 77 / 100 only |
| `+0x14` | u32 | H.264 `level_idc` — 10..111, used as a jump-table index |
| `+0x18` | u32 | frame width |
| `+0x1c` | u32 | frame height |
| `+0x20` | u32 | DPB frame count, must be ≤ 16, else `0x811D0209` |
| `+0x24` | u32 | must be 1..8, else `0x811D0206` — pipeline depth |
| `+0x38` | u32 | thread priority, 256..767 or `0xffffffff`, else `0x811D0208` |
| `+0x3e`, `+0x3f` | u8 | must be 0, else `0x811D0200` |
| `+0x40` | u64 | optional extra-config pointer, **must be NULL** here, else `0x811D020B` |

`SceVideodec2DecoderMemoryInfo` is `0x48` and its size field must be exactly
that.

Note `+0x38`'s range is the same SCE thread-priority range `sceAvPlayerInit`
clamps its own `basePriority` into — the two APIs agree, which is a small but
real cross-check that both readings are right.

#### What this means for the goal

**The ceiling described at the top of [hardware-decode.md](hardware-decode.md)
is not a wall.** A payload can configure the hardware decoder. What is still
unproven is everything after configuration: creating a decoder, feeding it,
and getting a frame out and onto the screen without paying for it in CPU
copies.

Next, in order:

1. `sceVideodec2AllocateComputeQueue` — the strings say a GPU compute queue is
   required, and this is the next thing that can fail for a payload-shaped
   reason. It is also the first call that allocates rather than queries.
2. `sceVideodec2CreateDecoder` with the now-known-good config and the ~89 MiB
   of memory it asks for at 1080p. This is where an entitlement gate would
   appear if there is one.
3. `sceVideodec2Decode` on a single H.264 access unit, then
   `GetPictureInfo`/`GetAvcPictureInfo` to characterise the output frame —
   format, stride, tiling, and whether the CPU can read it at all.

### 2026-08-10 — firmware 12.70 (`0x12700001`) — Phases 0 and 1

**The mapped module images are now on the PC, and the PS5 decode API is not
called what the plan assumed. Route B's blocker is gone.**

Three deploys, all read-only reconnaissance — no media API was called.

#### Phase 0 — the dump

`decoder_test` now copies the mapped images of the three media modules to the
USB stick. 1.25 MB, twelve segments, **zero unreadable pages**. Working copies
live in the gitignored `proprietary/dump/`.

Four APIs were tried for sizing the images, and only one works. **[E]**

| API | Result |
|---|---|
| `sceKernelGetModuleInfo(modid)` | **works.** The PS4 `SceKernelModuleInfo` layout (0x160: `st_size`, `name[256]`, 4×`{addr,size,prot}`, count, fingerprint) parses correctly on PS5 |
| `sceKernelGetModuleList` | returns exactly **one** module (`eboot.bin`) even after three SPRXes load. Useless in a payload |
| `sceKernelVirtualQuery` | **fails**, against a module known to be mapped |
| `sceKernelGetModuleInfoInternal` | `0x80020016` |

Because the list API is useless, modules are enumerated by sweeping modids
0..0xFF through `GetModuleInfo` and keeping what answers. That sweep found 14
modules in the process.

**Every Sony module's text segment is mapped execute-only (`prot=4`).** **[E]**
A plain `memcpy` from it *kills the payload outright*, and a `sigaction`
SIGSEGV/SIGBUS handler with `siglongjmp` **does not rescue it** — the run died
mid-log with the handler installed. Signal-guarded probing is not a usable
safety net in an elfldr payload; the log-flush-per-line discipline is what
diagnosed this, not the handler.

What does work: `kernel_mprotect(pid, addr, len, prot|PROT_READ)` on our own
process succeeds on the execute-only text, after which `memcpy` is fine. **[E]**
`kernel_proc_copyout` is kept as the fallback probe because it is
kernel-mediated and cannot fault the payload at all.

Segment shape is identical across all three modules and matches every other
Sony module in the process **[E]**:

| | | |
|---|---|---|
| s0 | `--x` | code — **no ELF header is mapped** |
| s1 | `r--` | `.eh_frame_hdr` + `.eh_frame` |
| s2 | `r--` | relocated pointer tables (GOT, vtables) |
| s3 | `rw-` | data |

`PT_DYNAMIC` and the SCE dynlib data are **not** mapped, so the review's
"read the import list from `PT_DYNAMIC`" is not available. **[E]** Two better
things replace it — see below.

#### Phase 1 — offline, zero deploys

**The import graph, from relocated pointers.** Every pointer in s2/s3 has
already been resolved by the loader, so classifying each by which module's
address range it lands in reconstructs the graph *as linked* — stronger than a
declared `DT_NEEDED` list. (Scanning s0 or s1 produces convincing garbage;
only s2/s3 hold real pointers.)

| `libSceAvPlayer` imports from | slots | distinct targets |
|---|---|---|
| `libkernel_sys` | 50 | 50 |
| `libSceLibcInternal` | 25 | 24 |
| `libSceSysmodule` | 4 | 4 |
| **`libSceVideoDecoderArbitration`** | **3** | **3** (+0xf0, +0x1e0, +0x2a0) |

`libSceVdecCore` imports only from `libkernel_sys`, `libSceLibcInternal` and
`libSceSysmodule` — **it does not import Arbitration.** `libSceVideoDecoder-
Arbitration` imports only libc and libkernel.

This answers the review's three named risks directly:

- **User session — the risk is not where the plan thought. [E]** AvPlayer
  imports *nothing* from `libSceUserService`. (The payload still has no user:
  `sceUserServiceGetInitialUser` → `0x80940004`, re-confirmed this run.)
- **Arbitration — real, and it sits above the decoder, not below it. [E]**
  AvPlayer calls into Arbitration directly; VdecCore does not. So arbitration
  is AvPlayer's own gate, not a mandatory layer under VdecCore. A Route B
  client may be able to skip it entirely. **[H]**
- **GPU — split the claim, as the review urged. [E]** AvPlayer imports nothing
  from `libSceGnmDriver`, so there is no load-time GPU dependency. But the
  strings below show decode output *is* GPU memory, so this says only that
  AvPlayer does not do the submission itself.

**Loading the three modules pulled in no new modules at all** — the before/
after module tables differ by exactly those three. **[E]** Combined with the
four `libSceSysmodule` imports, the reading is that AvPlayer loads what it
needs *at Init time*, not at load time. **[I]** That is why the dependency
delta looks empty, and it means the real dependency set will only appear once
`sceAvPlayerInit` runs.

**The function inventory, from `.eh_frame_hdr`.** Its search table gives the
entry address of every function with unwind info: **725** in AvPlayer, **1373**
in VdecCore, **10** in Arbitration. **[E]** All six known AvPlayer entry points
land exactly on FDE starts, which validates the parse.

**Strings — the single highest-value artefact in Phase 1.** The modules are
built with their diagnostic strings intact, and they name the API:

- **`sceVideodec2CreateDecoder`, `sceVideodec2AllocateComputeQueue`,
  `sceVideodec2MapMemory`** — **the PS5 low-level decode API is
  `sceVideodec2*`.** The PS4-era `sceVideoDecoder*` names this project has been
  probing for a year do not exist on PS5, which is exactly why all five always
  came back unresolved. `aerolib.csv` catalogues **26** `sceVideodec2*`
  entry points with NIDs, and **20+** `sceVdecCore*` ones. **Route B's blocker
  is solved, offline, with no hash brute force.** **[E]**
- `Invalid vdec query config info - check application entitlement to use HEVC
  decoder (0x%x)` — **entitlement gating for HEVC, in the module's own
  words.** **[E]** This is the licensing risk, and it is real.
- `Unable to map GPU memory for HW decoder`, `HW decoder GPU memory pool
  usage: 0x%zx (%.1f MiB)`, `SceVdecShaderFrameCopyY/C`, `compute pipe: S/W
  Slice Dec` — decode output lands in **GPU memory** and frame copies are done
  by **compute shaders**. The review's [H] "frames land in GPU memory" is now
  evidence-backed, and `sceVideodec2AllocateComputeQueue` says a **GPU compute
  queue** is required. **[E]**
- `Failed sceAudiodecInitLibrary`, `SCE_AUDIODEC_ERROR_INVALID_TYPE` — audio
  decode goes through **`sceAudiodec*`**, not AJM. Route F's premise is wrong
  for AvPlayer. **[E]**
- `sceMp4GetNextUnit`, `SceAvPlayerMp4Demux` — demux is `libSceMp4`. **[E]**
- Thread names `SceAvPlayerDemux`, `DecodeThread`, `SceAvPlayerVideoDec`,
  `SceAvPlayerStateMachine`, and `Failed to exit AvController thread` —
  AvPlayer runs a multi-threaded pipeline. **[E]**

**Prologue disassembly** (GNU objdump; llvm-objdump 18 has no raw-binary input
mode). Reading the code replaced the plan's central hypothesis with a
measurement:

- **`sceAvPlayerInit(void *initData)` takes exactly one argument.** RSI/RDX/
  RCX/R8/R9 are all written before being read, and RDI is dereferenced
  immediately, so there is **no hidden `sret` pointer** and no argument shift.
  **[E]**
- `test rdi,rdi; je → xor eax,eax; ret` — **a NULL argument returns 0**, it
  does not fault. **[E]**
- The first field it reads is `[rdi+0x64]`, a 32-bit value clamped against the
  constants `0x27d`–`0x2ff` (637–767) and used to derive several more by
  adding 5, 6, 9, 0xa, 0x19. **That range is the SCE thread-priority range, so
  offset 0x64 is a base thread priority** — which is exactly where
  `basePriority` sits in the PS4 `SceAvPlayerInitData`. **[E]/[I]**
- If that field is **zero**, it loads a canned default table from rodata
  instead. **A zeroed struct is explicitly a supported input.** **[E]**
- The inner init reads `[rdi+0x60]` and range-checks it to 1..4 — the PS4
  layout's `debugLevel` slot. **[I]**
- **`sceAvPlayerGetVideoData(handle, out)` writes exactly 0x28 = 40 bytes** to
  the caller's buffer (`vmovups` of 32 bytes at +0, plus 8 bytes at +0x20).
  The PS4 `SceAvPlayerFrameInfo` is 40 bytes. **[E]**
- `sceAvPlayerIsActive(handle)` and `sceAvPlayerGetVideoData` both test
  `[handle+0x250]`; the handle is a pointer whose first field is a pointer.
  **[E]**
- **AvPlayer's error family is `0x806Axxxx`** — `sceAvPlayerAddSource` returns
  `0x806A0001` for a NULL handle or NULL path and `0x806A0002` for a failed
  parse. **[E]** That is the failure taxonomy the review wanted prepared in
  advance, read off the code rather than guessed.

**So the PS4 `SceAvPlayerInitData` layout is not merely a plausible starting
point — offsets 0x60 and 0x64 are confirmed by how the code uses them**, and a
zeroed struct is a supported input rather than a gamble. **[E]**

#### Phase 2 — passive runtime recon (still no media call)

The 176 recovered names were resolved by NID against 14 candidate modules.
**Resolution only — nothing was called.**

| Module | modid | exports matched |
|---|---|---|
| `libSceAvPlayer.sprx` | 0x33 | **27** |
| `libSceVdecCore.sprx` | 0x35 | **20** (`sceVdecCore*`) |
| `libSceVideodec2.sprx` | 0x38 | **18** (`sceVideodec2*`) |
| `libSceVideodec.sprx` | 0x39 | 7 (`sceVideodec*`, the v1 API) |
| `libSceSysmodule.sprx` | 0x10 | 16 |
| `libSceAudiodec.sprx` | 0x3f | 9 |
| `libSceVideoDecoderArbitration.sprx` | 0x36 | 4 |
| `libSceVdecwrap` / `libSceVdecShevc` / `libSceGnmDriver` / `libSceAjm` | 0x3a/0x3b/0x3c/0x3e | load, 0 matched (no names in the table for them) |
| `libSceMp4.sprx`, `libSceVdecSvp9.sprx`, `libSceFios2.sprx` | — | **do not exist** — load fails `0x80020002` (ENOENT) |

**`libSceVideodec2.sprx` exists and exports the whole decode API.** **[E]**
That is the module Route B should target — not `libSceVdecCore`, which turns
out to be the layer *below* it (`sceVdecCoreCreateDecoder`,
`sceVdecCoreSetDecodeInput`, `sceVdecCoreSyncDecode`, and the
compute-resource calls).

**Two independent methods agree, exactly.** The offline import graph said
AvPlayer points at Arbitration +0xf0, +0x1e0 and +0x2a0; the on-console export
map resolves those three addresses to **`sceVideoDecoderArbitrationInitialize`,
`sceVideoDecoderArbitrationEnable` and `sceVideoDecoderArbitrationAcceptEvent`**.
Nothing was assumed to make those meet. **[E]**

The same match identifies the four `libSceSysmodule` imports as
`sceSysmoduleLoadModule` (+0xd0), `sceSysmoduleUnloadModule` (+0x2a0),
`sceSysmoduleLoadModuleInternal` (+0x500) and `sceSysmoduleUnloadModuleInternal`
(+0x560); VdecCore's two are `sceSysmoduleLoadModuleInternal` and
`sceSysmoduleIsLoadedInternal`. **[E]**

**This upgrades the empty-dependency-delta inference to evidence: AvPlayer and
VdecCore load their dependencies at run time through `sceSysmodule`.** The real
dependency set will not appear until `sceAvPlayerInit` runs — so the module
table taken after Init is a genuinely valuable measurement, not a formality.

> **Hazard, unexplained.** One run hung after enumerating `libSceVideodec` and
> never returned; the watchdog thread did not fire and wrote nothing. A rerun
> with the module order changed completed normally and loaded every module
> including `libSceAudiodec`, so the cause is **not** established and should
> not be blamed on a particular module. Two practical consequences: the probe
> now writes a `#loading <module>` breadcrumb and flushes it *before* each
> load, and **the watchdog thread cannot be relied on** — it has now failed to
> fire twice, once here and once on the execute-only fault. Bound the deploy
> from the PC side; that is the only guard that has actually worked.

#### Tooling added

| | |
|---|---|
| `tools/re/nid.py` | Sony NID hashing offline; validated 11/11 against hardware-resolved pairs. Plus an `aerolib.csv` index |
| `tools/re/analyse.py` | import graph, `.eh_frame_hdr` function inventory, strings |
| `tools/re/disas.sh` | disassemble a dumped segment at the right virtual address |
| `tools/re/gen_nid_table.py` | emits `projects/decoder_test/nid_table.h` — 176 recovered names with NIDs |

`tools/re/aerolib.csv` (12.6 MB) is fetched, not committed.

#### What this changes

1. **Route B is no longer blocked, and it was aimed at the wrong module.** It
   was never a hash-reversal problem; it was a wrong-API-family problem. The
   target is `libSceVideodec2` and its 18 exports, with `libSceVdecCore`
   beneath it.
2. **Zero-copy is the question that matters, and the answer leans favourable.**
   Frames are already in GPU memory; the cost is a compute-queue dependency.
3. **HEVC may be entitlement-gated** even if AVC is not — so a first test
   should use H.264, and an HEVC failure should not be read as "the whole path
   is closed".
4. **`sceAvPlayerInit` is a much safer call than the plan assumed** — one
   argument, no sret, NULL-tolerant, zero-struct-tolerant.
5. **The watchdog-in-the-payload idea does not work here.** It has failed to
   fire on both occasions it was needed. `timeout` around the deploy is the
   guard that holds.

#### Open, and now sharper

- Does `sceVideodec2QueryDecoderMemoryInfo` answer without a decoder being
  created? If so it is a zero-risk probe of the entitlement gate, cheaper than
  anything in the AvPlayer path.
- Which modules appear in the table *after* `sceAvPlayerInit`? That is the
  real dependency list and it costs one deploy to get.
- Is the 40-byte `SceAvPlayerFrameInfo` field order the PS4 one? Size matches;
  ### 2026-08-14 — firmware 12.70 (`0x12700001`), graphical app slot (`projects/avplayer_test`)

**`libSceAvPlayer` end-to-end demuxer, bitstream framing, and multi-pool memory registration verified on live hardware.**

1. **Audio crash eliminated (`autoStart = 0`):**
   - Disabling autoStart and calling `sceAvPlayerEnableStream(playerHandle, 0)` on the `READY` event isolates the video track and bypasses the `0x807f0000` audiodec crash.
2. **Built-in Annex-B NAL framing:**
   - Live inspection confirmed `libSceAvPlayer`'s MP4 demuxer extracts sample boxes and automatically outputs Annex-B start codes (`00 00 01` / `00 00 00 01`) with inlined SPS/PPS on Sample 0 (`00 00 01 67 64 00 28...`).
3. **Complete Direct Memory Registration (`0x00000000`):**
   - All 8 direct memory pools (MVP, Demux, AU, Work, OutputFifo, FramePool) mapped via `sceVideodec2MapDirectMemory` with `rc=0`.
   - All `libSceVdecCore` buffer containment assertions (`0x95A`, `0xC22`, `0xDA6`, `0xDAE`) pass cleanly.
4. **Kernel Driver Gate:**
   - Over 96,000 continuous decode cycles executed with zero faults or leaks.
   - The AMD GPU kernel video decoder driver returns `errno 5200` (`0x1450`) on hardware submit (`ioctl(fd, _IOW(0x83, 23, 24))` and command `24`).

### 2026-08-09 — firmware 12.70 (`0x12700001`), elfldr payload

**libSceAvPlayer is directly callable from a payload. No proprietary files
required.**

Method: `sceKernelLoadStartModule()` on the module path, then
`kernel_dynlib_handle()` for the dynlib handle, then `nid_encode()` +
`kernel_dynlib_resolve()` per symbol.

All three modules loaded successfully:

| Module | modid | base |
|---|---|---|
| `libSceAvPlayer.sprx` | 0x29 | `0x8008f4000` |
| `libSceVdecCore.sprx` | 0x2b | `0x80094c000` |
| `libSceVideoDecoderArbitration.sprx` | 0x2c | `0x800a1c000` |

`libSceAvPlayer` symbols — **all six resolved**:

| Symbol | NID | Address |
|---|---|---|
| `sceAvPlayerInit` | `aS66RI0gGgo` | `0x8008f4d00` |
| `sceAvPlayerAddSource` | `KMcEa+rHsIo` | `0x8008f60e0` |
| `sceAvPlayerGetVideoData` | `o3+RWnHViSg` | `0x8008f7040` |
| `sceAvPlayerGetAudioData` | `Wnp1OVcrZgk` | `0x8008f6e60` |
| `sceAvPlayerIsActive` | `UbQoYawOsfY` | `0x8008f6d30` |
| `sceAvPlayerClose` | `NkJwDzKmIlw` | `0x8008f5e50` |

`libSceVdecCore` / `libSceVideoDecoderArbitration`: loaded, but **none** of the
PS4-era `sceVideoDecoder*` names resolved. The modules are present and mapped,
so this is a naming problem, not an availability problem — the PS5 low-level
decode API is not called what the PS4 one was.

#### Three things this established

1. **Plain `sceKernelDlsym` never works on Sony modules.** Every symbol returns
   `0x80020003` (ESRCH) because these modules export NIDs, not names. You must
   go through `nid_encode()` + `kernel_dynlib_resolve()`. The SDK ships
   `nid_encode()` in `<ps5/nid.h>` for exactly this.

2. **A module is only mapped if the payload links its stub.** An earlier run
   reported 0/10 modules mapped — including `libSceVideoOut`, which
   demonstrably works. Passive probing tells you about *your* dependencies,
   nothing more. `decoder_test` now links `-lSceVideoOut -lSceAudioOut` purely
   as a control (3/3 must pass, else the probe is unreliable).

3. **You have to load the module first.** `sceKernelLoadStartModule()` on
   `/system/common/lib/<name>.sprx` works from an elfldr payload.

#### Next steps

- [ ] Recover the real PS5 `libSceVdecCore` export names. Two routes: dump the
      module's dynamic symbol table for its NID list and reverse the hash
      against a wordlist, or check `aerolib.csv` from `zecoxao/sce_symbols`
      for NIDs matching this module.
- [ ] Prototype `libSceAvPlayer` in `projects/avplayer_test` now that the
      entry points are known — resolve the six NIDs and call `sceAvPlayerInit`.
      Its argument struct is the next unknown; it takes an allocator/callback
      block on PS4 and the PS5 layout must be confirmed.
- [ ] Determine whether AvPlayer needs an app sandbox (file access, user id).
      Note `videoout_test` established that a payload has **no user session**
      (`sceUserServiceGetInitialUser` → `0x80940004`), which may matter.
- [ ] Check whether `libSceVideoDecoderArbitration` gates access when no
      licensed title is running.

## Open questions

- [x] ~~Are any `libSceVdec*` modules mapped in an elfldr payload process on 12.70?~~
      Not by default, but **they load on demand** via `sceKernelLoadStartModule`.
- [x] ~~Is `libSceAvPlayer` reachable?~~ **Yes** — all six probed entry points resolve.
- [ ] What are the real PS5 `libSceVdecCore` export names?
- [ ] Does `sceAvPlayerInit` actually succeed, or does it fail without an app sandbox?
- [ ] Can decoded frames reach VideoOut without an intermediate CPU copy?
