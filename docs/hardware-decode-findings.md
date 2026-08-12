# PS5 media stack — findings

Everything established about the PS5's native media modules on firmware
**12.70 (`0x12700001`)**, as of **2026-08-11**, through Phase 6.

**Phase 6 has not produced a picture yet.** What it *has* produced: `Decode`'s
ABI confirmed 18/18, the frame-size formula confirmed 7/7, and
`sceVideodec2MapDirectMemory` fully characterised — four successive gates found
and cleared, each diagnosed from the module's own diagnostic lines without a
single exploratory sweep. `Decode` now reaches the hardware submit and the
driver refuses the job with ioctl errno **5200**. See §7.

**The 2026-08-11 session closed four hypotheses and opened one.** In order of
how much they cost:

- **The arbitration hang is solved and arbitration is ruled out.** `+0x350` is
  a **PLT stub**, not a function body; its GOT slot was unbound. The missing
  provider is `libSceVideoArbitration.sprx` — a *different module* from
  `libSceVideoDecoderArbitration.sprx`. Load it first and Initialize returns.
  It changes nothing about the refusal. §7.
- **The command buffer is not a dead end and it is well formed.** This document
  previously said the trail ended at a kernel-side consumer. It does not: the
  object is in *our own* work memory and can simply be read. Width, height,
  mode and real physical addresses are all correct. §7.
- **Annex-B is the right framing.** AVCC is rejected *earlier*, in software.
- **A payload cannot open video out, and trying KERNEL-PANICKED the console.**
  §9. This is the expensive lesson of the session and the one most worth not
  repeating.
- **Open, and the next phase: the kernel is directly readable**, so errno 5200
  can be read rather than guessed at. §13.

This is the *reference*. It answers "what is true and how do we know", so that
nothing here has to be re-derived on a console.

- [hardware-decode-next-steps.md](hardware-decode-next-steps.md) — **what to do
  next**, as phases 4–10, with the go/no-go points
- [hardware-decode.md](hardware-decode.md) — the original plan and the console
  workflow rules
- [hardware-decode-review.md](hardware-decode-review.md) — the critique that
  set the phase order this work followed
- [native-media-research.md](native-media-research.md) — the chronological
  results log, newest first

Raw evidence lives in a **separate repository, `PS5-Research`** — console logs,
deploy transcripts, disassembly of every function cited here, extracted
strings, the import graph, and offline psdevwiki mirrors. It was extracted from
this checkout's gitignored `research-logs/` on 2026-08-11, so every
`research-logs/...` path cited below is now relative to that repository's root.
Module images stay here, in `proprietary/dump/` (gitignored), and were
deliberately never copied into it.

## Confidence tags

| Tag | Means |
|---|---|
| **[E]** | Observed on this console, on 12.70, and logged in the `PS5-Research` repository (§11) |
| **[I]** | Inference from evidence plus how x86-64/SysV/ELF demonstrably work |
| **[H]** | Hypothesis. Plausible, untested, could be wrong |

---

## 1. The headline

**A payload can reach and configure the PS5 hardware video decoder.** **[E]**

`sceVideodec2QueryDecoderMemoryInfo` returns `0` and real, resolution-scaled
memory requirements. No user session, no licensed title, no entitlement error.

**And a payload can obtain a GPU compute queue.** **[E]** Phase 4:
`sceVideodec2AllocateComputeQueue` returns `0` with a live handle, and
`ReleaseComputeQueue` gives it back cleanly. That was the phase's stated
single most valuable outcome, and the risk register's most likely hard
blocker. **It is not a blocker.**

It requires `libSceGnmDriver.sprx` to be loaded first — the queue bottoms out
in `sceGnmMapComputeQueue`, and without that module the call hangs rather than
failing. That cost one deploy and is now a one-line requirement.

**And the memory is there, provided the code runs in the right launch slot.**
**[E]** Under `elfldr` the direct-memory ceiling is between 41 and 64 MiB —
1080p decode needs 89 MiB and cannot get it. **In the PS Now app slot, every
size up to and including the 322 MiB 4K working set allocates.** Same binary,
same console, same ladder. The ceiling is a property of the host process, not
of the console or of homebrew.

This is not a new thing to build: EVO Player already runs in that slot. It does
mean **research probes must be launched with `install-homebrew.sh --run`, not
`deploy.sh`**, or they measure the wrong process.

**And a payload creates a decoder — for H.264 and for HEVC.** **[E]** Phase 5,
2026-08-11: `sceVideodec2CreateDecoder` returns `0` with a live decoder object,
and `DeleteDecoder` gives it back cleanly. **No entitlement gate appeared
anywhere** — not at query time, not at create time, and not for HEVC, which
this phase was the first to ask for correctly. VP9 is the one codec refused,
and `libSceVdecSvp9.sprx` does not exist on this firmware. See §7.

What remains unproven is everything after: feeding it, and getting a frame back
without paying for it in CPU copies.

**Feeding it has been tried once and refused, for a named reason.** **[E]**
2026-08-11, `projects/decodeframe_test/`. `sceVideodec2Decode`'s four-argument
ABI is confirmed by 18/18 exact validation controls, and the `mapMemorySize`
formula by 7/7 exact predictions — `mapMemorySize` is exactly one **NV12/P010
frame at a 256-aligned stride**, which answers most of Phase 7. But the decode
returned `0x811D0111`: **`sceVdecCoreMapMemoryBlock` must be called before the
decoder will accept input**, and the probe had skipped it on the strength of an
offline reading that only covered the top layer. `sceVideodec2MapDirectMemory`
is the bound public route to it. That is the next thing to try, and it is a
one-call change.

---

## 2. What works and what does not, inside a payload

The single most expensive thing in this work was discovering which system APIs
are simply broken in a payload process. **[E]** for every row.

| API | Result |
|---|---|
| `sceKernelLoadStartModule` | **works** on `/system/common/lib/*.sprx` |
| `sceKernelGetModuleInfo(modid)` | **works**, PS4 `SceKernelModuleInfo` layout is correct |
| `sceKernelGetModuleList` | **useless** — reports one module (`eboot.bin`) even after three SPRXes have loaded successfully |
| `kernel_get_proc` + `kernel_copyout` | **work cross-process.** `p_comm` sits at **proc+0x5e4** on 12.70, validated by reading our own (`payload.elf`). 87 live processes on an idle console |
| `kernel_dynlib_handle(pid, ...)` | **works for other processes**, so module sets are inspectable console-wide |
| `sceKernelVirtualQuery` | **fails**, against a module known to be mapped |
| `sceKernelGetModuleInfoInternal` | `0x80020016` |
| `sceKernelDlsym` by name | **always fails** `0x80020003` (ESRCH) — Sony modules export NIDs, not names |
| `kernel_dynlib_handle` / `_mapbase_addr` / `_entry_addr` / `_init_addr` / `_fini_addr` | **work** |
| `kernel_dynlib_resolve` by NID | **works** — this is the only symbol lookup that does |
| `kernel_mprotect` | **works**, including adding `PROT_READ` to execute-only system text |
| `kernel_proc_copyout` | **works**, and cannot fault the caller |
| `sceUserServiceGetInitialUser` | `0x80940004` under `deploy.sh`, `0x80960006` in the app slot. **Not the right question** — see the row below |
| `sceUserServiceInitialize` + `GetLoginUserIdList` | **work in the app slot**, returning `0` and a real user id. **[E]** 2026-08-11. The long-standing "a payload has no user session" entry was measured by `decoder_test` under `deploy.sh`, i.e. in `SceSpZeroConf` — the same wrong-host-process error as the 41 MiB memory ceiling. **There IS a session where the player runs.** `GetInitialUser` still fails there; `GetLoginUserIdList` is the call that works |
| `sceVideoOutOpen` | **REFUSED, and trying it PANICKED THE KERNEL.** `0x80290001` for the real logged-in user; the `0xFF` system-user retry returns `0x4E100000`, which is not a handle. Allocating a compute queue afterwards panicked the console. See §9.1. **Do not call this from a payload** |
| `kernel_copyout` from `KERNEL_ADDRESS_TEXT_BASE` | **available and read-only.** The SDK resolves the kernel text base for 12.70, so kernel code can be dumped and disassembled offline. No firmware decryption needed — §13 |
| `kernel_get_proc_file(pid, fd)` | **available** — gives the kernel `struct file *` for an fd, e.g. the decoder's fd 16 |
| `kernel_get_ucred_authid` / `_caps` | **available**, read and write. `prospero_media_standalone/core/pt.c` already elevates to `0x4800000000010003` + all-`0xFF` caps and restores. Writing is a kernel write — §13 |
| `sigaction(SIGSEGV)` + `siglongjmp` | **does not rescue a fault** — the process dies anyway |
| detached `pthread` watchdog | **never fires** — failed on both occasions it was needed |
| `fopen`/`fwrite` to `/mnt/usb0` | works; retrieve over websrv `/fs`, which is read-only |
| payload `printf` | comes back on the elfldr deploy socket, but only the tail survives |

**Consequences for how to write a probe:**

- Enumerate modules by sweeping modids 0..0xFF through `sceKernelGetModuleInfo`.
  Nothing else enumerates them.
- Never speculatively dereference module memory. Decide from the segment's
  declared `prot`, and use `kernel_proc_copyout` as the probe.
- Do not rely on an in-payload watchdog. Bound the deploy with `timeout` on the
  PC side — that is the only guard that has ever worked here.
- **Flush the log after every line.** Every hang in this work was diagnosed
  purely from where the log stopped. It is the highest-value habit in the set.
- **Write the log to `/mnt/usb0` as well, and read it before drawing any
  conclusion.** The deploy socket is not reliable: a run that completed
  successfully still exited 124 with its tail missing, and looked identical to
  the hang it had just fixed.
- **Load every module in the call chain, not just the one you are calling.** An
  unresolved lazy import blocks silently and indefinitely — it neither faults
  nor returns an error.

---

## 3. Process and module layout

Fourteen modules are mapped in a payload after the media modules load. **[E]**

Every Sony module has exactly **four segments**, always in this shape:

| | prot | contents |
|---|---|---|
| s0 | `--x` | code. **Execute-only.** No ELF header is mapped |
| s1 | `r--` | `.eh_frame_hdr` + `.eh_frame` |
| s2 | `r--` | relocated pointer tables — GOT, vtables, `.data.rel.ro` |
| s3 | `rw-` | data |

Sizes for the media modules (1080p run):

| Module | text | total |
|---|---|---|
| `libSceAvPlayer.sprx` | 0x40000 | ~360 KB |
| `libSceVdecCore.sprx` | 0x80000 | ~832 KB |
| `libSceVideodec2.sprx` | 0x8000 | ~80 KB |
| `libSceVideoDecoderArbitration.sprx` | 0x4000 | ~64 KB |

**Base addresses move between boots; offsets do not.** **[E]** `sceAvPlayerInit`
was at `+0xd00` on both 2026-08-09 and 2026-08-10 with different bases. Resolve
at run time, and record offsets rather than addresses.

**`PT_DYNAMIC` and the SCE dynlib data are not mapped.** **[E]** The import and
export NID tables are therefore *not* in a dump. Two things replace them, both
better than they sound — see §5 and §6.

### Present, absent, and hazardous

| Module | Status |
|---|---|
| `libSceAvPlayer`, `libSceVdecCore`, `libSceVideodec2`, `libSceVideodec`, `libSceVideoDecoderArbitration`, `libSceVdecwrap`, `libSceVdecShevc`, `libSceGnmDriver`, `libSceAjm`, `libSceSysmodule` | load cleanly **[E]** |
| `libSceAudiodec` | loads, **but hangs the payload** if loaded straight after the video modules. Reproduced twice. Loads fine once GnmDriver and Ajm are up **[E]** |
| `libSceMp4`, `libSceVdecSvp9`, `libSceFios2` | **do not exist** — `0x80020002` (ENOENT) **[E]** |

`libSceMp4` being absent while `libSceAvPlayer` contains the strings
`sceMp4GetNextUnit` and `SceAvPlayerMp4Demux` suggests the MP4 demuxer is
**statically linked into AvPlayer** rather than being a separate module. **[H]**

---

## 4. Architecture, corrected

The review's reconstructed diagram was largely right in shape and wrong in
several names. This is what the evidence supports:

```
   file
     │
     ▼
┌──────────────────────────────────────────┐
│ libSceAvPlayer                           │
│   demux (MP4 appears to be built in [H]) │
│   A/V sync, state machine                │
│   threads: SceAvPlayerDemux, DecodeThread,
│            SceAvPlayerVideoDec,          │
│            SceAvPlayerStateMachine  [E]  │
└───┬───────────────────┬──────────────┬───┘
    │ imports [E]       │ loads at run │ audio
    ▼                   │ time via     ▼
┌───────────────────┐   │ sceSysmodule ┌──────────────┐
│ libSceVideoDecoder│   │  [E]         │ libSceAudiodec│ [E] not AJM
│ Arbitration       │   │              └──────────────┘
│  Initialize       │   ▼
│  Enable           │ ┌────────────────────────────────┐
│  AcceptEvent  [E] │ │ libSceVideodec2                │
└───────────────────┘ │   the public decode API        │
                      │   Query/Create/Decode/Flush    │
                      │   AllocateComputeQueue    [E]  │
                      └───────────────┬────────────────┘
                                      ▼
                      ┌────────────────────────────────┐
                      │ libSceVdecCore                 │
                      │   sceVdecCore*, "GpDec"        │
                      │   compute-shader frame copy    │
                      │   SceVdecShaderFrameCopyY/C [E]│
                      └───────────────┬────────────────┘
                                      ▼
                        decoded frames in GPU memory [E]
                                      │
                                      ▼
                      ┌────────────────────────────────┐
                      │ libSceVideoOut                 │ [E] already used
                      └────────────────────────────────┘
```

Corrections worth stating explicitly, because the earlier documents assert
otherwise:

1. **The public decode API is `libSceVideodec2`, not `libSceVdecCore`.** [E]
   VdecCore is the layer beneath it.
2. **The PS4-era `sceVideoDecoder*` names do not exist on PS5.** [E] Probing
   for them is why this looked blocked for a year.
3. **Audio decode is `sceAudiodec*`, not AJM.** [E]
4. **Arbitration sits above the decoder, not below it.** [E] AvPlayer calls it;
   VdecCore does not import it. A Route B client may be able to skip it. [H]
5. **AvPlayer does not import `libSceUserService`, `libSceVideoOut`,
   `libSceAudioOut` or `libSceGnmDriver`.** [E] It hands you buffers; it does
   not own the display, audio or GPU submission.

---

## 5. The import graph

`PT_DYNAMIC` is not mapped, so the imports were recovered from the **relocated
pointer tables** instead. Every pointer in s2/s3 has already been resolved by
the loader, so classifying each by which module's address range it lands in
gives the graph *as linked* — stronger evidence than a declared `DT_NEEDED`
list. Scanning s0 or s1 produces convincing garbage; only s2/s3 hold real
pointers.

| Importer | Imports from | slots | distinct |
|---|---|---|---|
| `libSceAvPlayer` | `libkernel_sys` | 50 | 50 |
| | `libSceLibcInternal` | 25 | 24 |
| | `libSceSysmodule` | 4 | 4 |
| | **`libSceVideoDecoderArbitration`** | **3** | **3** |
| `libSceVdecCore` | `libkernel_sys` | 53 | 47 |
| | `libSceLibcInternal` | 48 | 24 |
| | `libSceSysmodule` | 2 | 2 |
| `libSceVideoDecoderArbitration` | `libSceLibcInternal` | 5 | 4 |
| | `libkernel_sys` | 3 | 3 |

The small dependencies were then **named** by resolving the export map on the
console and matching addresses. Two independent methods, agreeing exactly:

| Importer → target | Function |
|---|---|
| AvPlayer → Arbitration `+0xf0` | `sceVideoDecoderArbitrationInitialize` |
| AvPlayer → Arbitration `+0x1e0` | `sceVideoDecoderArbitrationEnable` |
| AvPlayer → Arbitration `+0x2a0` | `sceVideoDecoderArbitrationAcceptEvent` |
| AvPlayer → Sysmodule `+0xd0` | `sceSysmoduleLoadModule` |
| AvPlayer → Sysmodule `+0x2a0` | `sceSysmoduleUnloadModule` |
| AvPlayer → Sysmodule `+0x500` | `sceSysmoduleLoadModuleInternal` |
| AvPlayer → Sysmodule `+0x560` | `sceSysmoduleUnloadModuleInternal` |
| VdecCore → Sysmodule `+0x500` | `sceSysmoduleLoadModuleInternal` |
| VdecCore → Sysmodule `+0x5d0` | `sceSysmoduleIsLoadedInternal` |

**Loading the media modules pulls in no new modules at all.** **[E]** Combined
with those `sceSysmodule` imports, the reading is that **AvPlayer and VdecCore
load their real dependencies at `Init` time, not at load time.** **[I]** So the
module table taken *after* a successful `sceAvPlayerInit` is a genuinely
valuable measurement — it is the only way to see the true dependency set.

---

## 6. Recovering names without a symbol table

Three techniques, in the order they should be tried. All offline.

**1. Strings.** The modules ship with their diagnostic strings intact, and they
name the API directly. This is what produced `sceVideodec2CreateDecoder`,
`sceVideodec2AllocateComputeQueue`, `sceVideodec2MapMemory`,
`sceAudiodecInitLibrary`, `sceMp4GetNextUnit` and
`sceVideoDecoderArbitrationEnableSuspendMode`. **[E]** Cheapest and highest
yield by a wide margin.

**2. `aerolib.csv`.** The community NID catalogue
([zecoxao/sce_symbols](https://github.com/zecoxao/sce_symbols), 12.6 MB) maps
NID → name for ~450k symbols. Once strings gave the family name, the catalogue
gave every member with its NID. **A lookup is seconds; never invert a hash you
can forward-compute.**

**3. Forward-hash candidates.** `tools/re/nid.py` implements Sony's NID hash
(SHA-1 of `name + 16-byte suffix`, first 8 bytes little-endian, base64 with a
`+-` alphabet), **validated 11/11 against hardware-resolved pairs**. Hash
candidate names and intersect. This was built but never needed — 1 and 2 were
enough.

**4. `.eh_frame_hdr`.** Its binary-search table holds the entry address of
every function with unwind info: **725** in AvPlayer, **1373** in VdecCore,
**10** in Arbitration. **[E]** A complete function inventory without a symbol
table. All six originally-known AvPlayer entry points land exactly on FDE
starts, which validates the parse.

### Export counts, measured on the console

| Module | exports matched |
|---|---|
| `libSceAvPlayer` | 27 |
| `libSceVdecCore` | 20 (`sceVdecCore*`) |
| `libSceVideodec2` | 18 (`sceVideodec2*`) |
| `libSceSysmodule` | 16 |
| `libSceAudiodec` | 9 |
| `libSceVideodec` | 7 (the v1 API) |
| `libSceVideoDecoderArbitration` | 4 |

Full lists with offsets: `research-logs/console/evo_dump_exports.txt`.
`libSceVideodec2`'s 18 include `CreateDecoder`, `CreateDecoderBid`,
`CreateHevcDecoder`, `Decode`, `Flush`, `Reset`, `DeleteDecoder`, `MapMemory`,
`MapDirectMemory`, `AllocateComputeQueue`, `ReleaseComputeQueue`,
`QueryComputeMemoryInfo`, `QueryDecoderMemoryInfo`,
`QueryHevcDecoderMemoryInfo`, `GetPictureInfo`, `GetAvcPictureInfo`,
`GetHevcPictureInfo`, `GetVp9PictureInfo`.

**Four of those 18 are aliases, in three groups.** **[E]** An export map is a
list of entry points, not a list of behaviours.

| entry points | body | what actually selects the behaviour |
|---|---|---|
| `CreateDecoder` (+0xba0) and `CreateHevcDecoder` (+0x1230) | +0xbb0 | byte-for-byte identical thunks, `mov ecx,0x1 ; jmp`. The codec comes from `cfg+0x0c` |
| `QueryDecoderMemoryInfo` (+0xa10) and `QueryHevcDecoderMemoryInfo` (+0xb90) | +0xa20 | both bare `jmp`. Same |
| `GetPictureInfo` (+0x2120) and **`GetAvcPictureInfo` (+0x30c0)** | +0x2130 | byte-for-byte identical, `xor ecx,ecx ; jmp`. The body dispatches on `outputInfo->codecType`, which the decoder wrote |
| **`GetHevcPictureInfo` (+0x30e0)** and **`GetVp9PictureInfo` (+0x30d0)** | +0x2130 | byte-for-byte identical to each other, `xor edx,edx ; xor ecx,ecx ; jmp`. They differ from the pair above only in forcing the second picture-info pointer to NULL |

So there is **one** picture-info body serving four names, and **nothing**
HEVC- or VP9-specific about any of them. Calling `GetHevcPictureInfo` on an
H.264 picture returns H.264 picture info in an H.264-shaped struct; the only
thing the name buys you is losing the ability to receive a second picture.

---

## 7. ABI

### `sceVideodec2QueryDecoderMemoryInfo(cfg, memInfo)` — **called, returns 0**

Both arguments are pointers to **size-prefixed** structures, which is what made
the very first call correct rather than a guess.

`SceVideodec2DecoderConfigInfo` — every row read from the module's own
validation code at `+0x3eb0` **[E]**; the names are the reading that fits the
ranges **[I]**:

| offset | type | meaning | rejection |
|---|---|---|---|
| `+0x00` | u64 | struct size — `0x48` or `0x50` | `0x811D0101` |
| `+0x08` | u32 | **resource class** — `1`, `0xb6c8`, `0x12384`, `0x24708`, `0x24709` | `0x811D0203` |
| `+0x0c` | u32 | **codec** — `1` H.264, `0xee049` HEVC, `0x245bfd` VP9 | `0x811D0204` |
| `+0x10` | u32 | profile, **codec-specific** — see below | `0x811D0205` |
| `+0x14` | u32 | level, **codec-specific** — see below | `0x811D0205` |
| `+0x18` | u32 | frame width **[I]** — the pair at `+0x18`/`+0x1c` is dimensions **[E]**; which is which is convention | |
| `+0x1c` | u32 | frame height **[I]** | |
| `+0x20` | u32 | DPB frame count, **1..16** | `0x811D0209` |
| `+0x24` | u32 | pipeline depth **1..8** **[I]**; ≥ 6 is silently clamped to 5 on the working classes | `0x811D0206` |
| `+0x28` | u64 | **compute-queue handle** from `AllocateComputeQueue` **[I]** — unread by the query, forwarded to `sceVdecCoreCreateDecoder` and stored at decoder`+0x128` | |
| `+0x30` | u64 | CPU affinity mask **[I]** — only range-checked when the resource class is `1` | `0x811D0207` |
| `+0x38` | u32 | thread priority — **256..767**, or `0xffffffff` for default | `0x811D0208` |
| `+0x3c` | u8 | flag, inverted into the output | |
| `+0x3d` | u8 | **`checkMemoryType`** — selects the memory checker's mode, see below | |
| `+0x3e`, `+0x3f` | u8 | must be 0 | `0x811D0200` |
| `+0x40` | u64 | optional extra-config pointer, **must be NULL** on the H.264 path | `0x811D020B` |

`+0x38`'s range is the same SCE thread-priority range `sceAvPlayerInit` clamps
its own `basePriority` into — two independently-read functions agreeing.

#### `+0x08` and `+0x0c` were recorded the wrong way round **[E]**

This document previously called `+0x08` the codec type and `+0x0c` a variant
that "must be 1". The validator says otherwise, and the evidence is the
profile and level checking that follows, which is codec-specific:

| `+0x0c` | profile accepted at `+0x10` | level accepted at `+0x14` | reading |
|---|---|---|---|
| `1` | 66, 77, 100 | 10..111, via a jump table | H.264 `profile_idc` / `level_idc` |
| `0xee049` | 1, 2 | {30, 63, 90, 93} ∪ 120..186 | HEVC Main / Main10, `general_level_idc` = level × 30 |
| `0x245bfd` | 0, 2 | {10,11,20,21,30,31,40,41,50,51,52,60,61,62} | VP9 profile 0 / 2, level × 10 |
| anything else | | | `0x811D0204` |

`+0x08` instead maps to a small resource class that `libSceVdecCore` uses to
decide which buffers it requires: `1` → 2 or 9, `0xb6c8` → 4, `0x12384` → 8,
`0x24708` → 0xa, `0x24709` → 0xb. `sceVdecCoreCreateDecoder` rejects any class
above 8 outright, which is why `0xb6c8` and `0x12384` are the only two values
that have ever worked.

**Consequence: HEVC has never been validly queried.** All six configurations
§7's table records as accepted used `+0x0c = 1`, i.e. H.264. The
`0xb6c8 v=ee049 → 0x811D0205` lines in the Phase 3 log are not the module
refusing HEVC — they are the probe offering HEVC a profile of 100 and a level
of 51, which are H.264 numbers, against an error code that means *unsupported
profile or level*. `0xb6c8` is not "the AVC type" and `0x12384` is not "the
HEVC-class one"; they are a small and a large resource class, both of which
were only ever asked for H.264.

Phase 5's probe asks each codec with its own profile and level, and includes a
deliberate-error pair — a bogus codec must give `0x811D0204` and a bogus
resource class `0x811D0203` — so the correction is confirmed rather than
assumed.

#### `SceVideodec2DecoderMemoryInfo` — 0x48 bytes, and half of it is *input*

The query fills the sizes and then **explicitly zeroes `+0x10`, `+0x20`,
`+0x30` and `+0x44`** before returning. **[E]** That is how we know those three
are pointer fields for the caller to fill in on the way into `CreateDecoder`,
and it is why the struct is 0x48 bytes rather than the four output words the
Phase 3 log recorded.

| offset | direction | meaning |
|---|---|---|
| `+0x00` | in | struct size, exactly `0x48` |
| `+0x08` | out | working memory, ~3.4 MiB, barely varies. Includes the `0x40000` decoder object |
| `+0x10` | **in** | pointer to it. Intended **`WB_ONION`** |
| `+0x18` | out | **frame buffer pool — scales with resolution × DPB** |
| `+0x20` | **in** | pointer to it. Intended **`WC_GARLIC`** |
| `+0x28` | out | a third size. **Never measured** — the Phase 3 probe logged three fields and not this one |
| `+0x30` | **in** | pointer to it, required only when `+0x28` is non-zero |
| `+0x38` | out | scales with resolution alone. **Has no pointer of its own** |
| `+0x40` | out | `256` — alignment. On the way in, its low byte must be clear (`0x811D0108`) |
| `+0x44` | **in** | must be zero (`0x811D010C`) |

### `sceVideodec2CreateDecoder(cfg, memInfo, &decoder)` — **called, returns 0**

**A payload creates a hardware video decoder, for H.264 *and* HEVC.** **[E]**
2026-08-11, two deploys, `projects/createdecoder_test/` in the app slot. `rc=0`
on both usable resource classes for H.264 and on `0x12384` for HEVC Main, with
a clean `DeleteDecoder` every time. Every structural prediction below was
confirmed by the run:

| | |
|---|---|
| `CreateDecoder(0xb6c8, H.264, 1080p, dpb 16)` | **`rc=0`**, decoder `0x2004e0000` |
| `CreateDecoder(0x12384, H.264, same)` | **`rc=0`** |
| **`CreateDecoder(0x12384, HEVC Main, 1080p, dpb 16)`** | **`rc=0`** — codec index `4` in the object |
| `DeleteDecoder` | **`rc=0`** all three times |
| decoder handle vs `memInfo+0x10` | **identical** — the handle is the caller's own buffer, as read |
| `decoder+0x78` (VdecCore object) | `0x200520000` = `pWorkMemory + 0x40000` **exactly**, as read |
| `+0x00` / `+0x68` cookies | `"U5JD7RL"` and `0xa824d9799010a455`, as read |
| `+0x5c` / `+0x64` | resource class echoed back; codec index `0` = H.264, as read |
| 9 cond vars at `+0x80`, 12 mutexes at `+0xc8` | present in the object dump, at exactly those offsets |
| validation controls | **9/9** exact — `0102`, `0101` ×2, `0105` ×2, `010C`, `0104`, `0106`, `0108` |

No entitlement error, no arbitration call, no user session. The decoder was
built, inspected and torn down cleanly inside a payload.

**The HEVC create was the run that could have hung** — `sceVdecCoreCreateDecoder`
reaches `sceSysmoduleLoadModuleInternal` on that path, which is the shape of the
Phase 4 hang. It did not hang. `libSceSysmodule.sprx` was loaded up front for
exactly this reason (standing rule 11), and the module load resolved. **[I]**
that the rule is what avoided a second hang; **[E]** that the call returned 0
with the module loaded.

Raw logs: `research-logs/console/evo_createdecoder_log-run1-avc.txt` and
`-run2-hevc.txt`.

#### The field correction, confirmed on hardware **[E]**

Five deliberate-error controls in the same run settle the `+0x08` / `+0x0c`
question by experiment rather than by reading:

| control | result |
|---|---|
| bogus **codec** at `+0x0c` | `0x811D0204` — unsupported codec |
| bogus **resource class** at `+0x08` | `0x811D0203` — unsupported resource class |
| H.264 with profile 99 | `0x811D0205` |
| HEVC handed H.264's profile 100 / level 51 | `0x811D0205` — **exactly what Phase 3 saw and misread** |
| DPB 17 | `0x811D0209` |

Had the labels been the other way round, the first two would have come back
swapped. They did not.

#### HEVC and VP9, asked properly for the first time **[E]**

**There is no HEVC entitlement gate — at query time or at create time.** HEVC
is accepted, and it returns its *own* memory arithmetic — different from H.264 in every field,
which is what proves a real codec configuration was accepted rather than a
value being waved through:

| configuration | work `+0x08` | frame `+0x18` | map `+0x38` |
|---|---|---|---|
| H.264 High 1080p dpb16 | 3,568,896 | 86,507,776 | 3,347,456 |
| **HEVC Main 1080p dpb16** | **5,874,688** | **63,242,496** | **3,322,880** |
| **HEVC Main10 1080p dpb16** | 5,874,688 | 91,685,120 | 6,640,640 |
| **HEVC Main 4K L5.1 dpb16** | 5,874,688 | 219,545,856 | 12,446,720 |
| **HEVC Main10 4K L5.1 dpb16** | 5,874,688 | 326,107,392 | 24,888,320 |

HEVC Main at 1080p is **cheaper than H.264** — 65.9 MiB of buffers against
85.9 MiB — because the frame pool scales with the codec's own DPB sizing.

Two more results from the same matrix:

- **HEVC requires resource class `0x12384`.** On `0xb6c8` it returns
  `0x811D0200`. So the classes really are a small and a large tier, and the
  large one is the one that carries HEVC and 4K. The old "0xb6c8 is AVC,
  0x12384 is HEVC" guess landed near the truth for entirely the wrong reason.
- **VP9 is refused at every configuration tried** — profile 0 and 2, 1080p and
  4K, `0x811D0200` from the size computation each time. **[E]** That is a
  capacity refusal, not a validation one: the config passes profile and level
  checks and then finds no resource behind it. It cross-checks against §3,
  where `libSceVdecSvp9.sprx` **does not exist on this firmware**. **[I]**

Resource classes `1`, `0x24708` and `0x24709` are likewise refused at query
time with `0x811D0200`, which agrees with `sceVdecCoreCreateDecoder` rejecting
every class above 8.

**`memInfo+0x28` is zero for every accepted configuration** — H.264 and HEVC,
1080p and 4K. **[E]** The third buffer is never needed on these paths, exactly
as `sceVdecCoreCreateDecoder`'s class check predicted. That open question is
closed.

### The ABI as read before any of it was called

`+0xba0` is a two-instruction thunk — `mov ecx,0x1 ; jmp <body at +0xbb0>` —
and **`sceVideodec2CreateHevcDecoder` (+0x1230) is byte-for-byte the same
thunk**. **[E]** It is an alias; there is nothing HEVC-specific about it and
calling it changes nothing. `CreateDecoderBid` (+0x1240) is the same body with
a caller-supplied `ecx` and an outright refusal of resource class 1.

So the "third argument beyond config and memory-info" the phase plan expected
is the output handle: three arguments, plus a fourth the thunk supplies.

What the body does, in order **[E]**:

| step | on failure |
|---|---|
| gated pointer validation on all three arguments | `0x811D0102` |
| `memInfo->thisSize == 0x48`, `cfg->thisSize ∈ {0x48, 0x50}` | `0x811D0101` |
| the **same validator the query uses**, writing a memory-info-shaped struct to the stack | `0x811D02xx` |
| memory check on `mem+0x10`, `mem+0x20`, and `mem+0x30` if `mem+0x28 ≠ 0` | `0x811D0105` |
| `mem+0x44 == 0` | `0x811D010C` |
| `mem+0x08`, `+0x28`, `+0x18` each ≥ the computed requirement | `0x811D0104` |
| `mem+0x38` ≥ the computed requirement | `0x811D0106` |
| computed alignment is `0x100` | `0x811D0111` |
| low byte of `mem+0x40` is clear | `0x811D0108` |
| writes `"U5JD7RL"` at the front of `mem+0x10`, builds 12 `ScePthread` mutexes at `+0xc8` and 9 cond vars at `+0x80` | `0x811D0100` |
| `sceVdecCoreCreateDecoder(buf3, {sizes, pointers}, cfg->computeQueue, &decoder[0x78])` | `0x811D0100`, or `0x811D020C` if VdecCore said `0x80C00004` |
| writes `0xa824d9799010a455` at `+0x68` and `*decoderOut = decoder` | — |

**The decoder handle is the caller's own buffer.** `mem+0x10` is not working
memory the module borrows: it places its object at the front and hands the same
pointer straight back. The first `0x40000` bytes are the object; everything
past that is what gets passed down. That is why the `+0x08` requirement is
~3.4 MiB and barely moves with resolution.

`DeleteDecoder` (+0x3220) checks the `+0x68` cookie, forwards `decoder+0x78` to
VdecCore, tears the mutexes down and clears the cookie. `0x811D0103` for
anything that is not a live decoder. **[E]**

#### Which memory type each buffer wants **[I]**

The checker at `+0x2a0` is a four-way jump table selected by a mode argument:

| mode | requirement | failure |
|---|---|---|
| 0 | `memoryType == 0`, i.e. `SCE_KERNEL_WB_ONION` | `0x811D0109` |
| 1 | `memoryType == 3`, i.e. `SCE_KERNEL_WC_GARLIC` | `0x811D010A` |
| 2 | protection includes write | `0x811D010B` |
| 3 | no check at all — NULL is still rejected | `0x811D0105` |

`CreateDecoder` computes the mode from `cfg->checkMemoryType` (`+0x3d`):
`mem+0x10` → mode 0, `mem+0x20` → mode 1, `mem+0x30` → mode 0 when the flag is
set, and mode 3 for all three when it is clear. **So the module's own intent is
onion for the working buffer, garlic for the frame pool** — which is what you
would expect of CPU-side state and a GPU-written frame pool respectively.

The check only runs when the module-global validation flag is set, and that
flag is clear here, so `checkMemoryType` is inert in a payload. The mapping is
still worth having: it is a statement of intent from the module itself, free,
and obtained without a deploy.

#### What the layer below needs **[E]**

`sceVdecCoreCreateDecoder` (VdecCore `+0x1410`) receives the buffers repacked
as `{sizeA-0x40000, ptrA+0x40000, sizeC, ptrC, sizeB, ptrB}` and requires:

| resource class | requires |
|---|---|
| 4 (`0xb6c8`) and 8 (`0x12384`) | `ptrA` and `ptrB` non-NULL |
| 2, 3 | `ptrA`, `ptrB` **and** `ptrC` |
| above 8 | rejected, `0x80C00002` |

**And on the HEVC codec index it calls `sceSysmoduleLoadModuleInternal` to pull
its codec module in at create time.** That is a lazy import into
`libSceSysmodule` — the exact shape of the Phase 4 hang, which blocks silently
and indefinitely rather than failing. `libSceSysmodule.sprx` therefore has to
be loaded before `CreateDecoder` is called, even though nothing in a client
calls it directly.

### Measured memory requirements **[E]**

**Every row here is H.264** — the codec field was left at `1` throughout, and
the column heading below is the *resource class*, not the codec. `+0x28` was
not logged; Phase 5 measured it as **0** for every accepted configuration. The
HEVC figures are in the Phase 5 subsection above.

| resource class | resolution | DPB | `+0x08` | `+0x18` | `+0x38` | total |
|---|---|---|---|---|---|---|
| `0x12384` | 1280×720 | 16 | 3,568,896 | 38,011,136 | 1,387,520 | **41.0 MiB** |
| `0x12384` | 1920×1080 | 16 | 3,568,896 | 86,507,776 | 3,347,456 | **89.1 MiB** |
| `0x12384` | 3840×2160 | 16 | 3,568,896 | 321,847,552 | 12,446,720 | **322.2 MiB** |
| `0x12384` | 3840×2160 | 4 | 3,539,456 | 97,911,040 | 12,446,720 | **108.6 MiB** |
| `0xb6c8` | 1280×720 | 16 | 3,568,896 | 38,011,136 | 1,387,520 | **41.0 MiB** |
| `0xb6c8` | 1920×1080 | 16 | 3,568,896 | 86,507,776 | 3,347,456 | **89.1 MiB** |
| `0xb6c8` | 3840×2160 | any | `0x811D0200` | | | rejected |
| `1` | any | any | `0x811D0200` | | | rejected |

**The numbers are computed, not tabulated** — which is the strongest evidence
that a real decoder configuration was accepted rather than a validation stub.

`0xb6c8` is capped below 4K and `0x12384` is not — with the field labels
corrected, that says the two are a **small and a large resource class**, not
two codecs. **[I]** The 4K refusal is `0x811D0200` from the size computation
itself, i.e. a capacity limit, not a codec or entitlement check.

**No entitlement error appeared at any resolution on either class.** But that
is a statement about H.264 only, since H.264 is all that was asked for. The
gate the AvPlayer strings describe has not yet been given the chance to bite:
the first properly-formed HEVC query is in Phase 5. **[H]**

Dropping DPB from 16 to 4 cuts 4K from 322 MiB to 109 MiB — worth knowing,
because 322 MiB is a lot to ask for in a payload.

### The compute-queue triple — read, called, and it **works**

Three functions, all read offline from the dumped image before any were
called, and all three structures confirmed on hardware by deliberate-error
controls **7/7** (§10).

```c
/* 0x18 bytes. Query fills +0x08 and zeroes +0x10. */
typedef struct {
    uint64_t thisSize;           /* +0x00  must be 0x18   else 0x811D0101 */
    uint64_t cpuGpuMemorySize;   /* +0x08  out from Query, in to Allocate */
    void    *cpuGpuMemory;       /* +0x10  NULL from Query, caller's in   */
} SceVideodec2ComputeMemoryInfo;

/* 0x10 bytes. */
typedef struct {
    uint64_t thisSize;           /* +0x00  must be 0x10   else 0x811D0101 */
    uint16_t computePipeId;      /* +0x08  < 5            else 0x811D0201 */
    uint16_t computeQueueId;     /* +0x0a  < 8            else 0x811D0202 */
    uint8_t  memoryCheckMode;    /* +0x0c  0 = unchecked, else ONION-only */
    uint8_t  reserved[3];        /* +0x0d  must be 0      else 0x811D0200 */
} SceVideodec2ComputeQueueInfo;

int sceVideodec2QueryComputeMemoryInfo(SceVideodec2ComputeMemoryInfo *);
int sceVideodec2AllocateComputeQueue(const SceVideodec2ComputeQueueInfo *,
                                     SceVideodec2ComputeMemoryInfo *,
                                     void **queueOut);
int sceVideodec2ReleaseComputeQueue(void *queue);   /* NULL -> 0x811D0110 */
```

**`QueryComputeMemoryInfo` takes one argument, not two** — worth stating,
because the phase plan assumed the `(cfg, memInfo)` shape of the decoder query.

| | |
|---|---|
| `QueryComputeMemoryInfo` | **returns 0.** `cpuGpuMemorySize` = **4,805,120** (4.58 MiB), `cpuGpuMemory` zeroed **[E]** |
| every validation control | **7/7 exactly as read** — sizes, reserved bytes, pipe/queue ranges, and the undersize check **[E]** |
| `AllocateComputeQueue(pipe 0, queue 0)` | **returns 0**, handle `0x2002b0500`, on `WB_ONION` memory **[E]** |
| `ReleaseComputeQueue(handle)` | **returns 0** **[E]** |

The requirement is a fixed 4.58 MiB, independent of the decoder config; it is
queried without one. That is small, and a payload gets it easily.

`AllocateComputeQueue` recomputes the requirement itself and rejects a caller
size below it with `0x811D0104` — which is how we know the value reached the
layer below rather than being validated and discarded.

**`libSceGnmDriver.sprx` must be loaded first.** Without it the call hangs and
never returns; with it, the same call succeeds. `libSceAjm.sprx` was loaded in
the same change and is not known to be required. **[E]**

`memoryCheckMode` was left 0 and pipe/queue 0/0 worked on the first attempt, so
nothing about the pipe or queue selection needed sweeping.

### The compute queue is a Gnm compute queue

The first attempt hung. The call chain was then disassembled all the way down,
at zero deploy cost, and it ends somewhere specific — which is what produced
the fix. **[E]** for every instruction, **[I]** for the naming of the final
import.

```
sceVideodec2AllocateComputeQueue        Videodec2 +0x660   validation only
  -> sceVdecCoreInitializeComputeResource  VdecCore +0x300   repacks args
       -> VdecCore +0xd1c0                 checks size and 0x100 alignment
            -> VdecCore +0x58bf0           carves the buffer:
                                             ring base = mem + 0x100000
                                             read ptr  = mem + 0x110000
                                             ring size = 0x4000 dwords
                 -> VdecCore +0x67270      thin wrapper
                      -> import, 5 args:  (pipeId, queueId, ringBase,
                                           ringSizeInDW, readPtrAddr)
```

That final signature is `sceGnmMapComputeQueue` exactly. **[I]** So the compute
queue is a **Gnm compute queue**, and obtaining one means calling
`libSceGnmDriver`.

**Its GOT slot is unresolved in the dump.** All four neighbouring Gnm import
slots still point back into their own PLT push sequences — lazily bound and
never called. **[E]** The first probe did not load `libSceGnmDriver.sprx`, so
the call had to resolve a symbol in a module that was not mapped in that
process.

**Loading the module fixed it.** Same payload, same call, one line different:
`rc=0`. **[E]** So an unresolvable lazy import here *blocks* rather than
faulting — worth knowing, because it is a silent failure mode that looks
exactly like a GPU problem and is not one. **[I]**

Also recorded: the inner layer checks `pipeId < 7`, while the public
`libSceVideodec2` wrapper checks `< 5`. The public API is the stricter of the
two. **[E]** And the 4.58 MiB buffer is carved as ring base at `mem+0x100000`,
read pointer at `mem+0x110000`, ring size `0x4000` dwords. **[E]**

### Videodec2 → VdecCore, resolved through the GOT

`libSceVideodec2`'s PLT stubs were followed into its s2 GOT and the resolved
addresses matched `libSceVdecCore`'s measured export offsets exactly. This
names the layer beneath the public API without a symbol table. **[E]**

| Videodec2 entry point | calls |
|---|---|
| `QueryComputeMemoryInfo`, and again inside `AllocateComputeQueue` | `sceVdecCoreQueryComputeResourceInfo` (+0x2b0) |
| `AllocateComputeQueue` | **`sceVdecCoreInitializeComputeResource`** (+0x300) — *this is what hangs* |
| `ReleaseComputeQueue` | `sceVdecCoreFinalizeComputeResource` (+0x3a0) |
| `MapDirectMemory` | `sceVdecCoreMapMemoryBlock` (+0x3f0) |
| `CreateDecoder` | `sceVdecCoreCreateDecoder` (+0x1410) |

### `sceVideodec2Decode(decoder, au, fb, out)` — **called; refused, and the refusal is diagnosed**

**Four arguments, not two.** **[E]** Read off `+0x1290`, then exercised on
2026-08-11 by `projects/decodeframe_test/`. **Every one of the 18 validation
controls below returned exactly the predicted code** — 11 for `Decode`, 7 for
`GetPictureInfo` — so the struct sizes, field offsets and error mapping in this
section are confirmed on hardware, not merely read.

**No picture yet.** Seven runs on 2026-08-11 walked `Decode` through four
successive gates, each diagnosed from the module's own diagnostic lines and
each fixed:

| run | refused at | cause | fix |
|---|---|---|---|
| 1 | GpDec `0xC22`, state 0 | nothing had been mapped | call `MapDirectMemory` |
| 3 | GpDec map, pin ioctl errno 5031 | `size` not page-aligned | round up to 16 KiB |
| 4 | GpDec `0x97F`, status `0x33` | every block given `physAddr` 0, so all overlapped | use the real physical offset |
| 6 | GpDec `0xDA6` | the AU, work memory and frame pool were not registered | register all of them |
| **8** | **the hardware submit itself** | **open** | — |

Run 8 gets through every software gate. The submit is a virtual call at
`[[gpdec+0x38]] + 0x18`, and it fails inside the codec layer with an ioctl
errno of **5200** (`0x1450`), reported up as GpDec `0xDC6` status 4:

    [VDECCORE@D0A10254:00001450]
    [VDECCORE@C0B504C1:00000001]
    [VDECCORE@B0A10DC6:00000000]
    [SCEVDECCORE@A01D07A8:00000004]
    [SCEVIDEODEC2@A01A07A7:80C00001]

This is qualitatively different from the four before it: those were the module
saying "you have not told me about X". This is the **driver refusing the decode
job**.

#### The submit is `ioctl(fd, _IOW(0x83, 23, 24), …)` **[E]**

`libSceVdecCore` `+0x2b870` is the submit. It picks one of three adjacent ioctl
commands from a mode field and builds a 24-byte argument:

```
mov  esi, 0x80188316            ; _IOW(0x83, 22, 24) - the base command
lea  rcx, [rdi+0x1898]          ; arg+0x00 = the job
mov  ecx, [rdi+0x1d6c]          ; arg+0x08
mov       eax                   ; arg+0x0c = the caller's argument
mov  eax, [rdi+0x1d70]          ; arg+0x10
mov  edi, [rdi+0x1d68]          ; the file descriptor
mov  eax, [rdi+0x2c0]           ; the mode
cmp  eax, 7    ; je  -> inc rsi     -> cmd 23, error line 0x254
cmp  eax, 0x10 ; jne -> (unchanged) -> cmd 22, error line 0x25c
               ;        add rsi,2   -> cmd 24, error line 0x24c
```

**The 2026-08-11 run reported line `0x254`, so the mode is `7`, the command is
`_IOW(0x83, 23, 24)`, and the driver rejected it with errno 5200.** The other
group-`0x83` commands VdecCore issues are 11, 18 and 20 — 20 being the memory
pin that `MapDirectMemory` drives.

##### What the job actually contains — the box, opened **[E]**

The 24 bytes the submit hands the driver are
`{ jobPtr, [obj+0x1d6c], callerArg, [obj+0x1d70], 0 }`, and the job at
`obj+0x1898` is three fields:

```
mov DWORD PTR [rbx+0x1898], 1        ; command   - takes 0, 1 or 2
mov QWORD PTR [rbx+0x18a0], rax      ; -> a command buffer
mov DWORD PTR [rbx+0x18a8], 0        ; flag
```

The buffer comes from a **ring of five slots** at `obj+0x58`, stride `0x80`,
indexed by `[obj+0x1d50] % 5`. `[obj+0x1d6c]` is an event-queue id — the same
value is passed to the wait call after the submit, so it is how the driver
signals completion — and `[obj+0x1d68]` is the file descriptor. Both are set
together when the device is opened, at `+0x2c470`.

**`[obj+0x1d70]` is read by the submit and written nowhere in the module.**
Either the object is zero-initialised and the driver expects `0`, or something
outside VdecCore fills it. Suggestive, not actionable.

**That reading was wrong, and it was wrong in an avoidable way.** It said: "the
job is a pointer to a command buffer; whatever the driver objects to is in that
buffer's contents, which the codec layer builds. There is no further handle
here, and the consumer is kernel-side."

Every sentence is true of the **dump**. None of it is true of a **live
decoder**. The object is in memory *this program allocated*, so every field the
submit reads can simply be looked at after the refusal. See below.

##### The job, read back off a live decoder **[E]**

Run 13. The VdecCore object at `decoder+0x78` is *not* the object the submit
operates on — applying these offsets to it returns zeroes in every field. The
submit's object is the **GpDec device object**, reached by a pointer chain.
Rather than chase the chain it was found by what is unique about it: `mode == 7`
at `+0x2c0` together with a plausible fd at `+0x1d68`, two constraints `0x1ab8`
bytes apart. **Exactly one candidate matched**, in the work-memory block:

```
obj+0x1d08 mode index  = 5  -> mode 7 -> ioctl command 23
obj+0x02c0 mode        = 7
obj+0x1d68 fd          = 16          (a CHARACTER DEVICE, confirmed by fstat)
obj+0x1d6c event queue = 0x00000011
obj+0x1d70 mystery     = 0x00000001   NON-ZERO - something does fill it in
obj+0x18a0 job.cmdBuf  = 0x2008d3f00  (inside the frame pool)
```

And the command buffer the driver rejected:

| offset | value | reading |
|---|---|---|
| `+0x00` | `0x0de8` | the size of the `obj+0x2b0` structure VdecCore memsets |
| `+0x08` | `0x01a0ec00` | a **physical address** inside the frame pool |
| `+0x10` | `7` | the mode |
| `+0x1c` | `0x780` = **1920** | width |
| `+0x20` | `0x440` = **1088** | height, macroblock-aligned |
| `+0x28` | `0x03630000` | a second physical address in the pool |

**The command is well formed.** Correct resolution, correct mode, real physical
addresses in the right ranges. So the codec layer *does* build a command, and
"the codec module was never loaded" is dead as an explanation — `libSceVdecSavc`
and `libSceVdecSavc2` were loaded for the first time in run 11 and changed
nothing.

`[obj+0x1d70]` reads back as **1**, not zero, so it is filled in by something.
That closes the "written nowhere in the module" question without answering who
writes it.

##### The submit path is chosen by the codec, and it does not vary **[E]**

`libSceVdecCore +0x2b870` picks the ioctl command from the mode at `+0x2c0`,
and the mode comes from a six-entry jump table indexed by `[obj+0x1d08]`:

| `[obj+0x1d08]` | mode | ioctl command |
|---|---|---|
| 0 | 0 | 22 |
| 1 | `0x10` | **24** |
| 2 | 4 | 22 |
| 3 | 3 or `0x80000003` | 22 |
| 4 | 1 | 22 |
| **5** | **7** | **23** ← ours |

`[obj+0x1d08]` is written by a four-field setter at `+0x8f0` that VdecCore
exports and the **codec module** calls — so the codec layer chooses the route
into the driver.

**It is invariant.** Run 14 swept resource class `0xb6c8` and `0x12384` against
pipeline depth 1 and 4: all four produced mode index 5, mode 7, command 23 and
errno 5200, identical field for field. **Configuration does not select the
submit path**, so there is no cheap way to reach commands 22 or 24 from the
public API.

##### `0x1450` is a genuine ioctl errno **[E]**

Read off the submit's own error path, so this is not an inference:

```
call 0x8009ea010          ; the ioctl
mov  r14d, 0x254          ; the line number
test eax, eax
je   <success>
call 0x8009e9ff0          ; __error()  -> &errno
mov  r8d, DWORD PTR [rax] ; *errno   <- 0x1450 = 5200 is logged from here
```

The ioctl returned failure and `errno` was 5200. The driver is refusing the
job, and everything on our side of that call is now known to be well formed.

##### A correction, and the method note that goes with it

This section previously claimed the refusing code was **not in the dump**, on
the strength of a raw byte search finding no `mov ecx,0x254` in VdecCore. The
search was correct and the conclusion was wrong: **the line number is loaded
into `r14d` and moved to `ecx` at a shared logging tail**, because three call
sites share it —

```
mov  r14d, 0x254
...
mov  ecx, r14d
```

so the immediate never appears next to `ecx`. **A byte search for an immediate
only disproves the immediate form.** Two supporting arguments were also wrong,
for the same underlying reason: the "vtables are not in the dump" observation is
true but unremarkable — those tables are built at run time — and the on-demand
codec module turned out not to load at all (below).

The claim cost one deploy, which is the cheap part; the expensive part would
have been carrying it forward. Recorded here rather than quietly fixed.

#### `CreateDecoder` loads no module on this path **[E]**

`sceVdecCoreCreateDecoder` *can* load a codec module through
`sceSysmoduleLoadModuleInternal`, dispatching on the codec index:

| codec index | internal sysmodule id |
|---|---|
| 0 — H.264 | `0x80000036` |
| 4 — HEVC | `0x8000003c` |
| a separate resource-class path | `0x80000035` |

with `0x805A1001` treated as "already loaded". But `projects/codecdump_test/`
measured it: **a modid sweep before and after an H.264 `CreateDecoder` finds
exactly the same 11 modules.** Nothing loads. Either the codec is already
resident or that path is not taken for H.264 on this firmware.

The same run dumped five modules that had never been dumped —
`libSceSysmodule`, `libSceVdecwrap`, `libSceVdecShevc`, `libSceGnmDriver` and
`libSceAjm`, 1.4 MB in `proprietary/dump/codec/` — so the negative result came
with the material to rule those out, and none of them contains the submit
either. It is in VdecCore, as above.

**One loose end: the modid sweep shows a gap.** The eight loaded modules take
`0x42`–`0x47` and `0x49`; **`0x48` is unaccounted for**, exactly as Phase 0 saw
`0x34` unaccounted for between AvPlayer and VdecCore. `sceKernelGetModuleInfo`
either fails for it or returns a blank name, and the probe silently drops blank
names. Worth fixing before the next enumeration.

#### Arbitration: the ABI is confirmed, and `Initialize` **works** **[E]**

**Runs 9 and 10 hung here. Run 11 did not. The cause was an unresolved lazy
import, and the fix is one module load.**

The ABI was read first and every prediction held — **7/7 controls exact**:

```c
typedef struct {
    uint64_t thisSize;   /* +0x00  exactly 0x18   else 0x81570001 */
    uint32_t priority;   /* +0x08  256..767       else 0x81570004 */
    uint32_t pad0c;
    uint64_t count;      /* +0x10  1..127         else 0x81570001 */
} SceVideoDecoderArbitrationParams;
```

- `sceVideoDecoderArbitrationInitialize(params)` — `0x81570002` if already done.
- `sceVideoDecoderArbitrationEnable(NULL, callback)` — the **first** argument
  must be NULL and the second must not be. The second is stored in a module
  global and tail-called through a trampoline at `+0x260`, so it is a callback,
  not a handle.
- `sceVideoDecoderArbitrationAcceptEvent(n)` — `n <= 1`, else `0x81570001`.

The priority range 256..767 is the same one `sceAvPlayerInit` clamps
`basePriority` into and the decoder config checks at `cfg+0x38`. Three
independently-read functions agreeing on one range.

##### Why it hung: `+0x350` is a PLT stub, not a function body **[E]**

This document previously called the hang "the body at `+0x350` blocks" and
hypothesised an IPC to a system service. **Both readings were wrong**, and
three instructions say so:

```
800a3c350:  jmp QWORD PTR [rip+0x7cf2]   # GOT, segment 2 +0x48
800a3c356:  push 0x1
800a3c35b:  jmp  0x800a3c330
```

`Initialize` validates its parameters and then **tail-calls an import**. In the
Phase 0 dump that import's GOT slot held `0x800a3c356` — the address of its own
`push`/`jmp` resolver sequence. **Unbound**, while its neighbours in the same
table were bound to libkernel and libSceLibcInternal.

That is not a new failure mode. It is the **third** instance in this project,
after `AllocateComputeQueue` without `libSceGnmDriver` and
`sceVideodec2MapMemory`. Standing rule 15 — *check the GOT slot before you call
the function* — was written after the second one and simply was not applied
here. An unresolved lazy import blocks silently and forever: no fault, no error
code, no log line. Which is exactly what runs 9 and 10 observed, and why "no
user session" looked plausible enough to spend a deploy on.

**The provider is `libSceVideoArbitration.sprx`** — a **different module** from
`libSceVideoDecoderArbitration.sprx`, present on the console, never loaded by
this project before. **[E]** With it loaded first, all three previously unbound
slots bind into one module's address range:

| GOT slot | before | after |
|---|---|---|
| s2 +0x48 | `base+0x356` (own PLT) | bound — **Initialize's tail call** |
| s2 +0x58 | `base+0x376` (own PLT) | bound |
| s2 +0x70 | `base+0x3a6` (own PLT) | bound |

**Load order matters.** Binding on this loader is eager at load time, so the
provider must be resident *before* the consumer is loaded. Every earlier run
loaded `libSceVideoDecoderArbitration` second, before anything that could
supply it.

##### Arbitration comes up, and it changes nothing **[E]**

Run 11, with the slot bound:

```
Initialize(priority 700, +0x10 = 4) -> 0x81570002   already initialised
Enable(NULL, callback)              -> 0x00000000
AcceptEvent(0) / AcceptEvent(1)     -> 0x00000000 / 0x00000000
```

Arbitration was then up for the first time in this project, and a decode run
immediately afterwards was **refused by the driver with the identical errno
5200 chain**. Two earlier attempts in the same run, unarbitrated, failed the
same way.

**So arbitration is ruled out as the cause of errno 5200.** **[E]** It had been
the prime suspect since run 8, on the reasoning that AvPlayer calls
`Initialize`/`Enable` before decoding and Route B never had. That reasoning was
sound and the answer is still no.

`sceUserServiceInitialize` plus a real logged-in user id in the process makes no
difference either — tested separately, same block before the fix, same refusal
after it.

Raw logs: `research-logs/console/evo_decodeframe_log-run1.txt` and
`research-logs/deploy/decodeframe-run{4,5,8}-stdout.txt`. **The stdout captures
matter more than the USB log here** — the module's diagnostic lines go to
stdout only, and they are what solved every one of the four.

```c
typedef struct {                 /* 0x30 */
    uint64_t thisSize;           /* +0x00  exactly 0x30       else 0x811D0101 */
    const void *auData;          /* +0x08  non-NULL           else 0x811D010E */
    uint64_t auSize;             /* +0x10  non-zero           else 0x811D010D */
    uint64_t ptsData;            /* +0x18  passed through to VdecCore         */
    uint64_t dtsData;            /* +0x20  ditto - moved as one xmm with pts  */
    uint64_t attachedData;       /* +0x28  ditto                              */
} SceVideodec2InputData;

typedef struct {                 /* 0x20 */
    uint64_t thisSize;           /* +0x00  exactly 0x20       else 0x811D0101 */
    void    *frameBuffer;        /* +0x08  non-NULL 0x811D0107; low byte must
                                  *        be clear, i.e. 256-aligned, else
                                  *        0x811D0108                         */
    uint64_t frameBufferSize;    /* +0x10  non-zero           else 0x811D0106 */
    uint8_t  isAccepted;         /* +0x18  out; cleared on entry              */
} SceVideodec2FrameBuffer;

typedef struct {                 /* 0x38, or 0x30 without the last two words  */
    uint64_t thisSize;           /* +0x00  0x30 or 0x38  (`or rax,8; cmp 0x38`)*/
    uint8_t  isValid;            /* +0x08  a picture came out                 */
    uint8_t  isErrorFrame;       /* +0x09  from two VdecCore error words       */
    uint8_t  pictureCount;       /* +0x0a  GetPictureInfo demands 1 or 2       */
    uint8_t  streamState;        /* +0x0b                                      */
    uint32_t codecType;          /* +0x0c  1 / 0xee049 / 0x245bfd, remapped
                                  *        from VdecCore's 0 / 4 / 6           */
    uint64_t ptsData;            /* +0x10                                      */
    uint32_t word18;             /* +0x18                                      */
    void    *frameBuffer;        /* +0x20  where the picture actually is       */
    uint64_t frameBufferSize;    /* +0x28                                      */
    uint32_t word30;             /* +0x30  0 / 0xd460 / 0xc24a; size 0x38 only */
    uint32_t word34;             /* +0x34                       size 0x38 only */
} SceVideodec2OutputInfo;
```

The order of checks, and then what it does **[E]**:

| step | on failure |
|---|---|
| `[decoder+0x68] == 0xa824d9799010a455` | `0x811D0103` |
| gated pointer validation on `au`, `fb`, `out` | `0x811D0102` |
| `(decoder+0x5c & ~1) != 0x24708` | `0x811D0103` |
| the three struct sizes above | `0x811D0101` |
| `fb->frameBuffer` non-NULL, 256-aligned; `fb->frameBufferSize` non-zero | `0x811D0107` / `0x0108` / `0x0106` |
| `au->auData` non-NULL, `au->auSize` non-zero | `0x811D010E` / `0x010D` |
| lock the mutex at `decoder+0xc8` | `0x811D0111` |
| `decoder+0x50` (the flush latch) is clear | `0x811D0100` |
| `decoder+0x48`, `decoder+0x4c` clear | `0x811D0300` / `0x0304` |
| `sceVdecCoreSetDecodeInput(core, {au, fb}, &status, &pending)` | mapped through a jump table to `0x811D0106`/`0111`/`0300`..`0304` |
| if nothing is pending, `sceVdecCoreSyncDecode(core, &count)`, then harvest `count` pictures through `sceVdecCoreGetDecodeOutput` | |

**The flush latch is one-way.** `sceVideodec2Flush` sets `decoder+0x50`, and
every subsequent `Decode` returns `0x811D0100` until `sceVideodec2Reset` clears
it. **[E]** So `Flush` goes last in any sequence.

`sceVideodec2Flush(decoder, fb, out)` takes three arguments and **accepts a NULL
`fb` on resource classes `0xb6c8` and `0x12384`** **[E]** — consistent with the
harvest path, which on those two classes overwrites `out->frameBuffer` with a
pointer VdecCore chose rather than the caller's.

### `sceVideodec2GetPictureInfo(out, pic0, pic1)` — **read, not yet called**

One body, four names (§6). It validates entirely from the `OutputInfo` the
caller hands it, so its controls need no decoded picture. **[E]**

| step | on failure |
|---|---|
| `pic0` non-NULL; `pic0->[+0x08]` and `pic1->[+0x08]` cleared | `0x811D0102` |
| `out->thisSize` is 0x30 or 0x38 | `0x811D0101` |
| `out->frameBuffer` 256-aligned, then non-NULL | `0x811D0108` / `0x0107` |
| `out->pictureCount` in 1..2 | `0x811D010F` |
| `out->frameBufferSize > pictureCount * 1024` | `0x811D0106` |
| per picture, `pic->thisSize`: AVC `\| 0x10 == 0x78`, HEVC `== 0xa8`, VP9 `== 0x58` | `0x811D0101` |

**Picture metadata lives in the tail of the frame buffer.** The body reads it
from `frameBuffer + frameBufferSize - pictureCount * 1024`, walking backwards
1 KiB per picture, and copies fields out of it into the caller's struct. **[E]**

### `mapMemorySize` is one output frame — and it names the pixel format **[I]**

`memInfo+0x38` was measured eight times by Phase 5 without anyone knowing what
it was. It is one decoded frame plus those metadata blocks:

    mapMemorySize = align(width, 256) * align(height, N) * bytes * 3/2 + 5 * 1024

`N` = 16 for H.264 and 1 for HEVC; `bytes` = 2 for Main10. It reproduces every
one of Phase 5's figures **to the byte**:

| configuration | predicted | measured **[E]** |
|---|---|---|
| AVC 1280×720 | 1280 × 720 × 3/2 + 5120 = 1,387,520 | 1,387,520 |
| AVC 1920×1080 | 2048 × **1088** × 3/2 + 5120 = 3,347,456 | 3,347,456 |
| AVC 3840×2160 | 3840 × 2160 × 3/2 + 5120 = 12,446,720 | 12,446,720 |
| HEVC Main 1920×1080 | 2048 × **1080** × 3/2 + 5120 = 3,322,880 | 3,322,880 |
| HEVC Main10 1920×1080 | × 2 = 6,640,640 | 6,640,640 |
| HEVC Main 3840×2160 | 12,446,720 | 12,446,720 |
| HEVC Main10 3840×2160 | 24,888,320 | 24,888,320 |

A `× 3/2` with half-height chroma is **NV12**; `× 3` is **P010**. The stride is
`align(width, 256)` — **2048 at 1080p, not 1920** — and H.264 pads height to a
macroblock while HEVC does not pad at all. The `5 * 1024` is the five
metadata slots.

That is most of Phase 7's first two questions, obtained without a deploy — and
**the 2026-08-11 run confirmed it on hardware, 7/7 MATCH** across AVC 720p /
1080p / 4K and HEVC Main and Main10 at 1080p and 4K. **[E]** The formula is no
longer a hypothesis about `memInfo+0x38`; it is a measured property of the
module on 12.70. No frame has been *looked at* yet, so the inference from the
formula to NV12/P010 pixels remains **[I]**.

#### `optimizeProgressiveVideo` costs one metadata block **[E]**

2026-08-11, and found by accident: the config field was set to `true` for the
first time (to match the working reference client) and **all seven rows
immediately read exactly 1024 bytes below prediction** — every codec, every
resolution, no exceptions.

    optimizeProgressiveVideo = false  ->  ... + 5 * 1024
    optimizeProgressiveVideo = true   ->  ... + 4 * 1024

So the `5 * 1024` tail is not fixed. One of the five 1 KiB metadata slots is
there only for interlaced content, and declaring the stream progressive drops
it. The rest of the formula is unchanged.

Two things follow. The field is **not cosmetic** — it changes the frame
layout, so anything reading metadata out of the tail must know which of the two
sizes it is dealing with. And a 7/7 match becoming 0/7 the moment an unrelated
config flag moved is a reminder that the frame-size control is sensitive to the
whole config, not just to resolution and codec.

### `MapMemory` and `MapDirectMemory` need a decoder, not memory — and one of them is a trap

Both check `[arg0 + 0x68] == 0xa824d9799010a455` — a magic cookie in a decoder
handle — and return `0x811D0103` otherwise. **[E]** They are **Phase 5+ calls,
not part of obtaining memory.** The phase plan had them in Phase 4; they cannot
be called before `CreateDecoder` exists.

**They are a mandatory step before `Decode`, and reading only the Videodec2
layer said otherwise.** The Phase 6 probe skipped them on the strength of
"nothing on the `Decode` path consults a mapped flag, and neither call is
reachable from it" — both of which are true of `libSceVideodec2` and neither of
which matters, because the state they set lives two layers down. **[E]**
`Decode` returned `0x811D0111` on the first run, and the module's own
diagnostics named the reason:

    [VDECCORE@B0A10C22:00000000]
    [SCEVDECCORE@A01D07A8:00000002]
    [SCEVIDEODEC2@A01A07A7:80C00001]

Read bottom-up: VdecCore returned `0x80C00001` to Videodec2, which has no jump
table entry for it and mapped it to `0x811D0111`. VdecCore's line `0x7A8` is the
default arm of a 28-entry table over the GpDec layer's return value, reached
with status `2`. And GpDec's line `0xC22` is a state check —

```
mov r8d, DWORD PTR [r14+0x40]   ; GpDec state, decoder+0x140 +0x40
cmp r8, 0x5
ja  ok                          ; >= 6 accepted
mov eax, 0x31                   ; bits 0, 4, 5
bt  eax, r8d
jae ok                          ; states 1, 2, 3 accepted
...                             ; states 0, 4, 5 refused -> status 2
```

— which logged `00000000`. **The GpDec object was in state 0 and input is
refused in state 0.** **[E]**

**`sceVdecCoreMapMemoryBlock` is what sets that state to 1.** **[E]** The only
two writes of `1` to `+0x40` in the module are at `+0x7235` and `+0x1291b`; the
first is in a helper called twice from `+0x2c40`, and `+0x2c40`'s only two
callers are both inside `sceVdecCoreMapMemoryBlock` (`+0x3f0`, whose body runs
to `+0x7af` — single prologue, single `ret` at `+0x4bd`, next prologue at
`+0x7b0`). It takes an array of 0x20-byte entries and a count, range-checks each
region, and on success increments `[gpdec+0x550]` and sets the state.

So the phase plan's original instinct — that `mapMemorySize` and the map calls
belong together — was **right**, and the offline correction that replaced it was
wrong. `sceVideodec2MapDirectMemory` is the bound public route to it;
`MapMemory` is the unbound one (below), so **use `MapDirectMemory`**.

This is standing rule 13 biting again, one layer lower: reading `libSceVideodec2`
to the end still only explained `libSceVideodec2`. Rule 10 says read the layer
below; the state that mattered was two layers below.

**`sceVideodec2MapMemory`'s single import is unbound.** **[E]** Its call goes
through the PLT stub at Videodec2 `+0x4c50`, whose GOT slot (s2 `+0x60`) still
contains `0x800a50c56` — the address of its own `push`/`jmp` resolver sequence,
i.e. lazily bound and never called. Every neighbouring slot that lands in
`libSceVdecCore` is resolved. So the target is **not** a VdecCore symbol, and
unlike the Phase 4 hang there is no module name to load: the slot never bound,
so nothing in the dump says which module owns it.

**Calling it would almost certainly hang** — that is what an unresolved lazy
import does here, silently and indefinitely (§9). **[I]** Nothing needs to call
it, so nothing does.

#### `sceVideodec2MapDirectMemory(decoder, info)` — **called, and it works** **[E]**

Bound, and the route to `sceVdecCoreMapMemoryBlock` (VdecCore +0x3f0). Four
runs on 2026-08-11 established the whole contract. **The field order is not the
obvious one — the size comes before the pointer:**

```c
typedef struct {
    uint64_t thisSize;   /* +0x00  exactly 0x20        else 0x811D0101 */
    uint64_t size;       /* +0x08  non-zero            else 0x811D0104 */
    void    *addr;       /* +0x10  CPU virtual address                 */
    uint64_t physAddr;   /* +0x18  direct-memory physical offset       */
} SceVideodec2MapDirectMemoryInfo;
```

Videodec2 repacks it as `{addr, size, physAddr, mode=0}` and passes a count of
1. The order is settled by what VdecCore does with the entry — `lea r15,[rdx+r13]`
computes `base + len` after an overflow check `base <= ~len` — not by guessing,
and the `size` reading is corroborated by `0x811D0104` ("a caller size is below
the computed requirement") being what a zero there produces.

**Three things it requires, each of which cost a run:**

| requirement | how it announces itself |
|---|---|
| **`size` rounded up to 16 KiB.** `mapMemorySize` is `0x331400`, not a multiple of any page size. The call bottoms out in `ioctl(fd, _IOW(0x83, 20, 40), {base, len, 0, 0, 1})` — a pin request | a laddered run: raw and 4 KiB refused, 16 KiB accepted |
| **`physAddr` distinct per block.** It is an address in a space of its own, and GpDec line `0x97F` refuses any block whose `[physAddr, physAddr+size)` overlaps one already registered. Zero for every block makes them all `[0, size)` and collide | block 0 accepted, block 1 refused with status `0x33` |
| **at most 16 blocks**, and only while the GpDec state is below 2 — i.e. between `CreateDecoder` and the first `Decode` | `[gpdec+0x550] < 0x10`, `[gpdec+0x40] < 2` |

A control confirms the second row rather than assuming it: an *unregistered*
CPU address carrying block 0's `physAddr` is refused, so `physAddr` really is
what the second overlap check uses. **[E]**

#### Everything the hardware will touch has to be registered **[E]**

`MapDirectMemory` is not "map the output frames". GpDec line `0xDA6` walks the
registered blocks asking whether each of three regions is **contained** in one
of them, and refuses the decode otherwise:

| region | what it is |
|---|---|
| `[gpdec+0xda0]`, size `[gpdec+0xd98]` | a create-time buffer |
| `[gpdec+0xdb8]`, size `[gpdec+0xdb0]` | a create-time buffer — **this is the one that refused** |
| the access-unit descriptor | `{auData, auSize, 0}` |

So the full set that must be registered before the first `Decode` is:

- every output frame buffer passed as `Decode`'s third argument,
- **the access unit itself** — the hardware DMAs the bitstream, so `auData`
  cannot point at ordinary process memory such as a `.rodata` array,
- **the work memory and the frame pool** that were handed to `CreateDecoder`.
  Giving `CreateDecoder` a pointer is *not* the same as telling the hardware
  about it.

With all seven registered, every containment check passes and `Decode` reaches
the actual hardware submission.

Both map functions map `0x80C00015` — a GnmDriver-family error — onto
`0x811D01FF`; everything else from VdecCore becomes `0x811D010C`, which is why
that code shows up here meaning something entirely different from its
`CreateDecoder` meaning.

### Videodec2's GOT, resolved slot by slot **[E]**

Following the PLT stubs at `+0x4c20` onward into s2 names the whole layer below
and, just as usefully, says which slots never bound:

| PLT | GOT | resolves to |
|---|---|---|
| +0x4c20 | s2 +0x48 | libkernel — the validation-gate flag getter |
| +0x4c30 | +0x50 | libkernel — `sceKernelVirtualQuery` |
| +0x4c40 | +0x58 | libSceLibcInternal — the diagnostic printer |
| **+0x4c50** | **+0x60** | **unbound** — `MapMemory`'s target |
| +0x4c70 | +0x70 | `sceVdecCoreMapMemoryBlock` |
| +0x4c80 | +0x78 | `sceVdecCoreQueryComputeResourceInfo` |
| +0x4c90 | +0x80 | `sceVdecCoreInitializeComputeResource` |
| +0x4ca0 | +0x88 | `sceVdecCoreFinalizeComputeResource` |
| +0x4cb0 | +0x90 | `sceVdecCoreCreateDecoder` |
| +0x4cd0 | +0xa0 | libkernel — `pthread_mutex_lock` |
| +0x4ce0 | +0xa8 | **`sceVdecCoreSetDecodeInput`** |
| +0x4cf0 | +0xb0 | libkernel — `pthread_mutex_unlock` |
| +0x4d00 | +0xb8 | **`sceVdecCoreSyncDecode`** |
| +0x4d10 | +0xc0 | **`sceVdecCoreGetDecodeOutput`** |
| +0x4d40 | +0xd8 | `sceVdecCoreFlushDecodeOutput` |
| +0x4d60 | +0xe8 | `sceVdecCoreResetDecoder` |
| +0x4d70 | +0xf0 | `sceVdecCoreDeleteDecoder` |

The three bold rows are the entire decode path. **`sceVdecCoreSyncDecode`
blocks on the hardware**, which is where Phase 6's hang risk lives. **[I]**

### Argument validation is compiled in but gated — and the gate is a landmine

Every one of these functions opens with the same shape:

```
call  <flag getter>          ; no arguments
test  eax, eax
je    <plain NULL check>     ; flag clear: only NULL is rejected
...   <full validation>      ; flag set:  sceKernelVirtualQuery(p, 0, &info, 0x48)
```

The validation call's signature and its 0x48-byte output struct — read at +0x1c
as a memory type and at +0x20 as protection bits — are `sceKernelVirtualQuery`
and the PS4 `SceKernelVirtualQueryInfo` layout exactly. **[I]**

**`sceKernelVirtualQuery` is one of the APIs measured broken in a payload
(§2).** So if that flag is ever set, every pointer argument in this module
fails validation and everything returns `0x811D0102` however correct it is.
Observed behaviour says the flag is currently clear — the calls above got past
their pointer checks — but `0x811D0102` on an obviously non-NULL pointer is a
diagnosis, not a mystery.

The same gated checker is what encodes the *intended* memory type for the
compute queue: with `memoryCheckMode` non-zero it requires
`memoryType == 0`, i.e. **`SCE_KERNEL_WB_ONION`, not `WC_GARLIC`**. Garlic is
for frame buffers; the compute queue is expected to be cached and CPU-coherent.
**[I]**

### Direct memory: the ceiling is a property of the *launch slot*, not the console **[E]**

The same binary, the same ladder, run in both execution contexts. This is the
sharpest single measurement in the phase.

| request | elfldr (`deploy.sh`) | app slot (`install-homebrew.sh --run`) |
|---|---|---|
| 4.6 MiB (compute requirement) | OK | OK |
| 16, 32, 41 MiB | OK | OK |
| 64 MiB | `0x80020023` EAGAIN | **OK** |
| 90 MiB (1080p decode) | `0x80020023` EAGAIN | **OK** |
| 109 MiB (4K, dpb 4) | `0x80020023` EAGAIN | **OK** |
| 160 MiB | `0x80020023` EAGAIN | **OK** |
| 322 MiB (4K, dpb 16) | `0x80020023` EAGAIN | **OK** |

Identical for `WB_ONION` and `WC_GARLIC` in both slots, so memory type is never
the issue. **In the app slot every size tested allocated, including the full 4K
dpb-16 working set. The real ceiling there is above 322 MiB and was not found.**

The reason is documented in [building.md](building.md): `deploy.sh` injects into
**`SceSpZeroConf`**, a background network service spawned with `dmem#0` — no
direct-memory budget of its own. `install-homebrew.sh --run` goes through
`hbldr_launch`, which borrows the **PS Now app slot** — a real application
process. `sceKernelAllocateDirectMemory` is useless in either;
`sceKernelAllocateMainDirectMemory` is the allocator that works.

**The compute queue allocates in both slots.** So nothing about hardware decode
requires the app slot except the memory, and the memory requires it absolutely.

The mapped memory is CPU-readable and writable at both ends of the range. **[E]**

### `libSceAvPlayer` — read, not called

- **`sceAvPlayerInit(void *initData)` takes exactly one argument.** [E]
  RSI/RDX/RCX/R8/R9 are all written before being read, and RDI is dereferenced
  immediately: **no hidden `sret`**, no argument shift.
- `test rdi,rdi; je → xor eax,eax; ret` — **NULL returns 0, it does not
  fault.** [E]
- `[rdi+0x60]` is range-checked to 1..4 — the PS4 layout's `debugLevel`. [I]
- `[rdi+0x64]` is clamped into 637..767 and used to derive several more
  priorities by adding 5, 6, 9, 0xa, 0x19 — **a base thread priority**, exactly
  where `basePriority` sits in the PS4 `SceAvPlayerInitData`. [E]/[I]
- **If `[rdi+0x64]` is zero it loads a canned default table from rodata — a
  zeroed struct is an explicitly supported input.** [E]
- `sceAvPlayerGetVideoData(handle, out)` writes **exactly 0x28 = 40 bytes**
  (32 via `vmovups`, then 8 at `+0x20`). The PS4 `SceAvPlayerFrameInfo` is 40
  bytes. [E] Field *order* is still [H].
- `sceAvPlayerIsActive(handle)` and `GetVideoData` both test `[handle+0x250]`;
  the handle is a pointer whose first field is a pointer. [E]
- `sceAvPlayerAddSource(handle, path)` — two args, returns `0x806A0001` for a
  NULL handle or path, `0x806A0002` for a failed parse. [E]

### Error families **[E]**

| Prefix | Owner |
|---|---|
| `0x806Axxxx` | `libSceAvPlayer` |
| `0x811Dxxxx` | `libSceVideodec2` |
| `0x8002xxxx` | kernel / module loader (`0x80020002` ENOENT, `0x80020003` ESRCH) |
| `0x8094xxxx` | user service (`0x80940004` no user) |

`libSceVideodec2` codes, as read from every site that produces them:

| code | meaning | code | meaning |
|---|---|---|---|
| `0100` | the layer below failed | `0111` | computed alignment was not `0x100` |
| `0101` | wrong struct size | `01FF` | GnmDriver said `0x80C00015` |
| `0102` | bad pointer | `0200` | invalid configuration — the size computation refused |
| `0103` | not a decoder handle | `0201` | compute pipe ≥ 5 |
| `0104` | a caller size is below the computed requirement | `0202` | compute queue ≥ 8 |
| `0105` | memory check failed, or a required buffer was NULL | `0203` | unsupported **resource class** (`cfg+0x08`) |
| `0106` | `memInfo+0x38` below the computed requirement | `0204` | unsupported **codec** (`cfg+0x0c`) |
| `0108` | `memInfo+0x40` alignment field not clear | `0205` | unsupported **profile or level** for that codec |
| `0109` | wrong memory type — wanted `WB_ONION` | `0206` | pipeline depth outside 1..8 |
| `010A` | wrong memory type — wanted `WC_GARLIC` | `0207` | CPU affinity mask out of range |
| `010B` | memory not writable | `0208` | priority outside 256..767 |
| `010C` | `memInfo+0x44` reserved field not zero | `0209` | DPB outside 1..16 |
| `0110` | release failed | `020B` | non-NULL extra-config pointer |
| | | `020C` | VdecCore returned `0x80C00004` |

Three of these were previously recorded wrongly and are worth calling out:
`0205` is *profile or level*, not "bad variant"; `010C` is the reserved field,
not "map failed"; and `0111` is the alignment sanity check, not "the VdecCore
query below failed".

`0x80020023` is EAGAIN from the direct-memory allocator: out of budget.

---

## 8. GPU and zero-copy

The two "no GPU" claims must be kept separate, as the review urged. **[E]** for
the strings, **[I]** for the reading:

- The sysroot has no hardware GL/Vulkan (OSMesa/llvmpipe). That is about
  *rendering*.
- The decoder nevertheless writes to **GPU memory** and moves frames with
  **compute shaders**: `Unable to map GPU memory for HW decoder`,
  `HW decoder GPU memory pool usage: 0x%zx (%.1f MiB)`,
  `SceVdecShaderFrameCopyY`, `SceVdecShaderFrameCopyC`,
  `SceVdecShaderCopyPackY/C`, `compute pipe: S/W Slice Dec`.
- `sceVideodec2AllocateComputeQueue` and `sceVdecCoreInitializeComputeResource`
  say a **GPU compute queue is a hard requirement**, not an optimisation.

**This is the question that decides the whole effort.** If frames land in a
tiling `libSceVideoOut` can scan out, the win is large. If every frame needs a
CPU read from uncached GPU memory plus a colour conversion, hardware decode may
be *slower* than the current FFmpeg path. It must be measured, not assumed —
and copy cost must be measured separately from decode cost.

---

## 9. Hazards

| Hazard | Detail |
|---|---|
| **Execute-only text** | Every Sony module's s0 is `--x`. A plain `memcpy` from it **kills the payload**, and a SIGSEGV handler does not save it. Use `kernel_mprotect` to add `PROT_READ` first, or `kernel_proc_copyout` |
| **`libSceAudiodec` load hang** | Hangs the payload when loaded straight after the video modules. Reproduced twice. Fine once GnmDriver/Ajm are loaded |
| **`sceVideoOutOpen` KERNEL-PANICS THE CONSOLE** | **The most expensive mistake in this project.** From a payload in the borrowed `hbldr` app slot: the real logged-in user is refused with `0x80290001`, and the `0xFF` system-user retry returns `1309671680` = `0x4E100000` - **not a handle**, real ones are small integers. A `< 0` check treats that garbage as success. Allocating a GPU compute queue afterwards **panicked the kernel**; `sceVideodec2AllocateComputeQueue` had succeeded in every prior run and the open video-out handle was the only new thing. Cost ~50 minutes of recovery. **Do not open video out from a payload.** §9.1 |
| **`sceVideoDecoderArbitrationInitialize` blocked** | **Solved.** It was an unresolved lazy import, not a body - `+0x350` is a PLT stub. Load `libSceVideoArbitration.sprx` **before** `libSceVideoDecoderArbitration.sprx` and it returns. Cost two deploys and a wrong hypothesis about user sessions. §7 |
| **Unresolved lazy imports block** | `AllocateComputeQueue` hung, silently and indefinitely, purely because `libSceGnmDriver.sprx` was not loaded. It did not fault and it did not return an error. **Load every module in the call chain, not just the one you are calling** |
| **An unbound GOT slot is visible offline** | The same hazard, findable without a deploy: an unbound lazy import still points into its own PLT `push`/`jmp` sequence. `sceVideodec2MapMemory`'s one import is unbound while every VdecCore neighbour is resolved, and no module name is recoverable for it. **Check the slot before calling the function** |
| **`EXIT=124` does not prove a hang** | The successful run also exited 124: the payload finished, but the elfldr socket did not close and its tail never reached the PC. **Always read the log off `/mnt/usb0` before concluding anything from the deploy socket** |
| **In-payload watchdogs do not work** | Failed to fire on both occasions needed. Bound the deploy with `timeout` instead |
| **A hung payload holds the elfldr socket** | `timeout` exit 124 is the expected signal, not an error. The console stayed healthy through both hangs; no reboot was needed |
| **Never stack launches** | Unchanged and still the expensive one — see `docs/hardware-decode.md` |

---

### 9.1 The video-out kernel panic — post-mortem **[E]**

2026-08-11. Worth a section rather than a table row, because the *reasoning*
error is more reusable than the fact.

**What was done and why.** Moonlight-ps4 — the only implementation known to
drive `libSceVideodec2` successfully — opens video out *before* it allocates a
compute queue and *before* `CreateDecoder`, and its comment ties that ordering
to `CPU_FAULT_SUBMITDONE_TIMEOUT`, a **submit** fault. Our decode dies at the
submit. No probe in this project had ever opened video out. It looked like the
largest remaining structural difference, and it was.

**What happened.** The log on `/mnt/usb0` survived because every line is
flushed (rule 4), and it locates the panic exactly:

```
sceVideoOutOpen(user 513995993, BUS_MAIN) -> 0x80290001 FAILED
sceVideoOutOpen(user 0xFF,      BUS_MAIN) -> 1309671680 "ok"
sceVideoOutSetFlipRate(60Hz) -> 0x00000000
--- compute queue ---
QueryComputeMemoryInfo -> 0  size 4805120
queue memory 4805120 B WB_ONION -> 0 virt 2000a0000
<panic - nothing further>
```

The next call was `sceVideodec2AllocateComputeQueue`, which bottoms out in
`sceGnmMapComputeQueue`. **That call had succeeded in every previous run.** The
only new thing in the process was an open video-out handle.

**Three lessons, in order of how much they generalise.**

1. **A lesson from a PS4 *title* does not transfer to a borrowed PS5 app slot.**
   Moonlight owns its process. This payload is injected into a running
   application that already owns the display pipeline. The reasoning behind the
   experiment was sound; the transfer was never checked. **When importing a
   practice from a reference implementation, check that the thing it assumes
   about its process is also true of ours.**

2. **Validate handles by range, not by sign.** `0x4E100000` is positive, so
   `if (h < 0)` passed it. The system-user retry did not fail cleanly — it
   returned garbage. The *first* call had already given the honest answer,
   `0x80290001`, and a fallback overrode it.

3. **"Low risk by construction" is not a risk assessment.** The same build
   carried a probe that issued deliberately malformed ioctls to the decoder fd,
   justified on the grounds that drivers validate their inputs. That probe
   never ran, so it is **not** what panicked the console — but the reasoning
   had nothing behind it. **Any call that reaches a kernel driver directly —
   raw `ioctl` on a device fd, video-out or GPU context acquisition — is a
   panic risk and needs explicit agreement before it is built into a payload.**
   Read-only observation of driver state, which is what produced every real
   finding in §7, carries none of that risk.

**What it cost and what it returned.** About fifty minutes of console recovery.
It returned three things: video out is genuinely unavailable to a payload
(`0x80290001` is a clean refusal, not a bug in the call), the Moonlight
ordering hypothesis is closed rather than left open, and
`optimizeProgressiveVideo` was measured — see §7's frame-size note.

---

## 10. Method notes

Two of these cost real time and are worth carrying forward.

**Read the code before searching the input space.** After the first successful
call, a blind search tried to solve the config struct by hill-climbing on the
error code, assuming a larger code meant more progress. **It does not** —
`0x811D020B` is a *rejection*, while the furthest-progressing path returns the
numerically *smaller* `0x811D0205`. The search fixed a field to a wrong value
and stalled after **1,335 calls**. It also never guessed 66/77/100 or a legal
level, because those are not values a generic list contains. Reading the
validator settled it in minutes. **Never let a search invent its own progress
metric.**

**Every probe needs a control, and the controls earned their keep twice.**
`-lSceVideoOut -lSceAudioOut` showing 3/3 proves the dynlib probe works. A
`sceKernelVirtualQuery` against a known-good module is what revealed that
`VirtualQuery` is broken here — before any conclusion had been built on it. And
deliberately passing wrong struct sizes confirmed the `0x811D0101` reading was
right rather than a coincidence.

**One deploy that dumps beats N deploys that guess.** The review's central
argument, and it held: one deploy turned every ABI question into an offline
`objdump` question. Twelve deploys covered Phases 0–3 — seven for
reconnaissance and the dump, five for the Route B calls. Three hung and cost
nothing but a `timeout`; the console never needed a reboot.

**Phase 4 cost two deploys. The first hung — and still answered four
questions.** Because the probe was batched (rule 6) and every line was flushed
(rule 4), the hang landed *after* the compute requirement, the whole
direct-memory ladder, and 7/7 validation controls had already been recorded.
A probe ordered cheapest-first turns a hang from a wasted trip into a bounded
one: everything before the hang is still evidence, and where it stopped is
itself the finding.

**Then the hang was fixed without a single exploratory deploy.** Reading the
chain down through VdecCore named the blocking import — `sceGnmMapComputeQueue`
— and showed its GOT slot unresolved. The fix was one line, and the next
deploy succeeded. The alternative, sweeping five pipes and eight queues on
hardware, would have cost many deploys and found nothing, because the pipe and
queue were never the problem.

**The second deploy nearly got misread as a second hang.** It exited 124 with
its socket output cut off at exactly the same line. Only the file on
`/mnt/usb0` showed `rc=0` and a released handle. Had the probe not written to
the stick, a success would have been recorded as a failure — and the obvious
next move would have been to abandon Route B.

**Deliberate-error controls confirmed a structure read purely offline, 7 for
7.** Wrong struct sizes, a non-zero reserved byte, an out-of-range pipe id, an
out-of-range queue id and a one-byte-short buffer each produced exactly the
error code the disassembly predicted. That is what makes it safe to say the
hang is not a malformed argument.

**Read the validator to the end, not to the first rejection.** Phases 2–4 read
`libSceVideodec2`'s config validator far enough to get a call accepted, and
stopped there. That was enough to make the query work and enough to get two
field labels backwards — `+0x08` and `+0x0c` were recorded as codec and variant
when they are resource class and codec, which is only visible in the *profile
and level* checks further down, several hundred bytes past the point where the
reading already "worked". The cost was not a wasted deploy; it was worse. It
was six accepted configurations recorded as a codec survey when all six were
H.264, and a phase plan whose HEVC discriminator could not have worked because
neither value in it was HEVC. **A reading that explains the successes you have
is not the same as a reading that is right.**

---

## 11. Tooling

| | |
|---|---|
| `projects/decoder_test/` | module dump + passive export map. No media calls |
| `projects/videodec2_test/` | Route B phase 3: the `sceVideodec2QueryDecoderMemoryInfo` call |
| `projects/computequeue_test/` | Route B phase 4: compute memory, the direct-memory ladder, the compute queue |
| `projects/createdecoder_test/` | Route B phase 5: the codec matrix and `sceVideodec2CreateDecoder` |
| `projects/decodeframe_test/` | Route B phase 6: `sceVideodec2Decode`. Carries its own H.264 stream in `.rodata`. Also locates the GpDec device object and dumps the submitted job. **Video out and the ioctl errno probe were removed after §9.1 — do not put either back** |
| `tools/gen-test-stream.sh` | regenerates that stream and its access-unit index with ffmpeg |
| `projects/slotcheck/` | **is the app slot free?** Sweeps pids, reads `p_comm` from the kernel proc struct, reports which processes hold the decoder modules. **Deploy with `deploy.sh`, not `--run`** - it lands in `SceSpZeroConf` on purpose, so it cannot stack on the slot it is asking about |
| `projects/decoder_test/nid_table.h` | generated — 176 recovered names with NIDs |
| `tools/re/nid.py` | Sony NID hashing, validated 11/11; `aerolib.csv` index |
| `tools/re/analyse.py` | import graph, `.eh_frame_hdr` function inventory, strings |
| `tools/re/disas.sh` | disassemble a dumped segment at its real virtual address |
| `tools/re/gen_nid_table.py` | regenerates `nid_table.h` |
| `tools/psdevwiki-dump.js` | exports psdevwiki from the browser console. The site is behind a Cloudflare managed challenge that rejects curl, wget and TLS-impersonating clients alike, so it runs in an already-cleared tab and drives MediaWiki's `api.php`. Takes every listable namespace |

`tools/re/aerolib.csv` (12.6 MB) is fetched, not committed:

```bash
curl -sfLo tools/re/aerolib.csv \
  https://raw.githubusercontent.com/zecoxao/sce_symbols/master/aerolib.csv
```

GNU `objdump` rather than `llvm-objdump` — LLVM 18's has no raw-binary input
mode, and the dumps have no ELF header.

### Raw evidence has moved out of this repository

`research-logs/` was extracted on 2026-08-11 into a **separate repository,
`PS5-Research`** — console transcripts, deploy logs, disassembly, strings, the
import graph, the methodology notes, and the psdevwiki mirrors with an offline
viewer (`wiki/viewer.html`). Every `research-logs/...` path cited in this
document is now relative to that repository's root.

| there | what |
|---|---|
| `console/` | logs pulled off the PS5, including `evo_decodeframe_log-run16-KP.txt` — the run that panicked the console, recovered off the USB stick |
| `deploy/` | PC-side transcripts, one per deploy |
| `derived/` | import graph, function inventory, strings, disassembly |
| `wiki/` | psdevwiki PS5 and PS4 mirrors + `viewer.html` |
| `references/` | third-party checkouts (gitignored): Moonlight-ps4, SharpProspero |

**Module images stay here**, in `proprietary/dump/`, and were deliberately not
copied into it.

---

## 13. What is left, and the two ways to reach it

### The reference implementations — what they proved and what they cost **[E]**

2026-08-11. Three independent projects drive or reimplement this same library,
and comparing against them was the single most productive hour of the session.
Cloned to `PS5-Research/references/` (gitignored).

| project | what it is | worth |
|---|---|---|
| [Moonlight-ps4](https://github.com/JaimeJimenezG/Moonlight-ps4) | homebrew that **actually hardware-decodes H.264** through `libSceVideodec2`, annotated with console-validated notes | the config diffs below, and the video-out trap |
| [SharpProspero](https://github.com/SvenGDK/SharpProspero) | a **PS5** SDK with Videodec2 bindings | its codec constants match ours exactly (`Hevc = 0xEE049`, `Vp9 = 0x245BFD`), confirming it describes this library |
| [shadPS4](https://github.com/shadps4-emu/shadPS4) | emulator reimplementation | independently asserts the config struct is `0x48` bytes |

**What they confirmed.** Our struct layouts are right, field for field — the
`0x38`/`0x40`/`0x44` error codes recorded in §7 land exactly on
`maxFrameBufferSize`, `frameBufferAlignment` and `reserved0`. Our memory types
are right too: Moonlight uses ONION for `cpuMemory` and `cpuGpuMemory` and
GARLIC for `gpuMemory`, which is what this project already does. Those were the
two most likely places for a silent error, and neither is wrong.

**The naming this project had been using is worth correcting.** What the probes
call "work memory" is `cpuMemory` (`memInfo+0x08`/`+0x10`), "frame pool" is
`gpuMemory` (`+0x18`/`+0x20`), and the third buffer measured as always-zero is
`cpuGpuMemorySize` (`+0x28`/`+0x30`).

**Four config differences**, none of which is a validated-error field, so none
would ever have shown up as a bad return code:

| field | this project | the working client |
|---|---|---|
| `cpuAffinityMask` | `0` (memset default) | `0x3F` — all six cores |
| `optimizeProgressiveVideo` | `false` | `true` |
| `maxFrameHeight` | 1080 | **1088**, macroblock-aligned |
| `maxDpbFrameCount` / depth | 16 / 1 | 4 / 2 |

All four are now aligned in `decodeframe_test`. **None has been tested against
a decode** — the run that would have done it panicked on video out first.

**One discrepancy deliberately not adopted.** Both references set
`resourceType = 1`. On 12.70 that is refused at query with `0x811D0200`, and
`CreateDecoderBid` is documented in §7 as refusing class 1 outright. `0xb6c8`
and `0x12384` stay. This is a real PS4/PS5 firmware difference, not an error to
correct.

**And one dead end, recorded so it is not re-investigated.**
[PS5-3.20_Libs](https://github.com/DNNDHH/PS5-3.20_Libs) has a promising
`libSceVideodec2.c`, but it is auto-generated `jmp qword ptr [rip + ...]` stubs
from `genstub.py`. No implementation. Nothing to learn.

### psdevwiki — mirrored, and mostly a negative **[E]**

1,147 PS5 pages and 5,282 PS4 pages exported with `tools/psdevwiki-dump.js`,
stored in `PS5-Research/wiki/` with an offline viewer.

- **Errno 5200 is not documented anywhere.** Every `5200` and `0x1450` hit is a
  coincidental substring inside unrelated NP/WebAPI codes. `Devices` knows
  `uvd_{dec,enc,bgt}` exists and annotates it only as *"Maybe related to
  gameplay recording"* — no ioctl table, no error codes. **The wiki will not
  answer this question; stop looking there.**
- Every decoder module shares auth ID `4900000000000002` — `VdecCore`, `Savc`,
  `Savc2`, `Shevc`, `Svp9`, `Vdecsw`, `Vdecwrap`, `Videodec`, `Videodec2`,
  `VideoDecoderArbitration`. One privilege family.
- PS4 brokers decode through a dedicated `SceVdecProxy.elf` process (auth
  `3800000000000003`). **The PS5 has no such process** — zero hits across 1,147
  pages. Sony moved to in-process decode against `uvd_dec`, which is the
  architecture §4 describes. A structural confirmation, not a fix.

### The kernel is directly readable — and firmware decryption is not needed **[E]**

The obvious next thought is "decrypt the firmware and read the driver". That is
the wrong tool twice over:

- The **userland** modules are already decrypted. `proprietary/dump/` holds
  them as mapped images pulled from live process memory, which is
  post-decryption by definition.
- The **kernel** does not need decrypting either. `ps5/kernel.h` in the payload
  SDK exports `KERNEL_ADDRESS_TEXT_BASE` already resolved for this firmware,
  plus `kernel_copyout` — arbitrary kernel read. That yields the *running,
  relocated* kernel, which is strictly better than a PUP dump because the
  addresses are the real ones.

The primitives that matter, all present in the SDK on this machine:

```c
extern const intptr_t KERNEL_ADDRESS_TEXT_BASE;
int32_t  kernel_copyout(intptr_t kaddr, void *udaddr, size_t len);
intptr_t kernel_get_proc_file(pid_t pid, int fd);   /* struct file * for fd 16 */
uint64_t kernel_get_ucred_authid(pid_t pid);
int32_t  kernel_set_ucred_authid(pid_t pid, uint64_t authid);
int32_t  kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]);
```

`kernel_copyout` is recorded in §2 as working and **unable to fault the
caller**, so reading is genuinely low risk — the opposite profile from §9.1.

**And this codebase already elevates process identity.**
`prospero_media_standalone/core/pt.c` temporarily sets its own authid to
`0x4800000000010003` and caps to all-`0xFF`, performs a privileged syscall,
then restores both. `elfldr.c` does the same with caps. So the technique is
proven here, not speculative.

**The hypothesis that follows, stated so it can be killed cheaply:** if the
`uvd_dec` driver checks the caller's authid or capabilities, then what it is
refusing is our process's *identity*, not our job — which would make errno 5200
the same shape as every other blocker in this effort. It is **[H]**. Nothing
yet shows the driver checks anything of the sort.

**How to settle it, cheapest and safest first:**

1. **Read-only, no driver calls.** Report `kernel_get_ucred_authid` and
   `kernel_get_ucred_caps` for the payload. Then scan kernel `.text` from
   `KERNEL_ADDRESS_TEXT_BASE` for the immediate `0x1450`, dump whatever
   function contains it with `kernel_copyout`, and disassemble it offline with
   the existing `tools/re/` setup. This *reads* the answer instead of guessing
   it — the method that cleared every gate in §7, applied one layer deeper
   (rule 10).
2. **Needs explicit sign-off, because it writes kernel state.** Wrap the
   `sceVideodec2Decode` call in the same authid/caps elevation `pt.c` already
   uses, and retry. Per-process, temporary and restorable — but a kernel write,
   and §9.1 is why that is not a decision to take unilaterally.

---

## 12. Open questions

In the order they should be answered.

1. ~~**Does `sceVideodec2CreateDecoder` succeed?**~~ **Answered: yes.** `rc=0`
   on both resource classes, no entitlement gate. §7.
2. ~~**Does the module accept HEVC and VP9?**~~ **Answered: HEVC yes, VP9 no.**
   HEVC needs resource class `0x12384` and returns its own memory arithmetic;
   VP9 is refused with `0x811D0200` at every configuration tried. §7.
3. ~~**How big is `memInfo+0x28`?**~~ **Answered: zero**, for every accepted
   configuration. The third buffer is never needed on these paths.
4. ~~**Does HEVC *create*?**~~ **Answered: yes.** `rc=0`, codec index `4`,
   clean delete, no hang on the `sceSysmodule` path. §7.
5. ~~**Which bitstream framing does `Decode` want?**~~ **Answered: Annex-B.**
   **[E]** Annex-B reaches the hardware submit and is refused there; AVCC is
   rejected *earlier*, in software, with `0x811D0303` (VdecCore stream error).
   A framing the decoder will not even look at is not the one it wants. AVCC is
   dropped from the probe.
5b. **Does `sceVideodec2Decode` produce a picture?** Still no. Everything on our
   side of the ioctl is now measured and well formed; the driver refuses with
   errno 5200. **This is the only question that matters, and §13 is how to
   answer it.**
6. **What does a decoded frame actually look like?** Partly answered without a
   deploy: the `mapMemorySize` arithmetic says **NV12 or P010 at a stride of
   `align(width, 256)`**, H.264 padding height to 16 and HEVC not padding, with
   **four or five** 1 KiB metadata blocks in the tail — five normally, four when
   `optimizeProgressiveVideo` is set. **[I]** Still open and still to be
   measured: tiling, plane pointers, CPU readability, cacheability, lifetime and
   ownership.
7. **Can a frame reach `libSceVideoOut` without a CPU copy?** The question that
   decides whether this is faster or merely different. **Note before designing
   this: a payload cannot open video out at all** — the logged-in user is
   refused with `0x80290001`, and trying it panicked the console (§9.1). Phase
   8 needs a route that does not involve this payload owning a video-out
   handle, and that constraint is now a measurement rather than a guess.
8. **Is the compute queue actually mandatory for `CreateDecoder`?** Deliberately
   left untested. VdecCore does not NULL-check it, so the failure mode is a
   fault rather than an error code, and a faulted payload holds the app slot.
9. **Which module owns `sceVideodec2MapMemory`'s import, and what is the call
   for?** Its GOT slot never bound, so the dump does not say. **Worth retrying
   now**: the identical-looking arbitration slot turned out to be supplied by a
   module nobody had loaded (`libSceVideoArbitration.sprx`), so the answer here
   may also just be an unloaded provider. Not on the decode path, so not urgent — but it is the one entry point in this module that
   cannot currently be called safely.
10. **What loads at `sceAvPlayerInit` time?** The module table after a successful
    Init is the only way to see the true dependency set. Route A is now the
    worked example for feeding a decoder, which is the next unknown.
11. **Is the 40-byte `SceAvPlayerFrameInfo` field order the PS4 one?** Size
    matches; order is unverified.
12. **Is the MP4 demuxer statically linked into AvPlayer?** `libSceMp4.sprx`
    does not exist, yet AvPlayer references `sceMp4*` by name.
13. **Does the `uvd_dec` driver check the caller's authid or capabilities?**
    The live question. §13 sets out how to read the answer out of the kernel
    rather than guess it.
14. **What are ioctl commands 22 and 24 for?** The codec layer always chooses
    23 for H.264 on this firmware (mode index 5, invariant across every
    configuration tried). Commands 22 and 24 are unreachable from the public
    API, so whatever they do is not selectable by us.
