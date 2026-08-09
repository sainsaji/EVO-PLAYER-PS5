# Native PS5 media library research

Research area for hardware-accelerated decoding. **Nothing here is implemented**
— this documents what was found and what to try, so experiments start from
facts rather than assumptions.

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
