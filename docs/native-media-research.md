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
against. Do not assume these APIs are directly callable.

## Two routes

### Route 1 — runtime symbol resolution (start here)

No proprietary files, nothing to install. The SDK's `<ps5/kernel.h>` exposes:

```c
int      kernel_dynlib_handle(pid_t pid, const char *basename, uint32_t *handle);
intptr_t kernel_dynlib_dlsym(pid_t pid, uint32_t handle, const char *sym);
intptr_t kernel_dynlib_resolve(pid_t pid, uint32_t handle, const char *nid);
intptr_t kernel_dynlib_mapbase_addr(pid_t pid, uint32_t handle);
```

If a module is already mapped into the payload's process, these hand back
function addresses directly. `sceKernelDlsym` and
`sceSysmoduleLoadModuleInternal` are also available from the libkernel and
Sysmodule stubs.

**`projects/decoder_test` does exactly this.** Build and run it, then read the
klog:

```bash
./scripts/build.sh decoder_test
PS5_HOST=192.168.1.50 ./scripts/deploy.sh output/elf/decoder_test.elf
```

It reports which candidate modules are mapped and which symbols resolve.
**Record the output in this file** — that result determines whether route 1 is
viable at all.

A likely outcome is that nothing media-related is mapped, because a payload
injected into a lightweight host process does not have the media stack loaded.
In that case try, in order:

1. `sceSysmoduleLoadModuleInternal()` with the module id
2. running the payload inside a process that already uses the decoder
3. route 2

### Route 2 — generated stubs from a decrypted SPRX

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

## Open questions

- [ ] Are any `libSceVdec*` modules mapped in an elfldr payload process on 12.70?
- [ ] Does `sceSysmoduleLoadModuleInternal` succeed for them from a payload?
- [ ] Do the PS4 `sceVideoDecoder*` signatures still hold on PS5, or has the ABI changed?
- [ ] Is `libSceAvPlayer` usable without a full application sandbox (it expects app-style file access and a user id)?
- [ ] Can decoded frames be handed to VideoOut without an intermediate CPU copy?
- [ ] Does `libSceVideoDecoderArbitration` gate access when no licensed title is running?

## Results log

*(append findings here, newest first — include firmware, date, and the raw
`decoder_test` output)*

Nothing recorded yet. `decoder_test` has not been run on hardware.
