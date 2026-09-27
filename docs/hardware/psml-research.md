# PSML / PSSR on PS5 Pro — can EVO use Sony's upscaler? (#103 step 4)

**Status (2026-09-26): researched offline + by read-only FTP on the dev PS5 Pro
(FW 12.70). No PSML function has been called or loaded. Route 1 (Pro mode)
was tried and hit a wall: EVO has no way to observe Trinity mode. See the
[experiment log](#experiment-log). All experiment code has been removed; only
these notes remain.** EVO's shipped AI upscaling
is Anime4K in EVO's own shaders ([upscaler.md](upscaler.md)); it does **not**
use PSML.

Method, all safe: `LIST`/`RETR` over the console's FTP (read-only), then
offline `llvm-readelf` on the downloaded modules. The downloads are in the
git-ignored `proprietary/sprx/psml/` and `proprietary/appmeta/`; never commit
them. No console or kernel memory was read.

## The modules

| Module | Location | Exports | Links against | App-importable? |
|---|---|---|---|---|
| `libScePsml.sprx` ("Prospero_**Game**_PRX_psml") | `/system/common/lib` | 41: MFSR + MFSR2 (= PSSR) | `libSceAgc`, `libSceAgcDriver` | **yes** (common lib) |
| `libScePsml_ND.sprx` | `/system/common/lib` | 43: same + `...1300` variants | same | yes |
| `libSceSisrMgr.sprx` | `/system/common/lib` | 0 | VideoOut, SystemService | nothing to call |
| **`libScePsmlBcSisr.sprx`** ("Prospero_**System**_PRX_psmlBcSisr") | **`/system/priv/lib`** | 6: BcSisr | **`libSceAgcVsh`** (system shell's AGC) | **no** |
| `libSceSisrSys.sprx` | `/system/priv/lib` | `sceSisrSysSetup`, `sceSisrSysProcessUmdEvent` | `libScePsmlBcSisr`, AgcVsh | no |

`/system/priv/lib` cannot be imported by a fake-signed app module (the loader
rejects the whole module, CE-108255-1 — proven with `libSceVcodec`, see
`tools/native-app/stubs/prx/README.md`).

### BcSisr — single-image SR (the video-shaped one)

Full API, including the two unnamed exports recovered by NID hashing
(`sha1(name + 518D64A635DED8C1E6B039B1C3E55230)`, verified against known pairs):

```
Opts9LtjSfo  scePsmlBcSisrGetInternalBufferRequirement
O4-EydkeC3M  scePsmlBcSisrInit
sIdqI1s+z50  scePsmlBcSisrGetPacketSizeInDwords
ZyqURPTSHl0  scePsmlBcSisrBuildPacket        <- recovered
SfklkVp-lhc  scePsmlBcSisrGetTexture         <- recovered
A0kDyeWNSvs  scePsmlBcSisrTerm
```

- **What it loads.** Model `/system/common/psml_package/BCSISR_v070.psp`
  (5.6 MB, `PSP\0` container with PSSL shader blobs, `barefoot`/`sl00`
  trailers, plus weights). Strings reference `BCSISR_unetv1`: a U-Net.
- **When it refuses.** `[PSML_BC_SISR ERROR] ... Current mode isn't Trinity.`
  It only runs in PS5 Pro mode.
- **Who uses it.** It is a *system* component (`system_shader=PsmlBcSisr`,
  links AgcVsh) used by `libSceSisrSys`, most likely the Pro's system-level
  upscaling of non-enhanced games. It is not an app API.

### MFSR / MFSR2 — PSSR, in `libScePsml` (importable)

- **What it is.** Temporal (multi-frame) upscaling for games. Its models are
  `MFSR4K_v070.psp` and `MFSR8K_v070.psp`, plus `KPN9_*.psp`.
- **What it needs.** Refuses unless `sceKernelIsTrinityMode()`
  (`[PSML] MFSR isn't supported in sceKernelIsTrinityMode() == 0`), and reads
  `mfsrVersion` from the title's `param.json`.
- **Why it doesn't fit video.** It needs per-pixel motion vectors, depth and
  camera jitter from a renderer. Decoded video has none of these.

Recovered MFSR names: `3WVD91e12ZQ scePsmlMfsrInit`,
`JaLBe0P3jSU scePsmlMfsrReleaseContext`,
`eWoKNeB6V-k scePsmlMfsrCreateSharedResources`,
`jEevBXmagOQ scePsmlMfsrReleaseSharedResources`.

## How a title gets Trinity (Pro) mode

`param.json` of installed games (`/system_data/priv/appmeta/<id>/param.json`):

| Title | Pro-enhanced | `psml` | `attribute3` |
|---|---|---|---|
| Avatar: Frontiers of Pandora | yes | `{"mfsrVersion": "11.00"}` | `0x08400040` |
| The Last of Us Part I | yes | `{"mfsrVersion": "09.60"}` | `0x004400D4` |
| Assassin's Creed Shadows | yes | `{"mfsrVersion": "11.00"}` | `0x08440054` |
| Alan Wake Remastered | no | — | `0` |
| **EVO Player (PPSA99039)** | no | — | `0x00080040` |

The only `attribute3` bit all three Pro titles share and EVO lacks is
**`0x00400000`**, the strongest candidate for "PS5 Pro enhanced", which would
make the system launch the title in Trinity mode. **Unverified.**

**Which query PSML actually uses** (imports read from the modules):

| Module | imports |
|---|---|
| `libScePsml` (MFSR) | `sceKernelIsTrinityMode` = NID **`tU5e3f9gSiU`** (computed; not in `third_party/new_nid.txt`) |
| `libScePsmlBcSisr` | `sceKernelIsAuthenticTrinity` (`X0HkB92+NRE`) |

`libScePsml` imports `sceKernelIsTrinityMode` as `#G#H`: module 7 /
library 6, which is its libkernel entry, and the same slot as
`sceKernelUsleep`. So it is a plain libkernel export, not a private one. EVO's
original probe never asked it; see the experiment log for how it is asked
now.

## Routes, cheapest first

1. **Trinity-mode experiment (cheap, reversible).** Set `attribute3 |=
   0x400000` and add `"psml": {"mfsrVersion": "11.00"}` to EVO's
   `sce_sys/param.json`, rebuild, launch.
   - **Pass/fail:** EVO still launches, and something observable changes,
     e.g. `evo_hw` also tries `sceKernelIsTrinityMode`.
   - **Worst case:** the title fails to launch. Revert the change.
   - **Benefits either way:** Auto network selection, and any Pro-only
     behaviour.
2. **BcSisr through Sony's library — blocked.** It is a priv-lib *import*
   (bricks the load), and a runtime `sceKernelLoadStartModule` from an app
   module has never worked for undeclared system PRXs. It also drags in
   `libSceAgcVsh`, the system shell's AGC, whose packets and state it would
   expect. Don't attempt it without a strong reason.
3. **Run the BcSisr model with EVO's own runtime (big).** The `.psp` holds
   compiled PSSL shaders and weights.
   - **What it takes:** reverse-engineer the container, rip the shaders (as
     ShaderRip-PS5 does for game dumps), and dispatch them through EVO's AGC
     runtime. That needs compute dispatch, which EVO doesn't have yet.
   - **Licensing:** the model must be **read from the user's own console at
     runtime** (`/system/common/psml_package/`), never shipped. It is Sony's
     copyrighted work.
   - **Mode check:** the shaders themselves don't check Trinity mode; the
     library does. Whether they need the Pro's ML hardware is unknown.
4. **MFSR (PSSR) for video — poor fit.** It could be driven with zero motion
   vectors, but a temporal accumulator on moving video without motion vectors
   ghosts. It would need an optical-flow pass first.

Recommendation: do **1**. It's small, and it's the key fact every other route
depends on. Decide on **3** after that.

## Experiment log

| Date | Build | Result |
|---|---|---|
| 2026-09-26 | `--pro-mode` (`attribute3 \|= 0x400000`, `psml.mfsrVersion "11.00"`) | EVO **launches normally**. The flags are accepted. |
| 2026-09-26 | probe asks `sceKernelIsTrinityMode` by `sceKernelDlsym` | `0x80020003`, like the other two queries: still blind. |
| 2026-09-26 | control: `sceKernelDlsym` of `sceKernelUsleep` | **`0x80020003` too.** `sceKernelDlsym` is unusable from the app module for any name, on handles `0x2001` / `0x2` and via `sceKernelLoadStartModule("libkernel.sprx")`. Every earlier "not found" was dlsym, not the Trinity functions. |
| 2026-09-27 | **direct import** of `sceKernelIsTrinityMode`, via a one-line `libkernel.sprx` link stub (`tU5e3f9gSiU#G#H`, the same module/library record `libScePsml` uses), with the Pro-mode flags | **Crashes at launch; no `evo.log` is written.** The loader rejects the module: libkernel does not export this function to a fake-signed app, even though a system PRX imports it the same way. **Don't retry.** |

### Conclusion

EVO cannot read the Trinity state:
- `sceKernelDlsym` resolves nothing from the app module.
- The direct import is refused by the loader.
- The other two queries are behind the same wall.

The `--pro-mode` build flag, the `libkernel.syms` stub and the probe changes
were all reverted. The `param.json` flags are harmless, but without a way to
read the mode there is no evidence they do anything, so they are not shipped.

What would reopen this:
- **Another read-only source for the console model** that works from an app
  module (a sysctl, or a VideoOut or system-service query).
- **Something observable that only happens in Trinity mode.** For example,
  `libScePsml`'s `scePsmlMfsr2Init` succeeding. But importing `libScePsml`
  means a positional DT_NEEDED on a common-lib PRX, which is loadable in
  principle and untested.
